#pragma once

#include "rml_doc.h"
#include "rml_engine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

struct DiagnosticItem {
    enum class Severity { Error, Warning, Unused };
    enum class SourceFile { Rml, Rcss };

    Severity severity = Severity::Warning;
    SourceFile source = SourceFile::Rcss;
    int line = 1;
    std::string code;
    std::string message;
    size_t byte_offset = 0;
    size_t byte_length = 0;
};

/// Real-time static + DOM diagnostics and Quick-Fix engine for RmlUI-Studio.
class RmlDiagnosticsEngine {
public:
    /// Runs all static RML/RCSS rules, DOM dead-selector queries against canvas_doc, and engine parser logs.
    static std::vector<DiagnosticItem> Analyze(
        const RmlDocument& doc,
        const RmlEngineHost* engine,
        const std::string& override_rml = "",
        const std::string& override_rcss = ""
    );

    /// Core analyzer operating on explicit sources, DOM document, and parser logs (also used by unit tests).
    static std::vector<DiagnosticItem> AnalyzeSources(
        const RmlDocument& doc,
        Rml::ElementDocument* canvas_doc,
        const std::vector<RmlEngineLogEntry>& engine_logs,
        const std::string& rml_source,
        const std::string& rcss_source,
        const RmlEngineHost* engine = nullptr
    );

    /// Removes all DiagnosticItem::Severity::Unused RCSS blocks from rcss_source in reverse byte order.
    static size_t RemoveUnusedRcssRules(
        std::string& rcss_source,
        const std::vector<DiagnosticItem>& items
    );
};
