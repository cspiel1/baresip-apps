/**
 * @file auwm8960.h wm8960 I2S audio driver module - internal interface
 *
 * Copyright (C) 2019 cspiel.at
 */

#define I2S_PORT           (0)
#define DMA_SIZE           (640)

enum I2SOnMask {
	I2O_NONE = 0,
	I2O_PLAY = 1,
	I2O_RECO = 2,
	I2O_BOTH = 3
};


int auwm8960_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		       struct ausrc_prm *prm, const char *device,
		       ausrc_read_h *rh, ausrc_error_h *errh, void *arg);
int auwm8960_play_alloc(struct auplay_st **stp, const struct auplay *ap,
			struct auplay_prm *prm, const char *device,
			auplay_write_h *wh, void *arg);


int auwm8960_start(enum I2SOnMask playrec, const struct auplay_prm *prm);


int auwm8960_stop(enum I2SOnMask playrec);
const struct device *auwm8960_i2s(void);

