#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

#include "convert/DngConverter.h"
#include "metadata/GpsMetadata.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static std::vector<uint8_t> gainMap(uint32_t top, uint32_t left, uint32_t bottom, uint32_t right) {
    std::vector<uint8_t> p(76 + 4 * 4, 0);
    const uint32_t values[] = {top, left, bottom, right, 0, 1, 2, 2, 2, 2};
    for (int i = 0; i < 10; ++i)
        for (int b = 0; b < 4; ++b) p[size_t(i) * 4 + b] = uint8_t(values[i] >> (24 - 8 * b));
    p[75] = 1;  // MapPlanes
    return p;
}

static int gpsConversion() {
    auto directory = (std::filesystem::temp_directory_path() / "rawr-gps-convert-XXXXXX").string();
    CHECK(mkdtemp(directory.data()));
    const auto sourcePath = directory + "/source.dng";
    tinydng_error error{};
    auto* context = tinydng_context_create(nullptr, &error);
    CHECK(context);
    tinydng_raw_info raw{};
    raw.white_level_present = 1;
    raw.white_level[0] = 4095;
    tinydng_cfa cfa{};
    cfa.present = 1;
    cfa.pattern_dim[0] = cfa.pattern_dim[1] = 2;
    cfa.pattern_size = 4;
    cfa.pattern[0] = 0; cfa.pattern[1] = cfa.pattern[2] = 1; cfa.pattern[3] = 2;
    tinydng_exif exif{};
    exif.has_exif_version = 1;
    std::memcpy(exif.exif_version, "0300", 4);
    auto gps = rawrcam::metadata::makeGps(0, 0, -12345.0, {}, 1793059200123LL);
    std::vector<uint16_t> pixels(64 * 64, 1000);
    tinydng_write_image image{};
    image.width = image.height = 64;
    image.bits_per_sample = 16;
    image.samples_per_pixel = 1;
    image.data = reinterpret_cast<const uint8_t*>(pixels.data());
    image.data_size = pixels.size() * sizeof(uint16_t);
    image.cfa = &cfa;
    image.raw = &raw;
    image.exif = &exif;
    image.gps = &*gps;
    tinydng_write_options options{};
    options.as_dng = 1;
    for (int sourceProfile = 0; sourceProfile < 3; ++sourceProfile) {
        image.exif = sourceProfile == 1 ? nullptr : &exif;
        gps->altitude_ref = sourceProfile == 1 ? 2 : 1;
        const char* expectedVersion = sourceProfile == 2 ? "0232" : "0300";
        std::memcpy(exif.exif_version, expectedVersion, 4);
        CHECK(tinydng_write_file(context, sourcePath.c_str(), &image, &options, &error) == TINYDNG_OK);
        for (int mode = 0; mode < 4; ++mode) {
            const auto outputPath = directory + "/output" + std::to_string(mode) + ".dng";
            const auto result = mode == 0 ? rawrcam::convert::recompressDng(sourcePath, outputPath) :
                rawrcam::convert::downscaleDng(sourcePath, outputPath, static_cast<rawrcam::convert::DngDownscale>(mode - 1));
            CHECK(result.ok);
            tinydng_document* document = nullptr;
            CHECK(tinydng_open_file(context, outputPath.c_str(), nullptr, &document, &error) == TINYDNG_OK);
            const auto* loaded = tinydng_image_get(document, 0);
            CHECK(loaded && rawrcam::metadata::sameGpsRecord(loaded->gps, *gps));
            CHECK(loaded->exif.has_exif_version && std::memcmp(loaded->exif.exif_version, expectedVersion, 4) == 0);
            tinydng_document_destroy(context, document);
        }
    }
    tinydng_context_destroy(context);
    std::filesystem::remove_all(directory);
    return 0;
}

int main() {
    CHECK(gpsConversion() == 0);
    using rawrcam::convert::BinnedRaw;
    tinydng_raw_info raw{};
    raw.black_level_present = 1;
    const double black[4] = {64.0, 64.5, 65.0, 65.5};
    for (int i = 0; i < 4; ++i) raw.black_level_exact[i] = black[i];
    raw.has_black_level_repeat = 1;
    raw.black_level_repeat[0] = raw.black_level_repeat[1] = 2;
    raw.white_level_present = 1;
    raw.white_level[0] = 1023;
    raw.noise_profile_count = 2;
    raw.has_crop = 1;
    raw.crop_origin[0] = raw.crop_origin[1] = 8;
    raw.crop_size[0] = 16304;
    raw.crop_size[1] = 12272;
    tinydng_cfa cfa{};
    cfa.present = 1;
    cfa.pattern_dim[0] = cfa.pattern_dim[1] = 2;
    const uint8_t rggb[4] = {0, 1, 1, 2};
    std::memcpy(cfa.pattern, rggb, 4);

    // An ActiveArea origin of 2 halves to 1: the anchored CFA, black levels
    // and GainMap phases all move by one row and column.
    raw.has_active_area = 1;
    const uint32_t active[4] = {2, 2, 12290, 16322};
    std::memcpy(raw.active_area, active, sizeof active);
    std::vector<std::vector<uint8_t>> params;
    std::vector<tinydng_opcode> ops;
    for (uint32_t phase = 0; phase < 4; ++phase) params.push_back(gainMap(phase / 2, phase % 2, 12288, 16320));
    for (auto& p : params) ops.push_back(tinydng_opcode{2, 9, 0, 0, p.data(), p.size()});
    raw.opcodes = ops.data();
    raw.opcode_count = ops.size();

    BinnedRaw binned;
    std::string error;
    CHECK(binned.build(raw, cfa, 16324, 12292, 8162, 6146, 4, &error));
    const auto& out = binned.raw();
    CHECK(out.active_area[0] == 1 && out.active_area[1] == 1 && out.active_area[2] == 6145 && out.active_area[3] == 8161);
    CHECK(out.white_level[0] == 4092);
    CHECK(out.noise_profile_count == 0);
    CHECK(out.crop_origin[0] == 4 && out.crop_size[0] == 8152);
    // Anchored at an odd origin, RGGB becomes BGGR and blacks swap the same way.
    const uint8_t bggr[4] = {2, 1, 1, 0};
    CHECK(std::memcmp(binned.cfa().pattern, bggr, 4) == 0);
    CHECK(out.black_level_exact[0] == 65.5 * 4 && out.black_level_exact[3] == 64.0 * 4);
    CHECK(out.has_black_level_exact && out.black_level_repeat[0] == 2);
    CHECK(out.opcode_count == 4);
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t* p = out.opcodes[i].params;
        CHECK(be32(p) == ((i / 2) ^ 1u) && be32(p + 4) == ((i % 2) ^ 1u));
        CHECK(be32(p + 8) == 6144 && be32(p + 12) == 8160);
    }

    // Refused: odd ActiveArea, LinearizationTable, value-domain opcodes.
    auto odd = raw;
    odd.active_area[0] = 1;
    CHECK(!binned.build(odd, cfa, 16324, 12292, 8162, 6146, 4, &error));
    auto linear = raw;
    uint16_t table[2] = {0, 1};
    linear.linearization_table = table;
    linear.linearization_table_count = 2;
    CHECK(!binned.build(linear, cfa, 16324, 12292, 8162, 6146, 4, &error));
    auto mapped = raw;
    tinydng_opcode map{2, 7, 0, 0, params[0].data(), params[0].size()};
    mapped.opcodes = &map;
    mapped.opcode_count = 1;
    CHECK(!binned.build(mapped, cfa, 16324, 12292, 8162, 6146, 4, &error));
    std::puts("convert_binned_raw_test ok");
    return 0;
}
