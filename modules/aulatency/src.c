/**
 * @file aulat/src.c Audio round-trip measurement -- source
 *
 * Copyright (C) 2023 c.spielberger@commend.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "aulatency.h"


struct ausrc_st {
	thrd_t thread;
	volatile bool run;
	ausrc_read_h *rh;
	void *arg;
	struct ausrc_prm prm;
	char *device;

	struct aulat *aulat;
};


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->run) {
		debug("aulatency: stopping ausrc thread (%s)\n", st->device);
		st->run = false;
		thrd_join(st->thread, NULL);
	}

	mem_deref(st->aulat);
}


static int read_thread(void *arg)
{
	struct ausrc_st *st = arg;
	uint64_t ts = tmr_jiffies_usec();
	struct auframe af;
	size_t sampc;
	bool filled = false;

	sampc = st->prm.srate * st->prm.ptime / 1000;
	auframe_init(&af, st->prm.fmt, NULL, sampc * st->prm.ch,
		     st->prm.srate, st->prm.ch);
	af.sampv = mem_zalloc(auframe_size(&af), NULL);

	while (st->run) {
		uint64_t t;

		af.timestamp = ts;
		filled = aulat_auframe_fill(st->aulat, &af, filled);
		st->rh(&af, st->arg);

		t = tmr_jiffies_usec();

		aulat_send_frame(st->aulat, &af, ts, t);

		ts += st->prm.ptime * 1000;
		t = tmr_jiffies_usec();
		if (ts > t)
			sys_usleep(ts - t);
	}

	mem_deref(af.sampv);
	return 0;
}


int aulat_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
	      struct ausrc_prm *prm, const char *device,
	      ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	(void)errh;
	(void)device;

	if (!stp || !as || !prm)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("aulat: ausrc supports only format s16le\n");
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm    = *prm;
	st->rh     = rh;
	st->arg    = arg;
	st->aulat = aulat_get();
	if (!st->aulat)
		return ENOMEM;

	st->run = true;
	err = thread_create_name(&st->thread, "aulat_src", read_thread, st);
	if (err) {
		st->run = false;
		goto out;
	}

	debug("aulatency: started src (%s) format=%s\n",
	      st->device, aufmt_name(prm->fmt));

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
