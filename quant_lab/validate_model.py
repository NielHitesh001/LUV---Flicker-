import ctypes
import numpy as np

lib = ctypes.CDLL("./quant_lab/luv_model.so")
lib.luv_treelite_predict.argtypes = [
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_float),
]
lib.luv_treelite_predict.restype = ctypes.c_int

# Test with all-zero features (baseline)
features = np.zeros(20, dtype=np.float32)
score = ctypes.c_float(0.0)
ret = lib.luv_treelite_predict(
    features.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
    ctypes.c_uint32(20),
    ctypes.byref(score),
)

assert ret == 0, f"predict returned error code {ret}"
assert 0.0 <= score.value <= 1.0, f"score {score.value} out of [0,1]"

# Test with extreme features (should not crash or produce NaN)
features_high = np.ones(20, dtype=np.float32) * 100.0
ret2 = lib.luv_treelite_predict(
    features_high.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
    ctypes.c_uint32(20),
    ctypes.byref(score),
)
assert ret2 == 0
assert not np.isnan(score.value)

print(f"[OK] luv_treelite_predict ABI verified")
print(f"     zero-feature score : {score.value:.4f}")
