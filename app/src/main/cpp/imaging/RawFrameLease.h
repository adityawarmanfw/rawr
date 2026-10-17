#pragma once
#include <utility>

#include "imaging/RawFrameIngress.h"
#include "support/UniqueFd.h"
namespace rawrcam::imaging {
// AHardwareBuffer is borrowed from the owned AImage. Release transfers both with its fence.
class RawFrameLease final {
   public:
    explicit RawFrameLease(AcquiredRawFrame frame) noexcept : frame_(frame), fence_(frame.acquireFenceFd) {}
    ~RawFrameLease() {
        if (frame_.image) AImage_delete(frame_.image);
    }
    RawFrameLease(const RawFrameLease&) = delete;
    RawFrameLease& operator=(const RawFrameLease&) = delete;
    RawFrameLease(RawFrameLease&& other) noexcept : RawFrameLease(other.release()) {}
    RawFrameLease& operator=(RawFrameLease&& other) noexcept {
        if (this != &other) {
            fence_.reset();
            if (frame_.image) AImage_delete(frame_.image);
            frame_ = other.release();
            fence_.reset(frame_.acquireFenceFd);
        }
        return *this;
    }
    const AcquiredRawFrame& get() const noexcept { return frame_; }
    AImage* image() const noexcept { return frame_.image; }
    AHardwareBuffer* ahb() const noexcept { return frame_.ahb; }
    int fence() const noexcept { return fence_.get(); }
    explicit operator bool() const noexcept { return frame_.image != nullptr; }
    AcquiredRawFrame release() noexcept {
        auto frame = std::exchange(frame_, {});
        frame.acquireFenceFd = fence_.release();
        return frame;
    }
    void reset() noexcept { *this = RawFrameLease(AcquiredRawFrame{}); }
    // Closes the held fence (if different) and adopts fd.
    void resetFence(int fd = -1) noexcept {
        fence_.reset(fd);
        frame_.acquireFenceFd = fd;
    }
    // Moves the image out without its fence; this lease keeps only the fence.
    RawFrameLease takeImage() noexcept {
        AcquiredRawFrame image = frame_;
        image.acquireFenceFd = -1;
        frame_.image = nullptr;
        frame_.ahb = nullptr;
        return RawFrameLease(image);
    }

   private:
    AcquiredRawFrame frame_;
    support::UniqueFd fence_;
};
}  // namespace rawrcam::imaging
