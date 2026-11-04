// Diagnostics surface, audit sinks.
#include "session/SessionEngine.h"

namespace rawrcam::session {

void SessionEngine::recordPipelineAuditLine(const std::string& line) {
    if (diagnosticSink_) diagnosticSink_->appendPipelineAudit(line);
}

void SessionEngine::recordDiagnosticLine(const std::string& line) {
    if (diagnosticSink_) {
        diagnosticSink_->appendValidation(line);
        diagnosticSink_->appendDcgSession(line);
    }
}

void SessionEngine::appendDiagnostic(const std::string& line) {
    if (diagnosticSink_) diagnosticSink_->appendValidation(line);
}

void SessionEngine::flushDiagnostics() {
    if (diagnosticSink_) diagnosticSink_->flush();
    if (frameAuditWriter_) frameAuditWriter_->flush();
}

}  // namespace rawrcam::session
