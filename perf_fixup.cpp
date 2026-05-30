#include <chrono>
#include <iostream>
#include <vector>
#include "luv_arena.hpp"
#include "luv_lob.hpp"

int main() {
    luv::Arena arena;
    if (!arena.init()) return 1;
    luv::LOBEngine lob;
    if (!lob.init(arena)) return 1;

    const uint16_t sym = 0;
    uint64_t ref = 1;

    // Add 100 levels
    for (int i = 0; i < 100; ++i) {
        lob.process(luv::TickMsg{'A', 0, sym, 0, 0, static_cast<int64_t>(1000000 - i * 10), 100, ref++, 0});
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < 100000; ++iter) {
        lob.process(luv::TickMsg{'D', 0, sym, 0, 0, 0, 0, 1, 0});
        lob.process(luv::TickMsg{'A', 0, sym, 0, 0, static_cast<int64_t>(1000000), 100, 1, 0});
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Insert/Remove Level Time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << " us" << std::endl;
    return 0;
}
