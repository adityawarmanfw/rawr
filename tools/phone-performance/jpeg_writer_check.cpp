// Exercise the production asynchronous writer with host adapters for logcat and
// Android properties. Actual encode, metadata, file IO, fsync and ownership run.
#include "encoding/jpeg/JpegCaptureWriter.h"
#include "encoding/jpeg/StripJpegEncoder.h"
#include <turbojpeg.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using namespace rawrcam::encoding::jpeg;

// Interpose only in this executable; production code still calls POSIX write.
// A one-shot error allows a mistaken SDR fallback after a partial write to
// succeed, so the test detects the corrupt-file behavior rather than ENOSPC.
static std::atomic<int> faultFd{-1};
static std::size_t bytesBeforeFault = 0;
static bool faultFired = false;
extern "C" ssize_t write(int fd, const void* data, size_t size) {
    using Write = ssize_t (*)(int, const void*, size_t);
    static const auto realWrite = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
    if (!realWrite) { errno = ENOSYS; return -1; }
    if (fd != faultFd.load() || faultFired) return realWrite(fd, data, size);
    if (bytesBeforeFault == 0) {
        faultFired = true;
        errno = ENOSPC;
        return -1;
    }
    const auto count = realWrite(fd, data, std::min(size, bytesBeforeFault));
    if (count > 0) bytesBeforeFault -= static_cast<std::size_t>(count);
    return count;
}

struct File {
    std::string path;
    int fd;
    File() {
        const char* dir = std::getenv("TMPDIR");
        path = std::string(dir ? dir : ".") + "/rawr-writer-XXXXXX";
        fd = ::mkstemp(path.data());
        assert(fd >= 0);
    }
    ~File() { if (fd >= 0) ::close(fd); ::unlink(path.c_str()); }
    int take() { const int result = fd; fd = -1; return result; }
    std::vector<unsigned char> bytes() const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};

static JpegWriteCompletion wait(JpegCaptureWriter& writer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto completed = writer.pollCompletion()) return *completed;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "writer timed out");
    return {};
}
static JpegCaptureContext context(int fd, ChromaSubsampling subsampling) {
    JpegCaptureContext result;
    result.outputFd.reset(fd);
    result.quality = 98;
    result.subsampling = subsampling;
    result.imageDescription = "rawr writer quality fixture";
    result.deviceMake = "fixture make";
    result.deviceModel = "fixture model";
    result.exifOrientation = 6;
    return result;
}
static std::vector<unsigned char> reference(const std::vector<unsigned char>& rgba, int w, int h, int subsamp) {
    unsigned char* jpeg = nullptr;
    std::size_t size = 0;
    int strips = 0;
    std::string error;
    if (!encodeStripsRgba8(rgba.data(), w, h, 98, subsamp, &jpeg, &size, &strips, &error)) {
        tjhandle handle = tj3Init(TJINIT_COMPRESS);
        assert(handle);
        assert(tj3Set(handle, TJPARAM_QUALITY, 98) == 0);
        assert(tj3Set(handle, TJPARAM_SUBSAMP, subsamp) == 0);
        assert(tj3Set(handle, TJPARAM_FASTDCT, 0) == 0);
        assert(tj3Compress8(handle, rgba.data(), w, 0, h, TJPF_RGBA, &jpeg, &size) == 0);
        tj3Destroy(handle);
    }
    std::vector<unsigned char> bytes(jpeg, jpeg + size);
    tj3Free(jpeg);
    return bytes;
}
static bool contains(const std::vector<unsigned char>& bytes, const std::string& text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

int main() {
    JpegCaptureWriter writer;
    for (const int w : {640, 1001, 1920}) {
        const int h = w == 640 ? 480 : (w == 1001 ? 1001 : 1080);
        std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<unsigned char>((i * 17 + i / 97) & 255);
        for (const auto ss : {ChromaSubsampling::Yuv444, ChromaSubsampling::Yuv422, ChromaSubsampling::Yuv420}) {
            File file;
            const int ownedFd = file.take();
            assert(writer.start(1, rgba.data(), rgba.size(), w, h, context(ownedFd, ss)));
            File rejected;
            assert(!writer.start(2, rgba.data(), rgba.size(), w, h, context(rejected.take(), ss)));
            const auto done = wait(writer);
            assert(done.success && done.requestId == 1 && done.fileBytes > 0);
            assert(!writer.busy());
            assert(::fcntl(ownedFd, F_GETFD) == -1 && errno == EBADF);
            const auto bytes = file.bytes();
            assert(bytes.size() == done.fileBytes);
            assert(contains(bytes, "rawr writer quality fixture"));
            assert(contains(bytes, "fixture make") && contains(bytes, "fixture model"));
            // Remove the EXIF/XMP APP1 segments inserted after SOI. Everything
            // else, including entropy, must match the contiguous encoder exactly.
            std::size_t offset = 2;
            for (int marker = 0; marker < 2; ++marker) {
                assert(bytes.size() > offset + 4 && bytes[offset] == 0xff && bytes[offset + 1] == 0xe1);
                offset += 2u + (static_cast<std::size_t>(bytes[offset + 2]) << 8) + bytes[offset + 3];
                assert(offset < bytes.size());
            }
            const int subsamp = ss == ChromaSubsampling::Yuv444 ? TJSAMP_444 :
                (ss == ChromaSubsampling::Yuv422 ? TJSAMP_422 : TJSAMP_420);
            const auto expected = reference(rgba, w, h, subsamp);
            assert(expected.size() == bytes.size() - offset + 2);
            assert(std::memcmp(expected.data() + 2, bytes.data() + offset, expected.size() - 2) == 0);
        }
    }
    std::vector<unsigned char> rgba(1920 * 1080 * 4, 127);
    File unwritable;
    const int readFd = ::open(unwritable.path.c_str(), O_RDONLY);
    assert(readFd >= 0);
    assert(writer.start(3, rgba.data(), rgba.size(), 1920, 1080, context(readFd, ChromaSubsampling::Yuv422)));
    const auto failed = wait(writer);
    assert(!failed.success && failed.error.find("write failed") != std::string::npos);
    assert(unwritable.bytes().empty());
    assert(::fcntl(readFd, F_GETFD) == -1 && errno == EBADF);
    File tooLarge;
    auto oversized = context(tooLarge.take(), ChromaSubsampling::Yuv422);
    oversized.imageDescription.assign(70000, 'X');
    assert(writer.start(4, rgba.data(), rgba.size(), 1920, 1080, std::move(oversized)));
    assert(!wait(writer).success);
    assert(tooLarge.bytes().empty());
    File cancelled;
    assert(writer.start(5, rgba.data(), rgba.size(), 1920, 1080, context(cancelled.take(), ChromaSubsampling::Yuv422)));
    writer.reset();
    assert(!writer.busy() && !writer.pollCompletion());
    // UltraHDR writes metadata and both encoded payloads as borrowed segments.
    File hdr;
    auto hdrContext = context(hdr.take(), ChromaSubsampling::Yuv422);
    hdrContext.ultraHdr.enabled = true;
    std::vector<unsigned char> map(64 * 48 * 4, 127);
    assert(writer.startUltraHdr(6, rgba.data(), rgba.size(), 1920, 1080, map.data(), map.size(), 64, 48, std::move(hdrContext)));
    assert(wait(writer).success);
    const auto hdrBytes = hdr.bytes();
    assert(contains(hdrBytes, "MPF"));
    const auto expectedBase = reference(rgba, 1920, 1080, TJSAMP_422);
    tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);
    assert(decoder);
    std::vector<unsigned char> hdrDecoded(rgba.size()), expectedDecoded(rgba.size());
    assert(tj3DecompressHeader(decoder, hdrBytes.data(), hdrBytes.size()) == 0);
    assert(tj3Get(decoder, TJPARAM_JPEGWIDTH) == 1920 && tj3Get(decoder, TJPARAM_JPEGHEIGHT) == 1080);
    assert(tj3Decompress8(decoder, hdrBytes.data(), hdrBytes.size(), hdrDecoded.data(), 0, TJPF_RGBA) == 0);
    assert(tj3Decompress8(decoder, expectedBase.data(), expectedBase.size(), expectedDecoded.data(), 0, TJPF_RGBA) == 0);
    assert(hdrDecoded == expectedDecoded);
    tj3Destroy(decoder);
    for (const std::size_t prefix : {std::size_t(0), std::size_t(7), hdrBytes.size() - 16}) {
        File file;
        const int fd = file.take();
        bytesBeforeFault = prefix;
        faultFired = false;
        faultFd.store(fd);
        auto request = context(fd, ChromaSubsampling::Yuv422);
        request.ultraHdr.enabled = true;
        assert(writer.startUltraHdr(9, rgba.data(), rgba.size(), 1920, 1080,
                                   map.data(), map.size(), 64, 48, std::move(request)));
        const auto done = wait(writer);
        faultFd.store(-1);
        assert(faultFired);
        const auto bytes = file.bytes();
        if (prefix == 0) {
            assert(done.success && done.fileBytes == bytes.size());
            assert(!contains(bytes, "MPF"));
            decoder = tj3Init(TJINIT_DECOMPRESS);
            assert(decoder);
            assert(tj3DecompressHeader(decoder, bytes.data(), bytes.size()) == 0);
            assert(tj3Decompress8(decoder, bytes.data(), bytes.size(), hdrDecoded.data(), 0, TJPF_RGBA) == 0);
            assert(hdrDecoded == expectedDecoded);
            tj3Destroy(decoder);
        } else {
            assert(!done.success && done.error.find("write failed") != std::string::npos);
            assert(bytes.size() == prefix);  // No appended SDR after any partial segment.
        }
        assert(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    }
    File hdrUnwritable;
    const int hdrReadFd = ::open(hdrUnwritable.path.c_str(), O_RDONLY);
    assert(hdrReadFd >= 0);
    auto hdrFailure = context(hdrReadFd, ChromaSubsampling::Yuv422);
    hdrFailure.ultraHdr.enabled = true;
    assert(writer.startUltraHdr(7, rgba.data(), rgba.size(), 1920, 1080, map.data(), map.size(), 64, 48, std::move(hdrFailure)));
    const auto hdrFailed = wait(writer);
    assert(!hdrFailed.success && hdrFailed.error.find("write failed") != std::string::npos);
    assert(hdrUnwritable.bytes().empty());
    assert(::fcntl(hdrReadFd, F_GETFD) == -1 && errno == EBADF);
    File hdrFallback;
    auto fallbackContext = context(hdrFallback.take(), ChromaSubsampling::Yuv422);
    fallbackContext.ultraHdr.enabled = true;
    fallbackContext.ultraHdr.gamma = 0;  // Invalid mux metadata, before any write.
    assert(writer.startUltraHdr(8, rgba.data(), rgba.size(), 1920, 1080, map.data(), map.size(), 64, 48, std::move(fallbackContext)));
    const auto fallbackDone = wait(writer);
    assert(fallbackDone.success && fallbackDone.fileBytes == hdrFallback.bytes().size());
    assert(!contains(hdrFallback.bytes(), "MPF"));
    assert(contains(hdrFallback.bytes(), "rawr writer quality fixture"));
    std::puts("PASS: writer metadata/entropy, fallback, reuse, rejected destinations, write failure, oversize metadata, reset, UltraHDR");
}
