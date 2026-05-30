## 2024-05-24 - [Algorithmic improvement for find_level]
**Learning:** The previous implementation of `LOBEngine::find_level` used a linear scan, assuming that books are shallow. However, order books can become very deep, rendering linear search a performance bottleneck `O(n)`. The array is strictly sorted (bids descending, asks ascending).
**Action:** Replaced linear scan in `find_level` with a binary search, reducing the time complexity to `O(log n)`. Tested to confirm binary search correctly matches logic based on `side`.

## 2024-05-24 - [Branchless Risk Validation]
**Learning:** Evaluated the existing branchless pre-trade risk checks implementation (`luv_execution.hpp`). Verified its robust performance using boolean masking (instead of conditional jumps). Branchless operations are critical on hot paths where CPU pipeline stalls result in massive performance penalties (branch misprediction costs 10-20 cycles).
**Action:** The logic to validate Fat-Finger limits, Max Position caps, Stale Alpha checks, and Trading Halts is successfully achieved via bitwise math yielding `pass/reject_mask` cleanly without triggering CPU branches. The timing achieved in a standalone test was ~2.18 ns, fully satisfying the < 10 ns strict objective.

## 2024-05-24 - [Zero-Allocation OUCH Message Templating]
**Learning:** Writing fields byte-by-byte via manual offset pointer arithmetic inside `build_enter_order` (the outbound hot-path network templating) involves multiple shifts and scalar writes which the compiler cannot fully optimize.
**Action:** Implemented a strictly packed C++ struct (`#pragma pack(push, 1)`) representing the Nasdaq OUCH 4.2 "Enter Order" message (`EnterOrderMsg`). Used a pointer overlay (`reinterpret_cast<ouch::EnterOrderMsg*>`) instead of offset arithmetic to directly assign the modified attributes (order token, shares, and price). We kept `memcpy` from the pre-initialized template to load all constant string bits, then just assigned dynamic fields. The execution timing of the whole injection was ~5.9 ns.
