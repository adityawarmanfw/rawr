#include "encoding/LensMetadata.h"
#include "encoding/jpeg/JpegLensMetadata.h"

#include <cassert>
#include <clocale>
#include <utility>

using namespace rawrcam::encoding;

int main() {
    const std::array<float, 2> teleSensor{9.16f, 6.87f};
       const auto teleOptics = lensIdentityOptics({22.5f}, widestLensAperture({4.0f, 2.7f}));
       const auto tele = buildLensMetadata("vivo", "X300 Ultra", 22.5f, teleOptics, teleSensor);
    assert(tele.make == "vivo");
    assert(tele.model == "vivo X300 Ultra telephoto camera 22.5mm f/2.7");
    assert(tele.focalLength35mm == 85);
    assert(buildLensMetadata("vivo", "vivo X300 Ultra", 22.5f, teleOptics, teleSensor).model == tele.model);
    assert(buildLensMetadata("vivo", "VIVO X300 Ultra", 22.5f, teleOptics, teleSensor).model ==
           "VIVO X300 Ultra telephoto camera 22.5mm f/2.7");
    assert(buildLensMetadata("  vivo ", " X300 Ultra ", 22.5f, teleOptics, teleSensor).model == tele.model);
    assert(buildLensMetadata("A", "Another", 22.5f, teleOptics, teleSensor).model ==
           "A Another telephoto camera 22.5mm f/2.7");
       assert(buildLensMetadata("Apple", "iPhone 17 Pro Max", 6.765f, lensIdentityOptics({6.765f}, widestLensAperture({1.78f})),
                             {9.75f, 7.31f}).model ==
           "Apple iPhone 17 Pro Max wide camera 6.765mm f/1.78");
       assert(buildLensMetadata("Google", "Pixel 6 Pro", 2.35f, lensIdentityOptics({2.35f}, widestLensAperture({2.2f})),
                             {5.08f, 3.81f}).model ==
           "Google Pixel 6 Pro ultra wide camera 2.35mm f/2.2");
       assert(buildLensMetadata("Example", "Phone", 4.0f, lensIdentityOptics({4.0f}, widestLensAperture({2.0f})), {9.6f, 7.2f}).model ==
           "Example Phone ultra wide camera 4mm f/2.0");
    for (const auto& [equivalent, expected] : {
             std::pair{0, "camera"}, {1, "ultra wide camera"}, {18, "ultra wide camera"},
             {19, "wide camera"}, {39, "wide camera"}, {40, "telephoto camera"},
             {149, "telephoto camera"}, {150, "ultra telephoto camera"}, {65535, "ultra telephoto camera"}}) {
        assert(std::string(lensCameraType(static_cast<std::uint16_t>(equivalent))) == expected);
    }
    assert(std::string(lensCameraType(std::nullopt)) == "camera");

    assert(!equivalentFocalLength35mm(std::nullopt, teleSensor));
    assert(!equivalentFocalLength35mm(22.5f, {}));
    assert(buildLensMetadata("vivo", "X300 Ultra", 22.5f, lensIdentityOptics({22.5f}, {}), {}).model ==
           "vivo X300 Ultra camera 22.5mm");
       assert(buildLensMetadata("vivo", "X300 Ultra", 22.5f, lensIdentityOptics({}, widestLensAperture({2.7f})), teleSensor).model ==
           "vivo X300 Ultra camera");
    assert(buildLensMetadata("vivo", "X300 Ultra", 22.5f,
                             lensIdentityOptics({6.0f, 22.5f}, widestLensAperture({1.8f, 2.7f})), teleSensor).model ==
           "vivo X300 Ultra camera");
    assert(buildLensMetadata("", "", 22.5f, teleOptics, teleSensor).model.empty());
    assert(buildLensMetadata("", "Phone", 22.5f, teleOptics, teleSensor).model ==
           "Phone telephoto camera 22.5mm f/2.7");
    const auto nonAscii = buildLensMetadata("Make", "Phone\n\xc3\xa9", 22.5f, teleOptics, teleSensor);
    assert(nonAscii.model.find('\n') == std::string::npos);
    for (unsigned char c : nonAscii.model) assert(c >= 0x20 && c <= 0x7e);

    // Decimal formatting must not inherit an application's/device's locale.
    const auto localeBefore = std::string(std::setlocale(LC_NUMERIC, nullptr));
    for (const char* locale : {"de_DE.UTF-8", "fr_FR.UTF-8", "C"}) {
        if (std::setlocale(LC_NUMERIC, locale)) {
                     assert(buildLensMetadata("vivo", "X300 Ultra", 22.5f, teleOptics, teleSensor).model == tele.model);
        }
    }
    std::setlocale(LC_NUMERIC, localeBefore.c_str());

    jpeg::JpegCaptureContext capture;
    capture.deviceMake = "vivo";
    capture.deviceModel = "X300 Ultra";
    capture.aperture = 2.7f;
    capture.focalLengthMm = 22.5f;
    capture.captureSensorSizeMm = teleSensor;
       capture.captureLensOptics = teleOptics;
       LensMetadata generated;
       const auto jpegLens = jpeg::lensMetadataForJpeg(capture, generated);
    assert(jpegLens.make == tele.make && jpegLens.model == tele.model);
    assert(jpegLens.focalLength35mm == tele.focalLength35mm);
       capture.aperture = 4.0f;
       assert(jpeg::lensMetadataForJpeg(capture, generated).model == tele.model);
       const auto zoomSpecification = lensSpecification({22.5f, 45.0f}, widestLensAperture({2.7f, 4.0f}));
       assert(zoomSpecification && (*zoomSpecification)[2] == 0 && (*zoomSpecification)[3] == 0);
       capture.aperture.reset();
       capture.focalLengthMm.reset();
       assert(jpeg::lensMetadataForJpeg(capture, generated).model == tele.model);
       assert(!jpeg::lensMetadataForJpeg(capture, generated).focalLength35mm);

    // Re-renders must preserve source names/omissions, even on another phone.
    capture.captureSensorSizeMm.reset();
    capture.lensMake = "Source maker";
    capture.lensModel = "Source lens 16.891mm f/2.8";
    capture.focalLength35mm = 200;
       auto source = jpeg::lensMetadataForJpeg(capture, generated);
    assert(source.make == capture.lensMake && source.model == capture.lensModel);
    assert(source.focalLength35mm == 200);
    capture.lensMake.clear();
    capture.lensModel.clear();
    capture.focalLength35mm.reset();
       source = jpeg::lensMetadataForJpeg(capture, generated);
    assert(source.make.empty() && source.model.empty() && !source.focalLength35mm);
}
