/**
 * @file aurec.c  Audio recording
 *
 * - Uses ausrc, auconf, auresamp, opus codec to record encoded audio to
 *   a buffer.
 * - The recorded audio can be streamed on demand to a SIP call. This module
 *   implements an ausrc that reads the recorded data, decodes and passes this
 *   to the SIP call.
 *
 * Copyright (C) 2026 commend.com - Christian Spielberger
 */

#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <stdint.h>


struct aurec {

	struct tmr tmr;
	mtx_t *lock;
	struct call *call;

	uint8_t ch;
	enum aufmt fmt;
	uint32_t srate;
	size_t pre_size;
	size_t buf_size;
	struct aubuf *buf;
	struct ausrc_st *src;
};

static struct aurec aurec;

static void timeout(void *arg);
static int stop_ausrc(void);


static int aurec_init(void )
{
	int err = 0;
	uint32_t pre_ms = 5000;
	uint32_t post_ms = pre_ms;
	size_t sz = aufmt_sample_size(AUFMT_S16LE);

	aurec.ch = 1;
	aurec.fmt = AUFMT_S16LE;
	aurec.srate = 16000;
	(void)conf_get_u32(conf_cur(), "aurec_srate", &aurec.srate);
	(void)conf_get_u32(conf_cur(), "aurec_pre", &pre_ms);
	(void)conf_get_u32(conf_cur(), "aurec_post", &post_ms);
	aurec.pre_size = sz * au_calc_nsamp(aurec.srate, aurec.ch, pre_ms);
	aurec.buf_size = aurec.pre_size +
			 sz * au_calc_nsamp(aurec.srate, aurec.ch, post_ms);

	aubuf_alloc(&aurec.buf, aurec.pre_size, aurec.buf_size);

	err = mutex_alloc(&aurec.lock);
	if (err)
		return err;

	tmr_init(&aurec.tmr);
	tmr_start(&aurec.tmr, 100, timeout, NULL);

	return 0;
}


static void aurec_close(void)
{
	tmr_cancel(&aurec.tmr);
	mem_deref(aurec.lock);
	stop_ausrc();
	mem_deref(aurec.buf);
}


static void ausrc_read_handler(struct auframe *af, void *arg)
{
	(void)arg;

	if (!aurec.buf)
		return;

	if (af->fmt != aurec.fmt) {
		warning("aurec: ausrc format mismatch\n");
		return;
	}

	aubuf_write_auframe(aurec.buf, af);

	static uint32_t logcnt = 0;
	if (++logcnt % 100 == 0) {
		debug("aurec: buffered %zu ms\n",
		      aubuf_cur_size(aurec.buf) * 1000 /
		      (aufmt_sample_size(aurec.fmt) * aurec.ch * aurec.srate));
	}
}


static int start_ausrc(void)
{
	struct config *cfg = conf_config();
	if (!cfg)
		return EINVAL;

	if (!aurec.src) {
		const char *mod = cfg->audio.src_mod;
		const char *dev = cfg->audio.src_dev;
		struct ausrc_prm prm = {
			.srate = aurec.srate,
			.ch    = aurec.ch,
			.ptime = 20,
			.fmt   = aurec.fmt
		};

		int err = ausrc_alloc(&aurec.src, baresip_ausrcl(), mod, &prm,
				      dev, ausrc_read_handler, NULL, NULL);
		if (err) {
			warning("aurec: could not open ausrc %s (%s)\n",
				mod, dev);
			return ENOENT;
		}
	}

	return 0;
}


static int stop_ausrc(void)
{
	mem_deref(aurec.src);
	aubuf_flush(aurec.buf);
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
			 "/usr/local/share/baresip/ring.wav");

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


static void timeout(void *arg)
{
	(void) arg;

	tmr_start(&aurec.tmr, 100, timeout, NULL);
}


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
