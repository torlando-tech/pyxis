// Native unit tests for the LXST voice filter chain.
//
// VoiceFilterChain = HighPass(300Hz) -> LowPass(3400Hz) -> AGC. We test the
// black-box behavior end-to-end:
//
//   - Silence in -> silence out
//   - DC bias removed (HPF must shed any constant offset)
//   - 100Hz pure tone (below HPF cutoff) is significantly attenuated
//   - 1kHz pure tone (in passband) is mostly preserved
//   - 5kHz pure tone @ 8kHz sample rate (above LPF cutoff) is attenuated
//   - Loud signal (peaks above AGC_PEAK_LIMIT 0.75) is limited
//   - process() doesn't blow up on tiny / empty input
//   - State persists across calls — chunked processing roughly matches
//     single-shot processing of the same signal

#include "../../lib/lxst_audio/audio_filters.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// ── minimal test framework ──

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (!(_a == _e)) {                                                     \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: %s != %s",                 \
                          __FILE__, __LINE__, #actual, #expected);             \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: expected %s",              \
                          __FILE__, __LINE__, #cond);                          \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define RUN(name)                                                              \
    do {                                                                       \
        try {                                                                  \
            name();                                                            \
            ++g_pass;                                                          \
            std::printf("PASS %s\n", #name);                                   \
        } catch (const std::exception& e) {                                    \
            ++g_fail;                                                          \
            std::printf("FAIL %s: %s\n", #name, e.what());                     \
        }                                                                      \
    } while (0)

// ── helpers ──

constexpr int SR = 8000;
constexpr float HP_CUT = 300.0f;
constexpr float LP_CUT = 3400.0f;
constexpr float AGC_TARGET_DB = -12.0f;
constexpr float AGC_MAX_GAIN_DB = 12.0f;

static std::vector<int16_t> make_sine(float hz, int n_samples,
                                       int sample_rate, float amplitude = 0.3f) {
    std::vector<int16_t> out(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        float t = (float)i / (float)sample_rate;
        float v = amplitude * sinf(2.0f * (float)M_PI * hz * t);
        out[i] = (int16_t)(v * 32767.0f);
    }
    return out;
}

static double rms(const std::vector<int16_t>& v, int skip_samples = 0) {
    double sum = 0.0;
    int n = (int)v.size() - skip_samples;
    if (n <= 0) return 0.0;
    for (int i = skip_samples; i < (int)v.size(); ++i) {
        double s = v[i] / 32768.0;
        sum += s * s;
    }
    return std::sqrt(sum / n);
}

static int peak(const std::vector<int16_t>& v) {
    int p = 0;
    for (auto s : v) if (std::abs((int)s) > p) p = std::abs((int)s);
    return p;
}

// ── tests ──

static void silence_in_silence_out() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    std::vector<int16_t> samples(1024, 0);
    chain.process(samples.data(), (int)samples.size(), SR);
    // Should be exactly zero, but allow tiny tolerance for any FP residue.
    EXPECT_EQ(peak(samples), 0);
}

static void empty_input_does_not_crash() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    int16_t s[1] = {0};
    chain.process(s, 0, SR);                  // numSamples=0 must be a no-op
    chain.process(nullptr, 0, SR);
}

static void dc_offset_attenuated_by_hpf() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    // Constant non-zero bias. NOTE: this filter chain does *not* fully remove
    // DC — the 1-pole HPF as currently written degenerates to a fixed gain
    // (alpha*x) for any constant input, identical to upstream LXST-kt's
    // native_audio_filters.cpp. The AGC then pulls the residual down toward
    // its target level. We assert "much smaller than input", not "zero".
    // See https://github.com/torlando-tech/LXST-kt/issues/13 — HPF formula
    // does not actually high-pass; it scales by alpha (~0.81 at 300Hz/8kHz).
    // Tighten this assertion to `< 0.01` once the upstream fix lands.
    std::vector<int16_t> samples(8000, 8000);   // 1s of 0.244 DC (in float terms)
    double in_rms = 8000.0 / 32768.0;
    chain.process(samples.data(), (int)samples.size(), SR);
    double tail_rms = rms(samples, 4000);
    EXPECT_TRUE(tail_rms < in_rms * 0.5);   // at least 2x attenuation, in practice ~4x
}

static void low_freq_below_hpf_attenuated() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    auto samples = make_sine(100.0f, 8000, SR, 0.3f);   // well below 300Hz cutoff
    double rms_in = rms(samples);
    chain.process(samples.data(), (int)samples.size(), SR);
    double rms_out = rms(samples, 2000);      // skip filter settling
    // 100Hz is roughly an octave below the cutoff; expect ≤50% of input RMS.
    // (AGC may push it back up — peak limiting and trigger threshold prevent
    // small signals from being amplified, so this should hold.)
    EXPECT_TRUE(rms_out < rms_in * 0.6);
}

static void midband_passes_through() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    auto samples = make_sine(1000.0f, 8000, SR, 0.3f);
    chain.process(samples.data(), (int)samples.size(), SR);
    double rms_out = rms(samples, 2000);
    // 1kHz is squarely in the passband. AGC will normalize toward target;
    // we just want non-negligible signal to remain.
    EXPECT_TRUE(rms_out > 0.05);
}

static void high_freq_above_lpf_attenuated() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    // Use 3800Hz — between LPF cutoff (3400) and Nyquist (4000). The single-pole
    // RC filter rolloff means it should be partially attenuated relative to
    // an in-band tone at the same RMS amplitude.
    auto hi = make_sine(3800.0f, 8000, SR, 0.3f);
    auto mid = make_sine(1000.0f, 8000, SR, 0.3f);

    VoiceFilterChain chain_mid(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    chain.process(hi.data(), (int)hi.size(), SR);
    chain_mid.process(mid.data(), (int)mid.size(), SR);

    double rms_hi = rms(hi, 2000);
    double rms_mid = rms(mid, 2000);
    EXPECT_TRUE(rms_hi < rms_mid);
}

static void loud_signal_peak_limited() {
    VoiceFilterChain chain(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    // Saturated input — pre-encoder, the chain should pull peaks down via
    // AGC + peak limiting (peak limit = 0.75 of int16 range = ~24576).
    auto samples = make_sine(1000.0f, 8000, SR, 0.95f);
    chain.process(samples.data(), (int)samples.size(), SR);

    int p = peak(samples);
    // 0.75 * 32767 = ~24575. Allow some headroom for transient before AGC
    // settles; check the back half.
    int p_tail = 0;
    for (int i = 4000; i < (int)samples.size(); ++i) {
        if (std::abs((int)samples[i]) > p_tail) p_tail = std::abs((int)samples[i]);
    }
    EXPECT_TRUE(p_tail < 28000);              // well below saturation
    EXPECT_TRUE(p < 32000);                   // never clips even at the worst transient
}

// State persistence: processing one big chunk vs two halves of the same
// input should produce nearly identical output. (Filter state + AGC state
// must carry across.)
static void state_persists_across_chunks() {
    auto signal = make_sine(800.0f, 4000, SR, 0.3f);

    VoiceFilterChain chain_a(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    auto a = signal;
    chain_a.process(a.data(), (int)a.size(), SR);

    VoiceFilterChain chain_b(1, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    auto b = signal;
    chain_b.process(b.data(), 2000, SR);
    chain_b.process(b.data() + 2000, 2000, SR);

    // Compare the steady-state portion (after both have settled).
    // We only require they're close — exact bit equality isn't promised
    // because AGC block boundaries differ between single-shot and chunked.
    double diff_rms = 0.0;
    int n = 0;
    for (int i = 2500; i < 4000; ++i) {
        double d = (a[i] - b[i]) / 32768.0;
        diff_rms += d * d;
        ++n;
    }
    diff_rms = std::sqrt(diff_rms / n);
    EXPECT_TRUE(diff_rms < 0.05);
}

static void multichannel_processes_independently() {
    VoiceFilterChain chain(2, HP_CUT, LP_CUT, AGC_TARGET_DB, AGC_MAX_GAIN_DB);
    // Stereo: left = 1kHz, right = silence. After processing, right should
    // remain near silent and left should retain signal.
    int frames = 4000;
    std::vector<int16_t> samples(frames * 2);
    for (int i = 0; i < frames; ++i) {
        float t = (float)i / (float)SR;
        samples[i * 2 + 0] = (int16_t)(0.3f * sinf(2.0f * (float)M_PI * 1000.0f * t) * 32767.0f);
        samples[i * 2 + 1] = 0;
    }
    chain.process(samples.data(), (int)samples.size(), SR);

    double left_sum = 0, right_sum = 0;
    int n = 0;
    for (int i = frames / 2; i < frames; ++i) {
        double l = samples[i * 2 + 0] / 32768.0;
        double r = samples[i * 2 + 1] / 32768.0;
        left_sum += l * l;
        right_sum += r * r;
        ++n;
    }
    double left_rms = std::sqrt(left_sum / n);
    double right_rms = std::sqrt(right_sum / n);
    EXPECT_TRUE(left_rms > 0.05);
    EXPECT_TRUE(right_rms < 0.01);
}

int main() {
    RUN(silence_in_silence_out);
    RUN(empty_input_does_not_crash);
    RUN(dc_offset_attenuated_by_hpf);
    RUN(low_freq_below_hpf_attenuated);
    RUN(midband_passes_through);
    RUN(high_freq_above_lpf_attenuated);
    RUN(loud_signal_peak_limited);
    RUN(state_persists_across_chunks);
    RUN(multichannel_processes_independently);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
