#include "encoding/dng/DngCaptureWriter.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <tinydng.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "encoding/dng/DngMetadataAdapter.h"

namespace rawrcam::encoding::dng {
namespace {
// Seekable-fd sink for the TinyDNG streaming writer. Coalesces the many
// small encoder/header writes into 1MB pwrite batches; large strips bypass
// the buffer. Each DNG byte is written exactly once, in increasing offset
// order, except the final header patch at offset 0 (which flushes first).
class FdWriteIo {
   public:
    explicit FdWriteIo(int fd) : fd_(fd) { buffer_.reserve(kBufferBytes); }

    static size_t writeThunk(tinydng_write_io* io, uint64_t off, const void* data, size_t len) {
        return static_cast<FdWriteIo*>(io->backend)->write(off, data, len);
    }
    static uint64_t sizeThunk(tinydng_write_io* io) { return static_cast<FdWriteIo*>(io->backend)->highWater(); }
    tinydng_write_io io() {
        tinydng_write_io out{};
        out.write = &FdWriteIo::writeThunk;
        out.size = &FdWriteIo::sizeThunk;
        out.close = nullptr;
        out.backend = this;
        return out;
    }

    size_t write(uint64_t off, const void* data, size_t len) {
        if (len == 0) return 0;
        if (!buffer_.empty() && off != pos_ && !flush()) return 0;
        if (len >= kBufferBytes && buffer_.empty()) {
            if (!writeAll(off, data, len)) return 0;
            pos_ = off + len;
            highWater_ = std::max(highWater_, pos_);
            return len;
        }
        if (buffer_.empty() && off != pos_) {
            // Small non-sequential write (the final header patch): direct.
            if (!writeAll(off, data, len)) return 0;
            pos_ = off + len;
            highWater_ = std::max(highWater_, pos_);
            return len;
        }
        if (buffer_.size() + len > kBufferBytes && !flush()) return 0;
        if (buffer_.empty()) {
            bufOff_ = off;
        } else if (off != pos_) {
            return 0;
        }
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + len);
        pos_ = off + len;
        highWater_ = std::max(highWater_, pos_);
        return len;
    }
    bool flush() {
        if (buffer_.empty()) return true;
        const bool ok = writeAll(bufOff_, buffer_.data(), buffer_.size());
        buffer_.clear();
        return ok;
    }
    uint64_t highWater() const { return highWater_; }

   private:
    static constexpr size_t kBufferBytes = 1u << 20;
    bool writeAll(uint64_t off, const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        size_t remaining = len;
        uint64_t cur = off;
        while (remaining > 0) {
            const ssize_t n = ::pwrite(fd_, p, remaining, static_cast<off_t>(cur));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += static_cast<size_t>(n);
            cur += static_cast<uint64_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
    int fd_ = -1;
    uint64_t pos_ = 0;
    uint64_t highWater_ = 0;
    uint64_t bufOff_ = 0;
    std::vector<uint8_t> buffer_;
};

std::string errorText(const tinydng_error& e) {
    return e.message[0] ? std::string("tinydng: ") + e.message : std::string("tinydng_write_failed");
}
}  // namespace

DngCaptureWriter::~DngCaptureWriter() { joinWorker(); }

bool DngCaptureWriter::busy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

bool DngCaptureWriter::start(std::shared_ptr<const rawrcam::imaging::RawSnapshot> frame, DngCaptureContext context) {
    if (!frame || context.outputFd < 0) {
        if (context.outputFd >= 0) ::close(context.outputFd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || completion_) {
            ::close(context.outputFd);
            return false;
        }
        busy_ = true;
    }
    const int ownedFd = context.outputFd;
    try {
        worker_ = std::thread([this, frame = std::move(frame), context = std::move(context)]() mutable {
            // Background priority: file encoding must never preempt camera or
            // preview threads, especially on throttled silicon.
            ::setpriority(PRIO_PROCESS, 0, 10);
            DngWriteCompletion done{};
            done.requestId = frame->requestId;
            done.displayName = context.displayName;
            const int fd = context.outputFd;
            try {
                std::string adaptError;
                auto params = makeTinyDngWriteParams(*frame, context, &adaptError);
                if (!params) {
                    done.error = "metadata_adapter: " + adaptError;
                } else {
                    params->rebind();
                    const auto writeStart = std::chrono::steady_clock::now();
                    tinydng_error err{};
                    tinydng_config cfg{};
                    tinydng_context* tctx = tinydng_context_create(&cfg, &err);
                    if (!tctx) {
                        done.error = errorText(err);
                    } else {
                        FdWriteIo fdio(fd);
                        tinydng_write_io io = fdio.io();
                        tinydng_writer* writer = nullptr;
                        tinydng_status st = tinydng_writer_create(tctx, io, &params->image, &params->options,
                                                                  &params->tiling, &writer, &err);
                        uint64_t endBytes = 0;
                        if (st != TINYDNG_OK) {
                            done.error = errorText(err);
                        } else {
                            st = writeTinyDngPayload(writer, *params, &err);
                            if (st == TINYDNG_OK) st = tinydng_writer_finish(writer, &err);
                            if (st != TINYDNG_OK) {
                                done.error = errorText(err);
                            } else if (!fdio.flush()) {
                                done.error = "output: failed writing buffered DNG bytes";
                            } else {
                                endBytes = fdio.highWater();
                            }
                        }
                        if (done.error.empty()) {
                            // Compressed payloads vary in size; drop any stale
                            // tail when the fd is reused (MediaStore).
                            if (::ftruncate(fd, static_cast<off_t>(endBytes)) != 0) {
                                done.error = "ftruncate failed: " + std::string(std::strerror(errno));
                            } else {
                                done.fileBytes = endBytes;
                            }
                        }
                        tinydng_context_destroy(tctx);
                    }
                    const auto writeEnd = std::chrono::steady_clock::now();
                    done.writeMs = std::chrono::duration<double, std::milli>(writeEnd - writeStart).count();
                    if (done.error.empty() && done.fileBytes == 0) {
                        struct stat stbuf{};
                        if (::fstat(fd, &stbuf) == 0 && stbuf.st_size >= 0)
                            done.fileBytes = static_cast<uint64_t>(stbuf.st_size);
                    }
                    if (done.error.empty()) {
                        const auto fsyncStart = std::chrono::steady_clock::now();
                        if (::fsync(fd) != 0) done.error = "fsync failed: " + std::string(std::strerror(errno));
                        const auto fsyncEnd = std::chrono::steady_clock::now();
                        done.fsyncMs = std::chrono::duration<double, std::milli>(fsyncEnd - fsyncStart).count();
                    }
                    // (fileBytes fallback above covers unexpected paths; the
                    // primary value is the writer high-water mark.)
                }
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "dng_encoding_failed";
            }
            if (::close(fd) != 0 && done.error.empty())
                done.error = "close failed: " + std::string(std::strerror(errno));
            done.success = done.error.empty();
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = std::move(done);
            busy_ = false;
        });
    } catch (...) {
        ::close(ownedFd);
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return false;
    }
    return true;
}

std::optional<DngWriteCompletion> DngCaptureWriter::pollCompletion() {
    std::optional<DngWriteCompletion> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completion_) return std::nullopt;
        out = std::move(completion_);
        completion_.reset();
    }
    joinWorker();
    return out;
}

void DngCaptureWriter::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace rawrcam::encoding::dng
