import sys
import struct
import numpy as np
import pandas as pd

FEATURE_NAMES = [
    "mid_delta_1",  "mid_delta_5",  "mid_delta_10", "mid_delta_30", "mid_delta_60",
    "imbal_1",      "imbal_3",      "imbal_5",      "imbal_10",     "imbal_20",
    "flow_1",       "flow_5",       "flow_10",       "flow_30",      "flow_60",
    "spread_raw",   "spread_ema_fast", "spread_ema_slow", "spread_max", "spread_min",
]

def main():
    FLOAT_SIZE = 4
    ROW_SIZE = 21 * FLOAT_SIZE

    rows = []

    # Read from stdin
    data = sys.stdin.buffer.read()

    num_rows = len(data) // ROW_SIZE

    if num_rows == 0:
        print("Error: No data read from stdin.")
        sys.exit(1)

    print(f"Read {num_rows} rows from stdin.")

    for i in range(num_rows):
        offset = i * ROW_SIZE
        row_data = struct.unpack('<21f' if sys.byteorder == 'big' else '21f', data[offset:offset+ROW_SIZE])

        # row_data[20] is label
        label = row_data[20]
        if label != 0.0:
            # Map -1 to 0 and 1 to 1 for xgboost binary classifier
            mapped_label = 1 if label > 0 else 0

            # Combine features + label
            features = list(row_data[:20])
            rows.append(features + [mapped_label])

    df = pd.DataFrame(rows, columns=FEATURE_NAMES + ["label"])

    # Output to parquet
    out_file = "quant_lab/features.parquet"
    if "--out" in sys.argv:
        idx = sys.argv.index("--out")
        if idx + 1 < len(sys.argv):
            out_file = sys.argv[idx + 1]

    df.to_parquet(out_file, index=False)
    print(f"Saved {len(df)} non-zero-label rows to {out_file}.")

if __name__ == "__main__":
    main()
