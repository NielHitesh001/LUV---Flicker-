## 2024-05-24 - [Algorithmic improvement for find_level]
**Learning:** The previous implementation of `LOBEngine::find_level` used a linear scan, assuming that books are shallow. However, order books can become very deep, rendering linear search a performance bottleneck `O(n)`. The array is strictly sorted (bids descending, asks ascending).
**Action:** Replaced linear scan in `find_level` with a binary search, reducing the time complexity to `O(log n)`. Tested to confirm binary search correctly matches logic based on `side`.
