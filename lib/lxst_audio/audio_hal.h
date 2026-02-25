/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AUDIO_HAL_H_
#define _AUDIO_HAL_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_HAL_CODEC_MODE_ENCODE = 1,
    AUDIO_HAL_CODEC_MODE_DECODE,
    AUDIO_HAL_CODEC_MODE_BOTH,
    AUDIO_HAL_CODEC_MODE_LINE_IN,
} audio_hal_codec_mode_t;

typedef enum {
    AUDIO_HAL_ADC_INPUT_LINE1 = 0x00,
    AUDIO_HAL_ADC_INPUT_LINE2,
    AUDIO_HAL_ADC_INPUT_ALL,
    AUDIO_HAL_ADC_INPUT_DIFFERENCE,
} audio_hal_adc_input_t;

typedef enum {
    AUDIO_HAL_DAC_OUTPUT_LINE1 = 0x00,
    AUDIO_HAL_DAC_OUTPUT_LINE2,
    AUDIO_HAL_DAC_OUTPUT_ALL,
} audio_hal_dac_output_t;

typedef enum {
    AUDIO_HAL_CTRL_STOP  = 0x00,
    AUDIO_HAL_CTRL_START = 0x01,
} audio_hal_ctrl_t;

typedef enum {
    AUDIO_HAL_MODE_SLAVE = 0x00,
    AUDIO_HAL_MODE_MASTER = 0x01,
} audio_hal_iface_mode_t;

typedef enum {
    AUDIO_HAL_08K_SAMPLES,
    AUDIO_HAL_11K_SAMPLES,
    AUDIO_HAL_16K_SAMPLES,
    AUDIO_HAL_22K_SAMPLES,
    AUDIO_HAL_24K_SAMPLES,
    AUDIO_HAL_32K_SAMPLES,
    AUDIO_HAL_44K_SAMPLES,
    AUDIO_HAL_48K_SAMPLES,
} audio_hal_iface_samples_t;

typedef enum {
    AUDIO_HAL_BIT_LENGTH_16BITS = 1,
    AUDIO_HAL_BIT_LENGTH_24BITS,
    AUDIO_HAL_BIT_LENGTH_32BITS,
} audio_hal_iface_bits_t;

typedef enum {
    AUDIO_HAL_I2S_NORMAL = 0,
    AUDIO_HAL_I2S_LEFT,
    AUDIO_HAL_I2S_RIGHT,
    AUDIO_HAL_I2S_DSP,
} audio_hal_iface_format_t;

typedef struct {
    audio_hal_iface_mode_t mode;
    audio_hal_iface_format_t fmt;
    audio_hal_iface_samples_t samples;
    audio_hal_iface_bits_t bits;
} audio_hal_codec_i2s_iface_t;

typedef struct {
    audio_hal_adc_input_t adc_input;
    audio_hal_dac_output_t dac_output;
    audio_hal_codec_mode_t codec_mode;
    audio_hal_codec_i2s_iface_t i2s_iface;
} audio_hal_codec_config_t;

#ifdef __cplusplus
}
#endif

#endif
