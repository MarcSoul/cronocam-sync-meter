// SPDX-License-Identifier: GPL-2.0-or-later
#include "delay.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("cronocam-sync-meter", "en-US")

namespace {
using namespace audio_sync;
constexpr uint64_t bin_ns = bin_ms * 1'000'000ULL;
constexpr size_t capacity = span + 400;
constexpr const char *camera_key = "camera_uuid";

struct Bin {
    uint64_t index = 0;
    double energy = 0;
    uint32_t samples = 0;
};

struct Tap {
    std::mutex mutex;
    obs_weak_source_t *source = nullptr;
    std::array<Bin, capacity> bins{};
    uint64_t latest = 0;
    uint64_t received = 0;
    uint32_t sample_rate = 0;
    size_t channels = 0;
};

void capture(void *data, obs_source_t *, const audio_data *audio, bool)
{
    auto &tap = *static_cast<Tap *>(data);
    const std::unique_lock lock(tap.mutex, std::try_to_lock);
    if (!lock || !tap.sample_rate || !audio->frames)
        return;

    for (uint32_t frame = 0; frame < audio->frames; ++frame) {
        const uint64_t timestamp = audio->timestamp + frame * 1'000'000'000ULL / tap.sample_rate;
        const uint64_t index = timestamp / bin_ns;
        auto &bin = tap.bins[index % capacity];
        if (bin.index != index)
            bin = {index, 0, 0};
        for (size_t channel = 0; channel < tap.channels; ++channel) {
            if (!audio->data[channel])
                continue;
            const float sample = reinterpret_cast<const float *>(audio->data[channel])[frame];
            if (std::isfinite(sample)) {
                bin.energy += static_cast<double>(sample) * sample;
                ++bin.samples;
            }
        }
        tap.latest = index;
    }
    tap.received = os_gettime_ns();
}

void bind(Tap &tap, obs_source_t *source)
{
    auto *previous = tap.source ? obs_weak_source_get_source(tap.source) : nullptr;
    if (previous && previous == source) {
        obs_source_release(previous);
        return;
    }
    if (previous) {
        obs_source_remove_audio_capture_callback(previous, capture, &tap);
        obs_source_release(previous);
    }
    if (tap.source)
        obs_weak_source_release(tap.source);
    tap.source = source ? obs_source_get_weak_source(source) : nullptr;
    {
        const std::lock_guard lock(tap.mutex);
        tap.bins = {};
        tap.latest = 0;
        tap.received = 0;
    }
    if (source)
        obs_source_add_audio_capture_callback(source, capture, &tap);
}

bool copy_envelope(const Tap &tap, uint64_t first, Envelope &values)
{
    int missing = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        const uint64_t index = first + i;
        const auto &bin = tap.bins[index % capacity];
        if (bin.index != index || !bin.samples) {
            ++missing;
            continue;
        }
        values[i] = static_cast<float>(std::log1p(1000 * std::sqrt(bin.energy / bin.samples)));
    }
    return missing < 8;
}

struct Filter {
    obs_source_t *self;
    std::mutex configuration;
    Tap reference, camera;
    obs_weak_source_t *parent = nullptr;
    std::string camera_uuid;
    std::string display;
    uint64_t next_refresh = 0;
};

void select_camera(Filter &filter, obs_source_t *parent)
{
    auto *camera = parent && !filter.camera_uuid.empty() ?
        obs_get_source_by_uuid(filter.camera_uuid.c_str()) : nullptr;
    bind(filter.camera, camera != parent ? camera : nullptr);
    obs_source_release(camera);
}

void update(void *data, obs_data_t *settings)
{
    auto &filter = *static_cast<Filter *>(data);
    const std::lock_guard lock(filter.configuration);
    filter.camera_uuid = obs_data_get_string(settings, camera_key);
    auto *parent = filter.parent ? obs_weak_source_get_source(filter.parent) : nullptr;
    select_camera(filter, parent);
    obs_source_release(parent);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
    obs_audio_info info{};
    if (!obs_get_audio_info(&info))
        return nullptr;
    auto *filter = new Filter{};
    filter->self = source;
    filter->camera.sample_rate = filter->reference.sample_rate = info.samples_per_sec;
    filter->camera.channels = filter->reference.channels = get_audio_channels(info.speakers);
    update(filter, settings);
    return filter;
}

void remove(void *data, obs_source_t *)
{
    auto &filter = *static_cast<Filter *>(data);
    const std::lock_guard lock(filter.configuration);
    bind(filter.camera, nullptr);
    bind(filter.reference, nullptr);
    if (filter.parent) {
        obs_weak_source_release(filter.parent);
        filter.parent = nullptr;
    }
}

void destroy(void *data)
{
    remove(data, nullptr);
    delete static_cast<Filter *>(data);
}

obs_audio_data *pass_audio(void *data, obs_audio_data *audio)
{
    auto &filter = *static_cast<Filter *>(data);
    // OBS only guarantees access to the parent from filter callbacks.
    const std::unique_lock lock(filter.configuration, std::try_to_lock);
    if (lock && !filter.parent) {
        if (auto *parent = obs_filter_get_parent(filter.self))
            filter.parent = obs_source_get_weak_source(parent);
    }
    return audio;
}

std::string reading_card(const std::string &value, const char *caption = nullptr)
{
    std::string card = "<html><table width=\"224\" cellspacing=\"0\" cellpadding=\"12\">"
                       "<tr><td align=\"center\" bgcolor=\"#182430\">"
                       "<span style=\"font-size:28pt; font-weight:bold; color:#A5F3FC;\">";
    card += value;
    card += "</span>";
    if (caption) {
        card += "<br/><span style=\"font-size:9pt; color:#E2E8F0;\">";
        card += caption;
        card += "</span>";
    }
    return card + "</td></tr></table></html>";
}

std::string reading(Filter &filter, uint64_t now)
{
    if (filter.camera_uuid.empty())
        return reading_card("&mdash;", obs_module_text("SelectCamera"));
    Envelope reference{}, camera{};
    {
        const std::scoped_lock lock(filter.reference.mutex, filter.camera.mutex);
        if (!filter.reference.received || !filter.camera.received ||
            now - std::min({now, filter.reference.received, filter.camera.received}) > 1'500'000'000ULL)
            return reading_card("&mdash;", obs_module_text("WaitingAudio"));
        const auto last = std::min(filter.reference.latest, filter.camera.latest);
        if (last < span || !copy_envelope(filter.reference, last - span, reference) ||
            !copy_envelope(filter.camera, last - span, camera))
            return reading_card("&mdash;", obs_module_text("CollectingAudio"));
    }

    const auto offset = measure(reference, camera);
    if (!offset)
        return reading_card("&mdash;", obs_module_text("Uncertain"));
    if (*offset == 0)
        return reading_card("0 ms");
    const auto value = std::string(*offset < 0 ? "&minus;" : "+") + std::to_string(std::abs(*offset)) + " ms";
    return reading_card(value, obs_module_text(*offset < 0 ? "CameraLater" : "CameraEarlier"));
}

void tick(void *data, float)
{
    auto &filter = *static_cast<Filter *>(data);
    const auto now = os_gettime_ns();
    if (now < filter.next_refresh)
        return;
    filter.next_refresh = now + 1'000'000'000ULL;
    {
        const std::lock_guard lock(filter.configuration);
        if (!filter.reference.source && filter.parent) {
            if (auto *parent = obs_weak_source_get_source(filter.parent)) {
                bind(filter.reference, parent);
                select_camera(filter, parent);
                obs_source_release(parent);
            }
        }
        auto display = obs_source_enabled(filter.self) ? reading(filter, now) : reading_card("&mdash;", obs_module_text("Disabled"));
        if (display == filter.display)
            return;
        filter.display = std::move(display);
    }
    obs_source_update_properties(filter.self);
}

struct SourceList {
    obs_property_t *list;
    obs_source_t *parent;
};

bool list_source(void *data, obs_source_t *source)
{
    const auto &context = *static_cast<SourceList *>(data);
    if (source != context.parent && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
        obs_property_list_add_string(context.list, obs_source_get_name(source), obs_source_get_uuid(source));
    return true;
}

obs_properties_t *properties(void *data)
{
    auto *props = obs_properties_create();
    auto *list = obs_properties_add_list(props, camera_key, obs_module_text("CameraSource"),
                                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(list, obs_module_text("SelectCamera"), "");
    auto *filter = static_cast<Filter *>(data);
    obs_source_t *parent = nullptr;
    if (filter) {
        const std::lock_guard lock(filter->configuration);
        if (filter->parent)
            parent = obs_weak_source_get_source(filter->parent);
        const auto display = filter->display.empty()
            ? reading_card("&mdash;", obs_module_text("CollectingAudio"))
            : filter->display;
        obs_properties_add_text(props, "reading", display.c_str(), OBS_TEXT_INFO);
        obs_properties_add_text(props, "reading_legend", obs_module_text("ReadingLegend"), OBS_TEXT_INFO);
    }
    SourceList context{list, parent};
    obs_enum_sources(list_source, &context);
    obs_source_release(parent);
    return props;
}
}

bool obs_module_load()
{
    obs_source_info info{};
    info.id = "marcsoul_audio_sync";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = [](void *) { return obs_module_text("FilterName"); };
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.filter_remove = remove;
    info.get_properties = properties;
    info.video_tick = tick;
    info.filter_audio = pass_audio;
    obs_register_source(&info);
    return true;
}
