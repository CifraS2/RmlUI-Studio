#include "rml_diagnostics.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string TrimWhitespace(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string NormalizeVPathLocal(const std::string& path) {
    std::string s = path;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    if (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}

std::string StripImportantSuffix(const std::string& val) {
    std::string t = TrimWhitespace(val);
    std::string low = ToLowerAscii(t);
    constexpr const char* kImp = "!important";
    if (low.size() >= 10 && low.compare(low.size() - 10, 10, kImp) == 0) {
        return TrimWhitespace(t.substr(0, t.size() - 10));
    }
    return t;
}

struct ParsedDecl {
    int line = 1;
    size_t offset = 0;
    size_t length = 0;
    std::string prop;
    std::string val;
};

struct ParsedRcssRule {
    bool is_at_rule = false;
    int start_line = 1;
    size_t rule_start_offset = 0;
    size_t rule_length = 0;
    std::string selector_text;
    std::vector<ParsedDecl> decls;
};

std::vector<ParsedRcssRule> ScanRcssRules(const std::string& src) {
    std::vector<ParsedRcssRule> rules;
    const size_t n = src.size();
    size_t i = 0;
    if (n >= 3 && static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        i = 3;
    }

    enum class LexState { Normal, InSingleQuote, InDoubleQuote, InBlockComment };
    LexState state = LexState::Normal;
    int cur_line = 1;
    int brace_depth = 0;
    int paren_depth = 0;

    std::string sel_buf;
    bool has_sel_start = false;
    size_t sel_start_off = 0;
    int sel_start_line = 1;

    ParsedRcssRule cur_rule;
    bool cur_rule_valid = false;
    std::string decl_buf;
    bool has_decl_start = false;
    size_t decl_start_off = 0;
    int decl_start_line = 1;

    auto flush_decl = [&](size_t end_off) {
        if (!cur_rule_valid || cur_rule.is_at_rule) {
            decl_buf.clear();
            has_decl_start = false;
            return;
        }
        std::string trimmed = TrimWhitespace(decl_buf);
        if (!trimmed.empty() && has_decl_start) {
            // Find top-level colon outside quotes and parens
            int pd = 0;
            bool sq = false, dq = false;
            size_t colon_pos = std::string::npos;
            for (size_t k = 0; k < decl_buf.size(); ++k) {
                char ch = decl_buf[k];
                if (sq) {
                    if (ch == '\\' && k + 1 < decl_buf.size()) { ++k; continue; }
                    if (ch == '\'') sq = false;
                } else if (dq) {
                    if (ch == '\\' && k + 1 < decl_buf.size()) { ++k; continue; }
                    if (ch == '"') dq = false;
                } else {
                    if (ch == '\'') sq = true;
                    else if (ch == '"') dq = true;
                    else if (ch == '(') ++pd;
                    else if (ch == ')' && pd > 0) --pd;
                    else if (ch == ':' && pd == 0) {
                        colon_pos = k;
                        break;
                    }
                }
            }
            if (colon_pos != std::string::npos) {
                std::string p = ToLowerAscii(TrimWhitespace(decl_buf.substr(0, colon_pos)));
                std::string v = TrimWhitespace(decl_buf.substr(colon_pos + 1));
                if (!p.empty()) {
                    size_t len = (end_off > decl_start_off) ? (end_off - decl_start_off) : decl_buf.size();
                    cur_rule.decls.push_back({decl_start_line, decl_start_off, len, p, v});
                }
            }
        }
        decl_buf.clear();
        has_decl_start = false;
    };

    auto push_sel_char = [&](char ch, size_t off) {
        if (!has_sel_start && !std::isspace(static_cast<unsigned char>(ch))) {
            has_sel_start = true;
            sel_start_off = off;
            sel_start_line = cur_line;
        }
        if (has_sel_start) sel_buf.push_back(ch);
    };

    auto push_decl_char = [&](char ch, size_t off) {
        if (!has_decl_start && !std::isspace(static_cast<unsigned char>(ch))) {
            has_decl_start = true;
            decl_start_off = off;
            decl_start_line = cur_line;
        }
        if (has_decl_start) decl_buf.push_back(ch);
    };

    while (i < n) {
        char c = src[i];
        if (c == '\n') {
            // Line counter incremented after handling comment/quote transitions cleanly below
        }

        if (state == LexState::InBlockComment) {
            if (c == '\n') ++cur_line;
            if (c == '*' && i + 1 < n && src[i + 1] == '/') {
                i += 2;
                state = LexState::Normal;
                continue;
            }
            ++i;
            continue;
        }

        if (state == LexState::InSingleQuote || state == LexState::InDoubleQuote) {
            char q = (state == LexState::InSingleQuote) ? '\'' : '"';
            if (brace_depth == 0) push_sel_char(c, i);
            else if (brace_depth == 1) push_decl_char(c, i);
            if (c == '\n') ++cur_line;
            if (c == '\\' && i + 1 < n) {
                if (src[i + 1] == '\n') ++cur_line;
                if (brace_depth == 0) push_sel_char(src[i + 1], i + 1);
                else if (brace_depth == 1) push_decl_char(src[i + 1], i + 1);
                i += 2;
                continue;
            }
            if (c == q) state = LexState::Normal;
            ++i;
            continue;
        }

        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            state = LexState::InBlockComment;
            continue;
        }
        if (c == '\'') {
            state = LexState::InSingleQuote;
            if (brace_depth == 0) push_sel_char(c, i);
            else if (brace_depth == 1) push_decl_char(c, i);
            ++i;
            continue;
        }
        if (c == '"') {
            state = LexState::InDoubleQuote;
            if (brace_depth == 0) push_sel_char(c, i);
            else if (brace_depth == 1) push_decl_char(c, i);
            ++i;
            continue;
        }

        if (c == '(') ++paren_depth;
        else if (c == ')' && paren_depth > 0) --paren_depth;

        if (brace_depth == 0) {
            if (c == '{') {
                std::string trimmed_sel = TrimWhitespace(sel_buf);
                cur_rule = ParsedRcssRule{};
                cur_rule.selector_text = trimmed_sel;
                cur_rule.is_at_rule = (!trimmed_sel.empty() && trimmed_sel[0] == '@');
                cur_rule.start_line = has_sel_start ? sel_start_line : cur_line;
                cur_rule.rule_start_offset = has_sel_start ? sel_start_off : i;
                cur_rule_valid = !trimmed_sel.empty();
                sel_buf.clear();
                has_sel_start = false;
                decl_buf.clear();
                has_decl_start = false;
                paren_depth = 0;
                brace_depth = 1;
            } else if (c == ';') {
                // Reset @import / @charset statements outside rule blocks
                sel_buf.clear();
                has_sel_start = false;
            } else {
                push_sel_char(c, i);
            }
        } else {
            if (c == '{') {
                ++brace_depth;
                if (brace_depth == 2) {
                    decl_buf.clear();
                    has_decl_start = false;
                }
            } else if (c == '}') {
                if (brace_depth == 1) {
                    flush_decl(i);
                    if (cur_rule_valid) {
                        cur_rule.rule_length = (i + 1) - cur_rule.rule_start_offset;
                        rules.push_back(std::move(cur_rule));
                    }
                    cur_rule = ParsedRcssRule{};
                    cur_rule_valid = false;
                    paren_depth = 0;
                    brace_depth = 0;
                } else {
                    --brace_depth;
                }
            } else if (brace_depth == 1 && c == ';' && paren_depth == 0) {
                flush_decl(i + 1);
            } else if (brace_depth == 1) {
                push_decl_char(c, i);
            }
        }

        if (c == '\n') ++cur_line;
        ++i;
    }

    return rules;
}

std::vector<std::string> SplitSelectorList(const std::string& selector_text) {
    std::vector<std::string> parts;
    std::string cur;
    int pd = 0;
    int bd = 0;
    bool sq = false;
    bool dq = false;

    for (size_t i = 0; i < selector_text.size(); ++i) {
        char c = selector_text[i];
        if (sq) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < selector_text.size()) cur.push_back(selector_text[++i]);
            else if (c == '\'') sq = false;
        } else if (dq) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < selector_text.size()) cur.push_back(selector_text[++i]);
            else if (c == '"') dq = false;
        } else {
            if (c == '\'') { sq = true; cur.push_back(c); }
            else if (c == '"') { dq = true; cur.push_back(c); }
            else if (c == '(') { ++pd; cur.push_back(c); }
            else if (c == ')' && pd > 0) { --pd; cur.push_back(c); }
            else if (c == '[') { ++bd; cur.push_back(c); }
            else if (c == ']' && bd > 0) { --bd; cur.push_back(c); }
            else if (c == ',' && pd == 0 && bd == 0) {
                std::string t = TrimWhitespace(cur);
                if (!t.empty()) parts.push_back(t);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
    }
    std::string t = TrimWhitespace(cur);
    if (!t.empty()) parts.push_back(t);
    return parts;
}

bool ContainsInternalEditorClass(const std::string& sel) {
    size_t pos = 0;
    while ((pos = sel.find(".fc-n", pos)) != std::string::npos) {
        size_t d = pos + 5;
        if (d < sel.size() && std::isdigit(static_cast<unsigned char>(sel[d]))) {
            return true;
        }
        pos = d;
    }
    return false;
}

bool IsGlobalOrBuiltinSelector(const std::string& clean_sel) {
    std::string low = ToLowerAscii(TrimWhitespace(clean_sel));
    if (low.empty()) return true;
    if (low == "rml" || low == "body" || low == "head" || low == "*") return true;
    // Built-in RmlUi scrollbar/widget sub-elements often styled globally before scrollbars are materialized
    if (low == "scrollbarvertical" || low == "scrollbarhorizontal" ||
        low == "slidertrack" || low == "sliderbar" ||
        low == "sliderarrowdec" || low == "sliderarrowinc" ||
        low == "handle" || low == "selection") {
        return true;
    }
    return false;
}

std::string StripInteractivePseudos(const std::string& sel) {
    static const char* kPseudos[] = {
        "::selection",
        "::placeholder",
        ":focus-visible",
        ":focus-within",
        ":disabled",
        ":checked",
        ":selected",
        ":active",
        ":hover",
        ":focus"
    };
    std::string out;
    out.reserve(sel.size());
    bool sq = false, dq = false;
    int bd = 0;

    for (size_t i = 0; i < sel.size();) {
        char c = sel[i];
        if (sq) {
            out.push_back(c);
            if (c == '\\' && i + 1 < sel.size()) out.push_back(sel[++i]);
            else if (c == '\'') sq = false;
            ++i;
            continue;
        }
        if (dq) {
            out.push_back(c);
            if (c == '\\' && i + 1 < sel.size()) out.push_back(sel[++i]);
            else if (c == '"') dq = false;
            ++i;
            continue;
        }
        if (c == '\'') { sq = true; out.push_back(c); ++i; continue; }
        if (c == '"') { dq = true; out.push_back(c); ++i; continue; }
        if (c == '[') { ++bd; out.push_back(c); ++i; continue; }
        if (c == ']' && bd > 0) { --bd; out.push_back(c); ++i; continue; }

        if (c == ':' && bd == 0) {
            bool matched = false;
            std::string rem = ToLowerAscii(sel.substr(i, 18));
            for (const char* p : kPseudos) {
                size_t plen = std::char_traits<char>::length(p);
                if (rem.compare(0, plen, p) == 0) {
                    size_t next_idx = i + plen;
                    bool boundary = (next_idx >= sel.size()) ||
                        (!std::isalnum(static_cast<unsigned char>(sel[next_idx])) &&
                         sel[next_idx] != '-' && sel[next_idx] != '_');
                    if (boundary) {
                        i = next_idx;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) continue;
        }
        out.push_back(c);
        ++i;
    }
    return TrimWhitespace(out);
}

bool IsUnsupportedCssProperty(const std::string& prop, const std::string& val_raw, std::string& out_reason) {
    static const std::unordered_set<std::string> kUnsupportedProps = {
        "box-shadow",
        "text-shadow",
        "grid-template-columns",
        "grid-template-rows",
        "grid-template-areas",
        "grid-column",
        "grid-row",
        "grid-gap",
        "float",
        "clear",
        "outline",
        "outline-width",
        "outline-color",
        "outline-style"
    };
    if (kUnsupportedProps.count(prop)) {
        out_reason = "Propriedade CSS '" + prop + "' não é suportada pelo RmlUi e será ignorada na renderização.";
        return true;
    }
    std::string val = ToLowerAscii(StripImportantSuffix(val_raw));
    if (prop == "position" && (val == "fixed" || val == "sticky")) {
        out_reason = "Valor 'position: " + val + "' não é suportado pelo RmlUi (use static, relative ou absolute).";
        return true;
    }
    if (prop == "display" && (val == "grid" || val == "inline-grid")) {
        out_reason = "Valor 'display: " + val + "' não é suportado pelo RmlUi (use flex ou block).";
        return true;
    }
    return false;
}

bool FindRmlAttributeOccurrence(
    const std::string& rml_src,
    const std::string& attr_name,
    const std::string& attr_val,
    int target_occ_zero_based,
    int& out_line,
    size_t& out_offset,
    size_t& out_len
) {
    if (rml_src.empty() || attr_name.empty()) return false;
    std::string low = ToLowerAscii(rml_src);
    std::string key = ToLowerAscii(attr_name);
    int occ = 0;

    size_t i = 0;
    const size_t n = rml_src.size();
    while (i < n) {
        if (rml_src[i] == '<') {
            if (i + 3 < n && rml_src.compare(i, 4, "<!--") == 0) {
                size_t end_c = rml_src.find("-->", i + 4);
                if (end_c == std::string::npos) break;
                i = end_c + 3;
                continue;
            }
            size_t tag_end = i + 1;
            bool sq = false, dq = false;
            while (tag_end < n) {
                char c = rml_src[tag_end];
                if (sq) { if (c == '\'') sq = false; }
                else if (dq) { if (c == '"') dq = false; }
                else {
                    if (c == '\'') sq = true;
                    else if (c == '"') dq = true;
                    else if (c == '>') break;
                }
                ++tag_end;
            }

            // Scan attributes inside [i+1, tag_end)
            size_t p = i + 1;
            while (p < tag_end && !std::isspace(static_cast<unsigned char>(rml_src[p]))) ++p;
            while (p < tag_end) {
                while (p < tag_end && (std::isspace(static_cast<unsigned char>(rml_src[p])) || rml_src[p] == '/')) ++p;
                if (p >= tag_end) break;
                size_t name_start = p;
                while (p < tag_end && rml_src[p] != '=' && !std::isspace(static_cast<unsigned char>(rml_src[p])) && rml_src[p] != '>') ++p;
                std::string cur_name = low.substr(name_start, p - name_start);
                while (p < tag_end && std::isspace(static_cast<unsigned char>(rml_src[p]))) ++p;
                if (p < tag_end && rml_src[p] == '=') {
                    ++p;
                    while (p < tag_end && std::isspace(static_cast<unsigned char>(rml_src[p]))) ++p;
                    std::string cur_val;
                    size_t val_end = p;
                    if (p < tag_end && (rml_src[p] == '"' || rml_src[p] == '\'')) {
                        char q = rml_src[p++];
                        size_t vs = p;
                        while (p < tag_end && rml_src[p] != q) ++p;
                        cur_val = rml_src.substr(vs, p - vs);
                        if (p < tag_end) ++p;
                        val_end = p;
                    } else {
                        size_t vs = p;
                        while (p < tag_end && !std::isspace(static_cast<unsigned char>(rml_src[p]))) ++p;
                        cur_val = rml_src.substr(vs, p - vs);
                        val_end = p;
                    }
                    if (cur_name == key && cur_val == attr_val) {
                        if (occ == target_occ_zero_based) {
                            int line = 1;
                            for (size_t k = 0; k < name_start; ++k) {
                                if (rml_src[k] == '\n') ++line;
                            }
                            out_line = line;
                            out_offset = name_start;
                            out_len = val_end - name_start;
                            return true;
                        }
                        ++occ;
                    }
                }
            }
            i = (tag_end < n) ? (tag_end + 1) : n;
            continue;
        }
        ++i;
    }
    return false;
}

bool FindNthImgTagWithoutSrc(const std::string& rml_src, int target_occ, int& out_line, size_t& out_offset, size_t& out_len) {
    std::string low = ToLowerAscii(rml_src);
    int occ = 0;
    size_t i = 0;
    while (i < rml_src.size()) {
        if (rml_src[i] == '<') {
            if (i + 3 < rml_src.size() && rml_src.compare(i, 4, "<!--") == 0) {
                size_t end_c = rml_src.find("-->", i + 4);
                if (end_c == std::string::npos) break;
                i = end_c + 3;
                continue;
            }
            size_t tag_end = low.find('>', i + 1);
            if (tag_end == std::string::npos) tag_end = rml_src.size();
            std::string tag_slice = low.substr(i, tag_end - i);
            if (tag_slice.rfind("<img", 0) == 0 &&
                (tag_slice.size() == 4 || std::isspace(static_cast<unsigned char>(tag_slice[4])) || tag_slice[4] == '/')) {
                if (tag_slice.find("src") == std::string::npos || tag_slice.find("src=\"\"") != std::string::npos || tag_slice.find("src=''") != std::string::npos) {
                    if (occ == target_occ) {
                        int line = 1;
                        for (size_t k = 0; k < i; ++k) {
                            if (rml_src[k] == '\n') ++line;
                        }
                        out_line = line;
                        out_offset = i;
                        out_len = std::min(rml_src.size() - i, (tag_end - i) + 1);
                        return true;
                    }
                    ++occ;
                }
            }
            i = tag_end + 1;
            continue;
        }
        ++i;
    }
    return false;
}

bool IsAssetPresentAnywhere(const RmlDocument& doc, const RmlNode& node, const RmlEngineHost* engine) {
    if (node.image_data && !node.image_data->empty()) return true;
    const std::string& src = node.image_src;
    if (src.empty()) return false;
    if (src.find("studio_ico_") != std::string::npos ||
        src == "studio_cp_hue" ||
        src == "studio_cp_sv_white" ||
        src == "studio_cp_sv_black") {
        return true;
    }

    std::string norm = NormalizeVPathLocal(src);
    std::string base = norm;
    auto slash = norm.find_last_of('/');
    if (slash != std::string::npos) base = norm.substr(slash + 1);

    if (doc.FindCachedAsset(src) || doc.FindCachedAsset(norm) || doc.FindCachedAsset(base)) return true;
    if (engine && engine->HasCanvasAsset(src)) return true;

    std::error_code ec;
    fs::path p = fs::u8path(src);
    if (p.is_relative() && !doc.base_dir.empty()) {
        p = fs::u8path(doc.base_dir) / p;
    }
    if (!p.empty() && fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
        return true;
    }
    return false;
}

DiagnosticItem::SourceFile InferLogSourceFile(const std::string& msg) {
    std::string low = ToLowerAscii(msg);
    if (low.find(".rcss") != std::string::npos ||
        low.find("property declaration") != std::string::npos ||
        low.find("stylesheet") != std::string::npos ||
        low.find("selector") != std::string::npos) {
        return DiagnosticItem::SourceFile::Rcss;
    }
    return DiagnosticItem::SourceFile::Rml;
}

int ExtractLineNumberFromLog(const std::string& msg) {
    // Match ".rcss: 7", ".rml: 12", ":12:", or "line 15"
    for (size_t i = 0; i < msg.size(); ++i) {
        if (msg[i] == ':') {
            size_t j = i + 1;
            while (j < msg.size() && msg[j] == ' ') ++j;
            if (j < msg.size() && std::isdigit(static_cast<unsigned char>(msg[j]))) {
                int val = std::atoi(msg.c_str() + j);
                if (val >= 1 && val < 1000000) return val;
            }
        }
    }
    std::string low = ToLowerAscii(msg);
    size_t lp = low.find("line ");
    if (lp != std::string::npos) {
        size_t j = lp + 5;
        while (j < low.size() && low[j] == ' ') ++j;
        if (j < low.size() && std::isdigit(static_cast<unsigned char>(low[j]))) {
            int val = std::atoi(low.c_str() + j);
            if (val >= 1 && val < 1000000) return val;
        }
    }
    return 1;
}

} // namespace

std::vector<DiagnosticItem> RmlDiagnosticsEngine::Analyze(
    const RmlDocument& doc,
    const RmlEngineHost* engine,
    const std::string& override_rml,
    const std::string& override_rcss
) {
    std::string gen_rml, gen_rcss;
    std::vector<RmlAsset> dummy_assets;
    if (override_rml.empty() || (!doc.has_imported_source && override_rcss.empty())) {
        doc.GenerateRmlAndRcss(gen_rml, gen_rcss, dummy_assets, "style.rcss", false);
    }

    const std::string& rml_src = !override_rml.empty() ? override_rml : gen_rml;
    std::string rcss_src = !override_rcss.empty()
        ? override_rcss
        : (doc.has_imported_source ? StripUtf8Bom(doc.imported_raw_rcss) : gen_rcss);

    Rml::ElementDocument* canvas_doc = engine ? engine->CanvasDocument() : nullptr;
    const std::vector<RmlEngineLogEntry> empty_logs;
    const std::vector<RmlEngineLogEntry>& logs = engine ? engine->LastCanvasLogs() : empty_logs;

    return AnalyzeSources(doc, canvas_doc, logs, rml_src, rcss_src, engine);
}

std::vector<DiagnosticItem> RmlDiagnosticsEngine::AnalyzeSources(
    const RmlDocument& doc,
    Rml::ElementDocument* canvas_doc,
    const std::vector<RmlEngineLogEntry>& engine_logs,
    const std::string& rml_source,
    const std::string& rcss_source,
    const RmlEngineHost* engine
) {
    std::vector<DiagnosticItem> out;

    // 1. Structural RML checks: RML_DUPLICATE_ID & RML_MISSING_ASSET
    std::unordered_map<std::string, int> id_seen_count;
    int empty_img_src_occ = 0;
    std::unordered_map<std::string, int> missing_src_occ;

    for (const auto& n : doc.nodes) {
        if (!n.rml_id.empty()) {
            int& cnt = id_seen_count[n.rml_id];
            if (cnt >= 1) {
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Error;
                item.source = DiagnosticItem::SourceFile::Rml;
                item.code = "RML_DUPLICATE_ID";
                item.message = "ID duplicado '#" + n.rml_id + "' em múltiplos elementos no RML.";
                FindRmlAttributeOccurrence(rml_source, "id", n.rml_id, cnt, item.line, item.byte_offset, item.byte_length);
                out.push_back(std::move(item));
            }
            ++cnt;
        }

        if (n.tag == "img") {
            if (n.image_src.empty()) {
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Error;
                item.source = DiagnosticItem::SourceFile::Rml;
                item.code = "RML_MISSING_ASSET";
                item.message = "Elemento <img> sem atributo 'src' definido.";
                FindNthImgTagWithoutSrc(rml_source, empty_img_src_occ++, item.line, item.byte_offset, item.byte_length);
                out.push_back(std::move(item));
            } else if (!IsAssetPresentAnywhere(doc, n, engine)) {
                int& socc = missing_src_occ[n.image_src];
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Error;
                item.source = DiagnosticItem::SourceFile::Rml;
                item.code = "RML_MISSING_ASSET";
                item.message = "Imagem '" + n.image_src + "' não encontrada nos assets ou no diretório base.";
                FindRmlAttributeOccurrence(rml_source, "src", n.image_src, socc++, item.line, item.byte_offset, item.byte_length);
                out.push_back(std::move(item));
            }
        }
    }

    // 2. Static & DOM RCSS analysis: RCSS_UNUSED_SELECTOR, RCSS_UNSUPPORTED_PROP, RCSS_DUPLICATE_PROP
    const auto rules = ScanRcssRules(rcss_source);
    for (const auto& rule : rules) {
        if (rule.is_at_rule) continue;

        // 2a. Dead selector check against real canvas_doc DOM
        if (canvas_doc && !rule.selector_text.empty()) {
            const auto sel_parts = SplitSelectorList(rule.selector_text);
            bool any_ignored_or_matched = sel_parts.empty();

            for (const auto& raw_part : sel_parts) {
                if (ContainsInternalEditorClass(raw_part)) {
                    any_ignored_or_matched = true;
                    break;
                }
                std::string clean = StripInteractivePseudos(raw_part);
                if (clean.empty() || IsGlobalOrBuiltinSelector(clean)) {
                    any_ignored_or_matched = true;
                    break;
                }
                try {
                    if (canvas_doc->Matches(clean)) {
                        any_ignored_or_matched = true;
                        break;
                    }
                    Rml::ElementList matched;
                    canvas_doc->QuerySelectorAll(matched, clean);
                    if (!matched.empty()) {
                        any_ignored_or_matched = true;
                        break;
                    }
                } catch (...) {
                    any_ignored_or_matched = true;
                    break;
                }
            }

            if (!any_ignored_or_matched) {
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Unused;
                item.source = DiagnosticItem::SourceFile::Rcss;
                item.line = rule.start_line;
                item.code = "RCSS_UNUSED_SELECTOR";
                item.message = "Regra '" + rule.selector_text + "' não corresponde a nenhum elemento no RML atual.";
                item.byte_offset = rule.rule_start_offset;
                item.byte_length = rule.rule_length;
                out.push_back(std::move(item));
            }
        }

        // 2b & 2c. Unsupported & Duplicate CSS properties within the same block
        std::unordered_map<std::string, int> seen_props;
        for (const auto& d : rule.decls) {
            std::string reason;
            if (IsUnsupportedCssProperty(d.prop, d.val, reason)) {
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Warning;
                item.source = DiagnosticItem::SourceFile::Rcss;
                item.line = d.line;
                item.code = "RCSS_UNSUPPORTED_PROP";
                item.message = std::move(reason);
                item.byte_offset = d.offset;
                item.byte_length = d.length;
                out.push_back(std::move(item));
            }

            auto it = seen_props.find(d.prop);
            if (it != seen_props.end()) {
                DiagnosticItem item;
                item.severity = DiagnosticItem::Severity::Warning;
                item.source = DiagnosticItem::SourceFile::Rcss;
                item.line = d.line;
                item.code = "RCSS_DUPLICATE_PROP";
                item.message = "Propriedade '" + d.prop + "' duplicada no mesmo bloco (já declarada na linha " +
                               std::to_string(it->second) + ").";
                item.byte_offset = d.offset;
                item.byte_length = d.length;
                out.push_back(std::move(item));
            } else {
                seen_props[d.prop] = d.line;
            }
        }
    }

    // 3. Native parser errors & warnings from StudioSystemInterface
    std::unordered_set<std::string> seen_logs;
    for (const auto& le : engine_logs) {
        std::string msg = TrimWhitespace(le.message);
        if (msg.empty()) continue;
        std::string dedup_key = (le.is_error ? "E:" : "W:") + msg;
        if (!seen_logs.insert(dedup_key).second) continue;

        DiagnosticItem item;
        item.severity = le.is_error ? DiagnosticItem::Severity::Error : DiagnosticItem::Severity::Warning;
        item.source = InferLogSourceFile(msg);
        item.line = ExtractLineNumberFromLog(msg);
        item.code = "PARSER_ERROR";
        item.message = std::move(msg);
        out.push_back(std::move(item));
    }

    // Order cleanly: Errors first, then Warnings, then Unused, sorted by source file & line
    std::stable_sort(out.begin(), out.end(), [](const DiagnosticItem& a, const DiagnosticItem& b) {
        if (a.severity != b.severity) return static_cast<int>(a.severity) < static_cast<int>(b.severity);
        if (a.source != b.source) return static_cast<int>(a.source) < static_cast<int>(b.source);
        return a.line < b.line;
    });

    return out;
}

size_t RmlDiagnosticsEngine::RemoveUnusedRcssRules(
    std::string& rcss_source,
    const std::vector<DiagnosticItem>& items
) {
    struct Span {
        size_t start;
        size_t end;
    };
    std::vector<Span> spans;
    for (const auto& it : items) {
        if (it.severity != DiagnosticItem::Severity::Unused ||
            it.source != DiagnosticItem::SourceFile::Rcss ||
            it.code != "RCSS_UNUSED_SELECTOR" ||
            it.byte_length == 0 ||
            it.byte_offset >= rcss_source.size()) {
            continue;
        }
        size_t end = std::min(rcss_source.size(), it.byte_offset + it.byte_length);
        if (end > it.byte_offset) {
            spans.push_back({it.byte_offset, end});
        }
    }
    if (spans.empty()) return 0;

    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        return a.start > b.start;
    });

    size_t removed_count = 0;
    size_t prev_start = rcss_source.size() + 1;

    for (const auto& sp : spans) {
        if (sp.end > prev_start) continue;

        size_t erase_start = sp.start;
        // Expand backwards to line start if only spaces/tabs precede the rule on that line
        size_t line_begin = erase_start;
        while (line_begin > 0 && rcss_source[line_begin - 1] != '\n') {
            --line_begin;
        }
        bool only_ws_before = true;
        for (size_t k = line_begin; k < erase_start; ++k) {
            if (rcss_source[k] != ' ' && rcss_source[k] != '\t') {
                only_ws_before = false;
                break;
            }
        }
        if (only_ws_before) erase_start = line_begin;

        // Expand forwards past trailing horizontal whitespace and up to one newline
        size_t erase_end = sp.end;
        while (erase_end < rcss_source.size() && (rcss_source[erase_end] == ' ' || rcss_source[erase_end] == '\t')) {
            ++erase_end;
        }
        if (erase_end < rcss_source.size() && rcss_source[erase_end] == '\r') ++erase_end;
        if (erase_end < rcss_source.size() && rcss_source[erase_end] == '\n') ++erase_end;

        rcss_source.erase(erase_start, erase_end - erase_start);
        prev_start = erase_start;
        ++removed_count;
    }

    // Collapse runs of 3+ blank lines down to at most one blank line (\n\n)
    if (removed_count > 0) {
        std::string cleaned;
        cleaned.reserve(rcss_source.size());
        int nl_run = 0;
        for (size_t k = 0; k < rcss_source.size(); ++k) {
            char c = rcss_source[k];
            if (c == '\r') continue;
            if (c == '\n') {
                ++nl_run;
                if (nl_run <= 2) cleaned.push_back('\n');
            } else {
                nl_run = 0;
                cleaned.push_back(c);
            }
        }
        while (!cleaned.empty() && cleaned.front() == '\n') cleaned.erase(0, 1);
        rcss_source = std::move(cleaned);
    }

    return removed_count;
}
