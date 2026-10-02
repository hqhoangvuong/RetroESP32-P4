/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "sdkconfig.h"

#define EXAMPLE_AUDIO_BUFFER_SIZE (2400)
#define EXAMPLE_SAMPLE_RATE       (16000)
#define EXAMPLE_MCLK_MULTIPLE     (256)
#define EXAMPLE_VOICE_VOLUME      CONFIG_EXAMPLE_VOICE_VOLUME

#if CONFIG_EXAMPLE_MODE_ECHO
#define EXAMPLE_MIC_GAIN_DB       ((float)CONFIG_EXAMPLE_MIC_GAIN)
#endif
