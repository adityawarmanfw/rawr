#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>

#include "imaging/RawFrameLease.h"
#include "metadata/FrameMetadataSnapshot.h"
namespace rawrcam::imaging {
class FrameIngressQueue final {
   public:
    using Payload = std::variant<std::monostate, RawFrameLease, metadata::FrameMetadataSnapshot>;
    struct Event {
        Payload payload;
        std::chrono::steady_clock::time_point enqueuedAt;
    };
    struct Drops {
        uint64_t images = 0;
        uint64_t metadata = 0;
    };
    using Consume = std::function<void(Event, Drops)>;
    explicit FrameIngressQueue(Consume consume);
    ~FrameIngressQueue();
    FrameIngressQueue(const FrameIngressQueue&) = delete;
    FrameIngressQueue& operator=(const FrameIngressQueue&) = delete;
    void start();
    void stop();
    void setLowPowerIdle(bool idle);
    void enqueue(AcquiredRawFrame frame);
    bool enqueue(const metadata::FrameMetadataSnapshot& metadata);
    uint64_t latestTimestamp(uint64_t generation) const;

   private:
    void run();
    Consume consume_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Event> queue_;
    Drops drops_;
    uint64_t latestTimestamp_ = 0, generation_ = 0;
    bool stopped_ = false;
    bool lowPowerIdle_ = false;
    std::thread worker_;
};
}  // namespace rawrcam::imaging
