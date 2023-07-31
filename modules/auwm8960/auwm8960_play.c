/**
 * @file auwm8960_play.c wm8960 I2S audio driver module - player
 *
 * Copyright (C) 2023 Christian Spielberger
 */
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "auwm8960.h"

#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>


struct auplay_st {
	thrd_t thread;
	RE_ATOMIC bool run;
	void *sampv;
	size_t sampc;
	size_t bytes;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
	const struct device *i2s;

	uint32_t *pcm;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	/* Wait for termination of other thread */
	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}

	mem_deref(st->sampv);
	mem_deref(st->pcm);
}


/* I2S example data  */
/*00 00 73 f3 00 80 75 f3 */
/*00 00 75 f3 00 00 71 f3 */
/*00 80 72 f3 00 00 7c f3 */
/*00 00 83 f3 00 80 7e f3 */
/*00 80 78 f3 00 80 76 f3 */
/*00 80 73 f3 00 80 6d f3 */
/*00 00 6a f3 00 80 6c f3 */
/*00 00 69 f3 00 00 63 f3 */
// -------------------------------------------------------------------------
static void convert_sampv(struct auplay_st *st, size_t sampc)
{
	uint32_t j;
	int16_t *sampv = st->sampv;
	return;
	for (j = 0; j < sampc; j++) {
		uint32_t v = sampv[j];
		st->pcm[j] = v << 17;
	}
}


static int write_thread(void *arg)
{
	struct auplay_st *st = arg;
	struct auframe af;
	int err = 0;

	auframe_init(&af, st->prm.fmt, st->sampv, st->sampc, st->prm.srate,
		     st->prm.ch);

/*        err = auwm8960_start(I2O_PLAY, &st->prm);*/
	if (err) {
		warning("auwm8960: could not start auplay\n");
		return err;
	}

/*        st->i2s = auwm8960_i2s();*/
/*        if (!st->i2s)*/
/*                return EINVAL;*/

	while (re_atomic_rlx(&st->run)) {
		st->wh(&af, st->arg);
		convert_sampv(st, st->sampc);
/*                i2s_buf_write(st->i2s, st->pcm, st->sampc * 2);*/
		sys_msleep(20);
	}

/*        auwm8960_stop(I2O_PLAY);*/
	info("auwm8960: stopped auplay thread\n");

	return 0;
}


int auwm8960_play_alloc(struct auplay_st **stp, const struct auplay *ap,
		    struct auplay_prm *prm, const char *device,
		    auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;

	(void) device;

	if (!stp || !ap || !prm || !wh)
		return EINVAL;

	if (prm->fmt!=AUFMT_S16LE) {
		warning("auwm8960: unsupported sample format %s\n",
			aufmt_name(prm->fmt));
		return EINVAL;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->wh  = wh;
	st->arg = arg;

	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
	st->bytes = 2 * st->sampc;
	st->sampv = mem_zalloc(st->bytes, NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}
	st->pcm = mem_zalloc(st->sampc * 2, NULL);
	if (!st->pcm) {
		err = ENOMEM;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "auwm8960_play", write_thread,
				 st);
	if (err) {
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	debug("auwm8960: playback started\n");

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
