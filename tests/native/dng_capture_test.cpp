#include "encoding/dng/DngMetadataAdapter.h"

#include <tinydng.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Host-link stub: the adapter calls into raw_denoise only for the denoise
// extension (disabled in this test). The device build links the real lib.
namespace raw_denoise {
bool GreenNoiseToNormalized(const float*, size_t, int, float, float, float&, float&) { return false; }
}  // namespace raw_denoise

static rawrcam::imaging::RawSnapshot makeFrame(bool resolvable) {
    rawrcam::imaging::RawSnapshot f{};
    f.requestId = 1;
    f.timestampNs = 1234;
    f.width = 8;
    f.height = 6;
    f.packedRowStrideBytes = 16;
    f.sourceRowStrideBytes = 16;
    f.raw16.resize(96);
    for (size_t i = 0; i < f.raw16.size(); ++i) f.raw16[i] = static_cast<uint8_t>(i);
    auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>();
    ctx->cameraId = "3";
    ctx->lensId = "main";
    ctx->camera2Cfa = 0;
    ctx->rawPreviewCfa = 0;
    ctx->geometry.rawBufferWidth = 8;
    ctx->geometry.rawBufferHeight = 6;
    ctx->geometry.pixelArrayWidth = resolvable ? 8 : 10;
    ctx->geometry.pixelArrayHeight = resolvable ? 6 : 10;
    ctx->geometry.preCorrectionActiveArray = {0, 0, resolvable ? 8 : 9, resolvable ? 6 : 9, true};
    ctx->geometry.sensorOrientationDegrees = 90;
    ctx->sensorPhysicalSizeMm = {8.0f, 6.0f};
    ctx->availableFocalLengthsMm = {6.0f};
    ctx->availableApertures = {1.8f};
    ctx->lensShadingMapWidth = 2;
    ctx->lensShadingMapHeight = 2;
    ctx->color.referenceIlluminant1 = 21;
    ctx->color.colorTransform1.valid = true;
    f.metadata.cameraContext = ctx;
    f.metadata.timestampNs = f.timestampNs;
    f.metadata.frameOrdinal = 7;
    f.metadata.effectiveWhiteLevel = 4095;
    f.metadata.blackLevelPhysicalRggb = {64, 65, 66, 67};
    f.metadata.hasNeutralColorPoint = true;
    f.metadata.neutralColorPoint = {0.5f, 1.0f, 0.75f};
    f.metadata.exposureTimeNs = 10000000;
    f.metadata.sensitivity = 100;
    f.metadata.aperture = 1.8f;
    f.metadata.focalLengthMm = 6.0f;
    f.metadata.focusDistanceDiopters = 2.0f;
    f.metadata.aeMode = 1;
    f.metadata.awbMode = 1;
    f.metadata.sensorNoiseProfile = {0.01, 0.001, 0.02, 0.002, 0.024, 0.0024, 0.03, 0.003};
    f.metadata.lensShadingMapWidth = 2;
    f.metadata.lensShadingMapHeight = 2;
    f.metadata.lensShadingMap.assign(16, 1.0f);
    return f;
}

static bool privateHas(const std::vector<uint8_t>& priv, const std::string& needle) {
    if (priv.empty() || needle.empty()) return false;
    const std::string hay(reinterpret_cast<const char*>(priv.data()), priv.size());
    return hay.find(needle) != std::string::npos;
}

int main() {
    rawrcam::encoding::dng::DngCaptureContext capture{};
    capture.deviceMake = "vivo";
    capture.deviceModel = "V2562";
    capture.deviceRotationDegrees = 0;
    capture.wallClockUnixMillis = 1787706000123LL;
    capture.utcOffsetMinutes = 420;
    capture.displayName = "RAWR_20260826_01000012.dng";
    capture.baselineExposureEV = 2.0f;
    capture.processingRecipe = R"({"schema":"com.rawrcam.processing-recipe","version":1,"filmSimEnabled":true})";
    capture.resolvedRecipe = R"({"version":1,"aePostGain":4})";
    capture.mergeReplayMetadata = "rawrReferenceIndex\t2\n";
    auto good = makeFrame(true);
    std::string error;

    for (int pass = 0; pass < 2; ++pass) {
        capture.compression = pass == 0 ? rawrcam::encoding::dng::DngCompression::LosslessJpeg
                                        : rawrcam::encoding::dng::DngCompression::Uncompressed;
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(good, capture, &error);
        if (!params) {
            std::cerr << error << "\n";
            return 1;
        }
        params->rebind();
        // Sensor noise metadata is exported independently of merge-noise selection.
        if (params->raw.noise_profile_count != 6) return 5;
        const std::array<double, 6> expectedNoise{0.01, 0.001, 0.022, 0.0022, 0.03, 0.003};
        for (size_t i = 0; i < 6; ++i)
            if (std::abs(params->raw.noise_profile[i] - expectedNoise[i]) >= 1e-12) return 5;
        if (!privateHas(params->privateData, "com.rawrcam.processing.recipe.v1") ||
            !privateHas(params->privateData, "filmSimEnabled") ||
            !privateHas(params->privateData, "rawrReferenceIndex\\\\u00092\\\\u000a") ||
            !privateHas(params->privateData, "com.rawrcam.processing.resolved.v1") ||
            !privateHas(params->privateData, "aePostGain") ||
            !privateHas(params->privateData, "com.rawrcam.processing.source_role") ||
            !privateHas(params->privateData, "1234"))
            return 6;
        if (params->exif.orientation != 6 || params->raw.opcode_count != 4 ||
            !params->exif.has_fnumber || !params->exif.has_focal_length ||
            !params->exif.has_shutter_speed || !params->exif.has_aperture_value ||
            !params->exif.has_max_aperture_value || !params->exif.has_subject_distance ||
            !params->exif.datetime_original || !params->raw.has_lens_info ||
            !params->raw.has_baseline_exposure ||
            std::abs(params->raw.baseline_exposure - 2.0) > 1e-9 ||
            !params->raw.unique_camera_model ||
            std::string(params->raw.unique_camera_model).find("vivo") == std::string::npos)
            return 4;
        if ((pass == 0) != (params->options.compression == 7)) return 4;

        // Serialize with the streaming writer path (memory sink) and
        // round-trip through the device reader.
        tinydng_config cfg{};
        tinydng_error err{};
        tinydng_context* tctx = tinydng_context_create(&cfg, &err);
        if (!tctx) return 7;
        uint8_t* out = nullptr;
        size_t outSize = 0;
        if (tinydng_write_memory(tctx, &params->image, &params->options, &out, &outSize, &err) !=
                TINYDNG_OK ||
            outSize == 0) {
            std::cerr << err.message << "\n";
            tinydng_context_destroy(tctx);
            return 2;
        }
        if (pass == 1 && outSize <= good.raw16.size()) {
            tinydng_buffer_free(tctx, out);
            tinydng_context_destroy(tctx);
            return 2;
        }
        tinydng_document* doc = nullptr;
        tinydng_open_options oopts{};
        if (tinydng_open_memory(tctx, out, outSize, &oopts, &doc, &err) != TINYDNG_OK) {
            std::cerr << err.message << "\n";
            tinydng_buffer_free(tctx, out);
            tinydng_context_destroy(tctx);
            return 2;
        }
        const tinydng_image_info* img = tinydng_image_get(doc, 0);
        const uint16_t wantComp = pass == 0 ? 7 : 1;
        bool ok = img && img->width == 8 && img->height == 6 && img->compression == wantComp &&
                  img->raw.white_level_present && img->raw.white_level[0] == 4095 &&
                  img->raw.has_active_area && img->raw.noise_profile_count == 6 &&
                  img->raw.opcode_count >= 1 && img->raw.gainmap_count == 4 &&
                  img->raw.rawr_private_data != nullptr &&
                  std::string(img->raw.rawr_private_data) == "RawrCam tinydng provenance" &&
                  img->exif.make && std::string(img->exif.make) == "vivo";
        if (ok) {
            tinydng_pixels px{};
            tinydng_decode_options dopts{};
            if (tinydng_decode_image(tctx, doc, 0, &dopts, &px, &err) != TINYDNG_OK) {
                ok = false;
            } else {
                const auto* got = reinterpret_cast<const uint8_t*>(px.data);
                if (px.size != good.raw16.size() || std::memcmp(got, good.raw16.data(), px.size) != 0)
                    ok = false;
                tinydng_pixels_free(tctx, &px);
            }
        }
        tinydng_document_destroy(tctx, doc);
        tinydng_buffer_free(tctx, out);
        tinydng_context_destroy(tctx);
        if (!ok) return 2;
        std::cout << "DNG_CAPTURE_SEMANTIC_TEST_PASS pass=" << pass << " bytes=" << outSize << "\n";
    }

    {
        // 16-bit merged DNG levels (black 1024 x7): BlackLevel is written as
        // RATIONAL and must stay exact above the old fixed-1e6 denominator limit.
        auto wide = makeFrame(true);
        wide.metadata.blackLevelPhysicalRggb = {7168, 7168, 7175, 7168};
        wide.metadata.effectiveWhiteLevel = 60984;
        capture.compression = rawrcam::encoding::dng::DngCompression::Uncompressed;
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(wide, capture, &error);
        if (!params) return 8;
        params->rebind();
        tinydng_config cfg{};
        tinydng_error err{};
        tinydng_context* tctx = tinydng_context_create(&cfg, &err);
        uint8_t* out = nullptr;
        size_t outSize = 0;
        tinydng_document* doc = nullptr;
        tinydng_open_options oopts{};
        bool ok = tctx && tinydng_write_memory(tctx, &params->image, &params->options, &out, &outSize, &err) ==
                              TINYDNG_OK &&
                  tinydng_open_memory(tctx, out, outSize, &oopts, &doc, &err) == TINYDNG_OK;
        const tinydng_image_info* img = ok ? tinydng_image_get(doc, 0) : nullptr;
        ok = ok && img && img->raw.white_level[0] == 60984 && std::abs(img->raw.black_level_exact[0] - 7168.0) < 1e-6 &&
             std::abs(img->raw.black_level_exact[2] - 7175.0) < 1e-6;
        if (doc) tinydng_document_destroy(tctx, doc);
        if (out) tinydng_buffer_free(tctx, out);
        if (tctx) tinydng_context_destroy(tctx);
        if (!ok) return 8;
        std::cout << "DNG_CAPTURE_16BIT_LEVELS_PASS\n";
    }

    // Lossless JPEG is written as 256x256 tiles through the streaming writer
    // (LibRaw/RawTherapee only decode the first strip of multi-strip LJPEG).
    // Frames cross tile edges; RAWR_DNG_TILE_OUT also writes a full-size file
    // for external decoders.
    const char* tileOut = std::getenv("RAWR_DNG_TILE_OUT");
    for (const auto dims : {std::pair<uint32_t, uint32_t>{600, 400}, std::pair<uint32_t, uint32_t>{4096, 3072}}) {
        if (dims.first == 4096 && !tileOut) continue;
        auto big = makeFrame(true);
        big.width = dims.first;
        big.height = dims.second;
        big.packedRowStrideBytes = big.sourceRowStrideBytes = int32_t(dims.first * 2);
        big.raw16.resize(size_t(dims.first) * dims.second * 2);
        auto* px = reinterpret_cast<uint16_t*>(big.raw16.data());
        for (uint32_t y = 0; y < dims.second; ++y)
            for (uint32_t x = 0; x < dims.first; ++x)
                px[size_t(y) * dims.first + x] = uint16_t(64 + ((x * 7 + y * 13 + (x * y) % 97) % 4000));
        auto ctxBig = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*big.metadata.cameraContext);
        ctxBig->geometry.rawBufferWidth = ctxBig->geometry.pixelArrayWidth = int32_t(dims.first);
        ctxBig->geometry.rawBufferHeight = ctxBig->geometry.pixelArrayHeight = int32_t(dims.second);
        ctxBig->geometry.preCorrectionActiveArray = {0, 0, int32_t(dims.first), int32_t(dims.second), true};
        big.metadata.cameraContext = ctxBig;
        capture.compression = rawrcam::encoding::dng::DngCompression::LosslessJpeg;
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(big, capture, &error);
        if (!params) {
            std::cerr << error << "\n";
            return 9;
        }
        params->rebind();
        if (params->tiling.tile_width != rawrcam::encoding::dng::kLjpegTileSize || params->tiling.rows_per_strip != 0)
            return 9;
        tinydng_config cfg{};
        tinydng_error err{};
        tinydng_context* tctx = tinydng_context_create(&cfg, &err);
        tinydng_write_io io{};
        tinydng_writer* writer = nullptr;
        uint8_t* out = nullptr;
        size_t outSize = 0;
        bool ok = tctx && tinydng_write_io_open_memory(tctx, &io, &err) == TINYDNG_OK &&
                  tinydng_writer_create(tctx, io, &params->image, &params->options, &params->tiling, &writer, &err) ==
                      TINYDNG_OK &&
                  rawrcam::encoding::dng::writeTinyDngPayload(writer, *params, &err) == TINYDNG_OK &&
                  tinydng_writer_finish(writer, &err) == TINYDNG_OK &&
                  tinydng_write_io_memory_take(tctx, &io, &out, &outSize, &err) == TINYDNG_OK;
        if (io.close) io.close(&io);
        tinydng_document* doc = nullptr;
        tinydng_open_options oopts{};
        tinydng_pixels decoded{};
        tinydng_decode_options dopts{};
        ok = ok && tinydng_open_memory(tctx, out, outSize, &oopts, &doc, &err) == TINYDNG_OK &&
             tinydng_decode_image(tctx, doc, 0, &dopts, &decoded, &err) == TINYDNG_OK && decoded.size == big.raw16.size() &&
             std::memcmp(decoded.data, big.raw16.data(), decoded.size) == 0;
        if (decoded.data) tinydng_pixels_free(tctx, &decoded);
        if (doc) tinydng_document_destroy(tctx, doc);
        if (ok && dims.first == 4096) {
            FILE* f = std::fopen(tileOut, "wb");
            ok = f && std::fwrite(out, 1, outSize, f) == outSize;
            if (f) std::fclose(f);
        }
        if (out) tinydng_buffer_free(tctx, out);
        if (tctx) tinydng_context_destroy(tctx);
        if (!ok) {
            std::cerr << "tiled ljpeg: " << err.message << "\n";
            return 9;
        }
        std::cout << "DNG_CAPTURE_TILED_LJPEG_PASS " << dims.first << "x" << dims.second << " bytes=" << outSize << "\n";
    }

    auto bad = makeFrame(false);
    error.clear();
    if (rawrcam::encoding::dng::makeTinyDngWriteParams(bad, capture, &error) ||
        error.find("unresolved stored RAW geometry") == std::string::npos)
        return 3;
    return 0;
}
