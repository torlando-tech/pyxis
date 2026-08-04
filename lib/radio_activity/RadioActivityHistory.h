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
    bool rssi_valid = true;
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
    bool current_rssi_valid = false;
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
        const std::size_t index = event_index(event);
        if (_pending_event_buckets[index] == 0) {
            _pending_event_buckets[index] = 1;
        }
    }

    void mark_event(Event event, std::size_t buckets) {
        if (event == Event::Noise || buckets == 0) return;
        const std::size_t index = event_index(event);
        const std::size_t total = _pending_event_buckets[index] + buckets;
        _pending_event_buckets[index] = static_cast<uint8_t>(
            total > CAPACITY ? CAPACITY : total);
    }

    void record(int16_t rssi_dbm) {
        const int16_t rssi = clamp_rssi(rssi_dbm);
        uint8_t events = take_pending_events();
        if (events == 0 && _noise_count >= MIN_BASELINE_SAMPLES &&
            rssi > static_cast<int16_t>(noise_floor() + ACTIVITY_THRESHOLD_DB)) {
            events |= event_bit(Event::Interference);
        }

        if (events == 0) {
            accept_noise(rssi);
        }

        append_sample(rssi, true, events);
    }

    void record_gap(bool consume_events = true) {
        append_sample(MIN_RSSI_DBM, false,
                      consume_events ? take_pending_events() : 0);
    }

    std::size_t pending_bucket_span() const {
        std::size_t span = 0;
        for (const uint8_t count : _pending_event_buckets) {
            if (count > span) span = count;
        }
        return span;
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
            const Sample& newest = result.samples[_count - 1];
            result.current_rssi_valid = newest.rssi_valid;
            result.current_rssi = newest.rssi_valid ? newest.rssi_dbm : MIN_RSSI_DBM;
            result.channel_load_percent = static_cast<uint8_t>((active * 100U) / _count);
        }
        result.noise_floor_ready = _noise_count >= MIN_BASELINE_SAMPLES;
        result.noise_floor = result.noise_floor_ready ? noise_floor() : MIN_RSSI_DBM;
        return result;
    }

private:
    static std::size_t event_index(Event event) {
        switch (event) {
            case Event::Rx: return 0;
            case Event::Tx: return 1;
            case Event::Interference: return 2;
            case Event::Noise: return 0;
        }
        return 0;
    }

    uint8_t take_pending_events() {
        uint8_t events = 0;
        for (std::size_t i = 0; i < _pending_event_buckets.size(); ++i) {
            if (_pending_event_buckets[i] > 0) {
                events |= static_cast<uint8_t>(1U << i);
                --_pending_event_buckets[i];
            }
        }
        return events;
    }

    void append_sample(int16_t rssi, bool rssi_valid, uint8_t events) {
        Sample sample;
        sample.rssi_dbm = rssi;
        sample.events = events;
        sample.rssi_valid = rssi_valid;
        sample.sequence = _next_sequence++;
        _samples[_write_index] = sample;
        _write_index = (_write_index + 1) % CAPACITY;
        if (_count < CAPACITY) {
            ++_count;
        }
    }

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
    std::array<uint8_t, 3> _pending_event_buckets{};
};

} // namespace RadioActivity
