#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace aurora::emulation {

struct ProcessWorkload {
    std::string_view id;
    std::string_view contract;
    std::size_t generation_count = 0;
    std::size_t symbol_size = 0;
    std::size_t payload_base_bytes = 0;
    std::size_t payload_step_bytes = 0;

    [[nodiscard]] std::size_t payload_bytes(std::size_t generation) const {
        return payload_base_bytes + generation * payload_step_bytes;
    }
};

inline constexpr std::array<std::string_view, 2> process_workload_ids{
    "smoke-v2",
    "policy-pilot-v1",
};

inline ProcessWorkload process_workload(std::string_view workload_id) {
    if (workload_id == "smoke-v2") {
        return {
            "smoke-v2",
            "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
            "backscatter:on;ris:4;reserve:0.05;max_repair_amplification:4;"
            "min_critical_overhead:1.5;seed:1701",
            2,
            64,
            448,
            128,
        };
    }
    if (workload_id == "policy-pilot-v1") {
        return {
            "policy-pilot-v1",
            "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
            "backscatter:on;ris:4;importance:elastic;reserve:0.05;"
            "max_repair_amplification:4;min_critical_overhead:1.5;"
            "segment:0-1023,critical,10s,0.999;"
            "segment:1024-2047,important,20s,0.99;seed:1701",
            8,
            64,
            2560,
            0,
        };
    }
    throw std::invalid_argument(
        "process workload: expected smoke-v2 or policy-pilot-v1");
}

} // namespace aurora::emulation
