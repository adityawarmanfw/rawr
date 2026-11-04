#include "DiagnosticSink.h"

#include <fstream>

#include "diagnostics/logging/DiagnosticFileRotation.h"

namespace rawrcam::diagnostics {
namespace {
std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}
// Appends one line, rotating first once the file has reached the cap.
void appendCapped(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out.seekp(0, std::ios::end);
    if (static_cast<std::uint64_t>(out.tellp()) >= kDiagnosticFileCapBytes) {
        out.close();
        rotateDiagnosticFile(path);
        out.open(path, std::ios::trunc);
        if (!out) return;
    }
    out << text;
}
std::string jsonLine(const char* type, const std::string& line) {
    return std::string("{\"type\":\"") + type + "\",\"message\":\"" + escapeJson(line) + "\"}\n";
}
// A new session keeps the previous one as "<path>.1" and starts the file fresh.
void startFile(const std::string& path, const char* header) {
    rotateDiagnosticFile(path);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << header;
}
}  // namespace
DiagnosticSink::DiagnosticSink(std::string filesDir)
    : validationPath_(filesDir + "/rawrcam_validation_current.txt"),
      dcgPath_(filesDir + "/rawrcam_dcg_session_audit.jsonl"),
      pipelinePath_(filesDir + "/rawrcam_post_session_pipeline_audit.jsonl") {}
void DiagnosticSink::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) startFiles();
}
void DiagnosticSink::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) startFiles();
}
void DiagnosticSink::startFiles() const {
    startFile(dcgPath_, "{\"type\":\"dcgAuditStart\",\"version\":1}\n");
    startFile(pipelinePath_, "{\"type\":\"postSessionPipelineAuditStart\",\"version\":1}\n");
    startFile(validationPath_, "RAWRCAM_DIAGNOSTICS_VERSION=9\npersistent_diagnostics=enabled\n");
}
void DiagnosticSink::appendValidation(const std::string& line) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) appendCapped(validationPath_, line + "\n");
}
void DiagnosticSink::appendDcgSession(const std::string& line) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) appendCapped(dcgPath_, jsonLine("cameraSessionDiagnostic", line));
}
void DiagnosticSink::appendPipelineAudit(const std::string& line) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) appendCapped(pipelinePath_, jsonLine("postSessionPipelineAudit", line));
}
}  // namespace rawrcam::diagnostics
