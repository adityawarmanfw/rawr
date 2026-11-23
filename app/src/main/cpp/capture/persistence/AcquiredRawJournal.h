#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/UniqueFd.h"

namespace rawrcam::capture::persistence {
struct CaptureJob;
// A pending journal, not a second RAW store. Append each acquired camera frame
// once; commit appends the finalized recipe and atomically publishes this inode.
class AcquiredRawJournal {
   public:
    AcquiredRawJournal(const std::string& filesDir, uint64_t frameBytes, uint32_t frames);
    ~AcquiredRawJournal();
    AcquiredRawJournal(const AcquiredRawJournal&) = delete;
    AcquiredRawJournal& operator=(const AcquiredRawJournal&) = delete;
    void append(const std::vector<uint8_t>& pixels);
    // One frame fewer will be appended (the camera returned an unusable one).
    // Safe beside an append running on another thread (the writes overlap the next frame's work).
    void dropExpected() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected_ > count_) --expected_;
    }
    void commit(const std::string& path, CaptureJob& job);
    const std::shared_ptr<void>& reservation() const { return reservation_; }
    const std::string& pendingPath() const { return path_; }

   private:
    support::UniqueFd fd_;
    std::string path_;
    uint64_t bytes_;
    std::mutex mutex_;  // guards expected_ and count_; appends themselves are one at a time
    uint32_t expected_, count_ = 0;
    std::shared_ptr<void> reservation_;
};
}  // namespace rawrcam::capture::persistence
