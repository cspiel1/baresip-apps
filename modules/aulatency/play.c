/**
 * @file aulat/play.c Audio round-trip measurement -- playback
 *
 * Copyright (C) 2023 c.spielberger@commend.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "aulatency.h"


struct auplay_st {
	thrd_t thread;
	volatile bool run;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
	char *device;

	struct aulat *aulat;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (st->run) {
		debug("aulatency: stopping auplay thread (%s)\n", st->device);
		st->run = false;
		thrd_join(st->thread, NULL);
	}

	mem_deref(st->aulat);
}


static int write_thread(void *arg)
{
	struct auplay_st *st = arg;
	uint64_t ts = tmr_jiffies_usec();
	struct auframe af;
	size_t sampc;

	sampc = st->prm.srate * st->prm.ptime / 1000;
	auframe_init(&af, st->prm.fmt, NULL, sampc, st->prm.srate,
		     st->prm.ch);
	af.sampv = mem_zalloc(auframe_size(&af), NULL);

	while (st->run) {
		uint64_t t = tmr_jiffies_usec();

		st->wh(&af, st->arg);

		aulat_recv_frame(st->aulat, &af, ts, t);

		ts += st->prm.ptime * 1000;
		t = tmr_jiffies_usec();
		if (ts > t)
			sys_usleep(ts - t);
	}

	mem_deref(af.sampv);
	return 0;
}


int aulat_play_alloc(struct auplay_st **stp, const struct auplay *ap,
	       struct auplay_prm *prm, const char *device,
	       auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;
	(void)device;

	if (!stp || !ap || !prm)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("aulatency: auplay supports only format s16le\n");
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->wh  = wh;
	st->arg = arg;

	st->aulat = aulat_get();
	if (!st->aulat)
		return ENOMEM;

	*stp = st;

	st->run = true;
	err = thread_create_name(&st->thread, "aulat_play", write_thread, st);
	if (err) {
		st->run = false;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return 0;
}
