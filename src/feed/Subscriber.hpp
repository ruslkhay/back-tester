#pragma once

#include <cstdint>

namespace cmf::feed
{

// Opaque handle returned by MarketDataPublisher::subscribe().  The trading
// engine keeps it and passes it back to unsubscribe().  An id of 0 is the
// "invalid / not subscribed" sentinel.
struct SubscriberHandle
{
    uint64_t id = 0;

    [[nodiscard]] bool valid() const noexcept { return id != 0; }
    [[nodiscard]] bool operator==(const SubscriberHandle&) const noexcept =
        default;
};

} // namespace cmf::feed
