#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace RadioActivity {

enum class Event : uint8_t {
    Noise = 0,
    Rx = 1U << 0,
    Tx = 1U << 1,
    Interference = 1U << 2,
};

constexpr uint8_t event_bit(Event event) {
    return static_cast<uint8_t>(event);
}

struct Sample {
    int16_t rssi_dbm = -135;
    uint8_t events = 0;
    uint32_t sequence = 0;
};

constexpr bool has_event(const Sample& sample, Event event) {
    return (sample.events & event_bit(event)) != 0;
}

struct Snapshot {
    static constexpr std::size_t CAPACITY = 96;

    std::array<Sample, CAPACITY> samples{};
    std::size_t count = 0;
    int16_t current_rssi = -135;
    int16_t noise_floor = -135;
    bool noise_floor_ready = false;
    uint8_t channel_load_percent = 0;
};

/**
 * Hardware-independent, fixed-capacity current-channel RSSI history.
 *
 * Samples within 11 dB of the learned floor are admitted to a bounded rolling
 * baseline. Decoded RX, local TX, and stronger undecodable activity are kept as
 * metadata but excluded from that baseline. This model is observational only;
 * it does not influence radio admission or modulation.
 */
class History {
public:
    static constexpr std::size_t CAPACITY = Snapshot::CAPACITY;
    static constexpr std::size_t NOISE_WINDOW_CAPACITY = 24;
    static constexpr std::size_t MIN_BASELINE_SAMPLES = 4;
    static constexpr int16_t ACTIVITY_THRESHOLD_DB = 11;
    static constexpr int16_t MIN_RSSI_DBM = -135;
    static constexpr int16_t MAX_RSSI_DBM = -20;

    void mark_event(Event event) {
        if (event == Event::Noise) return;
        _pending_events |= event_bit(event);
    }

    void record(int16_t rssi_dbm) {
        const int16_t rssi = clamp_rssi(rssi_dbm);
        uint8_t events = _pending_events;
        _pending_events = 0;
        if (events == 0 && _noise_count >= MIN_BASELINE_SAMPLES &&
            rssi > static_cast<int16_t>(noise_floor() + ACTIVITY_THRESHOLD_DB)) {
            events |= event_bit(Event::Interference);
        }

        if (events == 0) {
            accept_noise(rssi);
        }

        Sample sample;
        sample.rssi_dbm = rssi;
        sample.events = events;
        sample.sequence = _next_sequence++;
        _samples[_write_index] = sample;
        _write_index = (_write_index + 1) % CAPACITY;
        if (_count < CAPACITY) {
            ++_count;
        }
    }

    Snapshot snapshot() const {
        Snapshot result;
        result.count = _count;
        const std::size_t oldest = (_count == CAPACITY) ? _write_index : 0;
        std::size_t active = 0;
        for (std::size_t i = 0; i < _count; ++i) {
            result.samples[i] = _samples[(oldest + i) % CAPACITY];
            if (result.samples[i].events != 0) {
                ++active;
            }
        }
        if (_count > 0) {
            result.current_rssi = result.samples[_count - 1].rssi_dbm;
            result.channel_load_percent = static_cast<uint8_t>((active * 100U) / _count);
        }
        result.noise_floor_ready = _noise_count >= MIN_BASELINE_SAMPLES;
        result.noise_floor = result.noise_floor_ready ? noise_floor() : MIN_RSSI_DBM;
        return result;
    }

private:
    static int16_t clamp_rssi(int16_t value) {
        if (value < MIN_RSSI_DBM) return MIN_RSSI_DBM;
        if (value > MAX_RSSI_DBM) return MAX_RSSI_DBM;
        return value;
    }

    int16_t noise_floor() const {
        if (_noise_count == 0) return MIN_RSSI_DBM;
        // Integer division intentionally gives a stable whole-dBm display value.
        return static_cast<int16_t>(_noise_sum / static_cast<int32_t>(_noise_count));
    }

    void accept_noise(int16_t rssi) {
        if (_noise_count < NOISE_WINDOW_CAPACITY) {
            _noise_samples[_noise_write] = rssi;
            _noise_sum += rssi;
            ++_noise_count;
        } else {
            _noise_sum -= _noise_samples[_noise_write];
            _noise_samples[_noise_write] = rssi;
            _noise_sum += rssi;
        }
        _noise_write = (_noise_write + 1) % NOISE_WINDOW_CAPACITY;
    }

    std::array<Sample, CAPACITY> _samples{};
    std::array<int16_t, NOISE_WINDOW_CAPACITY> _noise_samples{};
    std::size_t _write_index = 0;
    std::size_t _count = 0;
    std::size_t _noise_write = 0;
    std::size_t _noise_count = 0;
    int32_t _noise_sum = 0;
    uint32_t _next_sequence = 0;
    uint8_t _pending_events = 0;
};

} // namespace RadioActivity
