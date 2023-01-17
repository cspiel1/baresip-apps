/**
 * @file aulatency.h  Audio round-trip measurement -- internal interface
 *
 * Copyright (C) 2023 c.spielberger@commend.com
 */


struct aulat;
struct ausrc_st;
struct auplay_st;


int aulat_play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg);
int aulat_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		       struct ausrc_prm *prm, const char *device,
		       ausrc_read_h *rh, ausrc_error_h *errh, void *arg);


struct aulat *aulat_get(void);
void aulat_send_frame(struct aulat *aulat, const struct auframe *f,
		       uint64_t ts, uint64_t t);
void aulat_recv_frame(struct aulat *aulat, struct auframe *f,
		       uint64_t ts, uint64_t t);
bool aulat_auframe_fill(struct aulat *st, struct auframe *f, bool clear);
