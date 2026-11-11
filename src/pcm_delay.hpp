// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_sync {
constexpr int max_delay_ms = 2000;

inline int delay_ms(int64_t value)
{
    return static_cast<int>(std::clamp<int64_t>(value, 0, max_delay_ms));
}

class PcmDelay {
public:
    PcmDelay(size_t channels, uint32_t sample_rate, int milliseconds)
        : channels(channels), rate(sample_rate), capacity(to_frames(max_delay_ms) + 1),
          history(channels * capacity), fade_frames(std::max<size_t>(1, to_frames(10))),
          from(to_frames(milliseconds)), to(from), remaining(0)
    {
    }

    void process(float *const *planes, size_t frames, int milliseconds)
    {
        const auto requested = to_frames(milliseconds);
        for (size_t frame = 0; frame < frames; ++frame) {
            if (remaining == 0 && requested != to) {
                from = to;
                to = requested;
                remaining = fade_frames;
            }
            const float blend = remaining
                ? static_cast<float>(fade_frames - remaining + 1) / static_cast<float>(fade_frames)
                : 1.0f;
            const size_t old_position = (position + capacity - from) % capacity;
            const size_t new_position = (position + capacity - to) % capacity;
            for (size_t channel = 0; channel < channels; ++channel) {
                auto *samples = planes[channel];
                auto *ring = history.data() + channel * capacity;
                ring[position] = samples ? samples[frame] : 0.0f;
                if (samples) {
                    const float next = ring[new_position];
                    samples[frame] = remaining ? (1.0f - blend) * ring[old_position] + blend * next : next;
                }
            }
            position = (position + 1) % capacity;
            if (remaining)
                --remaining;
        }
    }

private:
    size_t to_frames(int milliseconds) const
    {
        return (static_cast<uint64_t>(rate) * delay_ms(milliseconds) + 500) / 1000;
    }

    const size_t channels;
    const uint32_t rate;
    const size_t capacity;
    std::vector<float> history;
    const size_t fade_frames;
    size_t from, to, remaining;
    size_t position = 0;
};
}
