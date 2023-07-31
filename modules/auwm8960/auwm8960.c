/**
 * @file auwm8960 wm8960 I2S audio driver module
 *
 * Copyright (C) 2019 cspiel.at
 */
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <zephyr/device.h>
#include <wm8960.h>
#include "auwm8960.h"

static struct ausrc *ausrc = NULL;
static struct auplay *auplay = NULL;

static struct rtio_wm8960 wm8960;

/*---------------------------------------------------------------
                            CONFIG
---------------------------------------------------------------*/

static int auwm8960_init(void)
{
	int err;

	memset(&wm8960, 0, sizeof(wm8960));
	err  = ausrc_register(&ausrc, baresip_ausrcl(),
			      "auwm8960", auwm8960_src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(),
			       "auwm8960", auwm8960_play_alloc);


	return err;
}


static enum I2SOnMask _i2s_on = I2O_NONE;


int auwm8960_start(enum I2SOnMask playrec, const struct auplay_prm *prm)
{
	int err = 0;
	const struct device *i2s;
	const struct device *i2c;
	bool start = _i2s_on == I2O_NONE;

	if (!prm)
		return EINVAL;

	_i2s_on = _i2s_on | playrec;

	info("%s start with _i2s_on=%d\n", __func__, _i2s_on);
	if (start) {
		i2s = DEVICE_DT_GET(DT_NODELABEL(sai1));
		if (!i2s) {
			warning("auwm8960: no valid i2s device\n");
			return EIO;
		}

		i2c = DEVICE_DT_GET(DT_NODELABEL(lpi2c5));
		if (!i2c) {
			warning("auwm8960: no valid i2c device\n");
			return EIO;
		}

		err = wm8960_configure(&wm8960, i2c, i2s, prm->srate, prm->ch,
				       prm->ptime, prm->fmt);
	}

	return err;
}


int auwm8960_stop(enum I2SOnMask playrec)
{
	_i2s_on &= (~playrec);

	info("%s _i2s_on=%d", __func__, _i2s_on);
	if (_i2s_on == I2O_NONE) {
		info("%s: %d before i2s_driver_uninstall\n", __func__, __LINE__);
	}

	return 0;
}


const struct device *auwm8960_i2s(void)
{
	return wm8960.i2s;
}


static int auwm8960_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);


	return 0;
}


const struct mod_export DECL_EXPORTS(auwm8960) = {
	"auwm8960",
	"sound",
	auwm8960_init,
	auwm8960_close
};
