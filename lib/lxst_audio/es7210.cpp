// ES7210 driver — LilyGO library (verbatim)
// Original: Xinyuan-LilyGO/T-Deck lib/es7210/src/es7210.cpp
// SPDX-License-Identifier: Espressif-MIT

#ifdef ESP32

#include <Wire.h>
#include <string.h>
#include "esp_log.h"
#include "es7210.h"

#define I2S_DSP_MODE_A 0
#define MCLK_DIV_FRE   256

#define ES7210_MCLK_SOURCE            FROM_CLOCK_DOUBLE_PIN
#define FROM_PAD_PIN                  0
#define FROM_CLOCK_DOUBLE_PIN         1

static TwoWire *es7210wire;
static es7210_gain_value_t gain;

struct _coeff_div_es7210 {
    uint32_t mclk;
    uint32_t lrck;
    uint8_t  ss_ds;
    uint8_t  adc_div;
    uint8_t  dll;
    uint8_t  doubler;
    uint8_t  osr;
    uint8_t  mclk_src;
    uint32_t lrck_h;
    uint32_t lrck_l;
};

static const char *TAG = "ES7210";

static es7210_input_mics_t mic_select = (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4);

static const struct _coeff_div_es7210 coeff_div[] = {
    //mclk      lrck    ss_ds adc_div  dll  doubler osr  mclk_src  lrckh   lrckl
    {12288000,  8000 ,  0x00,  0x03,  0x01,  0x00,  0x20,  0x00,    0x06,  0x00},
    {16384000,  8000 ,  0x00,  0x04,  0x01,  0x00,  0x20,  0x00,    0x08,  0x00},
    {19200000,  8000 ,  0x00,  0x1e,  0x00,  0x01,  0x28,  0x00,    0x09,  0x60},
    {4096000,   8000 ,  0x00,  0x01,  0x01,  0x00,  0x20,  0x00,    0x02,  0x00},
    {11289600,  11025,  0x00,  0x02,  0x01,  0x00,  0x20,  0x00,    0x01,  0x00},
    {12288000,  12000,  0x00,  0x02,  0x01,  0x00,  0x20,  0x00,    0x04,  0x00},
    {19200000,  12000,  0x00,  0x14,  0x00,  0x01,  0x28,  0x00,    0x06,  0x40},
    {4096000,   16000,  0x00,  0x01,  0x01,  0x01,  0x20,  0x00,    0x01,  0x00},
    {19200000,  16000,  0x00,  0x0a,  0x00,  0x00,  0x1e,  0x00,    0x04,  0x80},
    {16384000,  16000,  0x00,  0x02,  0x01,  0x00,  0x20,  0x00,    0x04,  0x00},
    {12288000,  16000,  0x00,  0x03,  0x01,  0x01,  0x20,  0x00,    0x03,  0x00},
    {11289600,  22050,  0x00,  0x01,  0x01,  0x00,  0x20,  0x00,    0x02,  0x00},
    {12288000,  24000,  0x00,  0x01,  0x01,  0x00,  0x20,  0x00,    0x02,  0x00},
    {19200000,  24000,  0x00,  0x0a,  0x00,  0x01,  0x28,  0x00,    0x03,  0x20},
    {12288000,  32000,  0x00,  0x03,  0x00,  0x00,  0x20,  0x00,    0x01,  0x80},
    {16384000,  32000,  0x00,  0x01,  0x01,  0x00,  0x20,  0x00,    0x02,  0x00},
    {19200000,  32000,  0x00,  0x05,  0x00,  0x00,  0x1e,  0x00,    0x02,  0x58},
    {11289600,  44100,  0x00,  0x01,  0x01,  0x01,  0x20,  0x00,    0x01,  0x00},
    {12288000,  48000,  0x00,  0x01,  0x01,  0x01,  0x20,  0x00,    0x01,  0x00},
    {19200000,  48000,  0x00,  0x05,  0x00,  0x01,  0x28,  0x00,    0x01,  0x90},
    {16384000,  64000,  0x01,  0x01,  0x01,  0x00,  0x20,  0x00,    0x01,  0x00},
    {19200000,  64000,  0x00,  0x05,  0x00,  0x01,  0x1e,  0x00,    0x01,  0x2c},
    {11289600,  88200,  0x01,  0x01,  0x01,  0x01,  0x20,  0x00,    0x00,  0x80},
    {12288000,  96000,  0x01,  0x01,  0x01,  0x01,  0x20,  0x00,    0x00,  0x80},
    {19200000,  96000,  0x01,  0x05,  0x00,  0x01,  0x28,  0x00,    0x00,  0xc8},
};

static esp_err_t es7210_write_reg(uint8_t reg_addr, uint8_t data)
{
    es7210wire->beginTransmission(ES7210_ADDR);
    es7210wire->write(reg_addr);
    es7210wire->write(data);
    return es7210wire->endTransmission();
}

static esp_err_t es7210_update_reg_bit(uint8_t reg_addr, uint8_t update_bits, uint8_t data)
{
    uint8_t regv;
    regv = es7210_read_reg(reg_addr);
    regv = (regv & (~update_bits)) | (update_bits & data);
    return es7210_write_reg(reg_addr, regv);
}

static int get_coeff(uint32_t mclk, uint32_t lrck)
{
    for (int i = 0; i < (sizeof(coeff_div) / sizeof(coeff_div[0])); i++) {
        if (coeff_div[i].lrck == lrck && coeff_div[i].mclk == mclk)
            return i;
    }
    return -1;
}

int8_t get_es7210_mclk_src(void)
{
    return ES7210_MCLK_SOURCE;
}

int es7210_read_reg(uint8_t reg_addr)
{
    uint8_t data;
    es7210wire->beginTransmission(ES7210_ADDR);
    es7210wire->write(reg_addr);
    es7210wire->endTransmission(false);
    es7210wire->requestFrom(ES7210_ADDR, (size_t)1);
    data = es7210wire->read();
    return (int)data;
}

esp_err_t es7210_config_sample(audio_hal_iface_samples_t sample)
{
    uint8_t regv;
    int coeff;
    int sample_fre = 0;
    int mclk_fre = 0;
    esp_err_t ret = ESP_OK;
    switch (sample) {
        case AUDIO_HAL_08K_SAMPLES: sample_fre = 8000; break;
        case AUDIO_HAL_11K_SAMPLES: sample_fre = 11025; break;
        case AUDIO_HAL_16K_SAMPLES: sample_fre = 16000; break;
        case AUDIO_HAL_22K_SAMPLES: sample_fre = 22050; break;
        case AUDIO_HAL_24K_SAMPLES: sample_fre = 24000; break;
        case AUDIO_HAL_32K_SAMPLES: sample_fre = 32000; break;
        case AUDIO_HAL_44K_SAMPLES: sample_fre = 44100; break;
        case AUDIO_HAL_48K_SAMPLES: sample_fre = 48000; break;
        default:
            ESP_LOGE(TAG, "Unable to configure sample rate %dHz", sample_fre);
            break;
    }
    mclk_fre = sample_fre * MCLK_DIV_FRE;
    coeff = get_coeff(mclk_fre, sample_fre);
    if (coeff < 0) {
        ESP_LOGE(TAG, "Unable to configure sample rate %dHz with %dHz MCLK", sample_fre, mclk_fre);
        return ESP_FAIL;
    }
    if (coeff >= 0) {
        regv = es7210_read_reg(ES7210_MAINCLK_REG02) & 0x00;
        regv |= coeff_div[coeff].adc_div;
        regv |= coeff_div[coeff].doubler << 6;
        regv |= coeff_div[coeff].dll << 7;
        ret |= es7210_write_reg(ES7210_MAINCLK_REG02, regv);
        regv = coeff_div[coeff].osr;
        ret |= es7210_write_reg(ES7210_OSR_REG07, regv);
        regv = coeff_div[coeff].lrck_h;
        ret |= es7210_write_reg(ES7210_LRCK_DIVH_REG04, regv);
        regv = coeff_div[coeff].lrck_l;
        ret |= es7210_write_reg(ES7210_LRCK_DIVL_REG05, regv);
    }
    return ret;
}

esp_err_t es7210_mic_select(es7210_input_mics_t mic)
{
    esp_err_t ret = ESP_OK;
    mic_select = mic;
    if (mic_select & (ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4)) {
        for (int i = 0; i < 4; i++) {
            ret |= es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43 + i, 0x10, 0x00);
        }
        ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0xff);
        ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0xff);
        if (mic_select & ES7210_INPUT_MIC1) {
            ESP_LOGI(TAG, "Enable ES7210_INPUT_MIC1");
            ret |= es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01, 0x0b, 0x00);
            ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x00);
            ret |= es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43, 0x10, 0x10);
        }
        if (mic_select & ES7210_INPUT_MIC2) {
            ESP_LOGI(TAG, "Enable ES7210_INPUT_MIC2");
            ret |= es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01, 0x0b, 0x00);
            ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x00);
            ret |= es7210_update_reg_bit(ES7210_MIC2_GAIN_REG44, 0x10, 0x10);
        }
        if (mic_select & ES7210_INPUT_MIC3) {
            ESP_LOGI(TAG, "Enable ES7210_INPUT_MIC3");
            ret |= es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01, 0x15, 0x00);
            ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x00);
            ret |= es7210_update_reg_bit(ES7210_MIC3_GAIN_REG45, 0x10, 0x10);
        }
        if (mic_select & ES7210_INPUT_MIC4) {
            ESP_LOGI(TAG, "Enable ES7210_INPUT_MIC4");
            ret |= es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01, 0x15, 0x00);
            ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x00);
            ret |= es7210_update_reg_bit(ES7210_MIC4_GAIN_REG46, 0x10, 0x10);
        }
    } else {
        ESP_LOGE(TAG, "Microphone selection error");
        return ESP_FAIL;
    }
    return ret;
}

esp_err_t es7210_adc_init(TwoWire *tw, audio_hal_codec_config_t *codec_cfg)
{
    esp_err_t ret = ESP_OK;
    es7210wire = tw;

    ret |= es7210_write_reg(ES7210_RESET_REG00, 0xff);
    ret |= es7210_write_reg(ES7210_RESET_REG00, 0x41);
    ret |= es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x1f);
    ret |= es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30);
    ret |= es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30);

    audio_hal_codec_i2s_iface_t *i2s_cfg = &(codec_cfg->i2s_iface);
    switch (i2s_cfg->mode) {
        case AUDIO_HAL_MODE_MASTER:
            ESP_LOGI(TAG, "ES7210 in Master mode");
            ret |= es7210_write_reg(ES7210_MODE_CONFIG_REG08, 0x20);
            switch (get_es7210_mclk_src()) {
                case FROM_PAD_PIN:
                    ret |= es7210_update_reg_bit(ES7210_MASTER_CLK_REG03, 0x80, 0x00);
                    break;
                case FROM_CLOCK_DOUBLE_PIN:
                    ret |= es7210_update_reg_bit(ES7210_MASTER_CLK_REG03, 0x80, 0x80);
                    break;
                default:
                    ret |= es7210_update_reg_bit(ES7210_MASTER_CLK_REG03, 0x80, 0x00);
                    break;
            }
            break;
        case AUDIO_HAL_MODE_SLAVE:
            ESP_LOGI(TAG, "ES7210 in Slave mode");
            break;
        default:
            break;
    }
    ret |= es7210_write_reg(ES7210_ANALOG_REG40, 0xC3);
    ret |= es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70);
    ret |= es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70);
    ret |= es7210_write_reg(ES7210_OSR_REG07, 0x20);
    ret |= es7210_write_reg(ES7210_MAINCLK_REG02, 0xc1);
    ret |= es7210_config_sample(i2s_cfg->samples);
    ret |= es7210_mic_select(mic_select);
    ret |= es7210_adc_set_gain_all(GAIN_0DB);
    return ESP_OK;
}

esp_err_t es7210_adc_deinit()
{
    return ESP_OK;
}

esp_err_t es7210_config_fmt(audio_hal_iface_format_t fmt)
{
    esp_err_t ret = ESP_OK;
    uint8_t adc_iface = 0;
    adc_iface = es7210_read_reg(ES7210_SDP_INTERFACE1_REG11);
    adc_iface &= 0xfc;
    switch (fmt) {
        case AUDIO_HAL_I2S_NORMAL:
            ESP_LOGD(TAG, "ES7210 in I2S Format");
            adc_iface |= 0x00;
            break;
        case AUDIO_HAL_I2S_LEFT:
        case AUDIO_HAL_I2S_RIGHT:
            ESP_LOGD(TAG, "ES7210 in LJ Format");
            adc_iface |= 0x01;
            break;
        case AUDIO_HAL_I2S_DSP:
            if (I2S_DSP_MODE_A) {
                ESP_LOGD(TAG, "ES7210 in DSP-A Format");
                adc_iface |= 0x03;
            } else {
                ESP_LOGD(TAG, "ES7210 in DSP-B Format");
                adc_iface |= 0x13;
            }
            break;
        default:
            adc_iface &= 0xfc;
            break;
    }
    ret |= es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, adc_iface);
    ret |= es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);
    return ret;
}

esp_err_t es7210_set_bits(audio_hal_iface_bits_t bits)
{
    esp_err_t ret = ESP_OK;
    uint8_t adc_iface = 0;
    adc_iface = es7210_read_reg(ES7210_SDP_INTERFACE1_REG11);
    adc_iface &= 0x1f;
    switch (bits) {
        case AUDIO_HAL_BIT_LENGTH_16BITS:
            adc_iface |= 0x60;
            break;
        case AUDIO_HAL_BIT_LENGTH_24BITS:
            adc_iface |= 0x00;
            break;
        case AUDIO_HAL_BIT_LENGTH_32BITS:
            adc_iface |= 0x80;
            break;
        default:
            adc_iface |= 0x60;
            break;
    }
    ret |= es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, adc_iface);
    return ret;
}

esp_err_t es7210_adc_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    esp_err_t ret = ESP_OK;
    ret |= es7210_set_bits(iface->bits);
    ret |= es7210_config_fmt(iface->fmt);
    ret |= es7210_config_sample(iface->samples);
    return ret;
}

esp_err_t es7210_start(uint8_t clock_reg_value)
{
    esp_err_t ret = ESP_OK;
    ret |= es7210_write_reg(ES7210_CLOCK_OFF_REG01, clock_reg_value);
    ret |= es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x00);
    ret |= es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x00);
    ret |= es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x00);
    ret |= es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x00);
    ret |= es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x00);
    ret |= es7210_mic_select(mic_select);
    return ret;
}

esp_err_t es7210_stop(void)
{
    esp_err_t ret = ESP_OK;
    ret |= es7210_write_reg(ES7210_MIC1_POWER_REG47, 0xff);
    ret |= es7210_write_reg(ES7210_MIC2_POWER_REG48, 0xff);
    ret |= es7210_write_reg(ES7210_MIC3_POWER_REG49, 0xff);
    ret |= es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0xff);
    ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0xff);
    ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0xff);
    ret |= es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x7f);
    ret |= es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x07);
    return ret;
}

esp_err_t es7210_adc_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    static uint8_t regv;
    esp_err_t ret = ESP_OK;
    ret = es7210_read_reg(ES7210_CLOCK_OFF_REG01);
    if ((ret != 0x7f) && (ret != 0xff)) {
        regv = es7210_read_reg(ES7210_CLOCK_OFF_REG01);
    }
    if (ctrl_state == AUDIO_HAL_CTRL_START) {
        ESP_LOGI(TAG, "The ES7210_CLOCK_OFF_REG01 value before stop is %x", regv);
        ret |= es7210_start(regv);
    } else {
        ESP_LOGW(TAG, "The codec is about to stop");
        regv = es7210_read_reg(ES7210_CLOCK_OFF_REG01);
        ret |= es7210_stop();
    }
    return ESP_OK;
}

esp_err_t es7210_adc_set_gain(es7210_input_mics_t mic_mask, es7210_gain_value_t gain)
{
    esp_err_t ret_val = ESP_OK;
    if (gain < GAIN_0DB) gain = GAIN_0DB;
    if (gain > GAIN_37_5DB) gain = GAIN_37_5DB;
    if (mic_mask & ES7210_INPUT_MIC1)
        ret_val |= es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43, 0x0f, gain);
    if (mic_mask & ES7210_INPUT_MIC2)
        ret_val |= es7210_update_reg_bit(ES7210_MIC2_GAIN_REG44, 0x0f, gain);
    if (mic_mask & ES7210_INPUT_MIC3)
        ret_val |= es7210_update_reg_bit(ES7210_MIC3_GAIN_REG45, 0x0f, gain);
    if (mic_mask & ES7210_INPUT_MIC4)
        ret_val |= es7210_update_reg_bit(ES7210_MIC4_GAIN_REG46, 0x0f, gain);
    return ret_val;
}

esp_err_t es7210_adc_set_gain_all(es7210_gain_value_t gain)
{
    esp_err_t ret = ESP_OK;
    uint32_t max_gain_vaule = 14;
    if (gain < 0) gain = (es7210_gain_value_t)0;
    else if (gain > max_gain_vaule) gain = (es7210_gain_value_t)max_gain_vaule;
    ESP_LOGD(TAG, "SET: gain:%d", gain);
    if (mic_select & ES7210_INPUT_MIC1)
        ret |= es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43, 0x0f, gain);
    if (mic_select & ES7210_INPUT_MIC2)
        ret |= es7210_update_reg_bit(ES7210_MIC2_GAIN_REG44, 0x0f, gain);
    if (mic_select & ES7210_INPUT_MIC3)
        ret |= es7210_update_reg_bit(ES7210_MIC3_GAIN_REG45, 0x0f, gain);
    if (mic_select & ES7210_INPUT_MIC4)
        ret |= es7210_update_reg_bit(ES7210_MIC4_GAIN_REG46, 0x0f, gain);
    return ret;
}

void es7210_read_all(void)
{
    for (int i = 0; i <= 0x4E; i++) {
        uint8_t reg = es7210_read_reg(i);
        ets_printf("REG:%02x, %02x\n", reg, i);
    }
}

#endif
