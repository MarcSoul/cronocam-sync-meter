// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace audio_sync {
constexpr int bin_ms = 5;
constexpr int max_lag = 1000 / bin_ms;
constexpr int window = 3 * 1000 / bin_ms;
constexpr int span = window + 2 * max_lag;
using Envelope = std::array<float, span>;

inline std::optional<int> measure(const Envelope &reference, const Envelope &camera)
{
    double sum_reference = 0, energy_reference = 0;
    for (int i = max_lag; i < max_lag + window; ++i) {
        sum_reference += reference[i];
        energy_reference += reference[i] * reference[i];
    }
    const double variance_reference = energy_reference - sum_reference * sum_reference / window;
    if (variance_reference < 0.1)
        return std::nullopt;

    std::array<double, 2 * max_lag + 1> scores{};
    int best = 0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double sum = 0, energy = 0, product = 0;
        for (int i = max_lag; i < max_lag + window; ++i) {
            const double sample = camera[i + lag];
            sum += sample;
            energy += sample * sample;
            product += sample * reference[i];
        }
        const double variance = energy - sum * sum / window;
        const int index = lag + max_lag;
        scores[index] = variance < 0.1 ? -1 :
            (product - sum_reference * sum / window) / std::sqrt(variance * variance_reference);
        if (scores[index] > scores[best])
            best = index;
    }

    double alternative = -1;
    for (int i = 0; i < static_cast<int>(scores.size()); ++i) {
        if (std::abs(i - best) >= 6)
            alternative = std::max(alternative, scores[i]);
    }
    if (best == 0 || best == 2 * max_lag || scores[best] < 0.45 || scores[best] - alternative < 0.08)
        return std::nullopt;

    return (max_lag - best) * bin_ms; // Negative when camera audio arrives later.
}
}
