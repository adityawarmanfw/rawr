#include "renderer/DngSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static uint32_t littleEndian(const uint8_t* bytes, size_t count) {
    uint32_t value = 0;
    for (size_t index = 0; index < count; ++index) value |= uint32_t(bytes[index]) << (8 * index);
    return value;
}

static void append(std::vector<uint8_t>& bytes, uint32_t value, size_t count) {
    for (size_t index = 0; index < count; ++index) bytes.push_back(uint8_t(value >> (8 * index)));
}

static void makeSubIfd(std::vector<uint8_t>& bytes, bool dng) {
    using Entry = std::array<uint8_t, 12>;
    std::vector<Entry> root, raw;
    const auto offset = littleEndian(bytes.data() + 4, 4);
    const auto count = littleEndian(bytes.data() + offset, 2);
    for (uint32_t index = 0; index < count; ++index) {
        Entry entry;
        std::copy_n(bytes.data() + offset + 2 + index * 12, 12, entry.begin());
        const auto tag = littleEndian(entry.data(), 2);
        const bool documentTag = tag == 50706 || tag == 50707 || (tag >= 50721 && tag <= 50728) ||
                     tag == 50778 || tag == 50779;
        if (!documentTag) raw.push_back(entry);
        if (!dng && tag == 50706) continue;
        if (tag == 33421 || tag == 33422 || tag == 50710 || tag == 50711) continue;
        if (tag == 262) {
            entry[8] = 1;
            entry[9] = 0;
        }
        root.push_back(entry);
    }
    const auto rootOffset = uint32_t(bytes.size());
    const auto rawOffset = rootOffset + 2 + uint32_t(root.size() + 1) * 12 + 4;
    std::vector<uint8_t> link;
    append(link, 330, 2);
    append(link, 4, 2);
    append(link, 1, 4);
    append(link, rawOffset, 4);
    Entry subIfd;
    std::copy(link.begin(), link.end(), subIfd.begin());
    root.push_back(subIfd);
    std::sort(root.begin(), root.end(), [](const Entry& left, const Entry& right) {
        return littleEndian(left.data(), 2) < littleEndian(right.data(), 2);
    });
    for (const auto* entries : {&root, &raw}) {
        append(bytes, uint32_t(entries->size()), 2);
        for (const auto& entry : *entries) bytes.insert(bytes.end(), entry.begin(), entry.end());
        append(bytes, 0, 4);
    }
    for (size_t index = 0; index < 4; ++index) bytes[4 + index] = uint8_t(rootOffset >> (8 * index));
}

static void writeFixture(const std::string& path, bool subIfd = false, bool bayer = true, bool dng = true) {
    tinydng_error error{};
    auto* context = tinydng_context_create(nullptr, &error);
    tinydng_raw_info raw{};
    raw.color_matrix_present = raw.has_as_shot_neutral = 1;
    raw.color_matrix1[0] = raw.color_matrix1[4] = raw.color_matrix1[8] = 1;
    raw.has_color_matrix2 = raw.camera_calibration_present = raw.has_camera_calibration2 = 1;
    raw.color_matrix2[0] = 1.2;
    raw.color_matrix2[4] = 0.9;
    raw.color_matrix2[8] = 0.8;
    raw.camera_calibration1[0] = 1.1;
    raw.camera_calibration1[4] = 1;
    raw.camera_calibration1[8] = 0.9;
    raw.camera_calibration2[0] = 0.9;
    raw.camera_calibration2[4] = 1.1;
    raw.camera_calibration2[8] = 1.05;
    raw.has_analog_balance = 1;
    raw.analog_balance[0] = 1.1;
    raw.analog_balance[1] = 1;
    raw.analog_balance[2] = 0.9;
    raw.as_shot_neutral[0] = 1;
    raw.as_shot_neutral[1] = 1;
    raw.as_shot_neutral[2] = 0.5;
    raw.calibration_illuminant1 = 21;
    raw.calibration_illuminant2 = 17;
    tinydng_cfa cfa{};
    cfa.present = 1;
    cfa.pattern_dim[0] = cfa.pattern_dim[1] = 2;
    cfa.pattern_size = 4;
    cfa.pattern[1] = cfa.pattern[2] = 1;
    cfa.pattern[3] = 2;
    std::vector<uint16_t> pixels(32 * 32 * (bayer ? 1 : 3), 1000);
    tinydng_write_image image{};
    image.width = image.height = 32;
    image.bits_per_sample = 16;
    image.samples_per_pixel = bayer ? 1 : 3;
    image.photometric = bayer ? 32803 : 34892;
    image.data = reinterpret_cast<const uint8_t*>(pixels.data());
    image.data_size = pixels.size() * sizeof(uint16_t);
    image.cfa = bayer ? &cfa : nullptr;
    image.raw = &raw;
    tinydng_write_options options{};
    options.as_dng = 1;
    uint8_t* buffer = nullptr;
    size_t size = 0;
    const auto result = tinydng_write_memory(context, &image, &options, &buffer, &size, &error);
    if (result != TINYDNG_OK) throw std::runtime_error(error.message);
    std::vector<uint8_t> bytes(buffer, buffer + size);
    tinydng_buffer_free(context, buffer);
    tinydng_context_destroy(context);
    if (subIfd) makeSubIfd(bytes, dng);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write DNG fixture");
}

static bool rejected(const std::string& path) {
    try {
        rawrcam::renderer::DngSource source(path);
        return false;
    } catch (const std::runtime_error& error) {
        return std::strcmp(error.what(), "Not a supported Bayer DNG") == 0;
    }
}

int main() {
    auto directory = (std::filesystem::temp_directory_path() / "rawr-dng-source-XXXXXX").string();
    CHECK(mkdtemp(directory.data()));
    struct DirectoryCleanup {
        const std::string& path;
        ~DirectoryCleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto path = directory + "/source.dng";
    writeFixture(path);
    rawrcam::renderer::DngSource root(path);
    CHECK(root.read(0, 0, 32, 32) == std::vector<uint16_t>(32 * 32, 1000));
    CHECK(root.wb == (std::array<float, 3>{1, 1, 2}));
    const auto subIfdPath = directory + "/subifd.dng";
    writeFixture(subIfdPath, true);
    try {
        rawrcam::renderer::DngSource subIfd(subIfdPath);
        CHECK(!subIfd.info().raw.has_dng_version && !subIfd.info().raw.color_matrix_present);
        CHECK(subIfd.read(0, 0, 32, 32) == root.read(0, 0, 32, 32));
        CHECK(subIfd.wb == root.wb);
        for (size_t index = 0; index < root.cameraToSrgb.size(); ++index)
            CHECK(std::abs(subIfd.cameraToSrgb[index] - root.cameraToSrgb[index]) < 1e-6f);
    } catch (const std::runtime_error& error) {
        std::fprintf(stderr, "Bayer SubIFD import failed: %s\n", error.what());
        return 1;
    }
    const auto unsupportedPath = directory + "/unsupported.dng";
    writeFixture(unsupportedPath, false, false);
    CHECK(rejected(unsupportedPath));
    writeFixture(unsupportedPath, true, true, false);
    CHECK(rejected(unsupportedPath));
    std::puts("renderer_dng_source_test ok");
    return 0;
}