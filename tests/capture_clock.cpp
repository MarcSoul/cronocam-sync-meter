#include "../src/capture_clock.hpp"
#include <cassert>

int main()
{
    using audio_sync::CaptureClock;
    CaptureClock local, ndi, untimed;
    constexpr uint64_t now = 300'000'000'000ULL;
    constexpr uint64_t remote = 1'700'000'000'000'000'000ULL;
    constexpr uint64_t block = 20'000'000ULL;

    auto a = local.place(now - block, now, block);
    auto b = ndi.place(remote, now + 185'000'000ULL, block);
    assert(!a.estimated && b.estimated && b.start - a.start == 185'000'000ULL);
    auto next = ndi.place(remote + block, now + 211'000'000ULL, block);
    assert(!next.reset && next.start - b.start == block);
    assert(ndi.place(remote - block, now + 230'000'000ULL, block).reset);
    auto first = untimed.place(0, now, block);
    assert(untimed.place(0, now + block + 100, block).start == first.start + block);
}
