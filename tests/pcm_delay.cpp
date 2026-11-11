// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/pcm_delay.hpp"

#include <array>
#include <cassert>

int main()
{
    audio_sync::PcmDelay stereo(2, 1000, 2);
    std::array<float, 3> left{1, 2, 3}, right{4, 5, 6};
    float *planes[]{left.data(), right.data()};
    stereo.process(planes, left.size(), 2);
    assert((left == std::array<float, 3>{0, 0, 1}));
    assert((right == std::array<float, 3>{0, 0, 4}));

    left = {7, 8, 9};
    right = {10, 11, 12};
    stereo.process(planes, left.size(), 2);
    assert((left == std::array<float, 3>{2, 3, 7}));
    assert((right == std::array<float, 3>{5, 6, 10}));

    audio_sync::PcmDelay adjustable(1, 1000, 0);
    std::array<float, 32> signal{};
    float *mono[]{signal.data()};
    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<float>(i);
    adjustable.process(mono, signal.size(), 0);
    for (size_t i = 0; i < signal.size(); ++i)
        assert(signal[i] == static_cast<float>(i));

    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<float>(i + 32);
    adjustable.process(mono, signal.size(), 10);
    assert(signal[0] == 31.0f);
    assert(signal[9] == 31.0f);
    assert(signal[10] == 32.0f);

    assert(audio_sync::delay_ms(-1) == 0);
    assert(audio_sync::delay_ms(3000) == 2000);
}
