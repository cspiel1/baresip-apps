/**
 * @file vidrot.c  Video rotation filter module
 *
 * Copyright (C) 2026 Christian Spielberger
 */
#include <string.h>
#include <time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


/**
 * @defgroup vidrot vidrot
 *
 * A video-filter module that rotates video frames
 *
 *
 * Example usage. Values are degrees to rotate clockwise:
 \verbatim
 vidrot 90
 vidrot 180
 vidrot 270
 vidrot normal
 \endverbatim
 *
 */


