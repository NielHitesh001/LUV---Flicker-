#include <stdint.h>
#include <math.h>

extern void predict(const float* data, uint32_t num_row, float* output);

int luv_treelite_predict(const float* features, uint32_t feature_count, float* out_score) {
    float raw_score = 0.0f;
    predict(features, 1, &raw_score);
    // Treelite binary classifier may output raw margins. We apply sigmoid.
    *out_score = 1.0f / (1.0f + expf(-raw_score));
    return 0;
}
