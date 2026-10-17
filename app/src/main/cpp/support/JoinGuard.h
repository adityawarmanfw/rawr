#pragma once
#include <thread>
#include <vector>
namespace rawrcam::support {
// Joins every started worker when it goes out of scope. Declare it right
// after the thread vector: if starting a later worker throws, the running
// ones are joined instead of destroyed joinable (which calls std::terminate).
class JoinGuard final {
   public:
    explicit JoinGuard(std::vector<std::thread>& threads) noexcept : threads_(threads) {}
    ~JoinGuard() {
        for (auto& thread : threads_)
            if (thread.joinable()) thread.join();
    }
    JoinGuard(const JoinGuard&) = delete;
    JoinGuard& operator=(const JoinGuard&) = delete;

   private:
    std::vector<std::thread>& threads_;
};
}  // namespace rawrcam::support
