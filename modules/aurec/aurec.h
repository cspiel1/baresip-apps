/**
 * @file aufile.h WAV Audio Source/Player internal interface
 *
 * Copyright (C) 2020 commend.com - Christian Spielberger
 */

#include <stdint.h>
struct aurec_packet {
	struct le le;

	uint64_t ts;       /**< Timestamp in RTP clock units */
	struct mbuf *mb;   /**< Opus encoded packet */
};

int aurec_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *dev,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg);

void aurec_get_srcprm(struct ausrc_prm *prm);

const struct aurec_packet *aurec_first_packet(const char *uuid,
					      uint32_t pos_ms);
uint64_t aurec_ts_max(const char *uuid);
struct aurec_packet *aurec_next_packet(const struct aurec_packet *pkt);
const struct aucodec *aurec_codec(void);
