# LUV Quant Lab

This directory contains the Python scripts to generate training data, train an XGBoost model, and compile it via Treelite to a shared object (`luv_model.so`) which the C++ LUV engine dynamically loads.

## 1. Setup

```bash
pip install -r requirements.txt
```

**Target platform:** Linux x86_64. The `.so` generation uses the local `gcc` toolchain.

## 2. Generate Features

A helper program `dump_features.cpp` is used to hook into the `SimFeedSource` and `Consumer` and dump binary floating-point rows (20 features + 1 label) directly to stdout.

Compile the dumper:
```bash
g++ -std=c++20 -O3 -Wall dump_features.cpp -o dump_features
```

Run the generator:
```bash
./dump_features | python3 quant_lab/generate_features.py --out quant_lab/features.parquet
```

*Expected output:*
```
Read X rows from stdin.
Saved Y non-zero-label rows to quant_lab/features.parquet.
```

## 3. Train the Model

Train an XGBoost binary classifier (shallow trees, `max_depth=4` for latency) on the generated features:
```bash
python3 quant_lab/train_model.py
```

*Expected output:*
```
Loaded Y rows from features.parquet
Train AUC: 1.0000
Validation AUC: 1.0000
Validation Accuracy: 1.0000
Top 5 features by weight: ...
Model saved to quant_lab/model.json
```
Note: The synthetic feature data creates perfect scores in test.

## 4. Compile to Shared Object

Using `tl2cgen`, we compile the model into a `.so` library:
```bash
python3 quant_lab/compile_model.py
gcc -shared -fPIC -O2 quant_lab/wrapper.c -Wl,-rpath,'$ORIGIN' -Lquant_lab -l:luv_model_raw.so -o quant_lab/luv_model.so
```

The C compiler command wraps the standard Treelite interface with a `luv_treelite_predict` compatible symbol.

## 5. Validate Model

Verify the C-ABI is exposed exactly as the C++ code expects:
```bash
python3 quant_lab/validate_model.py
```

*Expected output:*
```
[OK] luv_treelite_predict ABI verified
     zero-feature score : 0.7526
```

## C++ Integration Note

The compiled Treelite model expects an input of **20 features** (one per column), but the `FeatureRow::data` buffer in C++ is interleaved (feature-major, 1280 floats).

Therefore, we have modified the `AIEngine::infer_symbol(uint16_t sym, uint32_t cursor = 0)` code inside `luv_ai.hpp` so that the C++ hot-path extracts a `float scratch[20]` containing the most recent state and passes that cleanly to the compiled model.

## Limitations

- The underlying data is fully synthetic and driven by PRNG. Consequently, accuracy results will not map to live trading conditions.
- The `quant_lab` environment is purely for offline model building. No Python routines execute on the engine hot path.
