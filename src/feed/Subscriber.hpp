#pragma once

#include <cstdint>

namespace cmf::feed
{

// Opaque handle from subscribe(); id 0 means invalid / not subscribed.
struct SubscriberHandle
{
    uint64_t id = 0;

    [[nodiscard]] bool valid() const noexcept { return id != 0; }
    [[nodiscard]] bool operator==(const SubscriberHandle&) const noexcept =
        default;
};

} // namespace cmf::feed
