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
std::string jsonLine(const char* type, const std::string& line) {
    return std::string("{\"type\":\"") + type + "\",\"message\":\"" + escapeJson(line) + "\"}\n";
}
}  // namespace
// Files stay open and buffered: diagnostics record all the time, and an
// open/close per line on the camera and frame paths stalls on slow storage
// (worst while a large DNG is being written). flush() completes them.
void DiagnosticSink::File::start(const char* header) {
    if (stream.is_open()) stream.close();
    rotateDiagnosticFile(path);
    stream.open(path, std::ios::out | std::ios::trunc);
    stream << header;
    bytes = std::char_traits<char>::length(header);
}
void DiagnosticSink::File::append(const std::string& text) {
    if (!stream.is_open()) return;
    if (bytes >= kDiagnosticFileCapBytes) start("");
    stream << text;
    bytes += text.size();
}
DiagnosticSink::DiagnosticSink(std::string filesDir)
    : validation_(filesDir + "/rawrcam_validation_current.txt"),
      dcg_(filesDir + "/rawrcam_dcg_session_audit.jsonl"),
      pipeline_(filesDir + "/rawrcam_post_session_pipeline_audit.jsonl") {}
void DiagnosticSink::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) startFiles();
}
void DiagnosticSink::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) {
        startFiles();
    } else {
        for (File* f : {&validation_, &dcg_, &pipeline_}) f->stream.close();
    }
}
void DiagnosticSink::startFiles() {
    dcg_.start("{\"type\":\"dcgAuditStart\",\"version\":1}\n");
    pipeline_.start("{\"type\":\"postSessionPipelineAuditStart\",\"version\":1}\n");
    validation_.start("RAWRCAM_DIAGNOSTICS_VERSION=9\npersistent_diagnostics=enabled\n");
}
void DiagnosticSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (File* f : {&validation_, &dcg_, &pipeline_}) {
        if (f->stream.is_open()) f->stream.flush();
    }
}
void DiagnosticSink::appendValidation(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) validation_.append(line + "\n");
}
void DiagnosticSink::appendDcgSession(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) dcg_.append(jsonLine("cameraSessionDiagnostic", line));
}
void DiagnosticSink::appendPipelineAudit(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) pipeline_.append(jsonLine("postSessionPipelineAudit", line));
}
}  // namespace rawrcam::diagnostics
