#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "core/event.h"

namespace graphflow::core {

// A small, strongly typed event shared by native and Python nodes.  Projects
// can add further event bindings following the same EventT pattern.
struct PayloadEvent : EventT<PayloadEvent> {
    std::string payload;
    std::uint64_t sequence = 0;

    PayloadEvent() = default;
    explicit PayloadEvent(std::string value, std::uint64_t seq = 0)
        : payload(std::move(value)), sequence(seq) {}
};

}  // namespace graphflow::core
