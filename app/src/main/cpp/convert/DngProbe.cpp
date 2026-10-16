#include "convert/DngProbe.h"

#include <tinydng.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rawrcam::convert {
namespace {

struct Document {
    tinydng_context* ctx = nullptr;
    tinydng_document* doc = nullptr;
    const tinydng_image_info* raw = nullptr;
    size_t index = 0;

    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document() {
        if (doc) tinydng_document_destroy(ctx, doc);
        if (ctx) tinydng_context_destroy(ctx);
    }

    bool open(const std::string& path, bool metadataOnly, std::string* error) {
        tinydng_error err{};
        tinydng_config config{};
        config.memory_cap_bytes = size_t(256) << 20;
        config.max_images = 64;
        config.max_image_pixels = 400000000ull;
        ctx = tinydng_context_create(&config, &err);
        tinydng_open_options options{};
        options.flags = TINYDNG_OPEN_PARSE_SUBIFDS | (metadataOnly ? TINYDNG_OPEN_METADATA_ONLY : 0);
        if (!ctx || tinydng_open_file(ctx, path.c_str(), &options, &doc, &err) != TINYDNG_OK) {
            *error = err.message[0] ? err.message : "Cannot read DNG";
            return false;
        }
        for (size_t i = 0; i < tinydng_image_count(doc); ++i) {
            const auto* image = tinydng_image_get(doc, i);
            if (!image->cfa.present) continue;
            if (!raw || uint64_t(image->width) * image->height > uint64_t(raw->width) * raw->height) {
                raw = image;
                index = i;
            }
        }
        if (!raw) {
            *error = "No Bayer RAW image in this DNG";
            return false;
        }
        return true;
    }
};

bool isRawrProvenance(const tinydng_raw_info& r) {
    if (!r.rawr_private_data || r.rawr_private_size <= 31) return false;
    const char* marker = r.rawr_private_data;
    // "RawrCam ..." markers come from DNGs written before the rename.
    return std::strncmp(marker, "Rawr tinydng provenance", r.rawr_private_size) == 0 ||
           std::strncmp(marker, "RawrCam tinydng provenance", r.rawr_private_size) == 0 ||
           std::strncmp(marker, "RawrCam dng_writer provenance", r.rawr_private_size) == 0;
}

}  // namespace

DngInfo probeDng(const std::string& path) {
    DngInfo info;
    Document d;
    if (!d.open(path, true, &info.error)) return info;
    const auto* image = d.raw;
    info.ok = true;
    info.width = image->width;
    info.height = image->height;
    info.bitsPerSample = image->bits_per_sample;
    info.compression = image->compression;
    const tinydng_exif* root = tinydng_document_exif(d.doc);
    const tinydng_exif& exif = image->exif;
    info.orientation = exif.orientation ? exif.orientation : (root && root->orientation ? root->orientation : 1);
    if (info.orientation < 1 || info.orientation > 8) info.orientation = 1;
    const char* make = exif.make ? exif.make : (root ? root->make : nullptr);
    const char* model = exif.model ? exif.model : (root ? root->model : nullptr);
    info.make = make ? make : "";
    info.model = model ? model : "";
    info.rawr = isRawrProvenance(image->raw);
    const char* dto = exif.datetime_original ? exif.datetime_original : (root ? root->datetime_original : nullptr);
    const char* offset = exif.offset_time_original ? exif.offset_time_original : (root ? root->offset_time_original : nullptr);
    info.dateTimeOriginal = dto ? dto : "";
    info.offsetTimeOriginal = offset ? offset : "";
    info.convertible = image->samples_per_pixel == 1 && image->cfa.pattern_dim[0] == 2 &&
                       image->cfa.pattern_dim[1] == 2 && image->bits_per_sample >= 8 &&
                       image->bits_per_sample <= 16 && (image->compression == 1 || image->compression == 7) &&
                       image->width >= 16 && image->height >= 16;
    return info;
}

std::vector<uint8_t> thumbnailDng(const std::string& path, uint32_t maxSide, uint32_t* outWidth,
                                  uint32_t* outHeight) {
    std::vector<uint8_t> empty;
    std::string error;
    Document d;
    if (maxSide < 8 || maxSide > 1024 || !d.open(path, false, &error)) return empty;
    const auto* image = d.raw;
    const auto& raw = image->raw;
    const uint32_t W = image->width & ~1u, H = image->height & ~1u;
    if (W < 16 || H < 16 || image->samples_per_pixel != 1) return empty;
    const double scale = double(maxSide) / std::max(W, H);
    const uint32_t ow = std::max(1u, uint32_t(W * scale)), oh = std::max(1u, uint32_t(H * scale));

    // Per-site black (anchored at the ActiveArea origin) and colors.
    const uint32_t top = raw.has_active_area ? raw.active_area[0] : 0, left = raw.has_active_area ? raw.active_area[1] : 0;
    double black[4];
    int color[4];
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x) {
            const uint32_t a = ((y + top) & 1u) * 2 + ((x + left) & 1u);
            black[y * 2 + x] = raw.black_level_present ? raw.black_level_exact[a] : 0.0;
            const uint8_t plane = image->cfa.pattern[a];
            color[y * 2 + x] = image->cfa.plane_color_count >= 3 && plane < image->cfa.plane_color_count
                                   ? image->cfa.plane_color[plane]
                                   : plane;
            if (color[y * 2 + x] > 2) return empty;
        }
    double white = raw.white_level_present ? raw.white_level[0] : double((1u << image->bits_per_sample) - 1);
    double neutral[3] = {1, 1, 1};
    if (raw.has_as_shot_neutral)
        for (int c = 0; c < 3; ++c)
            if (raw.as_shot_neutral[c] > 0) neutral[c] = raw.as_shot_neutral[c];
    // Camera (white balanced) -> XYZ D50 -> linear sRGB; white balance only
    // when the DNG has no ForwardMatrix.
    double toSrgb[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (raw.has_forward_matrix1) {
        static const double xyzToSrgb[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415,
                                            0.0334540, 0.0719453,  -0.2289914, 1.4052427};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                double v = 0;
                for (int k = 0; k < 3; ++k) v += xyzToSrgb[r * 3 + k] * raw.forward_matrix1[k * 3 + c];
                toSrgb[r * 3 + c] = v;
            }
    }

    // Sample a sparse grid of 2x2 Bayer cells inside each output pixel. Bands
    // follow the source tiling so lossless JPEG tiles are decoded once.
    const uint32_t band = std::max(2u, image->tile_length & ~1u);
    std::vector<double> sum(size_t(ow) * oh * 3, 0.0);
    std::vector<uint32_t> count(size_t(ow) * oh, 0);
    const double cellW = double(W) / ow, cellH = double(H) / oh;
    const uint32_t stepX = std::max(2u, uint32_t(cellW / 4) & ~1u), stepY = std::max(2u, uint32_t(cellH / 4) & ~1u);
    tinydng_decode_options options{};
    options.num_threads = 1;
    // Untiled sources read just the sampled row pairs.
    const bool tiled = image->tile_length != 0;
    for (uint32_t y0 = 0; y0 < H; y0 += tiled ? band : stepY) {
        const uint32_t rows = tiled ? std::min(band, H - y0) : 2u;
        tinydng_pixels px{};
        tinydng_error err{};
        if (tinydng_decode_region(d.ctx, d.doc, d.index, 0, y0, W, rows, &options, &px, &err) != TINYDNG_OK)
            return empty;
        auto at = [&](uint32_t x, uint32_t y) -> double {
            const size_t i = size_t(y) * W + x;
            return px.bits_per_sample == 16 ? reinterpret_cast<const uint16_t*>(px.data)[i] : px.data[i];
        };
        for (uint32_t y = y0 & ~1u; y + 1 < y0 + rows; y += 2) {
            if (y % stepY) continue;
            const uint32_t oy = std::min(oh - 1, uint32_t(y / cellH));
            for (uint32_t x = 0; x + 1 < W; x += stepX) {
                const uint32_t ox = std::min(ow - 1, uint32_t(x / cellW));
                double rgb[3] = {0, 0, 0}, n[3] = {0, 0, 0};
                for (uint32_t s = 0; s < 4; ++s) {
                    const uint32_t sx = x + (s & 1u), sy = y + (s >> 1);
                    const double v = std::max(0.0, (at(sx, sy - y0) - black[s]) / (white - black[s]));
                    rgb[color[s]] += v;
                    n[color[s]] += 1;
                }
                double* dst = &sum[(size_t(oy) * ow + ox) * 3];
                for (int c = 0; c < 3; ++c) dst[c] += n[c] ? rgb[c] / n[c] / neutral[c] : 0.0;
                ++count[size_t(oy) * ow + ox];
            }
        }
        tinydng_pixels_free(d.ctx, &px);
    }
    // Simple auto exposure: average luminance to mid grey, gain within 1/2..8x.
    double luma = 0;
    size_t sampled = 0;
    for (size_t i = 0; i < size_t(ow) * oh; ++i)
        if (count[i]) {
            luma += (0.2126 * sum[i * 3] + 0.7152 * sum[i * 3 + 1] + 0.0722 * sum[i * 3 + 2]) / count[i];
            ++sampled;
        }
    const double gain = sampled && luma > 0 ? std::clamp(0.18 / (luma / sampled), 0.5, 8.0) : 1.0;
    std::vector<uint8_t> out(size_t(ow) * oh * 3);
    for (size_t i = 0; i < size_t(ow) * oh; ++i) {
        const double* cam = &sum[i * 3];
        const double k = count[i] ? gain / count[i] : 0.0;
        for (int r = 0; r < 3; ++r) {
            double v = (toSrgb[r * 3] * cam[0] + toSrgb[r * 3 + 1] * cam[1] + toSrgb[r * 3 + 2] * cam[2]) * k;
            v = std::clamp(v, 0.0, 1.0);
            v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            out[i * 3 + r] = uint8_t(v * 255 + 0.5);
        }
    }
    *outWidth = ow;
    *outHeight = oh;
    return out;
}

}  // namespace rawrcam::convert
