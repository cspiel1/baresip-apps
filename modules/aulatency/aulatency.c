/**
 * @file aulatency.h  Audio round-trip latency measurement
 *
 * Copyright (C) 2023 c.spielberger@commend.com
 */
#include <stdlib.h>
#include <math.h>
#include <re.h>
#include <re_atomic.h>
#include <rem.h>
#include <baresip.h>
#include <string.h>
#include "aulatency.h"

enum {
	EMA_COEFF       = 10,    /* Divisor of EMA coefficient               */
	MAX_BLOCKING    = 20,    /* Maximum blocking delay in [ms]           */
	COMPUTE_PERIOD  = 1000,  /* Jitter compute period in [ms]            */
	SAMPLE_LEVEL    = 32767 / 10 * 9, /* Level for samples               */
	IMPULSE_TIMEOUT = 1000,  /* Timeout of audio impulse                 */
};


/**
 * @defgroup aulat aulat
 *
 * Audio roundtrip latency measurement module
 *
 * This modules registers an AUPLAY and an AUSRC. The AUSRC injects sine waves
 * frame by frame with different frequencies to encode sequence numbers into
 * the signal. The AUPLAY decodes the sequence number and computes the
 * round-trip latency and jitter of the signal. E.g. module menu can be used
 * to invoke a call with command `dial` and start the measurement. The data is
 * collected and printed after the call terminates.
 */


static struct ausrc *ausrc;
static struct auplay *auplay;

/**
 * Write handler invocation statistics
 */
struct whstat {
	uint64_t ts0;       /* Reference time stamp in [us]                  */
	uint64_t tr0;       /* Reference real time in [us]                   */
	uint64_t jitter;    /* Invocation jitter in [us]                     */

	uint32_t bcnt;      /* Write handler long blocking counter           */
};

/**
 * Read handler invocation statistics
 */
struct rhstat {
	uint64_t ts0;       /* Reference time stamp in [us]                  */
	uint64_t tr0;       /* Reference real time in [us]                   */
	uint64_t jitter;    /* Invocation jitter in [us]                     */

	uint32_t bcnt;      /* Read handler long blocking counter            */
};

struct aulat {
	RE_ATOMIC uint64_t tr; /* Point in time when audio impulse was sent  */
	uint64_t latency;      /* Round-trip latency in [us]                 */
	uint64_t jitter;       /* Round-trip jitter in [us]                  */
	bool timeout;          /* Impulse timeout occurred flag              */
	uint32_t timeout_cnt;  /* Impulse timeout counter                    */

	struct whstat ws;      /* Write handler invocation statistics        */
	struct rhstat rs;      /* Read handler invocation statistics         */

	uint64_t t00;          /* Time reference for plot                    */
};


static struct aulat *_d = NULL;
static mtx_t *_lock = NULL;
static uint32_t nr = 0;


bool aulat_auframe_fill(struct aulat *st, struct auframe *f, bool clear)
{
	int16_t *si = f->sampv;
	size_t dj = f->sampc/4;


	if (st->tr) {
		if (clear)
			memset(f->sampv, 0, auframe_size(f));

		return false;
	}

	if (f->fmt != AUFMT_S16LE)
		return false;

	for (size_t j = dj; j < 2*dj; ++j)
		si[j] = + SAMPLE_LEVEL;

	for (size_t j = 2*dj; j < 3*dj; ++j)
		si[j] = - SAMPLE_LEVEL;

	st->tr = tmr_jiffies_usec();
	return true;
}


static bool aulat_detect_impulse(struct aulat *aulat,
				  const struct auframe *f)
{
	int16_t *vp;
	int16_t v = 0;
	uint32_t cnt = 0;
	size_t dj = f->sampc/4;

	vp = f->sampv;

	for (size_t j = dj; j < 3*dj; ++j) {
		v = vp[j];
		if (abs(v) > SAMPLE_LEVEL / 8)
			cnt++;

		if (cnt > 5)
			return true;
	}

	if (tmr_jiffies_usec() - aulat->tr > IMPULSE_TIMEOUT * 1000) {
		aulat->timeout = true;
		debug("aulatency: impulse timeout\n");
		return true;
	}

	return false;
}


static void aulat_destructor(void *arg)
{
	struct aulat *aulat = arg;

	if (aulat == _d) {
		(void)re_trace_close();
		_d = NULL;
	}
}


struct aulat *aulat_get(void)
{
	int err = 0;

	if (!_d) {
		char fname[32];
		_d = mem_zalloc(sizeof(*_d), aulat_destructor);

		re_snprintf(fname, sizeof(fname), "aulat-%u.json", ++nr);
		err = re_trace_init(fname);
	}
	else {
		mem_ref(_d);
	}

	if (err)
		_d = mem_deref(_d);

	return _d;
}


void aulat_send_frame(struct aulat *aulat, const struct auframe *f,
		       uint64_t ts, uint64_t t)
{
	int64_t d;     /**< Time shift in [us]           */
	uint64_t da;   /**< Absolut time shift in [us]   */
	(void) f;

	uint64_t tr = tmr_jiffies_usec();

	/* read handler invocation statistics */
	if (!aulat->rs.tr0 && !aulat->rs.ts0) {
		aulat->rs.tr0 = tr;
		aulat->rs.ts0 = ts;
	}

	d = (int64_t) ( (tr - aulat->rs.tr0) -
			(ts - aulat->rs.ts0) );
	da = labs(d);
	aulat->rs.jitter += (int64_t) (da - aulat->rs.jitter) / EMA_COEFF;

	/* long blocking check */
	d = (int64_t) (tr - t);
	if (d > MAX_BLOCKING * 1000) {
		++aulat->rs.bcnt;
		debug("aulatency: long async blocking of ausrc read handler "
			"(%ld ms)\n", d);
	}

	if (tr - aulat->rs.tr0 > COMPUTE_PERIOD * 1000) {
		aulat->rs.tr0 = tr;
		aulat->rs.ts0 = ts;
	}
}


void aulat_recv_frame(struct aulat *aulat, struct auframe *f,
		       uint64_t ts, uint64_t t)
{
	int64_t d;     /**< Time shift in [us]           */
	uint64_t da;   /**< Absolut time shift in [us]   */
	uint64_t l;    /**< Current latency in [us]      */
	char buf[128];
	uint64_t tr = tmr_jiffies_usec();

	/* write handler invocation statistics */
	if (!aulat->ws.tr0 && !aulat->ws.ts0) {
		aulat->ws.tr0 = tr;
		aulat->ws.ts0 = ts;
	}

	d = (int64_t) ( (tr - aulat->ws.tr0) -
			(ts - aulat->ws.ts0) );
	da = (uint64_t) labs(d);
	aulat->ws.jitter += (int64_t) (da - aulat->ws.jitter) / EMA_COEFF;

	/* long blocking check */
	d = (int64_t) (tmr_jiffies_usec() - t);
	if (d > MAX_BLOCKING * 1000) {
		++aulat->ws.bcnt;
		debug("aulatency: long async blocking of auplay write handler "
			"(%ld ms)\n", d);
	}

	/* round-trip latency/jitter computation */
	if (!aulat->tr || !aulat_detect_impulse(aulat, f))
		return;

	if (t < aulat->tr) {
		debug("aulatency: negative latency %lu < %lu\n", t, aulat->tr);
		return;
	}

	l = t - aulat->tr;
	aulat->tr = 0;
	if (aulat->timeout) {
		aulat->timeout = 0;
		++aulat->timeout_cnt;
		return;
	}

	d = (int64_t) (l - aulat->latency);
	aulat->latency += d / EMA_COEFF;

	da = (uint64_t) labs(d);
	aulat->jitter += (int64_t) (da - aulat->jitter) / EMA_COEFF;

	if (t - aulat->ws.tr0 > COMPUTE_PERIOD * 1000) {
		aulat->ws.tr0 = tr;
		aulat->ws.ts0 = ts;
	}

	if (!aulat->t00)
		aulat->t00 = t;

	re_snprintf(buf, sizeof(buf), "%lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu,"
		    " %u, %u",
	    (t - aulat->t00)/1000, /* Time when frame arrived at auplay [ms] */
	    l,                     /* Current round-trip latency in [us]     */
	    aulat->latency,        /* Average round-trip latency in [us]     */
	    aulat->jitter,         /* Jitter in [us]                         */
	    aulat->rs.jitter,      /* Read handler invocation jit. in [us]   */
	    aulat->rs.bcnt,        /* Read handler long blocking counter     */
	    aulat->ws.jitter,      /* Write handler invocation jit. in [us]  */
	    aulat->ws.bcnt,        /* Write handler long blocking counter    */
	    aulat->timeout,        /* Impulse timeout flag                   */
	    aulat->timeout_cnt     /* Impulse timeout counter                */
	    );

	re_trace_event("aulatency", "plot", 'P', NULL, 0,
		       RE_TRACE_ARG_STRING_COPY, "line", buf);
}


static int module_init(void)
{
	int err;

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "aulatency",
			      aulat_src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(),
			       "aulatency", aulat_play_alloc);

	err |= mutex_alloc(&_lock);

	info("aulatency: loaded (%m)\n", err);
	return err;
}


static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	_lock = mem_deref(_lock);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(aulat) = {
	"aulatency",
	"application",
	module_init,
	module_close,
};
