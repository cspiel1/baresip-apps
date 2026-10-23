/**
 * @file aurec.c  Audio recording for emergency events
 *
 * - Continously reads audio from the microphone and buffers a configurable
 *   amount of pre-emergency audio.
 * - When an emergency event is reported with command `aurec <uuid>`, then a
 *   configurable time of the pre-buffered audio stream is opus encoded and
 *   stored in a packet list.
 * - The packet list can be streamed on demand to a SIP call. This module
 *   implements an ausrc that reads the recorded data, decodes and passes this
 *   to the SIP call. There it has to be encoded again with the codec used in
 *   the call.
 *
 * Copyright (C) 2026 commend.com - Christian Spielberger
 */

#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <stdint.h>
#include "aurec.h"

enum {
	MAX_PACKETS = 1500,         /**< 30 seconds maximum  */
	MAX_RECORDINGS = 3,         /**< maximum number of recordings */
	PTIME = 20,                 /**< ptime for the ausrc */
};

struct aurec {

	/* config */
	struct ausrc_prm srcprm;
	uint32_t bitrate;
	uint32_t pre_ms;
	uint32_t post_ms;
	char *fmtp;

	/* buffering (thread: ausrc) */
	struct ausrc_st *src;
	struct aubuf *buf;
	RE_ATOMIC uint64_t ts;     /**< timestamp of nearly now */

	/* recording */
	const struct aucodec *ac;
	struct auenc_state *enc;
	struct auframe af;
	struct list recordings;
	struct list packets;
	struct tmr tmr;
	struct tmr tmr_stop;

	/* call handling */
	struct call *call;
};

struct aurec_recording {
	struct le le;

	char *uuid;     /**< UUID of the recording */
	uint64_t ts;    /**< Timestamp nearly at allocation */
};

static struct aurec aurec = {
	.srcprm = {
		.srate = 48000,
		.ch    = 1,
		.ptime = PTIME,
		.fmt   = AUFMT_S16LE
	},
	.bitrate = 20000,
	.pre_ms = 5000,
	.post_ms = 5000,
};


static int stop_ausrc(void);


static int aurec_init(void )
{
	size_t sz = aufmt_sample_size(aurec.srcprm.fmt);
	size_t pre_size;
	struct pl fmtp = PL_INIT;
	int err;

	(void)conf_get_u32(conf_cur(), "aurec_srate", &aurec.srcprm.srate);
	(void)conf_get_u32(conf_cur(), "opus_bitrate", &aurec.bitrate);
	(void)conf_get(conf_cur(), "opus_fmtp", &fmtp);
	(void)conf_get_u32(conf_cur(), "aurec_pre", &aurec.pre_ms);
	(void)conf_get_u32(conf_cur(), "aurec_post", &aurec.post_ms);
	err = pl_strdup(&aurec.fmtp, &fmtp);
	pre_size = sz * au_calc_nsamp(aurec.srcprm.srate, aurec.srcprm.ch,
				      aurec.pre_ms);

	err |= aubuf_alloc(&aurec.buf, 0, pre_size);
	tmr_init(&aurec.tmr);
	return err;
}


static void aurec_close(void)
{
	tmr_cancel(&aurec.tmr);
	tmr_cancel(&aurec.tmr_stop);
	stop_ausrc();
	aurec.buf = mem_deref(aurec.buf);
	aurec.fmtp = mem_deref(aurec.fmtp);
	list_flush(&aurec.recordings);
}


static void ausrc_read_handler(struct auframe *af, void *arg)
{
	(void)arg;

	if (!aurec.buf)
		return;

	if (af->fmt != (enum aufmt) aurec.srcprm.fmt) {
		warning("aurec: ausrc format mismatch\n");
		return;
	}

	aubuf_write_auframe(aurec.buf, af);
	re_atomic_rlx_set(&aurec.ts, af->timestamp);

	static uint32_t logcnt = 0;
	if (++logcnt % 100 == 0) {
		debug("aurec: buffered %zu ms\n",
		      aubuf_cur_size(aurec.buf) / auframe_size(af));
	}
}


static int start_ausrc(void)
{
	struct config *cfg = conf_config();
	if (!cfg)
		return EINVAL;

	int err = 0;
	if (!aurec.src) {
		const char *mod = cfg->audio.src_mod;
		const char *dev = cfg->audio.src_dev;

		err = ausrc_alloc(&aurec.src, baresip_ausrcl(), mod,
				  &aurec.srcprm,
				  dev, ausrc_read_handler, NULL, NULL);
		if (err) {
			warning("aurec: could not open ausrc %s (%s)\n",
				mod, dev);
			return ENOENT;
		}

		const struct aucodec *ac;
		ac = aucodec_find(baresip_aucodecl(), "opus",
				  aurec.srcprm.srate, aurec.srcprm.ch);
		if (!ac) {
			warning("aurec: could not find codec opus\n");
			err = ENOENT;
			goto out;
		}

		struct auenc_param ac_prm;

		ac_prm.bitrate = aurec.bitrate;
		err = ac->encupdh(&aurec.enc, ac, &ac_prm, aurec.fmtp);
		if (err) {
			warning("audio: could not alloc encoder (%m)\n", err);
			goto out;
		}

		aurec.ac = ac;

		uint32_t sampc = au_calc_nsamp(aurec.srcprm.srate,
					       aurec.srcprm.ch, PTIME);
		size_t sz = sampc * aufmt_sample_size(aurec.srcprm.fmt);
		void *sampv = mem_zalloc(sz, NULL);
		if (!sampv) {
			err = ENOMEM;
			goto out;
		}

		auframe_init(&aurec.af, aurec.srcprm.fmt, sampv, sampc,
			     aurec.srcprm.srate, aurec.srcprm.ch);
		info("aurec: started ausrc %s (%s)\n", mod, dev);
	}
out:
	if (err)
		stop_ausrc();

	return 0;
}


static int stop_ausrc(void)
{
	tmr_cancel(&aurec.tmr);
	tmr_cancel(&aurec.tmr_stop);
	aurec.src = mem_deref(aurec.src);
	aubuf_flush(aurec.buf);
	aurec.enc = mem_deref(aurec.enc);
	aurec.af.sampv = mem_deref(aurec.af.sampv);
	list_flush(&aurec.recordings);
	info("aurec: stopped ausrc\n");
	return 0;
}


static uint64_t ms_to_ts(uint32_t ms)
{
	return ((uint64_t) ms) * AUDIO_TIMEBASE / 1000;
}


static uint64_t recording_ts_start(struct aurec_recording *rec)
{
	if (!rec)
		return 0;

	uint64_t pre_ts = ms_to_ts(aurec.pre_ms);
	return rec->ts > pre_ts ? rec->ts - pre_ts : 0;
}


static void recording_destructor(void *arg)
{
	struct aurec_recording *rec = arg;

	if (!rec)
		return;

	info("aurec: recording for uuid=%s with ts=%"PRIu64" deleted\n",
	     rec->uuid, rec->ts);
	list_unlink(&rec->le);
	rec->uuid = mem_deref(rec->uuid);
	/* delete old packets */
	if (rec->le.next) {
		rec = rec->le.next->data;

		uint64_t ts = recording_ts_start(rec);
		uint64_t ts0, ts1;
		bool found = false;
		struct le *le, *len;
		LIST_FOREACH_SAFE(&aurec.packets, le, len) {
			struct aurec_packet *pkt = le->data;
			if (pkt->ts < ts) {
				mem_deref(pkt);
				if (!found)
					ts0 = pkt->ts;
				else
					ts1 = pkt->ts;

				found = true;
			}
			else {
				break;
			}
		}

		info("aurec: deleted packets from %"PRIu64" to %"PRIu64"\n",
		     ts0, ts1);
	}
	else {
		list_flush(&aurec.packets);
		info("aurec: all packets deleted\n");
	}
}


static void packet_destructor(void *arg)
{
	struct aurec_packet *pkt = arg;

	if (!pkt)
		return;

	list_unlink(&pkt->le);
	mem_deref(pkt->mb);
}


static size_t max_packet_size(void)
{
	/* 8 byte reserve for safety */
	return aurec.bitrate * PTIME / 8000 + 8;
}


static int packet_alloc(struct aurec_packet **pktp,
			      const struct auframe *af)
{
	struct aurec_packet *pkt;
	pkt = mem_zalloc(sizeof(*pkt), packet_destructor);
	if (!pkt)
		return ENOMEM;

	pkt->ts = af->timestamp;
	pkt->mb = mbuf_alloc(max_packet_size());
	list_append(&aurec.packets, &pkt->le, pkt);

	if (list_count(&aurec.packets) > MAX_PACKETS) {
		struct le *le = list_head(&aurec.packets);
		mem_deref(le->data);
	}

	*pktp = pkt;
	return 0;
}


static void record_frame(void *arg)
{
	struct aurec_recording *rec = arg;

	tmr_start(&aurec.tmr, 1, record_frame, rec);

	size_t sampsz = auframe_size(&aurec.af);
	if (aubuf_cur_size(aurec.buf) < sampsz)
		return;

	aubuf_read_auframe(aurec.buf, &aurec.af);

	struct aurec_packet *pkt;
	int err = packet_alloc(&pkt, &aurec.af);
	if (err)
		return;

	bool marker = false;
	size_t len = mbuf_get_space(pkt->mb);
	err = aurec.ac->ench(aurec.enc, &marker, mbuf_buf(pkt->mb), &len,
			     aurec.srcprm.fmt, aurec.af.sampv, aurec.af.sampc);
	if (err) {
		warning("aurec: opus encode failed (%m)\n", err);
		mem_deref(pkt);
		mem_deref(rec);
		return;
	}

	if (!rec->ts)
		rec->ts = pkt->ts;
}


static void stop_recording(void *arg)
{
	(void) arg;

	tmr_cancel(&aurec.tmr);
}


static struct aurec_recording *find_recording(const char *uuid)
{
	struct le *le;
	LIST_FOREACH(&aurec.recordings, le) {
		struct aurec_recording *rec = le->data;
		if (!str_cmp(rec->uuid, uuid))
			return rec;
	}

	return NULL;
}


const struct aurec_packet *aurec_first_packet(const char *uuid)
{
	struct aurec_recording *rec = find_recording(uuid);
	if (!rec)
		return NULL;

	uint64_t ts = recording_ts_start(rec);
	struct le *le;
	LIST_FOREACH(&aurec.packets, le) {
		struct aurec_packet *pkt = le->data;
		if (pkt->ts >= ts)
			return pkt;
	}

	return NULL;
}


uint64_t aurec_ts_max(const char *uuid)
{
	struct aurec_recording *rec = find_recording(uuid);
	if (!rec)
		return 0;

	uint64_t ts = rec->ts + ms_to_ts(aurec.post_ms);
	return ts;
}


struct aurec_packet *aurec_next_packet(const struct aurec_packet *pkt)
{
	if (!pkt)
		return NULL;

	struct le *le = pkt->le.next;
	return le ? le->data : NULL;
}


static int add_recording(const char *uuid)
{
	struct aurec_recording *rec;
	if (!str_isset(uuid))
		return EINVAL;

	rec = mem_zalloc(sizeof(*rec), recording_destructor);
	if (!rec)
		return ENOMEM;

	int err = str_dup(&rec->uuid, uuid);
	if (err) {
		mem_deref(rec);
		return err;
	}

	rec->ts = re_atomic_rlx(&aurec.ts);
	tmr_start(&aurec.tmr, 1, record_frame, rec);
	tmr_start(&aurec.tmr_stop, aurec.post_ms, stop_recording, NULL);

	list_append(&aurec.recordings, &rec->le, rec);
	info("aurec: recording started for uuid=%s with ts=%"PRIu64"\n",
	     uuid, rec->ts);

	if (list_count(&aurec.recordings) > MAX_RECORDINGS) {
		struct le *le = list_head(&aurec.recordings);
		mem_deref(le->data);
	}

	return 0;
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call   *call = bevent_get_call(event);
	struct ua     *ua   = bevent_get_ua(event);
	(void) arg;

	switch (ev) {
		case BEVENT_END_OF_FILE:
			audio_set_source(call_audio(call), NULL, NULL);
			call_update_media(call);
			module_event("aurec", "switch_to_live", ua, call,
				     "uuid");

			break;
		default:
			break;
	}
}


static int cmd_aurec(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	const char *usage = "usage: /aurec <uuid>\n";

	if (!str_isset(carg->prm)) {
		(void)re_hprintf(pf, "%s", usage);
		return EINVAL;
	}

	if (!aurec.src) {
		(void)re_hprintf(pf, "aurec: not enabled\n");
		return EINVAL;
	}

	struct aurec_recording *rec = find_recording(carg->prm);
	mem_deref(rec);

	int err = add_recording(carg->prm);
	if (err) {
		(void)re_hprintf(pf, "failed to add recording (%m)\n", err);
		return err;
	}

	(void)re_hprintf(pf, "aurec: uuid=%s\n", carg->prm);
	return 0;
}


static int cmd_auplay(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *prm = carg->prm;
	struct ua *ua = carg->data;
	char *uri = NULL;
	int err = 0;

	const char *usage = "usage: /aurec_play <uuid> <sipuri>"
			" [cuser=<string>]\n"
			"Stream audio recording in an outgoing SIP call\n";

	struct pl uuid = PL_INIT;
	struct pl dname = PL_INIT;
	struct pl sipuri = PL_INIT;
	/* with display name */
	err = re_regex(prm, str_len(prm),
		       "[^ ]+ [~ \t\r\n<]*[ \t\r\n]*<[^>]+>[ \t\r\n]*",
		       &uuid, &dname, NULL, &sipuri, NULL);
	if (err) {
		/* without display name */
		dname = pl_null;
		err = re_regex(prm, str_len(prm), "[^ ]+ [^ ]+",
			       &uuid, &sipuri);
	}

	if (err) {
		(void)re_hprintf(pf, "%s", usage);
		return EINVAL;
	}

	if (!aurec.src) {
		(void)re_hprintf(pf, "aurec: not enabled\n");
		return EINVAL;
	}

	struct pl cuser = PL_INIT;
	(void)cparam_decode(prm, "cuser", &cuser);
	if (!ua && pl_isset(&cuser)) {
		ua = uag_find(&cuser);
		if (!ua) {
			re_hprintf(pf, "could not find UA for cuser=%r\n",
				   &cuser);
			return EINVAL;
		}
	}

	if (!ua)
		ua = uag_find_requri_pl(&sipuri);

	if (!ua) {
		re_hprintf(pf, "no User-Agent found for request URI %r\n",
			   &sipuri);
		err = EINVAL;
		goto out;
	}

	if (pl_isset(&dname)) {
		err = re_sdprintf(&uri, "\"%r\" <%r>",
				  &dname, &sipuri);
	}
	else {
		err = account_uri_complete_strdup(ua_account(ua),
						  &uri, &sipuri);
	}

	if (err) {
		(void)re_hprintf(pf, "failed to complete dial uri\n");
		goto out;
	}

	re_hprintf(pf, "call uri: %s\n", uri);

	/* Set audio module and device */
	struct call *call = NULL;
	err  = ua_enable_autoanswer(ua, 0, ANSM_RFC5373);
	err |= ua_connect(ua, &call, NULL, uri, VIDMODE_OFF);
	if (err) {
		re_hprintf(pf, "call failed (%m)\n", err);
		mem_deref(call);
		goto out;
	}

	re_hprintf(pf, "call id: %s\n", call_id(call));

	/* switch to buffer */
	audio_set_source(call_audio(call), "aufile",
			 "/usr/share/baresip/ring.wav");

 out:
	mem_deref(uri);

	return err;
}


static int cmd_enable(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *prm = carg->prm;

	const char *usage = "usage: /aurec_enable <bool>\n";

	if (!str_isset(prm)) {
		(void)re_hprintf(pf, "%s", usage);
		return EINVAL;
	}

	bool enable = false;

	int err = str_bool(&enable, prm);
	if (err)
		return err;

	if (enable) {
		err = start_ausrc();
	}
	else {
		(void) stop_ausrc();
	}

	return err;
}


static const struct cmd cmdv[] = {
{"aurec"         ,0,    CMD_PRM, "Start audio recording",    cmd_aurec},
{"aurec_play"    ,0,    CMD_PRM, "Play audio recording",    cmd_auplay},
{"aurec_enable"  ,0,    CMD_PRM, "Enable/Disable audio source", cmd_enable},
};


static int module_init(void)
{
	info("aurec: module loaded\n");
	aurec_init();
	int err;
	err  = bevent_register(event_handler, NULL);
	err |= cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
	bevent_unregister(event_handler);
	aurec_close();
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(aurec) = {
	"aurec",
	"application",
	module_init,
	module_close
};
