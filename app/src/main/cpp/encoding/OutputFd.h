#pragma once
#include <utility>

#include "support/UniqueFd.h"
namespace rawrcam::encoding {
// Output destination carried by an encoder context. Moving transfers the
// descriptor. Copying yields an empty one: copies are journal/persistence
// snapshots of the capture intent and never own the file.
class OutputFd final {
   public:
    OutputFd() noexcept = default;
    explicit OutputFd(int fd) noexcept : fd_(fd) {}
    OutputFd(const OutputFd&) noexcept {}
    OutputFd& operator=(const OutputFd& other) noexcept {
        if (this != &other) fd_.reset();
        return *this;
    }
    OutputFd(OutputFd&&) noexcept = default;
    OutputFd& operator=(OutputFd&&) noexcept = default;

    int get() const noexcept { return fd_.get(); }
    bool valid() const noexcept { return fd_.valid(); }
    int release() noexcept { return fd_.release(); }
    void reset(int fd = -1) noexcept { fd_.reset(fd); }

   private:
    support::UniqueFd fd_{-1};
};
}  // namespace rawrcam::encoding
