// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <limits>

namespace audio_sync {
constexpr int bin_ms = 5;
constexpr int max_lag = 1000 / bin_ms;
constexpr int window = 3 * 1000 / bin_ms;
constexpr int span = window + 2 * max_lag;
constexpr float missing_bin = std::numeric_limits<float>::quiet_NaN();
using Envelope = std::array<float, span>;

inline std::optional<int> correlate(const Envelope &reference, const Envelope &camera,
                                    double minimum_score, double minimum_margin)
{
    std::array<double, 2 * max_lag + 1> scores{};
    int best = 0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double sum = 0, energy = 0, product = 0;
        double sum_reference = 0, energy_reference = 0;
        int pairs = 0;
        for (int i = max_lag; i < max_lag + window; ++i) {
            const double sample = camera[i + lag];
            const double ref = reference[i];
            if (!std::isfinite(sample) || !std::isfinite(ref))
                continue;
            ++pairs;
            sum_reference += ref;
            energy_reference += ref * ref;
            sum += sample;
            energy += sample * sample;
            product += sample * ref;
        }
        const int index = lag + max_lag;
        scores[index] = -1;
        // Require at least two seconds of shared valid samples at each candidate lag.
        if (pairs >= 2000 / bin_ms) {
            const double variance = energy - sum * sum / pairs;
            const double variance_reference = energy_reference - sum_reference * sum_reference / pairs;
            if (variance >= 0.1 && variance_reference >= 0.1)
                scores[index] = (product - sum_reference * sum / pairs) /
                    std::sqrt(variance * variance_reference);
        }
        if (scores[index] > scores[best])
            best = index;
    }

    double alternative = -1;
    for (int i = 0; i < static_cast<int>(scores.size()); ++i) {
        if (std::abs(i - best) >= 6)
            alternative = std::max(alternative, scores[i]);
    }
    if (best == 0 || best == 2 * max_lag || scores[best] < minimum_score ||
        scores[best] - alternative < minimum_margin)
        return std::nullopt;

    return (max_lag - best) * bin_ms; // Negative when camera audio arrives later.
}

inline Envelope onset_envelope(const Envelope &input)
{
    Envelope onsets;
    onsets.fill(missing_bin);
    // Compare adjacent 20 ms averages to suppress noise and slow gain changes.
    for (int i = 7; i < span; ++i) {
        float recent = 0, previous = 0;
        int recent_count = 0, previous_count = 0;
        for (int j = 0; j < 4; ++j) {
            const float current = input[i - j];
            const float earlier = input[i - 4 - j];
            if (std::isfinite(current)) {
                recent += current;
                ++recent_count;
            }
            if (std::isfinite(earlier)) {
                previous += earlier;
                ++previous_count;
            }
        }
        if (recent_count < 3 || previous_count < 3)
            continue;
        const float rise = std::max(0.0f, recent / recent_count - previous / previous_count);
        onsets[i] = std::sqrt(rise);
    }
    return onsets;
}

inline std::optional<int> measure(const Envelope &reference, const Envelope &camera)
{
    const auto level = correlate(reference, camera, 0.45, 0.08);
    const auto onsets = correlate(onset_envelope(reference), onset_envelope(camera), 0.38, 0.08);
    // Conflicting, individually credible peaks are unsafe for automatic correction.
    if (level && onsets && std::abs(*level - *onsets) > 15)
        return std::nullopt;
    return level ? level : onsets;
}
}
