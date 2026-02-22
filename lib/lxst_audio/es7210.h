// ES7210 driver — LilyGO library + thin wrapper for LXST
// Original: Xinyuan-LilyGO/T-Deck lib/es7210/src/es7210.h (Espressif MIT License)

#ifndef _ES7210_H
#define _ES7210_H

#include "audio_hal.h"
#include <Wire.h>

typedef enum {
    ES7210_AD1_AD0_00 = 0x40,
    ES7210_AD1_AD0_01 = 0x41,
    ES7210_AD1_AD0_10 = 0x42,
    ES7210_AD1_AD0_11 = 0x43,
} es7210_address_t;

#ifdef ES7210_ADDR
#undef ES7210_ADDR
#endif
#define ES7210_ADDR  ES7210_AD1_AD0_00

#ifdef __cplusplus
extern "C" {
#endif

#define  ES7210_RESET_REG00                 0x00
#define  ES7210_CLOCK_OFF_REG01             0x01
#define  ES7210_MAINCLK_REG02               0x02
#define  ES7210_MASTER_CLK_REG03            0x03
#define  ES7210_LRCK_DIVH_REG04             0x04
#define  ES7210_LRCK_DIVL_REG05             0x05
#define  ES7210_POWER_DOWN_REG06            0x06
#define  ES7210_OSR_REG07                   0x07
#define  ES7210_MODE_CONFIG_REG08           0x08
#define  ES7210_TIME_CONTROL0_REG09         0x09
#define  ES7210_TIME_CONTROL1_REG0A         0x0A
#define  ES7210_SDP_INTERFACE1_REG11        0x11
#define  ES7210_SDP_INTERFACE2_REG12        0x12
#define  ES7210_ADC_AUTOMUTE_REG13          0x13
#define  ES7210_ADC34_HPF2_REG20            0x20
#define  ES7210_ADC34_HPF1_REG21            0x21
#define  ES7210_ADC12_HPF1_REG22            0x22
#define  ES7210_ADC12_HPF2_REG23            0x23
#define  ES7210_ANALOG_REG40                0x40
#define  ES7210_MIC12_BIAS_REG41            0x41
#define  ES7210_MIC34_BIAS_REG42            0x42
#define  ES7210_MIC1_GAIN_REG43             0x43
#define  ES7210_MIC2_GAIN_REG44             0x44
#define  ES7210_MIC3_GAIN_REG45             0x45
#define  ES7210_MIC4_GAIN_REG46             0x46
#define  ES7210_MIC1_POWER_REG47            0x47
#define  ES7210_MIC2_POWER_REG48            0x48
#define  ES7210_MIC3_POWER_REG49            0x49
#define  ES7210_MIC4_POWER_REG4A            0x4A
#define  ES7210_MIC12_POWER_REG4B           0x4B
#define  ES7210_MIC34_POWER_REG4C           0x4C

typedef enum {
    ES7210_INPUT_MIC1 = 0x01,
    ES7210_INPUT_MIC2 = 0x02,
    ES7210_INPUT_MIC3 = 0x04,
    ES7210_INPUT_MIC4 = 0x08
} es7210_input_mics_t;

typedef enum gain_value {
    GAIN_0DB = 0,
    GAIN_3DB,
    GAIN_6DB,
    GAIN_9DB,
    GAIN_12DB,
    GAIN_15DB,
    GAIN_18DB,
    GAIN_21DB,
    GAIN_24DB,
    GAIN_27DB,
    GAIN_30DB,
    GAIN_33DB,
    GAIN_34_5DB,
    GAIN_36DB,
    GAIN_37_5DB,
} es7210_gain_value_t;

esp_err_t es7210_adc_init(TwoWire *tw, audio_hal_codec_config_t *codec_cfg);
esp_err_t es7210_adc_deinit(void);
esp_err_t es7210_adc_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface);
esp_err_t es7210_adc_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state);
esp_err_t es7210_adc_set_gain(es7210_input_mics_t mic_mask, es7210_gain_value_t gain);
esp_err_t es7210_adc_set_gain_all(es7210_gain_value_t gain);
esp_err_t es7210_mic_select(es7210_input_mics_t mic);
int es7210_read_reg(uint8_t reg_addr);
void es7210_read_all(void);

#ifdef __cplusplus
}
#endif

#endif /* _ES7210_H_ */
