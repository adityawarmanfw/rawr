#include "encoding/dng/DngMetadataAdapter.h"

#include "color/DcpProfile.h"
#include <filesystem>
#include <fstream>

#include <tinydng.h>
#include <tiny_dng_ljpeg92_v2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

// T.81 H.1.2.2 / DNG: an SSSS 16 residual (diff -32768) carries no appended
// bits. True 16-bit merged DNGs hit it at hard edges; writing 16 stray bits
// desyncs Adobe/LibRaw for the rest of the tile.
static bool ljpegSsss16Conformant() {
    auto encode = [](std::vector<uint16_t> px, int w) {
        uint8_t* enc = nullptr;
        int len = 0;
        if (tdng_lj92_encode(px.data(), w, 1, 16, w, 0, nullptr, 0, &enc, &len) != TDNG_LJ92_ERROR_NONE) return -1;
        std::free(enc);
        return len;
    };
    // One sample against the 32768 initial predictor: 0 is SSSS 16, 32769 is
    // SSSS 1. Both are a single 1-bit code; the residual must not add bytes.
    const int len16 = encode({0}, 1), len1 = encode({32769}, 1);
    if (len16 < 0 || len16 != len1) {
        std::cerr << "ssss16 stream " << len16 << " bytes vs " << len1 << "\n";
        return false;
    }
    std::vector<uint16_t> px = {0, 40000, 7232, 40000, 7232, 7232, 65535, 32767, 0};
    uint8_t* enc = nullptr;
    int len = 0;
    if (tdng_lj92_encode(px.data(), int(px.size()), 1, 16, int(px.size()), 0, nullptr, 0, &enc, &len) !=
        TDNG_LJ92_ERROR_NONE)
        return false;
    tdng_lj92 lj = nullptr;
    int w = 0, h = 0, bits = 0, comps = 0;
    std::vector<uint16_t> out(px.size());
    const bool ok = tdng_lj92_open(&lj, enc, len, &w, &h, &bits, &comps) == TDNG_LJ92_ERROR_NONE &&
                    tdng_lj92_decode(lj, out.data(), w, 0, nullptr, 0) == TDNG_LJ92_ERROR_NONE && out == px;
    if (lj) tdng_lj92_close(lj);
    std::free(enc);
    return ok;
}

int main() {
    if (!ljpegSsss16Conformant()) return 10;
    rawrcam::encoding::dng::DngCaptureContext capture{};
    capture.deviceMake = "vivo";
    capture.deviceModel = "V2562";
    capture.deviceDisplayModel = "X300 Ultra";
    capture.deviceRotationDegrees = 0;
    capture.wallClockUnixMillis = 1787706000123LL;
    capture.utcOffsetMinutes = 420;
    capture.displayName = "RAWR_20260826_01000012.dng";
    capture.baselineExposureEV = 2.0f;
    capture.processingRecipe = R"({"schema":"com.rawrcam.processing-recipe","version":1,"filmSimEnabled":true})";
    capture.resolvedRecipe = R"({"version":1,"aePostGain":4})";
    capture.appLensShadingCorrection = true;
    auto good = makeFrame(true);
    std::string error;

    // DNG keeps the sensor raster and composes the mirror before display rotation.
    constexpr uint16_t mirrored[4][4] = {{1, 6, 3, 8}, {2, 7, 4, 5}, {4, 5, 2, 7}, {3, 8, 1, 6}};
    for (uint32_t mirror = 0; mirror < 4; ++mirror) {
        for (int turn = 0; turn < 4; ++turn) {
            auto frame = makeFrame(true);
            auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*frame.metadata.cameraContext);
            ctx->profileMirror = mirror;
            ctx->geometry.sensorOrientationDegrees = turn * 90;
            frame.metadata.cameraContext = ctx;
            auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(frame, capture, &error);
            if (!params || params->exif.orientation != mirrored[mirror][turn]) return 70;
            if (frame.raw16 != good.raw16) return 71;
        }
    }

    // The GainMap is always written: neither the HAL flag nor the app's own
    // lens shading setting decides it; the app setting only lands in provenance.
    for (const bool hal : {false, true}) {
        for (const bool app : {false, true}) {
            auto frame = makeFrame(true);
            auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*frame.metadata.cameraContext);
            ctx->hasLensShadingApplied = true;
            ctx->lensShadingApplied = hal;
            frame.metadata.cameraContext = ctx;
            auto shading = capture;
            shading.appLensShadingCorrection = app;
            auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(frame, shading, &error);
            if (!params) return 30;
            // Always written, whatever the app setting or HAL flag.
            if (params->raw.opcode_count != 4) return 31;
            if (!privateHas(params->privateData, "\"written\"")) return 32;
            if (!privateHas(params->privateData, app ? "\"true\"" : "\"false\"")) return 33;
        }
    }

    // A lens profile calibration goes in as WarpRectilinear (OpcodeList3)
    // exactly when the app undistorts, and reads back through the reader.
    for (const bool app : {false, true}) {
        auto frame = makeFrame(true);
        auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*frame.metadata.cameraContext);
        rawrcam::geometry::LensDistortionModel lens;
        lens.width = 4096;
        lens.height = 3072;
        lens.fx = 1698.182744;
        lens.fy = 1696.500137;
        lens.cx = 2042.360233;
        lens.cy = 1548.313918;
        lens.k = {-0.051307, 0.015171, 0.000123, 0.000357, 0, 0, -0.023536, 0.014256};
        lens.outputScale = 0.975811;
        ctx->profileDistortion = lens;
        frame.metadata.cameraContext = ctx;
        auto warp = capture;
        warp.appDistortionCorrection = app;
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(frame, warp, &error);
        if (!params) return 40;
        if (params->raw.opcode_count != (app ? 5u : 4u)) return 41;
        if (!privateHas(params->privateData, "com.rawrcam.distortion.warp") ||
            !privateHas(params->privateData, app ? "\"written\"" : "\"skipped_app_off\""))
            return 42;
        if (!app) continue;
        params->rebind();
        const auto fit = rawrcam::geometry::fitWarpRectilinear(lens, frame.width, frame.height);
        tinydng_config cfg{};
        tinydng_error err{};
        tinydng_context* tctx = tinydng_context_create(&cfg, &err);
        uint8_t* out = nullptr;
        size_t outSize = 0;
        tinydng_document* doc = nullptr;
        tinydng_open_options oopts{};
        bool ok = fit && tctx &&
                  tinydng_write_memory(tctx, &params->image, &params->options, &out, &outSize, &err) == TINYDNG_OK &&
                  tinydng_open_memory(tctx, out, outSize, &oopts, &doc, &err) == TINYDNG_OK;
        const tinydng_image_info* img = ok ? tinydng_image_get(doc, 0) : nullptr;
        ok = img && img->raw.warp_count == 1 && img->raw.gainmap_count == 4;
        if (ok) {
            const auto& w = img->raw.warps[0];
            ok = w.list == 3 && w.plane_count == 1 && std::abs(w.center[0] - fit->center[0]) < 1e-12 &&
                 std::abs(w.center[1] - fit->center[1]) < 1e-12;
            for (size_t i = 0; i < 4; ++i) ok = ok && std::abs(w.coeff[0][i] - fit->kr[i]) < 1e-12;
            for (size_t i = 0; i < 2; ++i) ok = ok && std::abs(w.coeff[0][4 + i] - fit->kt[i]) < 1e-12;
        }
        if (doc) tinydng_document_destroy(tctx, doc);
        if (out) tinydng_buffer_free(tctx, out);
        if (tctx) tinydng_context_destroy(tctx);
        if (!ok) return 43;
    }

    // A lens camera profile replaces the camera calibration in the DNG: its
    // matrices and tables, no CameraCalibration. The written IFD0 reads back
    // through the DCP parser as the same profile.
    {
        auto frame = makeFrame(true);
        auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*frame.metadata.cameraContext);
        ctx->color.calibrationTransform1.valid = true;
        rawrcam::color::DcpProfile dcp;
        dcp.name = "Lens Profile";
        dcp.copyright = "rawr test";
        dcp.illuminant1 = 17;
        dcp.illuminant2 = 21;
        dcp.colorMatrix1 = {1.2, -0.3, -0.1, -0.4, 1.3, 0.1, -0.1, 0.2, 0.6};
        dcp.colorMatrix2 = std::array<double, 9>{0.8, -0.2, -0.1, -0.5, 1.4, 0.1, -0.1, 0.3, 0.5};
        dcp.forwardMatrix1 = std::array<double, 9>{0.6, 0.3, 0.1, 0.2, 0.9, -0.1, 0.0, -0.3, 1.1};
        dcp.forwardMatrix2 = std::array<double, 9>{0.5, 0.2, 0.3, 0.2, 0.7, 0.1, 0.0, -0.4, 1.2};
        for (auto* t : {&dcp.hueSatMap1, &dcp.hueSatMap2, &dcp.lookTable}) {
            t->hues = 6;
            t->sats = 2;
            t->vals = t == &dcp.lookTable ? 2 : 1;
            t->data.assign(size_t(t->hues) * t->sats * t->vals * 3, 1.0f);
        }
        dcp.hueSatMap1.data[0] = -30.0f;
        dcp.hueSatMap2.data[0] = 30.0f;
        dcp.lookTable.encoding = 1;
        dcp.toneCurve = {0.0f, 0.0f, 0.5f, 0.6f, 1.0f, 1.0f};
        dcp.embedPolicy = 1;
        dcp.baselineExposureOffset = -0.25;
        ctx->dcpProfile = std::make_shared<const rawrcam::color::DcpProfile>(dcp);
        frame.metadata.cameraContext = ctx;
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(frame, capture, &error);
        if (!params) return 44;
        if (!privateHas(params->privateData, "com.rawrcam.color.dcp")) return 45;
        params->rebind();
        tinydng_config cfg{};
        tinydng_error err{};
        tinydng_context* tctx = tinydng_context_create(&cfg, &err);
        uint8_t* out = nullptr;
        size_t outSize = 0;
        tinydng_document* doc = nullptr;
        tinydng_open_options oopts{};
        bool ok = tctx &&
                  tinydng_write_memory(tctx, &params->image, &params->options, &out, &outSize, &err) == TINYDNG_OK &&
                  tinydng_open_memory(tctx, out, outSize, &oopts, &doc, &err) == TINYDNG_OK;
        const tinydng_image_info* img = ok ? tinydng_image_get(doc, 0) : nullptr;
        ok = img && !img->raw.camera_calibration_present && !img->raw.has_camera_calibration2;
        std::optional<rawrcam::color::DcpProfile> back;
        if (ok) back = rawrcam::color::parseDcp(std::vector<uint8_t>(out, out + outSize), &error);
        ok = ok && back && back->name == dcp.name && back->copyright == dcp.copyright && back->illuminant1 == 17 &&
             back->illuminant2 == 21 && back->forwardMatrix2 && back->hueSatMap1.data == dcp.hueSatMap1.data &&
             back->hueSatMap2.data == dcp.hueSatMap2.data && back->lookTable.data == dcp.lookTable.data &&
             back->lookTable.vals == 2 && back->lookTable.encoding == 1 && back->toneCurve == dcp.toneCurve &&
             back->embedPolicy == 1u && back->baselineExposureOffset &&
             std::abs(*back->baselineExposureOffset + 0.25) < 1e-6;
        for (size_t i = 0; ok && i < 9; ++i)
            ok = std::abs(back->colorMatrix1[i] - dcp.colorMatrix1[i]) < 1e-6 &&
                 std::abs((*back->forwardMatrix2)[i] - (*dcp.forwardMatrix2)[i]) < 1e-6;
        if (ok) {
            const auto path = std::filesystem::temp_directory_path() / "dcp_embed_readback.dng";
            {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(out), std::streamsize(outSize));
            }
            const auto embedded = rawrcam::color::loadEmbeddedDcp(path.string(), &error);
            ok = embedded && embedded->name == dcp.name && embedded->toneCurve == dcp.toneCurve &&
                 embedded->hueSatMap1.data == dcp.hueSatMap1.data && embedded->lookTable.data == dcp.lookTable.data &&
                 embedded->bytes.size() < outSize;
            std::filesystem::remove(path);
        }
        if (doc) tinydng_document_destroy(tctx, doc);
        if (out) tinydng_buffer_free(tctx, out);
        if (tctx) tinydng_context_destroy(tctx);
        if (!ok) {
            std::cerr << "dcp embed: " << error << "\n";
            return 46;
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        capture.compression = pass == 0 ? rawrcam::encoding::dng::DngCompression::LosslessJpeg
                                        : rawrcam::encoding::dng::DngCompression::Uncompressed;
        // The frame recipe is rendered upstream and embedded verbatim; empty is omitted.
        capture.frameRecipe = pass == 0 ? R"({"version":1,"frameMarker":1})" : "";
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(good, capture, &error);
        if (!params) {
            std::cerr << error << "\n";
            return 1;
        }
        if (params->model != "X300 Ultra" || params->uniqueModel.find("V2562") == std::string::npos ||
            params->uniqueModel.find("X300 Ultra") != std::string::npos) return 20;
        params->rebind();
        // Sensor noise metadata is exported independently of merge-noise selection.
        if (params->raw.noise_profile_count != 6) return 5;
        const std::array<double, 6> expectedNoise{0.01, 0.001, 0.022, 0.0022, 0.03, 0.003};
        for (size_t i = 0; i < 6; ++i)
            if (std::abs(params->raw.noise_profile[i] - expectedNoise[i]) >= 1e-12) return 5;
        if (!privateHas(params->privateData, "com.rawrcam.processing.recipe.v1") ||
            !privateHas(params->privateData, "filmSimEnabled") ||
            !privateHas(params->privateData, "com.rawrcam.processing.resolved.v1") ||
            !privateHas(params->privateData, "aePostGain") ||
            !privateHas(params->privateData, "com.rawrcam.processing.source_role") ||
            !privateHas(params->privateData, "1234"))
            return 6;
        if (privateHas(params->privateData, "com.rawrcam.processing.frame.v1") != (pass == 0) ||
            privateHas(params->privateData, "frameMarker") != (pass == 0))
            return 6;
        // Unread replay blobs stay out of the DNG (merge replay lives in RZSL bundles).
        if (privateHas(params->privateData, "processing.merge.v1") ||
            privateHas(params->privateData, "processing.denoise.v1"))
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
                  std::string(img->raw.rawr_private_data) == "Rawr tinydng provenance" &&
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
        if (params->model != "X300 Ultra" || params->uniqueModel.find("V2562") == std::string::npos ||
            params->uniqueModel.find("X300 Ultra") != std::string::npos) return 20;
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
        // Hard 16-bit edges with exact -32768 residuals mid-tile (SSSS 16).
        for (uint32_t y = 5; y < dims.second; y += 37)
            for (uint32_t x = 3; x + 1 < dims.first; x += 53) {
                px[size_t(y) * dims.first + x] = 40000;
                px[size_t(y) * dims.first + x + 1] = 7232;
            }
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
        if (params->model != "X300 Ultra" || params->uniqueModel.find("V2562") == std::string::npos ||
            params->uniqueModel.find("X300 Ultra") != std::string::npos) return 20;
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

    // Vendor full-size mode (vivo 50 MP): the stored RAW is 2x the advertised
    // binned pixel array; the active area scales with it.
    {
        auto full = makeFrame(true);
        auto ctx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*full.metadata.cameraContext);
        ctx->geometry.pixelArrayWidth = 4;
        ctx->geometry.pixelArrayHeight = 3;
        ctx->geometry.preCorrectionActiveArray = {0, 0, 4, 3, true};
        full.metadata.cameraContext = ctx;
        error.clear();
        auto params = rawrcam::encoding::dng::makeTinyDngWriteParams(full, capture, &error);
        if (!params || !params->raw.has_active_area || params->raw.active_area[0] != 0 ||
            params->raw.active_area[1] != 0 || params->raw.active_area[2] != 6 || params->raw.active_area[3] != 8) {
            std::cerr << "full-size geometry: " << error << "\n";
            return 10;
        }
        // A non-integer ratio stays unresolved.
        auto odd = makeFrame(true);
        auto oddCtx = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*odd.metadata.cameraContext);
        oddCtx->geometry.pixelArrayWidth = 3;
        oddCtx->geometry.pixelArrayHeight = 2;
        oddCtx->geometry.preCorrectionActiveArray = {0, 0, 3, 2, true};
        odd.metadata.cameraContext = oddCtx;
        error.clear();
        if (rawrcam::encoding::dng::makeTinyDngWriteParams(odd, capture, &error) ||
            error.find("unresolved stored RAW geometry") == std::string::npos)
            return 11;
        std::cout << "DNG_CAPTURE_FULL_SIZE_GEOMETRY_PASS\n";
    }

    auto bad = makeFrame(false);
    error.clear();
    if (rawrcam::encoding::dng::makeTinyDngWriteParams(bad, capture, &error) ||
        error.find("unresolved stored RAW geometry") == std::string::npos)
        return 3;
    return 0;
}
