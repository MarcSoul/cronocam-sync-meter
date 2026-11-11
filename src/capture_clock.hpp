// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace audio_sync {
class CaptureClock {
public:
    struct Block {
        uint64_t start;
        bool estimated;
        bool reset;
    };

    Block place(uint64_t timestamp, uint64_t arrival, uint64_t duration)
    {
        const auto arrival_start = arrival > duration ? arrival - duration : 0;
        const bool estimated = !timestamp || distance(timestamp, arrival_start) > 10'000'000'000ULL;
        const bool reset = !initialized || estimated != previous_estimated ||
            arrival < last_arrival || arrival - last_arrival > 1'000'000'000ULL ||
            (estimated && (timestamp == 0) != (last_timestamp == 0)) ||
            (estimated && timestamp && (timestamp < last_timestamp ||
                distance(timestamp - last_timestamp, arrival - last_arrival) > 250'000'000ULL));

        uint64_t start = timestamp;
        if (estimated) {
            if (reset)
                start = arrival_start;
            else if (timestamp)
                start = last_start + (timestamp - last_timestamp);
            else
                start = last_end;
        }

        initialized = true;
        previous_estimated = estimated;
        last_arrival = arrival;
        last_timestamp = timestamp;
        last_start = start;
        last_end = start + duration;
        return {start, estimated, reset};
    }

private:
    static uint64_t distance(uint64_t a, uint64_t b) { return a < b ? b - a : a - b; }

    bool initialized = false;
    bool previous_estimated = false;
    uint64_t last_arrival = 0, last_timestamp = 0, last_start = 0, last_end = 0;
};
}
