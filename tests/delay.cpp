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
    std::cout << "Delay tests passed\n";
}
