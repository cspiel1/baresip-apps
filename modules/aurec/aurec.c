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
	struct aubuf *buf;         /**< buffer for one frame (ptime) */
	size_t psize;              /**< size of one frame (ptime) */
	struct list prebuf;        /**< pre-buffered opus packets */
	uint32_t precnt;           /**< length of prebuf list */
	RE_ATOMIC uint64_t ts;     /**< timestamp of nearly now */

	/* recording */
	const struct aucodec *ac;
	struct auenc_state *enc;
	struct auframe af;           /**< frame for reading from aubuf */
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
static void aurec_close(void);


static int aurec_init(void )
{
	size_t sz = aufmt_sample_size(aurec.srcprm.fmt);
	struct pl fmtp = PL_INIT;
	int err = 0;

	(void)conf_get_u32(conf_cur(), "aurec_srate", &aurec.srcprm.srate);
	/* TODO: separate bitrate config for aurec */
	(void)conf_get_u32(conf_cur(), "opus_bitrate", &aurec.bitrate);
	(void)conf_get(conf_cur(), "opus_fmtp", &fmtp);
	(void)conf_get_u32(conf_cur(), "aurec_pre", &aurec.pre_ms);
	(void)conf_get_u32(conf_cur(), "aurec_post", &aurec.post_ms);
	if (pl_isset(&fmtp))
		err = pl_strdup(&aurec.fmtp, &fmtp);

	aurec.psize = sz * au_calc_nsamp(aurec.srcprm.srate, aurec.srcprm.ch,
					 aurec.srcprm.ptime);
	err |= aubuf_alloc(&aurec.buf, 0, 2 * aurec.psize);
	aurec.precnt = aurec.pre_ms / aurec.srcprm.ptime;

	debug("aurec: pre-buffer length %u\n", aurec.precnt);
	tmr_init(&aurec.tmr);
	if (err) {
		warning("aurec: init failed (%m)\n", err);
		aurec_close();
	}

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
	list_flush(&aurec.prebuf);
	list_flush(&aurec.packets);
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
	/* 16 byte reserve for safety */
	return aurec.bitrate * PTIME / 8000 + 16;
}


static struct aurec_packet *auframe_encode(const struct auframe *af)
{
	struct aurec_packet *pkt;
	pkt = mem_zalloc(sizeof(*pkt), packet_destructor);
	if (!pkt)
		return NULL;

	pkt->ts = af->timestamp;
	pkt->mb = mbuf_alloc(max_packet_size());
	if (!pkt->mb) {
		mem_deref(pkt);
		return NULL;
	}

	bool marker = false;
	size_t len = mbuf_get_space(pkt->mb);
	int err = aurec.ac->ench(aurec.enc, &marker, mbuf_buf(pkt->mb), &len,
				 aurec.srcprm.fmt, af->sampv, af->sampc);

	pkt->mb->pos = 0;
	pkt->mb->end = len;
	if (err) {
		warning("aurec: opus encode failed (%m)\n", err);
		mem_deref(pkt);
		return NULL;
	}

	return pkt;
}


static void ausrc_read_handler(struct auframe *af, void *arg)
{
	(void)arg;

	if (af->fmt != (enum aufmt) aurec.srcprm.fmt) {
		warning("aurec: ausrc format mismatch\n");
		return;
	}

	int err = aubuf_write_auframe(aurec.buf, af);
	if (err)
		return;

	if (aubuf_cur_size(aurec.buf) < aurec.psize)
		return;

	aubuf_read_auframe(aurec.buf, &aurec.af);
	struct aurec_packet *pkt = auframe_encode(&aurec.af);
	if (!pkt)
		return;

	re_atomic_rlx_set(&aurec.ts, af->timestamp);

	list_append(&aurec.prebuf, &pkt->le, pkt);
	if (list_count(&aurec.prebuf) >= aurec.precnt) {
		struct le *le = list_head(&aurec.prebuf);
		mem_deref(le->data);
	}

	static uint32_t logcnt = 0;
	if (log_level_get() == LEVEL_DEBUG && ++logcnt % 100 == 0) {
		uint32_t n = list_count(&aurec.prebuf);
		uint32_t ms = n * aurec.srcprm.ptime;
		debug("aurec: buffered %u ms (%u) ts=%"PRIu64"\n", ms, n,
		      aurec.af.timestamp);
	}
}


static int start_ausrc(void)
{
	struct config *cfg = conf_config();
	if (!cfg)
		return EINVAL;

	int err = 0;
	if (!aurec.src) {
		/* TODO: separate microphone config for aurec */
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
	list_flush(&aurec.prebuf);
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
	/* delete old packets */
	if (rec->le.next) {
		struct aurec_recording *next = rec->le.next->data;
		uint64_t ts = recording_ts_start(next);
		uint64_t ts0, ts1;
		bool found = false;
		struct le *le, *len;
		LIST_FOREACH_SAFE(&aurec.packets, le, len) {
			struct aurec_packet *pkt = le->data;
			if (pkt->ts < ts) {
				if (!found)
					ts0 = pkt->ts;
				else
					ts1 = pkt->ts;

				found = true;
				mem_deref(pkt);
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

	list_unlink(&rec->le);
	rec->uuid = mem_deref(rec->uuid);
}


static void append_packet(struct aurec_packet *pkt)
{
	list_append(&aurec.packets, &pkt->le, pkt);

	if (list_count(&aurec.packets) > MAX_PACKETS) {
		struct le *le = list_head(&aurec.packets);
		mem_deref(le->data);
	}
}


static void record_frame(void *arg)
{
	struct aurec_recording *rec = arg;

	tmr_start(&aurec.tmr, 1, record_frame, rec);

	struct le *le = list_head(&aurec.prebuf);
	if (!le)
		return;

	struct aurec_packet *pkt = le->data;
	list_unlink(le);
	append_packet(pkt);
}


static void stop_recording(void *arg)
{
	(void) arg;

	info("aurec: stop recording\n");
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


void aurec_get_srcprm(struct ausrc_prm *prm)
{
	if (!prm)
		return;

	*prm = aurec.srcprm;
}


const struct aucodec *aurec_codec(void)
{
	return aurec.ac;
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
			info("aurec: switch to live %s\n", call_id(call));
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
		(void)re_hprintf(pf, "aurec: failed to add recording (%m)\n",
				 err);
		return err;
	}

	(void)re_hprintf(pf, "aurec: uuid=%s\n", carg->prm);
	return 0;
}


static struct call *find_call_peer(struct ua *ua, const char *peer)
{
	struct le *le;
	LIST_FOREACH(ua_calls(ua), le) {
		struct call *call = le->data;
		if (!str_ncmp(call_peeruri(call), peer, str_len(peer)))
			return call;
	}

	return NULL;
}


static struct call *connect_to_operator(struct re_printf *pf,
					struct pl *dname, struct pl *sipuri,
					struct pl *cuser, struct ua *ua)
{
	struct call *call = NULL;
	char *uri = NULL;
	int err = 0;

	if (!ua && pl_isset(cuser)) {
		ua = uag_find(cuser);
		if (!ua) {
			re_hprintf(pf, "could not find UA for cuser=%r\n",
				   &cuser);
			return NULL;
		}
	}

	if (!ua)
		ua = uag_find_requri_pl(sipuri);

	if (!ua) {
		re_hprintf(pf, "no User-Agent found for request URI %r\n",
			   &sipuri);
		return NULL;
	}

	if (pl_isset(dname)) {
		err = re_sdprintf(&uri, "\"%r\" <%r>", &dname, &sipuri);
	}
	else {
		err = account_uri_complete_strdup(ua_account(ua),
						  &uri, sipuri);
	}

	if (err) {
		(void)re_hprintf(pf, "failed to complete dial uri\n");
		goto out;
	}

	re_hprintf(pf, "call uri: %s\n", uri);

	/* Does call already exist? */
	call = find_call_peer(ua, uri);
	if (call) {
		re_hprintf(pf, "aurec: call already exists (%s)\n",
			   call_id(call));
		goto out;
	}

	err  = ua_enable_autoanswer(ua, 0, ANSM_RFC5373);
	err |= ua_connect(ua, &call, NULL, uri, VIDMODE_OFF);
	if (err) {
		re_hprintf(pf, "aurec: call failed (%m)\n", err);
		goto out;
	}

	re_hprintf(pf, "aurec: call id: %s\n", call_id(call));
out:
	ua_disable_autoanswer(ua, ANSM_RFC5373);
	mem_deref(uri);
	return call;
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

	char *uuid_str;
	err = pl_strdup(&uuid_str, &uuid);
	if (err)
		goto out;

	if (!find_recording(uuid_str)) {
		re_hprintf(pf, "aurec: no recording found for uuid=%s\n",
			   uuid_str);
		err = EINVAL;
		goto out;
	}

	if (!aurec.src) {
		(void)re_hprintf(pf, "aurec: not enabled\n");
		return EINVAL;
	}

	struct pl cuser = PL_INIT;
	(void)cparam_decode(prm, "cuser", &cuser);
	struct call *call;
	call = connect_to_operator(pf, &dname, &sipuri, &cuser,	ua);

	/* switch to recording */
	audio_set_source(call_audio(call), "aurec", uuid_str);

 out:
	mem_deref(uuid_str);
	mem_deref(uri);

	return err;
}


static int cmd_live(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *prm = carg->prm;
	struct ua *ua = carg->data;
	int err = 0;

	const char *usage = "usage: /aurec_live <sipuri>"
			" [cuser=<string>]\n"
			"Stream the microphone to the specified peer\n";

	if (!str_isset(prm)) {
		(void)re_hprintf(pf, "%s", usage);
		return EINVAL;
	}

	struct pl dname = PL_INIT;
	struct pl sipuri = PL_INIT;
	/* with display name */
	if (re_regex(prm, str_len(prm),
		     "[~ \t\r\n<]*[ \t\r\n]*<[^>]+>[ \t\r\n]*",
		     &dname, NULL, &sipuri, NULL)) {
		/* without display name */
		dname = pl_null;
		pl_set_str(&sipuri, prm);
	}

	struct pl cuser = PL_INIT;
	(void)cparam_decode(prm, "cuser", &cuser);
	struct call *call;
	call = connect_to_operator(pf, &dname, &sipuri, &cuser,	ua);

	/* switch to microphone */
	audio_set_source(call_audio(call), NULL, NULL);
	call_update_media(call);

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
{"aurec_live"    ,0,    CMD_PRM, "Switch to the microphone", cmd_live},
};


static struct ausrc *ausrc;

static int module_init(void)
{
	info("aurec: module loaded\n");
	int err = aurec_init();
	if (err)
		return err;

	err  = bevent_register(event_handler, NULL);
	err |= cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	err |= ausrc_register(&ausrc, baresip_ausrcl(), "aurec",
			      aurec_src_alloc);

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
	bevent_unregister(event_handler);
	mem_deref(ausrc);
	aurec_close();
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(aurec) = {
	"aurec",
	"application",
	module_init,
	module_close
};
