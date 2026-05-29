#include <iostream>
#include "luv_arena.hpp"

int main() {
    luv::Arena arena;
    if (!arena.init()) {
        std::cerr << "Arena initialization failed" << std::endl;
        return 1;
    }
    auto report = arena.report();
    std::cout << "Memory Usage Report:" << std::endl;
    std::cout << "LOB bytes: " << report.lob_bytes << std::endl;
    std::cout << "Tick ring bytes: " << report.tick_ring_bytes << std::endl;
    std::cout << "Feature bytes: " << report.feature_bytes << std::endl;
    std::cout << "Signal bytes: " << report.signal_bytes << std::endl;
    std::cout << "Exec bytes: " << report.exec_bytes << std::endl;
    std::cout << "Telemetry bytes: " << report.telem_bytes << std::endl;
    std::cout << "Total infrastructure bytes (rounded): " << report.infra_total_bytes << std::endl;
    std::cout << "AI region bytes: " << report.ai_region_bytes << std::endl;
    std::cout << "Grand total bytes: " << report.grand_total_bytes << std::endl;
    std::cout << "mlocked: " << (report.mlocked ? "yes" : "no") << std::endl;
    return 0;
}
