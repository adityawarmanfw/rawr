#pragma once
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace rawrcam::diagnostics {
class DiagnosticSink {
   public:
    explicit DiagnosticSink(std::string filesDir);
    void initialize();
    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void appendValidation(const std::string& line);
    void appendDcgSession(const std::string& line);
    void appendPipelineAudit(const std::string& line);
    // Writes buffered lines to disk; call off the frame path (viewfinder stop).
    void flush();

   private:
    struct File {
        explicit File(std::string p) : path(std::move(p)) {}
        std::string path;
        std::ofstream stream;
        std::uint64_t bytes = 0;
        void start(const char* header);
        void append(const std::string& text);
    };
    void startFiles();

    File validation_;
    File dcg_;
    File pipeline_;
    std::mutex mutex_;
    std::atomic<bool> enabled_{false};
};
}  // namespace rawrcam::diagnostics
