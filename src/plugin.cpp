// SPDX-License-Identifier: GPL-2.0-or-later
#include "delay.hpp"
#include "capture_clock.hpp"
#include "pcm_delay.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
    CaptureClock clock;
    bool estimated = false;
    uint32_t sample_rate = 0;
    size_t channels = 0;
};

void capture(void *data, obs_source_t *, const audio_data *audio, bool)
{
    auto &tap = *static_cast<Tap *>(data);
    const std::unique_lock lock(tap.mutex, std::try_to_lock);
    if (!lock || !tap.sample_rate || !audio->frames)
        return;

    const auto arrival = os_gettime_ns();
    const auto duration = audio->frames * 1'000'000'000ULL / tap.sample_rate;
    const auto block = tap.clock.place(audio->timestamp, arrival, duration);
    if (block.reset)
        tap.bins = {};
    tap.estimated = block.estimated;

    for (uint32_t frame = 0; frame < audio->frames; ++frame) {
        const uint64_t timestamp = block.start + frame * 1'000'000'000ULL / tap.sample_rate;
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
    tap.received = arrival;
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
        tap.clock = {};
        tap.estimated = false;
    }
    if (source)
        obs_source_add_audio_capture_callback(source, capture, &tap);
}

int copy_envelope(const Tap &tap, uint64_t first, Envelope &values)
{
    int missing = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        const uint64_t index = first + i;
        const auto &bin = tap.bins[index % capacity];
        if (bin.index != index || !bin.samples) {
            ++missing;
            values[i] = missing_bin;
            continue;
        }
        values[i] = static_cast<float>(std::log1p(1000 * std::sqrt(bin.energy / bin.samples)));
    }
    return span - missing;
}

constexpr const char *delay_id = "marcsoul_audio_delay";
constexpr const char *delay_key = "delay_ms";
constexpr uint64_t verify_ns = (span * bin_ms + 1000ULL) * 1'000'000;

struct SourceRelease {
    void operator()(obs_source_t *source) const { obs_source_release(source); }
};
using SourceRef = std::unique_ptr<obs_source_t, SourceRelease>;

struct DelayFilter {
    DelayFilter(size_t channels, uint32_t rate, int ms)
        : pcm(channels, rate, ms), milliseconds(ms), channels(channels)
    {
    }

    PcmDelay pcm;
    std::atomic<int> milliseconds;
    size_t channels;
};

void *delay_create(obs_data_t *settings, obs_source_t *)
{
    obs_audio_info info{};
    if (!obs_get_audio_info(&info))
        return nullptr;
    return new DelayFilter(get_audio_channels(info.speakers), info.samples_per_sec,
                           delay_ms(obs_data_get_int(settings, delay_key)));
}

void delay_update(void *data, obs_data_t *settings)
{
    static_cast<DelayFilter *>(data)->milliseconds.store(
        delay_ms(obs_data_get_int(settings, delay_key)), std::memory_order_relaxed);
}

obs_audio_data *delay_audio(void *data, obs_audio_data *audio)
{
    auto &delay = *static_cast<DelayFilter *>(data);
    std::array<float *, MAX_AV_PLANES> planes{};
    for (size_t channel = 0; channel < delay.channels; ++channel)
        planes[channel] = reinterpret_cast<float *>(audio->data[channel]);
    // Shift the samples themselves so the source monitor receives the same delay.
    delay.pcm.process(planes.data(), audio->frames, delay.milliseconds.load(std::memory_order_relaxed));
    return audio;
}

obs_properties_t *delay_properties(void *)
{
    auto *props = obs_properties_create();
    auto *slider = obs_properties_add_int_slider(props, delay_key, obs_module_text("DelayValue"),
                                                0, max_delay_ms, 1);
    obs_property_int_set_suffix(slider, " ms");
    obs_properties_add_text(props, "delay_help", obs_module_text("DelayHelp"), OBS_TEXT_INFO);
    return props;
}

SourceRef find_delay(obs_source_t *parent)
{
    struct Search {
        SourceRef match;
        unsigned count = 0;
    } search;
    if (parent) {
        obs_source_enum_filters(parent, [](obs_source_t *, obs_source_t *source, void *data) {
            auto &result = *static_cast<Search *>(data);
            if (std::string_view(obs_source_get_id(source)) != delay_id)
                return;
            if (++result.count == 1)
                result.match.reset(obs_source_get_ref(source));
        }, &search);
    }
    if (search.count != 1 || !search.match || !obs_source_enabled(search.match.get()))
        return {};
    return std::move(search.match);
}

int configured_delay(obs_source_t *source)
{
    auto *settings = obs_source_get_settings(source);
    const auto milliseconds = delay_ms(obs_data_get_int(settings, delay_key));
    obs_data_release(settings);
    return milliseconds;
}

struct Filter {
    obs_source_t *self;
    std::mutex configuration;
    Tap reference, camera;
    obs_weak_source_t *parent = nullptr;
    std::string camera_uuid;
    std::string display;
    uint64_t next_refresh = 0;
    uint64_t verify_until = 0;
    uint64_t measured_at = 0;
    std::optional<int> offset;
    bool estimated = false;
    std::string delay_uuid;
    std::optional<int> applied_delay;
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
    const std::string camera_uuid = obs_data_get_string(settings, camera_key);
    if (camera_uuid != filter.camera_uuid)
        filter.offset.reset();
    filter.camera_uuid = camera_uuid;
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
    filter.offset.reset();
    filter.applied_delay.reset();
    filter.delay_uuid.clear();
    filter.verify_until = 0;
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
        const auto recent = [now](const Tap &tap) {
            return tap.received && now - std::min(now, tap.received) <= 1'500'000'000ULL;
        };
        if (!recent(filter.reference))
            return reading_card("&mdash;", obs_module_text("WaitingReference"));
        if (!recent(filter.camera))
            return reading_card("&mdash;", obs_module_text("WaitingCamera"));
        filter.estimated = filter.reference.estimated || filter.camera.estimated;
        const auto last = std::min(filter.reference.latest, filter.camera.latest);
        if (last < span)
            return reading_card("&mdash;", obs_module_text("CollectingAudio"));
        const int ref_count = copy_envelope(filter.reference, last - span, reference);
        const int cam_count = copy_envelope(filter.camera, last - span, camera);
        if (std::min(ref_count, cam_count) < span * 7 / 10) {
            const auto coverage = std::to_string(ref_count * 100 / span) + "% / " +
                                  std::to_string(cam_count * 100 / span) + "%";
            return reading_card(coverage, obs_module_text("AudioCoverage"));
        }
    }

    const auto offset = measure(reference, camera);
    filter.offset = offset;
    filter.measured_at = now;
    if (!offset)
        return reading_card("&mdash;", obs_module_text("Uncertain"));
    if (*offset == 0)
        return reading_card("0 ms", filter.estimated ? obs_module_text("EstimatedClock") : nullptr);
    const auto value = std::string(*offset < 0 ? "&minus;" : "+") + std::to_string(std::abs(*offset)) + " ms";
    const std::string caption = obs_module_text(*offset < 0 ? "CameraLater" : "CameraEarlier");
    return reading_card(value, (filter.estimated ? caption + "<br/>" + obs_module_text("EstimatedClock") : caption).c_str());
}

std::optional<int> correction(const Filter &filter, uint64_t now)
{
    if (!filter.offset || !filter.applied_delay || now < filter.verify_until ||
        now - filter.measured_at > 1'500'000'000ULL || !obs_source_enabled(filter.self))
        return {};
    const int target = *filter.applied_delay - *filter.offset;
    if (*filter.offset == 0 || target < 0 || target > max_delay_ms)
        return {};
    return target;
}

bool apply_correction(obs_properties_t *props, obs_property_t *, void *)
{
    auto &filter = *static_cast<Filter *>(obs_properties_get_param(props));
    SourceRef parent;
    {
        const std::lock_guard lock(filter.configuration);
        if (filter.parent)
            parent.reset(obs_weak_source_get_source(filter.parent));
    }
    auto delay = find_delay(parent.get());
    if (!delay)
        return false;

    const int actual = configured_delay(delay.get());
    std::optional<int> target;
    {
        const std::lock_guard lock(filter.configuration);
        const auto now = os_gettime_ns();
        if (filter.delay_uuid != obs_source_get_uuid(delay.get()) || filter.applied_delay != actual)
            return false;
        target = correction(filter, now);
        if (!target)
            return false;
        filter.offset.reset();
        filter.verify_until = now + verify_ns;
        filter.applied_delay = *target;
        filter.display = reading_card("&mdash;", obs_module_text("Verifying"));
    }
    auto *settings = obs_data_create();
    obs_data_set_int(settings, delay_key, *target);
    obs_source_update(delay.get(), settings);
    obs_data_release(settings);
    return true;
}

void tick(void *data, float)
{
    auto &filter = *static_cast<Filter *>(data);
    const auto now = os_gettime_ns();
    if (now < filter.next_refresh)
        return;
    filter.next_refresh = now + 1'000'000'000ULL;

    SourceRef parent;
    {
        const std::lock_guard lock(filter.configuration);
        if (filter.parent)
            parent.reset(obs_weak_source_get_source(filter.parent));
        if (!filter.reference.source && parent) {
            bind(filter.reference, parent.get());
            select_camera(filter, parent.get());
        }
    }
    auto delay = find_delay(parent.get());
    const std::string uuid = delay ? obs_source_get_uuid(delay.get()) : "";
    const std::optional<int> applied = delay ? std::optional<int>(configured_delay(delay.get())) : std::nullopt;
    {
        const std::lock_guard lock(filter.configuration);
        if (!filter.parent)
            return;
        const bool changed = uuid != filter.delay_uuid || applied != filter.applied_delay;
        if (changed) {
            filter.delay_uuid = uuid;
            filter.applied_delay = applied;
            filter.verify_until = now + verify_ns;
        }
        const bool had_offset = filter.offset.has_value();
        filter.offset.reset();
        auto display = obs_source_enabled(filter.self) ? reading(filter, now)
                                                       : reading_card("&mdash;", obs_module_text("Disabled"));
        if (now < filter.verify_until) {
            filter.offset.reset();
            display = reading_card("&mdash;", obs_module_text("Verifying"));
        }
        if (!changed && display == filter.display && had_offset == filter.offset.has_value())
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
        if (filter->applied_delay) {
            const auto applied = std::string(obs_module_text("AppliedDelay")) + " " +
                std::to_string(*filter->applied_delay) + " ms";
            obs_properties_add_text(props, "applied_delay", applied.c_str(), OBS_TEXT_INFO);
            const auto target = correction(*filter, os_gettime_ns());
            const auto button_label = target
                ? std::string(obs_module_text("ApplyDelay")) + " " + std::to_string(*target) + " ms"
                : std::string(obs_module_text("ApplyCorrection"));
            obs_properties_set_param(props, filter, nullptr);
            auto *button = obs_properties_add_button(props, "apply_delay", button_label.c_str(), apply_correction);
            obs_property_set_enabled(button, target.has_value());
            if (filter->offset && *filter->applied_delay - *filter->offset < 0)
                obs_properties_add_text(props, "other_source", obs_module_text("DelayOtherSource"), OBS_TEXT_INFO);
            else if (filter->offset && *filter->applied_delay - *filter->offset > max_delay_ms)
                obs_properties_add_text(props, "delay_limit", obs_module_text("DelayLimit"), OBS_TEXT_INFO);
        }
        obs_properties_add_text(props, "reading_legend", obs_module_text("ReadingLegend"), OBS_TEXT_INFO);
        obs_properties_add_text(props, "meter_help", obs_module_text("MeterHelp"), OBS_TEXT_INFO);
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

    obs_source_info delay{};
    delay.id = delay_id;
    delay.type = OBS_SOURCE_TYPE_FILTER;
    delay.output_flags = OBS_SOURCE_AUDIO;
    delay.get_name = [](void *) { return obs_module_text("DelayName"); };
    delay.create = delay_create;
    delay.destroy = [](void *data) { delete static_cast<DelayFilter *>(data); };
    delay.update = delay_update;
    delay.get_properties = delay_properties;
    delay.filter_audio = delay_audio;
    obs_register_source(&delay);
    return true;
}
