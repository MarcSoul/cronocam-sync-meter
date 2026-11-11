// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/delay.hpp"

#include <cassert>
#include <iostream>
#include <random>

using namespace audio_sync;

int main()
{
    Envelope reference{};
    std::mt19937 random(42);
    for (auto &sample : reference)
        sample = static_cast<float>(random() % 1000) / 100;

    for (const int lag : {-15, 0, 60, 199}) {
        Envelope camera{};
        for (int i = 0; i < span; ++i) {
            const int from = i - lag;
            if (from >= 0 && from < span)
                camera[i] = 1 + 0.7f * reference[from] + static_cast<float>(random() % 100) / 500;
        }
        assert(measure(reference, camera) == -lag * bin_ms);
    }

    Envelope silence{};
    assert(!measure(reference, silence));
    assert(!measure(silence, reference));

    Envelope periodic{};
    for (int i = 0; i < span; ++i)
        periodic[i] = static_cast<float>(i % 20);
    assert(!measure(periodic, periodic));
    Envelope close_mic{}, room_mic{};
    for (int i = 0; i < span; ++i) {
        close_mic[i] = 2.0f + 1.5f * std::sin(i * 0.019f);
        room_mic[i] = 3.0f + 1.7f * std::cos(i * 0.013f);
    }
    for (const int onset : {220, 279, 353, 466, 501, 599, 715, 780}) {
        for (int j = 0; j < 25; ++j) {
            close_mic[onset + j] += 1.8f * std::exp(-j / 6.0f);
            room_mic[onset + 37 + j] += 1.1f * std::exp(-j / 11.0f);
        }
    }
    assert(!correlate(close_mic, room_mic, 0.45, 0.08));
    const auto different_mics = measure(close_mic, room_mic);
    assert(different_mics && std::abs(*different_mics + 185) <= 10);

    Envelope unrelated{};
    for (auto &sample : unrelated)
        sample = static_cast<float>(random() % 1000) / 100;
    assert(!measure(reference, unrelated));

    std::cout << "Delay tests passed\n";
}
