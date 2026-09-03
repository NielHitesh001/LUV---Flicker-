#pragma once

// LUV AI integration.
//
// The hot path stays dependency-free: model bytes are mapped into Arena::ai_region,
// inference reads FeatureRow data in-place, and outputs land in SignalOutput.
// A Treelite/TL2cgen shared object can be bound through a small C predict ABI,
// while tests and offline tooling can use the built-in linear model ABI below.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "luv_arena.hpp"

#if defined(LUV_ENABLE_DYNAMIC_MODEL_LOADING) && \
    (defined(__unix__) || defined(__APPLE__))
#include <dlfcn.h>
#endif

namespace luv {

namespace ai {

inline constexpr uint64_t kModelMagic = 0x3156414C49414C55ULL; // "LUVAIAV1" LE
inline constexpr uint32_t kModelVersion = 1;

enum class ModelKind : uint32_t {
    kUnknown = 0,
    kLinear = 1,
    kTreeliteSharedObject = 2,
};

// File layout for the dependency-free test/fixture model:
//   ModelHeader
//   float weights[input_floats]
//
// score = bias + dot(weights, FeatureRow::data)
struct alignas(kCacheLine) ModelHeader {
    uint64_t magic = kModelMagic;
    uint32_t version = kModelVersion;
    uint32_t kind = static_cast<uint32_t>(ModelKind::kLinear);
    uint32_t model_id = 0;
    uint32_t input_floats = FeatureRow::kFloats;
    uint32_t output_count = 1;
    uint32_t payload_offset = sizeof(ModelHeader);
    uint64_t payload_bytes = 0;
    float bias = 0.0f;
    uint8_t reserved[20] = {};
};
static_assert(sizeof(ModelHeader) == kCacheLine, "ModelHeader must be 64 bytes");

}  // namespace ai

class AIEngine {
public:
    using PredictFn = int (*)(const float* features,
                              uint32_t feature_count,
                              float* out_score);

    AIEngine() = default;
    AIEngine(const AIEngine&) = delete;
    AIEngine& operator=(const AIEngine&) = delete;

    ~AIEngine() { close_library(); }

    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;
        _arena = &arena;
        return true;
    }

    // Maps a LUV model ABI file directly over arena.ai_region.
    [[nodiscard]] bool load_model_file(const char* path) noexcept {
        if (!_arena || !path) return false;

        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return false;
        }

        const size_t file_size = static_cast<size_t>(st.st_size);
        if (file_size < sizeof(ai::ModelHeader) ||
            file_size > _arena->ai_region_size) {
            ::close(fd);
            return false;
        }

        void* mapped = ::mmap(
            _arena->ai_region,
            file_size,
            PROT_READ,
            MAP_PRIVATE | MAP_FIXED,
            fd,
            0
        );
        ::close(fd);

        if (mapped == MAP_FAILED || mapped != _arena->ai_region) {
            return false;
        }

        _model_base = static_cast<const uint8_t*>(mapped);
        _model_size = file_size;
        _header = reinterpret_cast<const ai::ModelHeader*>(_model_base);
        _kind = static_cast<ai::ModelKind>(_header->kind);

        if (!validate_header()) {
            _model_base = nullptr;
            _model_size = 0;
            _header = nullptr;
            _kind = ai::ModelKind::kUnknown;
            return false;
        }

        return true;
    }

    // Treelite/TL2cgen integration hook.
    //
    // The shared object is still mapped into ai_region for accounting/pinning
    // discipline, then dlopen() binds a small exported predict symbol:
    //   extern "C" int luv_treelite_predict(const float*, uint32_t, float*);
    [[nodiscard]] bool load_treelite_shared_object(
        const char* path,
        const char* predict_symbol = "luv_treelite_predict") noexcept
    {
#if defined(LUV_ENABLE_DYNAMIC_MODEL_LOADING) && \
    (defined(__unix__) || defined(__APPLE__))
        if (!_arena || !path || !predict_symbol) return false;

        if (!map_artifact_bytes(path)) return false;

        close_library();
        _library = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!_library) return false;

        void* sym = ::dlsym(_library, predict_symbol);
        if (!sym) {
            close_library();
            return false;
        }

        _predict = reinterpret_cast<PredictFn>(sym);
        _kind = ai::ModelKind::kTreeliteSharedObject;
        _model_id = 1;
        return true;
#else
        (void)path;
        (void)predict_symbol;
        return false;
#endif
    }

    void bind_predict_fn(PredictFn fn, uint8_t model_id = 1) noexcept {
        _predict = fn;
        _kind = ai::ModelKind::kTreeliteSharedObject;
        _model_id = model_id;
    }

    [[nodiscard]] bool infer_symbol(uint16_t sym) noexcept {
        if (!_arena || sym >= Config::kSymbols) [[unlikely]] return false;

        const uint64_t start = now_ns();
        const FeatureRow& features = _arena->feature_rows[sym];

        float score = 0.0f;
        if (_kind == ai::ModelKind::kLinear) {
            if (!_header) return false;
            score = infer_linear(features);
        } else if (_kind == ai::ModelKind::kTreeliteSharedObject) {
            if (!_predict) return false;
            if (_predict(features.data, FeatureRow::kFloats, &score) != 0)
                return false;
        } else {
            return false;
        }

        write_signal(sym, score);
        _last_inference_us =
            static_cast<float>(now_ns() - start) / 1000.0f;
        ++_inference_count;
        return true;
    }

    [[nodiscard]] uint32_t infer_all() noexcept {
        uint32_t n = 0;
        for (uint16_t sym = 0; sym < Config::kSymbols; ++sym) {
            if (infer_symbol(sym)) ++n;
        }
        return n;
    }

    [[nodiscard]] float last_inference_us() const noexcept {
        return _last_inference_us;
    }

    [[nodiscard]] uint64_t inference_count() const noexcept {
        return _inference_count;
    }

    [[nodiscard]] ai::ModelKind model_kind() const noexcept {
        return _kind;
    }

    [[nodiscard]] uint8_t model_id() const noexcept {
        return _model_id;
    }

    [[nodiscard]] const void* model_base() const noexcept {
        return _model_base;
    }

    [[nodiscard]] size_t model_size() const noexcept {
        return _model_size;
    }

private:
    [[nodiscard]] bool validate_header() noexcept {
        if (!_header) return false;
        if (_header->magic != ai::kModelMagic) return false;
        if (_header->version != ai::kModelVersion) return false;
        if (_kind != ai::ModelKind::kLinear) return false;
        if (_header->input_floats == 0 ||
            _header->input_floats > FeatureRow::kFloats) return false;
        if (_header->output_count != 1) return false;

        const size_t payload_offset = _header->payload_offset;
        const size_t payload_bytes = static_cast<size_t>(_header->payload_bytes);
        const size_t needed =
            static_cast<size_t>(_header->input_floats) * sizeof(float);

        if (payload_offset < sizeof(ai::ModelHeader)) return false;
        if (payload_bytes < needed) return false;
        if (payload_offset > _model_size) return false;
        if (payload_bytes > _model_size - payload_offset) return false;

        _weights = reinterpret_cast<const float*>(_model_base + payload_offset);
        _model_id = static_cast<uint8_t>(
            std::min<uint32_t>(_header->model_id, 255u));
        return true;
    }

    [[nodiscard]] bool map_artifact_bytes(const char* path) noexcept {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return false;
        }

        const size_t file_size = static_cast<size_t>(st.st_size);
        if (file_size == 0 || file_size > _arena->ai_region_size) {
            ::close(fd);
            return false;
        }

        void* mapped = ::mmap(
            _arena->ai_region,
            file_size,
            PROT_READ,
            MAP_PRIVATE | MAP_FIXED,
            fd,
            0
        );
        ::close(fd);

        if (mapped == MAP_FAILED || mapped != _arena->ai_region)
            return false;

        _model_base = static_cast<const uint8_t*>(mapped);
        _model_size = file_size;
        _header = nullptr;
        _weights = nullptr;
        return true;
    }

    [[nodiscard]] float infer_linear(const FeatureRow& row) const noexcept {
        float score = _header->bias;
        const uint32_t n = _header->input_floats;
        for (uint32_t i = 0; i < n; ++i) {
            score += _weights[i] * row.data[i];
        }
        return score;
    }

    void write_signal(uint16_t sym, float score) noexcept {
        SignalOutput& out = _arena->signal_slots[sym];
        out.expected_move = score;
        out.direction = (score > 0.0f) ? 1 : ((score < 0.0f) ? -1 : 0);
        out.confidence = confidence_from_score(score);
        out.model_id = _model_id;
        out.stale_count = 0;
    }

    [[nodiscard]] static float confidence_from_score(float score) noexcept {
        const float mag = std::min(std::fabs(score), 20.0f);
        return 1.0f / (1.0f + std::exp(-mag));
    }

    [[nodiscard]] static uint64_t now_ns() noexcept {
        struct timespec ts {};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }

    void close_library() noexcept {
#if defined(LUV_ENABLE_DYNAMIC_MODEL_LOADING) && \
    (defined(__unix__) || defined(__APPLE__))
        if (_library) {
            ::dlclose(_library);
            _library = nullptr;
        }
#endif
        _predict = nullptr;
    }

    Arena* _arena = nullptr;
    const uint8_t* _model_base = nullptr;
    size_t _model_size = 0;
    const ai::ModelHeader* _header = nullptr;
    const float* _weights = nullptr;
    ai::ModelKind _kind = ai::ModelKind::kUnknown;
    uint8_t _model_id = 0;
    PredictFn _predict = nullptr;
    float _last_inference_us = 0.0f;
    uint64_t _inference_count = 0;

#if defined(LUV_ENABLE_DYNAMIC_MODEL_LOADING) && \
    (defined(__unix__) || defined(__APPLE__))
    void* _library = nullptr;
#endif
};

}  // namespace luv
