#include "convert/DngConverter.h"

#include "convert/BayerResample.h"
#include "convert/NaturalBin.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <cmath>
#include <memory>
#include <thread>

namespace rawrcam::convert {
namespace {

constexpr uint32_t kRecompressTile = 1024;  // even, Bayer-safe
constexpr uint32_t kDownscaleTile = 256;    // output rows produced per pass through the stages

struct Cancelled {};

struct Source {
    tinydng_context* ctx = nullptr;
    tinydng_document* doc = nullptr;
    const tinydng_image_info* info = nullptr;
    size_t index = 0;

    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source() {
        if (doc) tinydng_document_destroy(ctx, doc);
        if (ctx) tinydng_context_destroy(ctx);
    }

    bool open(const std::string& path, std::string* error) {
        tinydng_error err{};
        tinydng_config config{};
        config.memory_cap_bytes = size_t(512) << 20;
        config.max_images = 64;
        config.max_image_pixels = 400000000ull;
        ctx = tinydng_context_create(&config, &err);
        if (!ctx) return fail(error, err.message);
        tinydng_open_options options{};
        options.flags = TINYDNG_OPEN_PARSE_SUBIFDS;
        if (tinydng_open_file(ctx, path.c_str(), &options, &doc, &err) != TINYDNG_OK) return fail(error, err.message);
        // The largest CFA image is the RAW; previews and thumbnails are not.
        for (size_t i = 0; i < tinydng_image_count(doc); ++i) {
            const auto* image = tinydng_image_get(doc, i);
            if (!image->cfa.present) continue;
            if (!info || uint64_t(image->width) * image->height > uint64_t(info->width) * info->height) {
                info = image;
                index = i;
            }
        }
        if (!info) return fail(error, "No Bayer RAW image in this DNG");
        if (info->samples_per_pixel != 1 || (info->sample_format != 0 && info->sample_format != 1) ||
            info->cfa.pattern_dim[0] != 2 || info->cfa.pattern_dim[1] != 2 || info->bits_per_sample < 8 ||
            info->bits_per_sample > 16 || (info->compression != 1 && info->compression != 7))
            return fail(error, "Only 8-16 bit Bayer DNGs, uncompressed or lossless JPEG, can be converted");
        if (info->width < 16 || info->height < 16 || (info->width & 1u) || (info->height & 1u))
            return fail(error, "DNG dimensions must be even and at least 16");
        return true;
    }

    // Image EXIF, or the document's when the RAW lives in a SubIFD.
    tinydng_exif exif(const ConvertOptions& options) const {
        tinydng_exif out = info->exif;
        const tinydng_exif* root = tinydng_document_exif(doc);
        if (root && !out.make && !out.model && !out.datetime_original) out = *root;
        if (!out.orientation && root && root->orientation >= 1 && root->orientation <= 8)
            out.orientation = root->orientation;
        if (!out.orientation) out.orientation = 1;
        if (!options.offsetTime.empty() && out.datetime_original && !out.offset_time_original) {
            char* offset = const_cast<char*>(options.offsetTime.c_str());
            out.offset_time_original = offset;
            if (!out.offset_time) out.offset_time = offset;
            if (out.datetime_digitized && !out.offset_time_digitized) out.offset_time_digitized = offset;
        }
        return out;
    }

    static bool fail(std::string* error, const char* message) {
        if (error) *error = message && *message ? message : "Cannot read DNG";
        return false;
    }
};

uint32_t readBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void writeBe32(uint8_t* p, uint32_t value) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

// Black levels expanded to a 2x2 pattern anchored at the ActiveArea origin
// (DNG's own anchoring), index 2*row+col.
bool blackByPhase(const tinydng_raw_info& raw, double out[4], std::string* error) {
    uint32_t rows = 1, cols = 1;
    if (raw.has_black_level_repeat) {
        rows = raw.black_level_repeat[0];
        cols = raw.black_level_repeat[1];
    } else if (raw.black_level_present && raw.black_level_exact[1] != raw.black_level_exact[0]) {
        rows = cols = 2;
    }
    if (rows < 1 || rows > 2 || cols < 1 || cols > 2) {
        if (error) *error = "BlackLevel must repeat over 1x1, 1x2, 2x1 or 2x2";
        return false;
    }
    for (uint32_t i = 0; i < 4; ++i)
        out[i] = raw.black_level_present ? raw.black_level_exact[(i / 2 % rows) * cols + i % 2 % cols] : 0.0;
    return true;
}

// Bayer colors (R=0, G=1, B=2) in buffer phase order.
bool bayerColors(const tinydng_cfa& cfa, uint8_t out[4]) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t plane = cfa.pattern[i];
        const uint8_t color = cfa.plane_color_count >= 3 && plane < cfa.plane_color_count ? cfa.plane_color[plane] : plane;
        if (color > 2) return false;
        out[i] = color;
    }
    return true;
}

void ensureExactBlack(tinydng_raw_info& raw, const double phase[4]) {
    for (int i = 0; i < 4; ++i) {
        raw.black_level_exact[i] = phase[i];
        raw.black_level[i] = int32_t(phase[i]);
    }
    raw.black_level_present = 1;
    raw.has_black_level_exact = 1;
    raw.black_level_repeat[0] = raw.black_level_repeat[1] = 2;
    raw.has_black_level_repeat = 1;
}

struct Writer {
    tinydng_write_io sink{};
    tinydng_writer* writer = nullptr;
    bool open = false;

    ~Writer() {
        if (open) sink.close(&sink);
    }

    bool create(tinydng_context* ctx, const std::string& path, const tinydng_write_image& image,
                uint8_t predictor, uint32_t tile, std::string* error) {
        tinydng_error err{};
        if (tinydng_write_io_open_file(ctx, path.c_str(), &sink, &err) != TINYDNG_OK)
            return Source::fail(error, err.message);
        open = true;
        tinydng_write_options options{};
        options.as_dng = 1;
        options.compression = 7;
        options.ljpeg_predictor = predictor;
        tinydng_tiling tiling{};
        tiling.tile_width = tiling.tile_length = tile;
        if (tinydng_writer_create(ctx, sink, &image, &options, &tiling, &writer, &err) != TINYDNG_OK)
            return Source::fail(error, err.message);
        return true;
    }

    bool tile(uint32_t index, const uint16_t* data, std::string* error) {
        tinydng_error err{};
        if (tinydng_writer_write_tile(writer, index, data, &err) != TINYDNG_OK) return Source::fail(error, err.message);
        return true;
    }

    bool finish(std::string* error) {
        tinydng_error err{};
        const bool ok = tinydng_writer_finish(writer, &err) == TINYDNG_OK;
        writer = nullptr;
        if (!ok) return Source::fail(error, err.message);
        return true;
    }
};

// All LJPEG predictors are lossless; keep the one that compresses a central
// sample best. Falls back to 1.
uint8_t choosePredictor(tinydng_context* ctx, const uint16_t* sample, uint32_t width, uint32_t height) {
    uint8_t best = 1;
    size_t bestSize = 0;
    for (uint8_t predictor = 1; predictor <= 7; ++predictor) {
        tinydng_error err{};
        tinydng_write_io sink{};
        if (tinydng_write_io_open_memory(ctx, &sink, &err) != TINYDNG_OK) continue;
        tinydng_write_image image{};
        image.width = width;
        image.height = height;
        image.samples_per_pixel = 1;
        image.bits_per_sample = 16;
        image.sample_format = 1;
        tinydng_write_options options{};
        options.compression = 7;
        options.ljpeg_predictor = predictor;
        tinydng_tiling tiling{};
        tiling.tile_width = width;
        tiling.tile_length = height;
        tinydng_writer* writer = nullptr;
        size_t size = 0;
        if (tinydng_writer_create(ctx, sink, &image, &options, &tiling, &writer, &err) == TINYDNG_OK &&
            tinydng_writer_write_tile(writer, 0, sample, &err) == TINYDNG_OK &&
            tinydng_writer_finish(writer, &err) == TINYDNG_OK) {
            uint8_t* blob = nullptr;
            if (tinydng_write_io_memory_take(ctx, &sink, &blob, &size, &err) == TINYDNG_OK)
                tinydng_buffer_free(ctx, blob);
        }
        sink.close(&sink);
        if (size && (!bestSize || size < bestSize)) {
            bestSize = size;
            best = predictor;
        }
    }
    return best;
}

// Decodes [x, y, w, h] as 16-bit codes.
bool decode(const Source& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint16_t>* out,
            std::string* error) {
    tinydng_error err{};
    tinydng_decode_options options{};
    options.num_threads = 1;
    tinydng_pixels pixels{};
    if (tinydng_decode_region(source.ctx, source.doc, source.index, x, y, w, h, &options, &pixels, &err) !=
        TINYDNG_OK)
        return Source::fail(error, err.message);
    bool ok = pixels.width == w && pixels.height == h && pixels.samples_per_pixel == 1;
    if (ok) {
        out->resize(size_t(w) * h);
        if (pixels.bits_per_sample == 16)
            std::memcpy(out->data(), pixels.data, out->size() * 2);
        else if (pixels.bits_per_sample == 8)
            for (size_t i = 0; i < out->size(); ++i) (*out)[i] = pixels.data[i];
        else
            ok = false;
    }
    tinydng_pixels_free(source.ctx, &pixels);
    if (!ok) return Source::fail(error, "Unexpected decoded DNG layout");
    return true;
}

ConvertResult finished(ConvertResult result, const std::string& destination) {
    if (!result.ok) std::remove(destination.c_str());
    return result;
}


// Moves a 2x2 pattern anchored at the ActiveArea origin (top, left) to the
// 2x2 buffer phase (index 2*(y&1)+(x&1)), or back with the same call.
template <class T>
void reanchor(const T in[4], T out[4], uint32_t top, uint32_t left) {
    for (uint32_t r = 0; r < 2; ++r)
        for (uint32_t c = 0; c < 2; ++c) out[r * 2 + c] = in[((r + top) & 1u) * 2 + ((c + left) & 1u)];
}

// Pull-based row producers: each stage returns full-width rows of its own
// output, asking its input for just the rows it needs.
class Stage {
   public:
    virtual ~Stage() = default;
    uint32_t width = 0, height = 0;
    virtual void rows(uint32_t y0, uint32_t y1, std::vector<uint16_t>* out) = 0;
};

struct StageError {
    std::string message;
};

class SourceStage final : public Stage {
   public:
    explicit SourceStage(const Source& source) : source_(source) {
        width = source.info->width;
        height = source.info->height;
    }
    void rows(uint32_t y0, uint32_t y1, std::vector<uint16_t>* out) override {
        std::string error;
        if (!decode(source_, 0, y0, width, y1 - y0, out, &error)) throw StageError{error};
    }

   private:
    const Source& source_;
};

template <class Check>
void parallelRows(uint32_t rows, uint32_t step, Check& check, const std::function<void(uint32_t, uint32_t)>& body) {
    const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 6u);
    std::vector<std::exception_ptr> errors(workers);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workers; ++t)
        threads.emplace_back([&, t] {
            try {
                for (uint32_t y = t * step; y < rows && !failed; y += workers * step) {
                    check();
                    body(t, y);
                }
            } catch (...) {
                failed = true;
                errors[t] = std::current_exception();
            }
        });
    for (auto& thread : threads) thread.join();
    for (auto& e : errors)
        if (e) std::rethrow_exception(e);
}

class BinStage final : public Stage {
   public:
    BinStage(std::unique_ptr<Stage> input, const BinCalibration& calibration, BinQuantize quantize,
             std::function<void()> check)
        : input_(std::move(input)), check_(std::move(check)) {
        std::string error;
        if (!probe_.configure(calibration, quantize, input_->width, input_->height, &error)) throw StageError{error};
        for (unsigned i = 0; i < 6; ++i) {
            bins_.push_back(std::make_unique<NaturalBin2x>());
            bins_.back()->configure(calibration, quantize, input_->width, input_->height, nullptr);
        }
        width = probe_.outputWidth();
        height = probe_.outputHeight();
    }
    void rows(uint32_t y0, uint32_t y1, std::vector<uint16_t>* out) override {
        uint32_t top, bottom;
        probe_.sourceRows(y0, y1 - y0, &top, &bottom);
        input_->rows(top, bottom, &in_);
        out->resize(size_t(width) * (y1 - y0));
        const uint32_t iw = input_->width;
        parallelRows(y1 - y0, NaturalBin2x::kBandRows, check_, [&](uint32_t t, uint32_t y) {
            const uint32_t n = std::min(NaturalBin2x::kBandRows, y1 - y0 - y);
            bins_[t]->band(y0 + y, n, [&](uint32_t x, uint32_t sy) { return in_[size_t(sy - top) * iw + x]; },
                           check_, out->data() + size_t(y) * width, width);
        });
    }

   private:
    std::unique_ptr<Stage> input_;
    std::function<void()> check_;
    NaturalBin2x probe_;
    std::vector<std::unique_ptr<NaturalBin2x>> bins_;
    std::vector<uint16_t> in_;
};

class ResampleStage final : public Stage {
   public:
    ResampleStage(std::unique_ptr<Stage> input, uint32_t outWidth, uint32_t outHeight, uint16_t maxCode,
                  std::function<void()> check)
        : input_(std::move(input)), check_(std::move(check)) {
        if (!resample_.configure(input_->width, input_->height, outWidth, outHeight, maxCode))
            throw StageError{"Cannot resample to the requested size"};
        width = outWidth;
        height = outHeight;
    }
    void rows(uint32_t y0, uint32_t y1, std::vector<uint16_t>* out) override {
        uint32_t top, bottom;
        resample_.sourceRows(y0, y1 - y0, &top, &bottom);
        input_->rows(top, bottom, &in_);
        out->resize(size_t(width) * (y1 - y0));
        const uint32_t iw = input_->width;
        parallelRows(y1 - y0, 8, check_, [&](uint32_t, uint32_t y) {
            const uint32_t n = std::min(8u, y1 - y0 - y);
            resample_.band(y0 + y, n, [&](uint32_t x, uint32_t sy) { return double(in_[size_t(sy - top) * iw + x]); },
                           out->data() + size_t(y) * width, width);
        });
    }

   private:
    std::unique_ptr<Stage> input_;
    std::function<void()> check_;
    BayerResample resample_;
    std::vector<uint16_t> in_;
};

}  // namespace

bool BinnedRaw::build(const tinydng_raw_info& source, const tinydng_cfa& cfa, uint32_t sourceWidth,
                      uint32_t sourceHeight, uint32_t outputWidth, uint32_t outputHeight, uint16_t gain,
                      std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (source.linearization_table_count) return fail("DNGs with a LinearizationTable cannot be downscaled");
    if (!source.white_level_present) return fail("Downscaling needs a WhiteLevel");
    const bool half = outputWidth * 2 == sourceWidth && outputHeight * 2 == sourceHeight;
    const double fx = double(outputWidth) / sourceWidth, fy = double(outputHeight) / sourceHeight;
    raw_ = source;
    cfa_ = cfa;
    uint32_t active[4] = {0, 0, sourceHeight, sourceWidth};
    if (source.has_active_area) std::copy(source.active_area, source.active_area + 4, active);
    if (active[0] >= active[2] || active[1] >= active[3] || active[2] > sourceHeight || active[3] > sourceWidth)
        return fail("Invalid ActiveArea");
    if (half && ((active[0] | active[1] | active[2] | active[3]) & 1u))
        return fail("Binning needs an ActiveArea with even coordinates");
    // Pixel boundaries scale with the image; exact for 2x, nearest otherwise.
    uint32_t scaled[4];
    for (int i = 0; i < 4; ++i)
        scaled[i] = uint32_t(std::lround(active[i] * (i % 2 ? fx : fy)));
    scaled[2] = std::min(scaled[2], outputHeight);
    scaled[3] = std::min(scaled[3], outputWidth);
    if (scaled[0] >= scaled[2] || scaled[1] >= scaled[3]) return fail("ActiveArea vanishes when downscaled");
    if (raw_.has_active_area) std::copy(scaled, scaled + 4, raw_.active_area);
    if (raw_.has_crop) {
        raw_.crop_origin[0] *= fx;
        raw_.crop_origin[1] *= fy;
        raw_.crop_size[0] *= fx;
        raw_.crop_size[1] *= fy;
    }
    // CFA, black levels and GainMap phases are anchored to the ActiveArea
    // origin; the buffer phase is preserved, so a change in origin parity
    // moves the anchored 2x2 patterns.
    const uint32_t shiftY = (active[0] ^ scaled[0]) & 1u, shiftX = (active[1] ^ scaled[1]) & 1u;
    double black[4];
    if (!blackByPhase(source, black, error)) return false;
    double anchored[4];
    for (uint32_t r = 0; r < 2; ++r)
        for (uint32_t c = 0; c < 2; ++c) {
            anchored[r * 2 + c] = black[((r + shiftY) & 1u) * 2 + ((c + shiftX) & 1u)] * gain;
            cfa_.pattern[r * 2 + c] = cfa.pattern[((r + shiftY) & 1u) * 2 + ((c + shiftX) & 1u)];
        }
    ensureExactBlack(raw_, anchored);
    raw_.white_level[0] = source.white_level[0] * gain;
    // NoiseProfile describes the source pixels, not the edge-adaptive output;
    // readers estimate noise when it is absent.
    raw_.noise_profile_count = 0;

    // Opcodes: four-phase GainMaps cover the new active area; normalized
    // lens corrections stay valid; anything else is refused.
    const uint32_t activeH = active[2] - active[0], activeW = active[3] - active[1];
    const uint32_t newH = scaled[2] - scaled[0], newW = scaled[3] - scaled[1];
    opcodes_.clear();
    params_.clear();
    opcodes_.reserve(source.opcode_count);
    params_.reserve(source.opcode_count);
    bool gainPhase[4]{};
    uint32_t gainMaps = 0;
    for (size_t i = 0; i < source.opcode_count; ++i) {
        const tinydng_opcode& in = source.opcodes[i];
        if (in.params_size && !in.params) return fail("DNG opcode parameters are unavailable");
        params_.emplace_back(in.params, in.params + in.params_size);
        auto& p = params_.back();
        if (in.id == 9u) {
            if (p.size() < 76u) return fail("Truncated GainMap opcode");
            const uint32_t top = readBe32(&p[0]), left = readBe32(&p[4]);
            const uint32_t bottom = readBe32(&p[8]), right = readBe32(&p[12]);
            const uint32_t plane = readBe32(&p[16]), planes = readBe32(&p[20]);
            const uint32_t rowPitch = readBe32(&p[24]), colPitch = readBe32(&p[28]);
            if (in.list != 2u || top > 1 || left > 1 || bottom != activeH || right != activeW || plane != 0 ||
                planes != 1 || rowPitch != 2 || colPitch != 2)
                return fail("This DNG's lens shading map cannot be downscaled");
            const uint32_t phase = top * 2 + left;
            if (gainPhase[phase]) return fail("Duplicate GainMap phase");
            gainPhase[phase] = true;
            ++gainMaps;
            writeBe32(&p[0], top ^ shiftY);
            writeBe32(&p[4], left ^ shiftX);
            writeBe32(&p[8], newH);
            writeBe32(&p[12], newW);
        } else if (in.id != 1u && in.id != 2u && in.id != 3u) {
            return fail("This DNG has a correction that cannot be downscaled");
        }
        tinydng_opcode out = in;
        out.params = p.empty() ? nullptr : p.data();
        opcodes_.push_back(out);
    }
    if (gainMaps && gainMaps != 4) return fail("Incomplete four-phase GainMap set");
    raw_.opcodes = opcodes_.empty() ? nullptr : opcodes_.data();
    raw_.opcode_count = opcodes_.size();
    // The writer emits generic opcodes; typed views still describe the source.
    raw_.gainmaps = nullptr;
    raw_.gainmap_count = 0;
    raw_.warps = nullptr;
    raw_.warp_count = 0;
    raw_.vignettes = nullptr;
    raw_.vignette_count = 0;
    return true;
}

ConvertResult recompressDng(const std::string& sourcePath, const std::string& destination,
                            const ConvertProgress& progress, const ConvertCancelled& cancelled,
                            const ConvertOptions& options) {
    ConvertResult result;
    Source source;
    if (!source.open(sourcePath, &result.error)) return result;
    const uint32_t w = source.info->width, h = source.info->height;
    tinydng_raw_info raw = source.info->raw;
    double black[4];
    if (!blackByPhase(raw, black, &result.error)) return result;
    if (raw.black_level_present) ensureExactBlack(raw, black);
    tinydng_exif exif = source.exif(options);
    tinydng_write_image image{};
    image.width = w;
    image.height = h;
    image.samples_per_pixel = 1;
    image.bits_per_sample = 16;
    image.sample_format = 1;
    image.cfa = &source.info->cfa;
    image.raw = &raw;
    image.exif = &exif;

    const uint32_t across = (w + kRecompressTile - 1) / kRecompressTile;
    const uint32_t down = (h + kRecompressTile - 1) / kRecompressTile;
    std::vector<uint16_t> tile;
    {
        const uint32_t sw = std::min(kRecompressTile, w) & ~1u, sh = std::min(kRecompressTile, h) & ~1u;
        if (!decode(source, ((w - sw) / 2) & ~1u, ((h - sh) / 2) & ~1u, sw, sh, &tile, &result.error))
            return result;
        const uint8_t predictor = choosePredictor(source.ctx, tile.data(), sw, sh);
        Writer writer;
        if (!writer.create(source.ctx, destination, image, predictor, kRecompressTile, &result.error))
            return finished(result, destination);
        for (uint32_t ty = 0; ty < down; ++ty) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = "Cancelled";
                return finished(result, destination);
            }
            for (uint32_t tx = 0; tx < across; ++tx) {
                const uint32_t x = tx * kRecompressTile, y = ty * kRecompressTile;
                const uint32_t tw = std::min(kRecompressTile, w - x), th = std::min(kRecompressTile, h - y);
                // Edge tiles are passed packed at their clipped size.
                if (!decode(source, x, y, tw, th, &tile, &result.error)) return finished(result, destination);
                if (!writer.tile(ty * across + tx, tile.data(), &result.error)) return finished(result, destination);
            }
            if (progress) progress(ty + 1, down);
        }
        if (!writer.finish(&result.error)) return finished(result, destination);
    }
    result.ok = true;
    result.width = w;
    result.height = h;
    return result;
}


namespace {

// Natural-bin calibration of an image described by raw/cfa (buffer phase).
bool calibrationOf(const tinydng_raw_info& raw, const tinydng_cfa& cfa, BinCalibration* out, std::string* error) {
    double anchoredBlack[4], black[4];
    uint8_t anchoredColors[4], colors[4];
    if (!blackByPhase(raw, anchoredBlack, error)) return false;
    if (!bayerColors(cfa, anchoredColors)) {
        if (error) *error = "Downscaling needs an RGB Bayer pattern";
        return false;
    }
    const uint32_t top = raw.has_active_area ? raw.active_area[0] : 0, left = raw.has_active_area ? raw.active_area[1] : 0;
    reanchor(anchoredBlack, black, top, left);
    reanchor(anchoredColors, colors, top, left);
    const int32_t white = raw.white_level_present ? raw.white_level[0] : 0;
    if (white <= 0 || white > 65535) {
        if (error) *error = "Invalid WhiteLevel";
        return false;
    }
    out->white = uint16_t(white);
    double neutral[3] = {1, 1, 1};
    if (raw.has_as_shot_neutral)
        for (int c = 0; c < 3; ++c) neutral[c] = raw.as_shot_neutral[c];
    for (int i = 0; i < 4; ++i) {
        out->black[i] = float(black[i]);
        out->cfa[i] = colors[i];
        out->wbGains[i] = neutral[colors[i]] > 0 ? float(neutral[1] / neutral[colors[i]]) : 0.f;
    }
    return true;
}

}  // namespace

ConvertResult downscaleDng(const std::string& sourcePath, const std::string& destination, DngDownscale scale,
                           const ConvertProgress& progress, const ConvertCancelled& cancelled,
                           const ConvertOptions& options) {
    ConvertResult result;
    Source source;
    if (!source.open(sourcePath, &result.error)) return result;
    const auto* info = source.info;
    std::atomic<bool> stop{false};
    std::function<void()> check = [&] {
        if (stop.load(std::memory_order_relaxed) || (cancelled && cancelled())) {
            stop = true;
            throw Cancelled{};
        }
    };

    // Stage chain and the matching metadata chain.
    std::unique_ptr<Stage> stage;
    std::vector<std::unique_ptr<BinnedRaw>> metadata;
    try {
        stage = std::make_unique<SourceStage>(source);
        const tinydng_raw_info* raw = &info->raw;
        const tinydng_cfa* cfa = &info->cfa;
        auto bin = [&] {
            BinCalibration calibration;
            if (!calibrationOf(*raw, *cfa, &calibration, &result.error)) throw StageError{result.error};
            const BinQuantize quantize = BinQuantize::forWhiteLevel(calibration.white);
            const uint32_t iw = stage->width, ih = stage->height;
            stage = std::make_unique<BinStage>(std::move(stage), calibration, quantize, check);
            metadata.push_back(std::make_unique<BinnedRaw>());
            if (!metadata.back()->build(*raw, *cfa, iw, ih, stage->width, stage->height, quantize.gain, &result.error))
                throw StageError{result.error};
            raw = &metadata.back()->raw();
            cfa = &metadata.back()->cfa();
        };
        bin();
        if (scale == DngDownscale::Sixteenth) bin();
        if (scale == DngDownscale::Eighth) {
            // Half the 2x-binned pixel count: sqrt(2) per side, even dimensions.
            const uint32_t iw = stage->width, ih = stage->height;
            const uint32_t ow = uint32_t(iw / std::sqrt(2.0)) & ~1u, oh = uint32_t(ih / std::sqrt(2.0)) & ~1u;
            const uint16_t maxCode = uint16_t(raw->white_level[0]);
            stage = std::make_unique<ResampleStage>(std::move(stage), ow, oh, maxCode, check);
            metadata.push_back(std::make_unique<BinnedRaw>());
            if (!metadata.back()->build(*raw, *cfa, iw, ih, ow, oh, 1, &result.error)) throw StageError{result.error};
            raw = &metadata.back()->raw();
            cfa = &metadata.back()->cfa();
        }
    } catch (const StageError& e) {
        result.error = e.message;
        return result;
    }

    const uint32_t ow = stage->width, oh = stage->height;
    tinydng_exif exif = source.exif(options);
    tinydng_write_image image{};
    image.width = ow;
    image.height = oh;
    image.samples_per_pixel = 1;
    image.bits_per_sample = 16;
    image.sample_format = 1;
    image.cfa = &metadata.back()->cfa();
    image.raw = &metadata.back()->raw();
    image.exif = &exif;

    const uint32_t across = (ow + kDownscaleTile - 1) / kDownscaleTile;
    const uint32_t down = (oh + kDownscaleTile - 1) / kDownscaleTile;
    std::vector<uint16_t> out, tile(size_t(kDownscaleTile) * kDownscaleTile);
    Writer writer;
    try {
        for (uint32_t ty = 0; ty < down; ++ty) {
            const uint32_t oy = ty * kDownscaleTile, rows = std::min(kDownscaleTile, oh - oy);
            stage->rows(oy, oy + rows, &out);
            if (ty == 0) {
                // Pick the predictor on the first band's centre, then open the output.
                const uint32_t pw = std::min(kDownscaleTile, ow) & ~1u, ph = rows & ~1u, px = ((ow - pw) / 2) & ~1u;
                std::vector<uint16_t> sample(size_t(pw) * ph);
                for (uint32_t r = 0; r < ph; ++r)
                    std::memcpy(&sample[size_t(r) * pw], &out[size_t(r) * ow + px], size_t(pw) * 2);
                const uint8_t predictor = choosePredictor(source.ctx, sample.data(), pw, ph);
                if (!writer.create(source.ctx, destination, image, predictor, kDownscaleTile, &result.error))
                    return finished(result, destination);
            }
            for (uint32_t tx = 0; tx < across; ++tx) {
                const uint32_t x = tx * kDownscaleTile, tw = std::min(kDownscaleTile, ow - x);
                // Edge tiles are passed packed at their clipped size.
                for (uint32_t r = 0; r < rows; ++r)
                    std::memcpy(&tile[size_t(r) * tw], &out[size_t(r) * ow + x], size_t(tw) * 2);
                if (!writer.tile(ty * across + tx, tile.data(), &result.error)) return finished(result, destination);
            }
            if (progress) progress(ty + 1, down);
        }
    } catch (const Cancelled&) {
        result.cancelled = true;
        result.error = "Cancelled";
        return finished(result, destination);
    } catch (const StageError& e) {
        result.error = e.message;
        return finished(result, destination);
    } catch (const std::exception& e) {
        result.error = e.what();
        return finished(result, destination);
    }
    if (!writer.finish(&result.error)) return finished(result, destination);
    result.ok = true;
    result.width = ow;
    result.height = oh;
    return result;
}

}  // namespace rawrcam::convert
