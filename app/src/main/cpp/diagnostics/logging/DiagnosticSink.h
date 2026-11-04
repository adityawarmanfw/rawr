#pragma once
#include <atomic>
#include <mutex>
#include <string>

namespace rawrcam::diagnostics {
class DiagnosticSink {
   public:
    explicit DiagnosticSink(std::string filesDir);
    void initialize();
    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void appendValidation(const std::string& line) const;
    void appendDcgSession(const std::string& line) const;
    void appendPipelineAudit(const std::string& line) const;

   private:
    void startFiles() const;

    std::string validationPath_;
    std::string dcgPath_;
    std::string pipelinePath_;
    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
};
}  // namespace rawrcam::diagnostics
