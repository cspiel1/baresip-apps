/**
 * @file auwm8960_src.c wm8960 I2S audio driver module - source
 *
 * Copyright (C) 2023 Christian Spielberger
 */
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "auwm8960.h"

#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>


struct ausrc_st {
	thrd_t thread;
	bool run;
	void *sampv;
	size_t sampc;
	ausrc_read_h *rh;
	void *arg;
	struct ausrc_prm prm;

	uint32_t *pcm;
};

static struct ausrc_st *d;

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	/* Wait for termination of other thread */
	if (st->run) {
		info("auwm8960: stopping recording thread\n");
		st->run = false;
		thrd_join(st->thread, NULL);
	}

	mem_deref(st->sampv);
	mem_deref(st->pcm);
}


static void convert_pcm(struct ausrc_st *st, size_t sampc)
{
	uint32_t j;
	uint16_t *sampv = st->sampv;
	for (j = 0; j < sampc; j++) {
		uint32_t v = st->pcm[j];
		uint16_t *o = sampv + j;
		*o = v >> 15;
		/* if negative fill with ff */
		if (v & 0x80000000)
			*o |= 0xfffe0000;
	}
}


static int auwm_read_thread(void *arg)
{
	int num_frames;
	struct auframe af;
	int err;
	struct ausrc_st *st = d;

/*        err = auwm8960_start(st->prm.srate, I2O_RECO);*/

	num_frames = 0;
	err = 0;
	if (err) {
		warning("auwm8960: could not start ausrc\n");
		return err;
	}

	auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
		     st->prm.srate, st->prm.ch);
	while (st->run) {
		/* set frames read really */
		if (st->prm.ch && st->prm.srate) {
			num_frames += st->sampc / st->prm.ch;
			af.timestamp = num_frames * AUDIO_TIMEBASE / st->prm.srate;
		}
/*                convert_pcm(st, st->sampc);*/

		st->rh(&af, st->arg);
		sys_msleep(20);
	}

/*        auwm8960_stop(I2O_RECO);*/
	info("auwm8960: stopped ausrc thread\n");

	return 0;
}


int auwm8960_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		   struct ausrc_prm *prm, const char *device,
		   ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	size_t sampc;
	int err;

	(void) device;
	(void) errh;

	if (!stp || !as || !prm || !rh)
		return EINVAL;

	if (prm->fmt!=AUFMT_S16LE) {
		warning("auwm8960: unsupported sample format %s\n", aufmt_name(prm->fmt));
		return EINVAL;
	}

	sampc = prm->srate * prm->ch * prm->ptime / 1000;
	if (sampc % (DMA_SIZE / 4)) {
		warning("auwm8960: sampc=%d has to be divisible by DMA_SIZE / 4\n",
				sampc);
		return EINVAL;
	}

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	d = st;
	st->prm = *prm;
	st->rh  = rh;
	st->arg = arg;

	st->sampc = sampc;
	st->sampv = mem_zalloc(st->sampc * 2, NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}
	st->pcm = mem_zalloc(st->sampc * 2, NULL);
	if (!st->pcm) {
		err = ENOMEM;
		goto out;
	}

	info("%s starting src thread sampc=%lu\n", __func__,
	     (unsigned long) st->sampc);
	st->run = true;
	err = thread_create_name(&st->thread, "auwm8960_src", auwm_read_thread,
				 NULL);
	if (err) {
		st->run = false;
		goto out;
	}

	debug("auwm8960: recording started\n");

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
