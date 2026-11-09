/**
 * @file aurec_src.c Audio source that reads and streams aurec_packet objects
 *
 * Copyright (C) 2022 commend.com - Christian Spielberger
 */
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "aurec.h"


struct ausrc_st {
	struct tmr tmr;
	struct ausrc_prm prm;           /**< Audio src parameter             */
	char *uuid;                     /**< UUID set by device parameter    */
	uint32_t pos_ms;		/**< Start position in [ms]          */
	const struct aucodec *ac;       /**< Audio codec of recordings       */
	struct audec_state *dec;        /**< Audio decoder state             */

	size_t sampc;
	RE_ATOMIC bool run;
	bool started;
	thrd_t thread;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
};


static void destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->started) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}

	tmr_cancel(&st->tmr);
	mem_deref(st->dec);
	mem_deref(st->uuid);
}


static int decode_packet(struct ausrc_st *st,
			 const struct aurec_packet *pkt, struct auframe *af)
{
	if (!st || !pkt || !af)
		return EINVAL;

	size_t len = mbuf_get_left(pkt->mb);
	bool marker = false;
	int err = st->ac->dech(st->dec, af->fmt, af->sampv, &af->sampc,
				 marker, mbuf_buf(pkt->mb), len);
	if (err) {
		warning("aurec: opus decode failed (%m)\n", err);
		return err;
	}

	af->timestamp = pkt->ts;
	return 0;
}


static int src_thread(void *arg)
{
	uint64_t now, ts = tmr_jiffies();
	struct ausrc_st *st = arg;
	int16_t *sampv;
	uint32_t ms = 4;

	sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!sampv)
		return ENOMEM;

	const struct aurec_packet *pkt = aurec_first_packet(st->uuid,
							    st->pos_ms);
	if (!pkt) {
		warning("aurec: no packet found for uuid=%s at %u ms\n",
			st->uuid, st->pos_ms);
		goto out;
	}

	uint64_t ts_max = aurec_ts_max(st->uuid);
	info("aurec: stream recording uuid=%s from %"PRIu64" to %"PRIu64"\n",
	     st->uuid, pkt->ts, ts_max);
	while (re_atomic_rlx(&st->run)) {
		struct auframe af;

		sys_msleep(ms);
		now = tmr_jiffies();
		if (ts > now)
			continue;

		auframe_init(&af, st->prm.fmt, sampv, st->sampc,
		             st->prm.srate, st->prm.ch);
		int err = decode_packet(st, pkt, &af);
		if (err)
			break;

		st->rh(&af, st->arg);

		ts += st->prm.ptime;

		pkt = aurec_next_packet(pkt);
		if (!pkt || pkt->ts > ts_max)
			break;
	}

out:
	mem_deref(sampv);
	re_atomic_rlx_set(&st->run, false);

	return 0;
}


static void timeout(void *arg)
{
	struct ausrc_st *st = arg;
	tmr_start(&st->tmr, st->prm.ptime ? st->prm.ptime : 40, timeout, st);

	/* check if audio buffer is empty */
	if (!re_atomic_rlx(&st->run)) {
		tmr_cancel(&st->tmr);

		info("aurec: end of recording\n");

		/* error handler must be called from re_main thread */
		if (st->errh)
			st->errh(0, "end of recording", st->arg);
	}
}


int aurec_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		    struct ausrc_prm *prm, const char *dev,
		    ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;

	if (!stp || !as || !prm || !rh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	aurec_get_srcprm(&st->prm);
	st->rh    = rh;
	st->errh  = errh;
	st->arg   = arg;
	st->ac    = aurec_codec();
	/* parse uuid,pos */
	struct pl uuid = PL_INIT;
	struct pl pos = PL_INIT;
	if (re_regex(dev, str_len(dev), "[^, ]+,[^ ]+", &uuid, &pos))
		re_regex(dev, str_len(dev), "[^ ]+", &uuid, NULL);

	if (!pl_isset(&uuid)) {
		warning("aurec: missing uuid parameter\n");
		err = EINVAL;
		goto out;
	}

	info("aurec: stream recording uuid=%r from pos=%r\n", &uuid, &pos);
	pl_strdup(&st->uuid, &uuid);
	st->pos_ms = pl_u32(&pos);
	if (!pl_isset(&uuid)) {
		warning("aurec: missing uuid parameter\n");
		err = EINVAL;
		goto out;
	}

	const struct aucodec *ac = st->ac;
	if (!ac)
		return ENOENT;

	if (ac->decupdh) {
		err = ac->decupdh(&st->dec, ac, NULL);
		if (err) {
			warning("audio_recv: alloc decoder: %m\n", err);
			goto out;
		}
	}

	/* return real format to caller */
	*prm = st->prm;
	st->sampc  = prm->srate * prm->ch * prm->ptime / 1000;

	info("aurec_src: audio %u/%u sampc=%zu\n", st->prm.srate,
	     st->prm.ch, st->sampc);

	tmr_start(&st->tmr, st->prm.ptime, timeout, st);

	re_atomic_rlx_set(&st->run, true);
	st->started = true;
	err = thread_create_name(&st->thread, "aurec_src", src_thread, st);
	if (err) {
		st->started = false;
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
