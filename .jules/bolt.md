## 2024-05-24 - [Algorithmic improvement for find_level]
**Learning:** The previous implementation of `LOBEngine::find_level` used a linear scan, assuming that books are shallow. However, order books can become very deep, rendering linear search a performance bottleneck `O(n)`. The array is strictly sorted (bids descending, asks ascending).
**Action:** Replaced linear scan in `find_level` with a binary search, reducing the time complexity to `O(log n)`. Tested to confirm binary search correctly matches logic based on `side`.

## 2024-05-24 - [Branchless Risk Validation]
**Learning:** Evaluated the existing branchless pre-trade risk checks implementation (`luv_execution.hpp`). Verified its robust performance using boolean masking (instead of conditional jumps). Branchless operations are critical on hot paths where CPU pipeline stalls result in massive performance penalties (branch misprediction costs 10-20 cycles).
**Action:** The logic to validate Fat-Finger limits, Max Position caps, Stale Alpha checks, and Trading Halts is successfully achieved via bitwise math yielding `pass/reject_mask` cleanly without triggering CPU branches. The timing achieved in a standalone test was ~2.18 ns, fully satisfying the < 10 ns strict objective.
