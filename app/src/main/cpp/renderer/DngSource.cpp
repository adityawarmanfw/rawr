#include "DngSource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "BayerBin.h"
#include "NaturalBayerBin.h"
#include "support/JoinGuard.h"
#include "color/ColorCalibration.h"

namespace rawrcam::renderer {
namespace {
using Matrix = std::array<double, 9>;
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix c{};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            for (int k = 0; k < 3; ++k) c[y * 3 + x] += a[y * 3 + k] * b[k * 3 + x];
    return c;
}
Matrix inverse(const Matrix& a) {
    Matrix b{a[4] * a[8] - a[5] * a[7], a[2] * a[7] - a[1] * a[8], a[1] * a[5] - a[2] * a[4],
             a[5] * a[6] - a[3] * a[8], a[0] * a[8] - a[2] * a[6], a[2] * a[3] - a[0] * a[5],
             a[3] * a[7] - a[4] * a[6], a[1] * a[6] - a[0] * a[7], a[0] * a[4] - a[1] * a[3]};
    double d = a[0] * b[0] + a[1] * b[3] + a[2] * b[6];
    if (!std::isfinite(d) || std::abs(d) < 1e-12) throw std::runtime_error("DNG has singular color calibration");
    for (auto& v : b) v /= d;
    return b;
}
std::array<double, 3> transform(const Matrix& a, const std::array<double, 3>& b) {
    return {a[0] * b[0] + a[1] * b[1] + a[2] * b[2], a[3] * b[0] + a[4] * b[1] + a[5] * b[2],
            a[6] * b[0] + a[7] * b[1] + a[8] * b[2]};
}
float gain(const tinydng_gainmap& g, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (y < g.top || y >= g.bottom || x < g.left || x >= g.right || !g.row_pitch || !g.col_pitch ||
        (y - g.top) % g.row_pitch || (x - g.left) % g.col_pitch)
        return 1;
    if (g.map_planes != 1) throw std::runtime_error("Unsupported Bayer gain-map plane count");
    double gx = std::clamp(((x + 0.5) / width - g.map_origin_h) / g.map_spacing_h, 0.0, double(g.map_points_h - 1));
    double gy = std::clamp(((y + 0.5) / height - g.map_origin_v) / g.map_spacing_v, 0.0, double(g.map_points_v - 1));
    auto x0 = uint32_t(gx), y0 = uint32_t(gy), x1 = std::min(x0 + 1, g.map_points_h - 1),
         y1 = std::min(y0 + 1, g.map_points_v - 1);
    auto at = [&](uint32_t xx, uint32_t yy) { return g.pixels[size_t(yy) * g.map_points_h + xx]; };
    return float((1 - (gy - y0)) * ((1 - (gx - x0)) * at(x0, y0) + (gx - x0) * at(x1, y0)) +
                 (gy - y0) * ((1 - (gx - x0)) * at(x0, y1) + (gx - x0) * at(x1, y1)));
}
}  // namespace
DngSource::DngSource(const std::string& path) : rawCachePath_(path + ".raw16-v1") {
    tinydng_error error{};
    tinydng_config config{};
    config.memory_cap_bytes = 128u * 1024u * 1024u;
    config.max_images = 64;
    config.max_image_pixels = 250000000;
    context_ = tinydng_context_create(&config, &error);
    if (!context_) throw std::runtime_error(error.message);
    try {
        tinydng_open_options options{};
        options.flags = TINYDNG_OPEN_PARSE_SUBIFDS | TINYDNG_OPEN_METADATA_ONLY;
        if (tinydng_open_file(context_, path.c_str(), &options, &document_, &error) != TINYDNG_OK)
            throw std::runtime_error(error.message);
        for (size_t i = 0; i < tinydng_image_count(document_); ++i) {
            auto* p = tinydng_image_get(document_, i);
            if (p->cfa.present &&
                (!image_ || uint64_t(p->width) * p->height > uint64_t(image_->width) * image_->height)) {
                image_ = p;
                index_ = i;
            }
        }
        if (!image_ || !image_->raw.has_dng_version) throw std::runtime_error("Not a supported Bayer DNG");
        const auto& p = *image_;
        if (p.width < 16 || p.height < 16 || (p.width & 1u) || (p.height & 1u))
            throw std::runtime_error("DNG sensor dimensions must be even and at least 16 pixels");
        if (p.samples_per_pixel != 1 || p.sample_format != 1 || p.cfa.pattern_dim[0] != 2 ||
            p.cfa.pattern_dim[1] != 2 || p.bits_per_sample < 8 || p.bits_per_sample > 16 ||
            (p.compression != 1 && p.compression != 7))
            throw std::runtime_error("Unsupported DNG layout, sample depth, or compression");
        rawCacheRequired_ = p.compression == TINYDNG_COMPRESSION_NEW_JPEG;
        for (size_t i = 0; i < p.segment_count; ++i)
            rawCacheRequired_ |= uint64_t(p.segments[i].w) * p.segments[i].h >= 8'000'000u;
        const uint8_t patterns[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
        bool found = false;
        for (uint32_t i = 0; i < 4; ++i)
            if (!std::memcmp(patterns[i], p.cfa.pattern, 4)) {
                cfa = i;
                found = true;
            }
        if (!found) throw std::runtime_error("Unsupported Bayer pattern");
        for (size_t i = 0; i < p.raw.opcode_count; ++i) {
            auto& o = p.raw.opcodes[i];
            bool supported = (o.id == 9 && o.list == 2) || (o.id == 1 && o.list == 3);
            if (!supported && !(o.flags & 1u))
                throw std::runtime_error("Unsupported required DNG opcode " + std::to_string(o.id));
        }
        for (size_t i = 0; i < p.raw.gainmap_count; ++i) {
            auto& g = p.raw.gainmaps[i];
            if (!g.map_points_h || !g.map_points_v || g.map_spacing_h <= 0 || g.map_spacing_v <= 0 || g.map_planes != 1)
                throw std::runtime_error("Invalid or unsupported gain map");
        }
        crop = {0, 0, p.width, p.height};
        if (p.raw.has_active_area)
            crop = {p.raw.active_area[1], p.raw.active_area[0], p.raw.active_area[3] - p.raw.active_area[1],
                    p.raw.active_area[2] - p.raw.active_area[0]};
        if (p.raw.has_crop) {
            for (double v : p.raw.crop_origin)
                if (!std::isfinite(v) || v < 0) throw std::runtime_error("Invalid DNG crop");
            for (double v : p.raw.crop_size)
                if (!std::isfinite(v) || v < 1 || v > 250000) throw std::runtime_error("Invalid DNG crop");
            crop[0] += uint32_t(p.raw.crop_origin[0]);
            crop[1] += uint32_t(p.raw.crop_origin[1]);
            crop[2] = uint32_t(p.raw.crop_size[0]);
            crop[3] = uint32_t(p.raw.crop_size[1]);
        }
        if (!crop[2] || !crop[3] || uint64_t(crop[0]) + crop[2] > p.width || uint64_t(crop[1]) + crop[3] > p.height)
            throw std::runtime_error("DNG crop is out of bounds");
        const auto* exif = tinydng_document_exif(document_);
        orientation = p.exif.orientation ? p.exif.orientation : (exif ? exif->orientation : 1);
        if (!orientation) orientation = 1;
        if (orientation > 8) throw std::runtime_error("Invalid DNG orientation");
        const auto* root = tinydng_image_get(document_, 0);
        captureExif_ = readSourceCaptureExif(p.exif, exif, p.raw.unique_camera_model ? p.raw.unique_camera_model
                                                                                       : root->raw.unique_camera_model);
        for (int i = 0; i < 4; ++i) {
            black[i] = p.raw.black_level_exact[i];
            if (!std::isfinite(black[i])) throw std::runtime_error("Invalid DNG black level");
        }
        noiseProfileCount = 0;
        if (p.raw.noise_profile_count == 8) {
            for (int i = 0; i < 8; ++i) {
                if (!std::isfinite(p.raw.noise_profile[i])) break;
                noiseProfile[i] = p.raw.noise_profile[i];
                if (i == 7) noiseProfileCount = 8;
            }
        }
        whiteLevel = p.raw.white_level_present ? double(p.raw.white_level[0]) : 0.0;
        auto& r = p.raw;
        // "RawrCam ..." markers come from DNGs written before the rename; keep reading them.
        if (r.rawr_private_data && r.rawr_private_size > 31 &&
            (std::string(r.rawr_private_data) == "Rawr tinydng provenance" ||
             std::string(r.rawr_private_data) == "RawrCam tinydng provenance" ||
             std::string(r.rawr_private_data) == "RawrCam dng_writer provenance")) {
            rawr = true;
            provenance = r.rawr_private_data + std::strlen(r.rawr_private_data) + 1;
        }
        if (!r.color_matrix_present || !r.has_as_shot_neutral)
            throw std::runtime_error("DNG is missing color calibration or white balance");
        Matrix color{};
        std::copy_n(r.color_matrix1, 9, color.begin());
        // Choose the closest calibration by reciprocal temperature, then adapt the
        // recorded neutral to D65 with Bradford. Refinement is performed below.
        auto illuminant = [](uint16_t id) {
            return id == 17   ? 2856.0
                   : id == 21 ? 6504.0
                   : id == 23 ? 5003.0
                   : id == 20 ? 5500.0
                   : id == 22 ? 7500.0
                              : 5003.0;
        };
        std::array<double, 3> neutral{r.as_shot_neutral[0], r.as_shot_neutral[1], r.as_shot_neutral[2]};
        for (double v : neutral)
            if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Invalid DNG white balance");
        for (int i = 0; i < 3; ++i) wb[i] = float(neutral[1] / neutral[i]);
        Matrix calibration{1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (r.camera_calibration_present) std::copy_n(r.camera_calibration1, 9, calibration.begin());
        Matrix analog{1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (r.has_analog_balance)
            for (int i = 0; i < 3; ++i) analog[i * 3 + i] = r.analog_balance[i];
        Matrix inv{};
        for (int iteration = 0; iteration < 5; ++iteration) {
            inv = inverse(multiply(analog, multiply(calibration, color)));
            auto xyz = transform(inv, neutral);
            double sum = xyz[0] + xyz[1] + xyz[2];
            double x = xyz[0] / sum, y = xyz[1] / sum, n = (x - 0.3320) / (y - 0.1858);
            double temp = std::clamp(-449 * n * n * n + 3525 * n * n - 6823.3 * n + 5520.33, 2000.0, 25000.0);
            if (r.calibration_illuminant2) {
                double t1 = illuminant(r.calibration_illuminant1), t2 = illuminant(r.calibration_illuminant2);
                double a = t1 == t2 ? 0 : std::clamp((1 / temp - 1 / t1) / (1 / t2 - 1 / t1), 0.0, 1.0);
                for (int i = 0; i < 9; ++i) color[i] = (1 - a) * r.color_matrix1[i] + a * r.color_matrix2[i];
                if (r.camera_calibration_present && std::any_of(r.camera_calibration2, r.camera_calibration2 + 9,
                                                                [](double value) { return value != 0; }))
                    for (int i = 0; i < 9; ++i)
                        calibration[i] = (1 - a) * r.camera_calibration1[i] + a * r.camera_calibration2[i];
            }
        }
        inv = inverse(multiply(analog, multiply(calibration, color)));
        auto white = transform(inv, neutral);
        const double whiteY = white[1];
        for (auto& v : white) v /= whiteY;
        Matrix bradford{.8951, .2664, -.1614, -.7502, 1.7135, .0367, .0389, -.0685, 1.0296};
        auto from = transform(bradford, white), to = transform(bradford, {.95047, 1, 1.08883});
        Matrix scale{};
        for (int i = 0; i < 3; ++i) scale[i * 3 + i] = to[i] / from[i];
        Matrix unbalance{};
        for (int i = 0; i < 3; ++i) unbalance[i * 3 + i] = 1 / wb[i];
        Matrix xyzToRgb{3.2404542, -1.5371385, -.4985314, -.969266, 1.8760108, .041556, .0556434, -.2040259, 1.0572252};
        auto final = multiply(
            xyzToRgb, multiply(inverse(bradford), multiply(scale, multiply(bradford, multiply(inv, unbalance)))));
        auto ref = transform(final, {1, 1, 1});
        double normal = ref[1];
        for (int i = 0; i < 9; ++i) {
            cameraToSrgb[i] = float(final[i] / normal);
            if (!std::isfinite(cameraToSrgb[i])) throw std::runtime_error("Invalid DNG color calibration");
        }
        // Rawr embeds the selected DCP in IFD0. Read its metadata without
        // pulling a potentially hundreds-of-megabytes RAW payload into RAM.
        if (provenance.find("com.rawrcam.color.dcp") != std::string::npos) {
            std::string why;
            auto profile = color::loadEmbeddedDcp(path, &why);
            if (!profile) throw std::runtime_error("Cannot read embedded camera profile: " + why);
            auto calibration = color::dcpColorCalibration(*profile);
            if (!calibration) throw std::runtime_error("Invalid embedded camera profile calibration");
            dcpProfile = std::make_shared<const color::DcpProfile>(std::move(*profile));
            metadata::FrameMetadataSnapshot frame;
            auto context = std::make_shared<metadata::CameraContextMetadata>();
            context->color = *calibration;
            frame.cameraContext = context;
            frame.colorCorrectionGainsRggb = {wb[0], wb[1], wb[1], wb[2]};
            const auto resolved = color::deriveFrameColorTransform(frame, color::PreviewColorMode::Auto);
            cameraToSrgb = resolved.cameraToLinearSrgbRowMajor;
            dcpWeight1 = resolved.calibrationWeight1;
        }
    } catch (...) {
        tinydng_document_destroy(context_, document_);
        tinydng_context_destroy(context_);
        throw;
    }
}
DngSource::~DngSource() {
    if (binCache_) munmap(const_cast<float*>(binCache_), binCacheBytes_);
    if (rawCache_) munmap(const_cast<uint16_t*>(rawCache_), rawCacheBytes_);
    tinydng_document_destroy(context_, document_);
    tinydng_context_destroy(context_);
}
void DngSource::ensureRawCache() {
    if (rawCache_) return;
    const size_t bytes = size_t(image_->width) * image_->height * sizeof(uint16_t);
    auto mapFile = [&](const std::string& path) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Cannot open decoded DNG cache");
        void* mapped = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) throw std::runtime_error("Cannot map decoded DNG cache");
        rawCache_ = static_cast<const uint16_t*>(mapped);
        rawCacheBytes_ = bytes;
    };
    if (std::filesystem::exists(rawCachePath_) && std::filesystem::file_size(rawCachePath_) == bytes) {
        mapFile(rawCachePath_);
        return;
    }
    const std::string temporary = rawCachePath_ + ".tmp";
    int fd = open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot create decoded DNG cache");
    void* mapped = MAP_FAILED;
    try {
#if defined(__APPLE__)
        // macOS (offline tools) has no posix_fallocate; a sparse file is enough there.
        if (ftruncate(fd, off_t(bytes)) != 0)
#else
        if (posix_fallocate(fd, 0, off_t(bytes)) != 0)
#endif
            throw std::runtime_error("Not enough storage for decoded DNG cache");
        mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("Cannot map decoded DNG cache");
        tinydng_decode_options options{};
        options.dst = mapped;
        options.dst_capacity = bytes;
        options.num_threads = 4;
        tinydng_pixels decoded{};
        tinydng_error error{};
        if (tinydng_decode_image(context_, document_, index_, &options, &decoded, &error) != TINYDNG_OK)
            throw std::runtime_error(error.message);
        if (decoded.width != image_->width || decoded.height != image_->height || decoded.bits_per_sample != 16 ||
            decoded.samples_per_pixel != 1)
            throw std::runtime_error("Invalid decoded DNG cache geometry");
        if (msync(mapped, bytes, MS_SYNC) != 0 || fsync(fd) != 0)
            throw std::runtime_error("Cannot save decoded DNG cache");
        munmap(mapped, bytes);
        mapped = MAP_FAILED;
        close(fd);
        fd = -1;
        std::filesystem::rename(temporary, rawCachePath_);
        mapFile(rawCachePath_);
    } catch (...) {
        if (mapped != MAP_FAILED) munmap(mapped, bytes);
        if (fd >= 0) close(fd);
        std::filesystem::remove(temporary);
        throw;
    }
}
std::vector<uint16_t> DngSource::read(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!w || !h || x >= image_->width || y >= image_->height || w > image_->width - x || h > image_->height - y)
        throw std::runtime_error("DNG region is out of bounds");
    if (rawCacheRequired_) {
        {
            std::lock_guard<std::mutex> lock(decodeMutex_);
            ensureRawCache();
        }
        std::vector<uint16_t> result(size_t(w) * h);
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(result.data() + size_t(row) * w, rawCache_ + size_t(y + row) * image_->width + x,
                        size_t(w) * sizeof(uint16_t));
        return result;
    }
    std::lock_guard<std::mutex> lock(decodeMutex_);
    tinydng_error error{};
    tinydng_pixels pixels{};
    tinydng_decode_options options{};
    // The file-backed LJPEG tiles are independent. Bound workers so a 200MP
    // decode does not multiply its scratch-memory use by the CPU core count.
    options.num_threads = 4;
    if (tinydng_decode_region(context_, document_, index_, x, y, w, h, &options, &pixels, &error) != TINYDNG_OK)
        throw std::runtime_error(error.message);
    if (pixels.width != w || pixels.height != h || pixels.samples_per_pixel != 1 ||
        pixels.size < uint64_t(w) * h * (pixels.bits_per_sample / 8)) {
        tinydng_pixels_free(context_, &pixels);
        throw std::runtime_error("Invalid decoded DNG region");
    }
    std::vector<uint16_t> result(size_t(w) * h);
    if (pixels.bits_per_sample == 16)
        std::memcpy(result.data(), pixels.data, result.size() * 2);
    else if (pixels.bits_per_sample == 8)
        std::copy_n(pixels.data, result.size(), result.begin());
    else {
        tinydng_pixels_free(context_, &pixels);
        throw std::runtime_error("Unsupported decoded sample depth");
    }
    tinydng_pixels_free(context_, &pixels);
    return result;
}
float DngSource::normalizedSample(uint16_t raw, uint32_t sx, uint32_t sy, bool shading) const {
    const uint32_t site = (sy % 2) * 2 + sx % 2;
    const auto& r = image_->raw;
    double v;
#ifndef NDEBUG
    if (!replayCfa_.empty()) {
        v = replayCfa_[size_t(sy) * image_->width + sx];
    } else {
#endif
    v = raw;
    if (r.linearization_table_count)
        v = r.linearization_table[std::min(size_t(v), r.linearization_table_count - 1)];
    double white = r.white_level_present ? r.white_level[0] : ((1u << image_->bits_per_sample) - 1);
    if (white <= black[site]) throw std::runtime_error("Invalid DNG black/white levels");
    v = (v - black[site]) / (white - black[site]);
#ifndef NDEBUG
    }
#endif
    if (shading)
        for (size_t k = 0; k < r.gainmap_count; ++k)
            v *= gain(r.gainmaps[k], sx, sy, image_->width, image_->height);
    return float(v * 255.0);
}
std::vector<float> DngSource::normalized(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool shading) {
#ifndef NDEBUG
    auto pixels = replayCfa_.empty() ? read(x, y, w, h) : std::vector<uint16_t>{};
#else
    auto pixels = read(x, y, w, h);
#endif
    std::vector<float> result(size_t(w) * h);
    for (uint32_t yy = 0; yy < h; ++yy)
        for (uint32_t xx = 0; xx < w; ++xx) {
            const size_t i = size_t(yy) * w + xx;
            result[i] = normalizedSample(pixels.empty() ? 0 : pixels[i], x + xx, y + yy, shading);
        }
    return result;
}
void DngSource::prepareBinned(const std::string& path, bool shading, const std::function<void()>& check) {
    if (binCache_ && binCachePath_ == path) return;
    if (binCache_) { munmap(const_cast<float*>(binCache_), binCacheBytes_); binCache_ = nullptr; }
    const uint32_t sw = image_->width, sh = image_->height, ow = sw / 2, oh = sh / 2;
    const size_t bytes = size_t(ow) * oh * sizeof(float);
    const std::string temp = path + ".tmp";
    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) != bytes) {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot create binned CFA cache");
        try {
            std::array<float,4> gains{};
            int red = 0;
            const float maxGain = std::max({wb[0],wb[1],wb[2]});
            for (int i=0;i<4;++i) {
                gains[i] = wb[image_->cfa.pattern[i]] / maxGain;
                if (!(gains[i] > 0) || !std::isfinite(gains[i])) throw std::runtime_error("Invalid binning calibration");
                if (image_->cfa.pattern[i] == 0) red = i;
            }
            const float floor = std::max(1e-20f, std::pow(0.25f * 255.f / float(std::max(1.0, whiteLevel)), 2.f));
            // Bands are independent (each reads its own 20-row halo), so a
            // batch of them runs on workers and is written back in order.
            auto binBand = [&](uint32_t y, NaturalBayerTile& tile) {
                const uint32_t rows=std::min(32u,oh-y), top=y*2>=20?y*2-20:0;
                const uint32_t bottom=std::min(sh,(y+rows)*2+20);
                const auto band=normalized(0,top,sw,bottom-top,shading);
                std::vector<float> out(size_t(ow)*rows);
                auto sample=[&](uint32_t sx,uint32_t sy){return band[size_t(sy-top)*sw+sx];};
                for(uint32_t x=0;x<ow;x+=128) {
                    check(); const uint32_t cols=std::min(128u,ow-x);
                    if(sw>=16 && sh>=16 && sw%4==0 && sh%4==0)
                        tile.run(x,y,cols,rows,sw,sh,gains,red,floor,sample,check,out.data()+x,ow);
                    else for(uint32_t yy=0;yy<rows;++yy)for(uint32_t xx=0;xx<cols;++xx)
                        out[size_t(yy)*ow+x+xx]=binSameColor2x(x+xx,y+yy,sw,sh,sample);
                }
                return out;
            };
            const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 6u);
            std::vector<NaturalBayerTile> tiles(workers);
            std::vector<std::vector<float>> outs(workers);
            std::vector<std::exception_ptr> errors(workers);
            for (uint32_t y0=0;y0<oh;y0+=32u*workers) {
                check();
                std::vector<std::thread> threads;
                rawrcam::support::JoinGuard joinAll(threads);
                for (unsigned t=0;t<workers;++t) {
                    const uint32_t y=y0+32u*t;
                    if (y>=oh) break;
                    threads.emplace_back([&,t,y] {
                        try { outs[t]=binBand(y,tiles[t]); } catch (...) { errors[t]=std::current_exception(); }
                    });
                }
                for (auto& thread : threads) thread.join();
                for (unsigned t=0;t<threads.size();++t) {
                    if (errors[t]) std::rethrow_exception(errors[t]);
                    file.write(reinterpret_cast<const char*>(outs[t].data()), std::streamsize(outs[t].size()*sizeof(float)));
                    if (!file) throw std::runtime_error("Cannot write binned CFA cache");
                }
            }
            file.close();
            if (!file) throw std::runtime_error("Cannot finish binned CFA cache");
            std::filesystem::rename(temp,path);
        } catch (...) { file.close(); std::filesystem::remove(temp); throw; }
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open binned CFA cache");
    void* mapped = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) throw std::runtime_error("Cannot map binned CFA cache");
    binCache_ = static_cast<const float*>(mapped); binCacheBytes_ = bytes; binCachePath_ = path;
}
std::vector<float> DngSource::binnedNormalized(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool shading) {
    const uint32_t sw = image_->width, sh = image_->height;
    if ((sw & 1u) || (sh & 1u) || x + w > sw / 2 || y + h > sh / 2)
        throw std::runtime_error("Invalid Bayer bin region");
    if (binCache_) {
        std::vector<float> out(size_t(w)*h);
        for (uint32_t yy=0; yy<h; ++yy)
            std::copy_n(binCache_+size_t(y+yy)*(sw/2)+x,w,out.data()+size_t(yy)*w);
        return out;
    }
    const uint32_t left = x * 2u >= 4u ? x * 2u - 4u : 0u;
    const uint32_t top = y * 2u >= 4u ? y * 2u - 4u : 0u;
    const uint32_t right = std::min(sw, (x + w) * 2u + 4u);
    const uint32_t bottom = std::min(sh, (y + h) * 2u + 4u);
    const uint32_t rw = right - left;
    const auto pixels = normalized(left, top, rw, bottom - top, shading);
    std::vector<float> out(size_t(w) * h);
    for (uint32_t yy = 0; yy < h; ++yy)
        for (uint32_t xx = 0; xx < w; ++xx)
            out[size_t(yy) * w + xx] = binSameColor2x(x + xx, y + yy, sw, sh, [&](uint32_t sx, uint32_t sy) {
                return pixels[size_t(sy - top) * rw + sx - left];
            });
    return out;
}
#ifndef NDEBUG
void DngSource::setReplayCfa(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto expected = size_t(image_->width) * image_->height * sizeof(float);
    if (!file || file.tellg() != std::streamoff(expected)) throw std::runtime_error("Invalid replay CFA length");
    replayCfa_.resize(size_t(image_->width) * image_->height);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(replayCfa_.data()), expected);
    if (!file) throw std::runtime_error("Cannot read replay CFA");
    for (float v : replayCfa_)
        if (!std::isfinite(v) || v < -0.5f || v > 16.0f) throw std::runtime_error("Invalid replay CFA value");
}
#endif
}  // namespace rawrcam::renderer
