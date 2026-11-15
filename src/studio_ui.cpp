#include "studio_ui.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>

#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string Utf16ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> data((size_t)sz);
    f.seekg(0);
    if (!f.read((char*)data.data(), sz)) return {};
    return data;
}

std::string OpenFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return Utf16ToUtf8(buf);
    return {};
}

std::string SaveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* def_ext, const wchar_t* default_name, const wchar_t* title) {
    wchar_t buf[MAX_PATH] = {};
    if (default_name) wcsncpy_s(buf, default_name, MAX_PATH - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = def_ext;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) return Utf16ToUtf8(buf);
    return {};
}

void CopyToClipboard(HWND owner, const std::string& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    std::wstring w = Utf8ToUtf16(text);
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hmem) {
        void* ptr = GlobalLock(hmem);
        if (ptr) {
            memcpy(ptr, w.c_str(), bytes);
            GlobalUnlock(hmem);
            SetClipboardData(CF_UNICODETEXT, hmem);
        } else {
            GlobalFree(hmem);
        }
    }
    CloseClipboard();
}

std::string EscapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string FormatFloat(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.05f) {
        std::snprintf(buf, sizeof(buf), "%d", (int)std::round(v));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", v);
    }
    return buf;
}

bool TryParseFloatStrict(const std::string& s, float& out) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t\r\n");
    std::string t = s.substr(a, b - a + 1);
    if (t.empty() || t == "-" || t == "+" || t == "." || t == "-." || t == "+." || t.back() == '.') return false;
    char* end = nullptr;
    float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

float ParseFloatSafe(const std::string& s, float fallback) {
    float v = fallback;
    return TryParseFloatStrict(s, v) ? v : fallback;
}

bool TryParseIntStrict(const std::string& s, int& out) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t\r\n");
    std::string t = s.substr(a, b - a + 1);
    if (t.empty() || t == "-" || t == "+") return false;
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return false;
    out = (int)v;
    return true;
}

int ParseIntSafe(const std::string& s, int fallback) {
    int v = fallback;
    return TryParseIntStrict(s, v) ? v : fallback;
}

std::string ColorToHex6(const RmlColor& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool ParseHex6(const std::string& in, RmlColor& out) {
    std::string s = in;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() == 3) {
        std::string exp;
        for (char c : s) { exp.push_back(c); exp.push_back(c); }
        s = exp;
    }
    if (s.size() != 6 && s.size() != 8) return false;
    unsigned int val = 0;
    if (std::sscanf(s.c_str(), "%x", &val) != 1) return false;
    if (s.size() == 6) {
        out.r = (uint8_t)((val >> 16) & 0xFF);
        out.g = (uint8_t)((val >> 8) & 0xFF);
        out.b = (uint8_t)(val & 0xFF);
    } else {
        out.r = (uint8_t)((val >> 24) & 0xFF);
        out.g = (uint8_t)((val >> 16) & 0xFF);
        out.b = (uint8_t)((val >> 8) & 0xFF);
        out.a = (uint8_t)(val & 0xFF);
    }
    return true;
}

Rml::Input::KeyIdentifier Win32VkToRmlKey(UINT vk) {
    using namespace Rml::Input;
    switch (vk) {
    case VK_BACK: return KI_BACK;
    case VK_TAB: return KI_TAB;
    case VK_RETURN: return KI_RETURN;
    case VK_ESCAPE: return KI_ESCAPE;
    case VK_SPACE: return KI_SPACE;
    case VK_LEFT: return KI_LEFT;
    case VK_RIGHT: return KI_RIGHT;
    case VK_UP: return KI_UP;
    case VK_DOWN: return KI_DOWN;
    case VK_HOME: return KI_HOME;
    case VK_END: return KI_END;
    case VK_DELETE: return KI_DELETE;
    case VK_INSERT: return KI_INSERT;
    case VK_PRIOR: return KI_PRIOR;
    case VK_NEXT: return KI_NEXT;
    case VK_SHIFT:
    case VK_LSHIFT: return KI_LSHIFT;
    case VK_RSHIFT: return KI_RSHIFT;
    case VK_CONTROL:
    case VK_LCONTROL: return KI_LCONTROL;
    case VK_RCONTROL: return KI_RCONTROL;
    case VK_MENU:
    case VK_LMENU: return KI_LMENU;
    case VK_RMENU: return KI_RMENU;
    case 'A': return KI_A;
    case 'C': return KI_C;
    case 'H': return KI_H;
    case 'V': return KI_V;
    case 'X': return KI_X;
    case 'Y': return KI_Y;
    case 'Z': return KI_Z;
    default: return KI_UNKNOWN;
    }
}

int GetRmlKeyModifiers() {
    int mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= Rml::Input::KM_CTRL;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= Rml::Input::KM_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= Rml::Input::KM_ALT;
    return mods;
}

std::string EscapeRmlText(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void HsvToRgb(float h_deg, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    float h = std::fmod(std::fmod(h_deg, 360.0f) + 360.0f, 360.0f);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    if (h < 60.0f)       { rp = c; gp = x; bp = 0; }
    else if (h < 120.0f) { rp = x; gp = c; bp = 0; }
    else if (h < 180.0f) { rp = 0; gp = c; bp = x; }
    else if (h < 240.0f) { rp = 0; gp = x; bp = c; }
    else if (h < 300.0f) { rp = x; gp = 0; bp = c; }
    else                 { rp = c; gp = 0; bp = x; }
    r = (uint8_t)std::clamp((int)std::round((rp + m) * 255.0f), 0, 255);
    g = (uint8_t)std::clamp((int)std::round((gp + m) * 255.0f), 0, 255);
    b = (uint8_t)std::clamp((int)std::round((bp + m) * 255.0f), 0, 255);
}

void RgbToHsv(uint8_t ri, uint8_t gi, uint8_t bi, float& out_h, float& out_s, float& out_v) {
    float r = ri / 255.0f, g = gi / 255.0f, b = bi / 255.0f;
    float cmax = std::max({r, g, b});
    float cmin = std::min({r, g, b});
    float d = cmax - cmin;
    float h = 0.0f;
    if (d > 1e-5f) {
        if (cmax == r) h = 60.0f * std::fmod(((g - b) / d), 6.0f);
        else if (cmax == g) h = 60.0f * (((b - r) / d) + 2.0f);
        else h = 60.0f * (((r - g) / d) + 4.0f);
        if (h < 0.0f) h += 360.0f;
    }
    out_h = h;
    out_s = (cmax > 1e-5f) ? (d / cmax) : 0.0f;
    out_v = cmax;
}

std::string EscapeCodeText(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2 + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case ' ': out += "&#160;"; break;
            case '\t': out += "&#160;&#160;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void AppendCodeSpaces(std::string& out, const std::string& s, size_t start, size_t end) {
    for (size_t k = start; k < end; ++k) {
        out += (s[k] == '\t') ? "&#160;&#160;" : "&#160;";
    }
}

void AppendTok(std::string& out, const char* cls, const std::string& text) {
    if (text.empty()) return;
    out += "<span class=\"";
    out += cls;
    out += "\">";
    out += EscapeCodeText(text);
    out += "</span>";
}

std::string BuildHighlightedRml(const std::string& code) {
    std::string out;
    out.reserve(code.size() * 3 + 256);
    out += "<div class=\"ch-line\">";

    bool in_comment = false;
    bool in_tag = false;
    bool expecting_tag_name = false;
    size_t n = code.size();
    size_t i = 0;

    while (i < n) {
        char c = code[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            out += "</div><div class=\"ch-line\">";
            ++i;
            continue;
        }
        if (in_comment) {
            size_t nl = code.find('\n', i);
            size_t end_c = code.find("-->", i);
            if (end_c != std::string::npos && (nl == std::string::npos || end_c < nl)) {
                AppendTok(out, "tok-comm", code.substr(i, end_c + 3 - i));
                i = end_c + 3;
                in_comment = false;
            } else {
                size_t stop = (nl == std::string::npos) ? n : nl;
                AppendTok(out, "tok-comm", code.substr(i, stop - i));
                i = stop;
            }
            continue;
        }
        if (!in_tag) {
            if (c == '<') {
                if (i + 3 < n && code.compare(i, 4, "<!--") == 0) {
                    in_comment = true;
                    continue;
                }
                if (i + 1 < n && code[i + 1] == '/') {
                    AppendTok(out, "tok-punct", "</");
                    i += 2;
                } else {
                    AppendTok(out, "tok-punct", "<");
                    i += 1;
                }
                in_tag = true;
                expecting_tag_name = true;
                continue;
            }
            size_t j = i;
            while (j < n && code[j] != '<' && code[j] != '\n' && code[j] != '\r') ++j;
            AppendTok(out, "tok-text", code.substr(i, j - i));
            i = j;
            continue;
        }

        // Inside <...>
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < n && (code[j] == ' ' || code[j] == '\t')) ++j;
            AppendCodeSpaces(out, code, i, j);
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '>') {
            AppendTok(out, "tok-punct", "/>");
            i += 2;
            in_tag = false;
            expecting_tag_name = false;
            continue;
        }
        if (c == '>') {
            AppendTok(out, "tok-punct", ">");
            i += 1;
            in_tag = false;
            expecting_tag_name = false;
            continue;
        }
        if (c == '=') {
            AppendTok(out, "tok-punct", "=");
            i += 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            size_t j = i + 1;
            while (j < n && code[j] != q && code[j] != '\n') ++j;
            if (j < n && code[j] == q) ++j;
            AppendTok(out, "tok-str", code.substr(i, j - i));
            i = j;
            continue;
        }
        size_t j = i;
        while (j < n && code[j] != ' ' && code[j] != '\t' && code[j] != '=' &&
               code[j] != '>' && !(code[j] == '/' && j + 1 < n && code[j + 1] == '>') &&
               code[j] != '"' && code[j] != '\'' && code[j] != '\n' && code[j] != '\r') {
            ++j;
        }
        if (j == i) ++j;
        AppendTok(out, expecting_tag_name ? "tok-tag" : "tok-attr", code.substr(i, j - i));
        expecting_tag_name = false;
        i = j;
    }

    out += "</div>";
    return out;
}

std::string BuildHighlightedRcss(const std::string& code) {
    std::string out;
    out.reserve(code.size() * 3 + 256);
    out += "<div class=\"ch-line\">";

    bool in_comment = false;
    int brace_depth = 0;
    bool after_colon = false;
    size_t n = code.size();
    size_t i = 0;

    while (i < n) {
        char c = code[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            out += "</div><div class=\"ch-line\">";
            ++i;
            continue;
        }
        if (in_comment) {
            size_t nl = code.find('\n', i);
            size_t end_c = code.find("*/", i);
            if (end_c != std::string::npos && (nl == std::string::npos || end_c < nl)) {
                AppendTok(out, "tok-comm", code.substr(i, end_c + 2 - i));
                i = end_c + 2;
                in_comment = false;
            } else {
                size_t stop = (nl == std::string::npos) ? n : nl;
                AppendTok(out, "tok-comm", code.substr(i, stop - i));
                i = stop;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '*') {
            in_comment = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < n && (code[j] == ' ' || code[j] == '\t')) ++j;
            AppendCodeSpaces(out, code, i, j);
            i = j;
            continue;
        }
        if (c == '{') {
            AppendTok(out, "tok-punct", "{");
            ++brace_depth;
            after_colon = false;
            ++i;
            continue;
        }
        if (c == '}') {
            AppendTok(out, "tok-punct", "}");
            brace_depth = std::max(0, brace_depth - 1);
            after_colon = false;
            ++i;
            continue;
        }
        if (c == ':' && brace_depth > 0 && !after_colon) {
            AppendTok(out, "tok-punct", ":");
            after_colon = true;
            ++i;
            continue;
        }
        if (c == ';') {
            AppendTok(out, "tok-punct", ";");
            after_colon = false;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            size_t j = i + 1;
            while (j < n && code[j] != q && code[j] != '\n') ++j;
            if (j < n && code[j] == q) ++j;
            AppendTok(out, "tok-str", code.substr(i, j - i));
            i = j;
            continue;
        }
        if (brace_depth > 0 && after_colon) {
            if (c == '#') {
                size_t j = i + 1;
                while (j < n && std::isxdigit((unsigned char)code[j])) ++j;
                AppendTok(out, "tok-hex", code.substr(i, j - i));
                i = j;
                continue;
            }
            if (std::isdigit((unsigned char)c) || (c == '-' && i + 1 < n && std::isdigit((unsigned char)code[i + 1]))) {
                size_t j = i + 1;
                while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '.' || code[j] == '%')) ++j;
                AppendTok(out, "tok-num", code.substr(i, j - i));
                i = j;
                continue;
            }
            if (std::isalpha((unsigned char)c) || c == '-') {
                size_t j = i;
                while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_')) ++j;
                std::string word = code.substr(i, j - i);
                if (j < n && code[j] == '(') {
                    AppendTok(out, "tok-fn", word);
                } else {
                    AppendTok(out, "tok-text", word);
                }
                i = j;
                continue;
            }
            std::string p(1, c);
            AppendTok(out, "tok-punct", p);
            ++i;
            continue;
        }

        if (brace_depth > 0) {
            size_t j = i;
            while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_')) ++j;
            if (j == i) ++j;
            AppendTok(out, "tok-attr", code.substr(i, j - i));
            i = j;
            continue;
        }

        // Selector context (brace_depth == 0)
        if (c == '#') {
            size_t j = i + 1;
            while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_')) ++j;
            AppendTok(out, "tok-id", code.substr(i, j - i));
            i = j;
            continue;
        }
        if (c == '.') {
            size_t j = i + 1;
            while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_')) ++j;
            AppendTok(out, "tok-sel", code.substr(i, j - i));
            i = j;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '*' || c == '@') {
            size_t j = i + 1;
            while (j < n && (std::isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_')) ++j;
            AppendTok(out, "tok-tag", code.substr(i, j - i));
            i = j;
            continue;
        }
        std::string p(1, c);
        AppendTok(out, "tok-punct", p);
        ++i;
    }

    out += "</div>";
    return out;
}

const char* kEditorRcss = R"RCSS(
* {
    box-sizing: border-box;
}

div {
    display: block;
}

body {
    font-family: Arial, Segoe UI;
    font-size: 12px;
    color: #E7EAF0;
    background-color: rgba(0, 0, 0, 0);
    margin: 0px;
    padding: 0px;
    width: 100%;
    height: 100%;
    overflow: hidden;
}

.mono {
    font-family: Consolas;
}

scrollbarvertical {
    width: 7px;
    background-color: #10131A;
}

scrollbarvertical slidertrack {
    background-color: #10131A;
}

scrollbarvertical sliderbar {
    width: 5px;
    margin: 1px;
    background-color: #2A303E;
    border-radius: 3px;
}

scrollbarvertical sliderbar:hover {
    background-color: #414B61;
}

scrollbarvertical sliderarrowdec,
scrollbarvertical sliderarrowinc {
    height: 0px;
}

scrollbarhorizontal {
    height: 7px;
    background-color: #10131A;
}

scrollbarhorizontal slidertrack {
    background-color: #10131A;
}

scrollbarhorizontal sliderbar {
    height: 5px;
    margin: 1px;
    background-color: #2A303E;
    border-radius: 3px;
}

scrollbarhorizontal sliderbar:hover {
    background-color: #414B61;
}

scrollbarhorizontal sliderarrowdec,
scrollbarhorizontal sliderarrowinc {
    width: 0px;
}

#ide-root {
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    position: relative;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

/* Icons rendered via studio_ico_<name> and tinted via image-color */
img.ico {
    display: inline-block;
    vertical-align: middle;
    flex-shrink: 0;
    image-color: #AEB6C6;
}
img.ico-12 { width: 12px; height: 12px; }
img.ico-13 { width: 13px; height: 13px; }
img.ico-14 { width: 14px; height: 14px; }
img.ico-15 { width: 15px; height: 15px; }
img.ico-16 { width: 16px; height: 16px; }
img.ico-17 { width: 17px; height: 17px; }
img.ico-28 { width: 28px; height: 28px; }

img.ico-white { image-color: #FFFFFF; }
img.ico-brand { image-color: #EAF0FF; }
img.ico-muted { image-color: #788296; }
img.ico-soft { image-color: #929BAD; }
img.ico-blue { image-color: #88AAFF; }
img.ico-light-blue { image-color: #EDF2FF; }
img.ico-hero-blue { image-color: #84A5FF; }
img.ico-green { image-color: #36C891; }

/* TOPBAR (50px) */
#topbar {
    height: 50px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 17px;
    background-color: #10131A;
    border-bottom: 1px #252A35;
}

.brand-side {
    flex: 1;
    display: flex;
    flex-direction: row;
    align-items: center;
    min-width: 0;
    gap: 9px;
}

.brand-mark {
    width: 27px;
    height: 27px;
    border-radius: 8px;
    decorator: vertical-gradient(#5787FF #335EDF);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
}

.brand-name {
    font-size: 14px;
    font-weight: bold;
    color: #E6EAF2;
    white-space: nowrap;
}

.brand-product {
    font-size: 9px;
    font-weight: bold;
    color: #8F98AA;
    white-space: nowrap;
}

.top-divider {
    width: 1px;
    height: 17px;
    background-color: #343946;
    margin: 0px 3px;
    flex-shrink: 0;
}

.project-name {
    font-size: 12px;
    color: #B0B7C5;
    white-space: nowrap;
}

.saved-pill {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    margin-left: 4px;
    color: #A7B1C2;
    font-size: 10px;
    white-space: nowrap;
}

.dot-green {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 3px;
    background-color: #36C891;
    flex-shrink: 0;
}

.dot-green-sm {
    display: inline-block;
    width: 5px;
    height: 5px;
    border-radius: 3px;
    background-color: #36C891;
    flex-shrink: 0;
}

.menu-nav {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: center;
    gap: 5px;
    flex-shrink: 0;
}

button.menu-item {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 4px;
    height: 28px;
    padding: 0px 9px;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 5px;
    background-color: rgba(0, 0, 0, 0);
    color: #AEB6C6;
    font-size: 11px;
    white-space: nowrap;
}

button.menu-item:hover {
    background-color: #202430;
    color: #FFFFFF;
}

.top-actions {
    flex: 1;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: flex-end;
    gap: 5px;
}

button.icon-button {
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 6px;
    background-color: rgba(0, 0, 0, 0);
}

button.icon-button:hover {
    background-color: #202430;
}

button.icon-button:hover img.ico {
    image-color: #FFFFFF;
}

button.icon-button.tiny {
    width: 24px;
    height: 24px;
}

.avatar {
    width: 25px;
    height: 25px;
    margin-left: 8px;
    border: 1px #4268CE;
    border-radius: 13px;
    background-color: #1B2A4B;
    color: #BFD0FF;
    font-size: 9px;
    font-weight: bold;
    text-align: center;
    line-height: 23px;
    flex-shrink: 0;
}

/* TOOLBAR (45px) */
#toolbar {
    height: 45px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 15px;
    background-color: #151820;
    border-bottom: 1px #252A35;
}

.toolbar-group {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
}

button.toolbar-btn {
    height: 29px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: center;
    gap: 7px;
    padding: 0px 10px;
    border: 1px #303644;
    border-radius: 5px;
    background-color: #1A1E27;
    color: #BBC3D1;
    font-size: 11px;
    white-space: nowrap;
}

button.toolbar-btn:hover,
button.toolbar-btn.selected {
    border-color: #3D65C9;
    background-color: #202D4A;
    color: #DCE6FF;
}

button.toolbar-btn:hover img.ico,
button.toolbar-btn.selected img.ico {
    image-color: #DCE6FF;
}

button.toolbar-btn.square {
    width: 30px;
    padding: 0px;
}

button.toolbar-btn.btn-play-on {
    border-color: #315344;
    background-color: #17251F;
    color: #BBEBD5;
}

button.toolbar-btn.btn-play-on img.ico {
    image-color: #36C891;
}

.toolbar-sep {
    height: 18px;
    width: 1px;
    background-color: #303541;
    margin: 0px 3px;
}

.toolbar-center {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 12px;
}

.device-switch {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 2px;
    padding: 3px;
    border-radius: 7px;
    background-color: #0E1117;
    border: 1px #252A35;
}

button.dev-btn {
    height: 25px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    padding: 0px 10px;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 4px;
    background-color: rgba(0, 0, 0, 0);
    color: #8992A2;
    font-size: 10px;
    white-space: nowrap;
}

button.dev-btn img.ico {
    image-color: #8992A2;
}

button.dev-btn:hover {
    color: #D7E0F0;
}

button.dev-btn.active {
    background-color: #252B39;
    color: #E8EDF7;
}

button.dev-btn.active img.ico {
    image-color: #E8EDF7;
}

.zoom-label {
    font-size: 10px;
    color: #9EA6B4;
    min-width: 32px;
    text-align: right;
    font-family: Consolas;
}

button.export-btn {
    height: 29px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 7px;
    padding: 0px 12px;
    border: 1px #5985F9;
    border-radius: 5px;
    background-color: #4778F5;
    color: #FFFFFF;
    font-size: 11px;
    font-weight: bold;
    white-space: nowrap;
}

button.export-btn:hover {
    background-color: #5685FF;
}

/* WORKSTATION 3-COLUMN LAYOUT (exact pixel heights/widths managed via ApplyPanelSizes) */
#workstation {
    position: relative;
    width: 100%;
    flex: 1;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

.gutter-v {
    position: absolute;
    top: 0px;
    width: 4px;
    height: 100%;
    background-color: #252A35;
}

.gutter-v:hover {
    background-color: #4778F5;
}

.gutter-h {
    position: absolute;
    left: 0px;
    width: 100%;
    height: 4px;
    background-color: #2A303B;
}

.gutter-h:hover {
    background-color: #4778F5;
}

/* LEFT PANEL: HIERARQUIA (226px) */
#left-panel {
    position: absolute;
    top: 0px;
    left: 0px;
    height: 100%;
    width: 226px;
    background-color: #11141B;
    display: flex;
    flex-direction: column;
    overflow: hidden;
}

.panel-heading {
    height: 45px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 13px;
    border-bottom: 1px #252A35;
}

.panel-heading-left {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 8px;
    color: #D4DAE5;
    font-size: 11px;
    font-weight: bold;
}

.search-wrap {
    height: 30px;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 7px;
    margin: 12px 11px 14px 11px;
    padding: 0px 9px;
    border: 1px #292E39;
    border-radius: 5px;
    background-color: #151820;
}

input.search-input {
    flex: 1;
    min-width: 0;
    height: 26px;
    line-height: 26px;
    border: 0px rgba(0, 0, 0, 0);
    background-color: rgba(0, 0, 0, 0);
    color: #E5E9F0;
    font-size: 10px;
    padding: 0px;
}

.tree-label-row {
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 14px 8px 14px;
    color: #788193;
    font-size: 9px;
    font-weight: bold;
}

.tree-count {
    color: #747C8C;
    font-weight: normal;
}

#layers-list {
    flex: 1;
    width: 100%;
    min-height: 0;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 0px 8px 6px 8px;
    background-color: #11141B;
}

.layer-row {
    display: flex;
    flex-direction: row;
    align-items: center;
    width: 100%;
    height: 28px;
    border-radius: 5px;
    margin-bottom: 1px;
    padding-right: 6px;
    white-space: nowrap;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
    color: #AAB2C1;
}

.layer-row:hover {
    background-color: #191E28;
    color: #DFE8FF;
}

.layer-row.selected {
    background-color: #1C2945;
    color: #DFE8FF;
}

.layer-row.hidden-node {
    color: #636B7C;
}

button.tree-collapse {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 16px;
    height: 16px;
    padding: 0px;
    margin: 0px 3px 0px 0px;
    flex-shrink: 0;
    background-color: rgba(0, 0, 0, 0);
    border: 0px rgba(0, 0, 0, 0);
}

.tree-spacer {
    display: block;
    width: 16px;
    height: 16px;
    margin: 0px 3px 0px 0px;
    flex-shrink: 0;
}

.node-ico-box {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 16px;
    height: 16px;
    margin-right: 7px;
    flex-shrink: 0;
}

.node-ico-box img.ico {
    image-color: #8291AD;
}

.layer-row.selected .node-ico-box img.ico {
    image-color: #7DA3FF;
}

.layer-name {
    display: block;
    flex: 1;
    min-width: 0;
    overflow: hidden;
    font-size: 11px;
    line-height: 16px;
    white-space: nowrap;
}

button.tree-vis-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 20px;
    height: 20px;
    padding: 0px;
    margin-left: 4px;
    flex-shrink: 0;
    background-color: rgba(0, 0, 0, 0);
    border: 0px rgba(0, 0, 0, 0);
}

button.tree-vis-btn img.ico {
    image-color: #6B758B;
}

button.tree-vis-btn:hover img.ico {
    image-color: #E2E8F0;
}

button.tree-vis-btn.off img.ico {
    image-color: #545D70;
}

.left-footer {
    height: 36px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 8px;
    padding: 0px 13px;
    border-top: 1px #252A35;
    color: #7F889A;
    font-size: 9px;
}

/* CENTER COLUMN: CANVAS + BOTTOM CODE DOCK */
#center-col {
    position: absolute;
    top: 0px;
    height: 100%;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

#viewport-shell {
    position: absolute;
    left: 0px;
    top: 0px;
    width: 100%;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

.canvas-toolbar {
    height: 39px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 14px;
    background-color: #0E1117;
    border-bottom: 1px #202530;
    color: #8992A2;
}

.canvas-breadcrumb {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 7px;
    font-size: 10px;
}

.canvas-breadcrumb .res-bold {
    font-weight: bold;
    color: #BDC4D0;
}

.canvas-controls {
    height: 25px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 5px;
    padding: 0px 6px;
    border: 1px #292F3A;
    border-radius: 5px;
    background-color: #151922;
    font-size: 10px;
}

button.canvas-ctrl-btn {
    min-width: 20px;
    height: 20px;
    padding: 0px 5px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 3px;
    background-color: rgba(0, 0, 0, 0);
    color: #AAB1BD;
    font-size: 10px;
    white-space: nowrap;
}

button.canvas-ctrl-btn:hover,
button.canvas-ctrl-btn.control-active {
    color: #88AAFF;
    background-color: #1E273B;
}

button.canvas-ctrl-btn.control-active img.ico {
    image-color: #88AAFF;
}

.control-divider {
    height: 15px;
    width: 1px;
    background-color: #303541;
    margin: 0px 2px;
}

button.canvas-bg-btn {
    height: 20px;
    padding: 0px 6px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 5px;
    border: 1px #2D3443;
    border-radius: 3px;
    background-color: #191E29;
    color: #CBD5E1;
    font-size: 9.5px;
    white-space: nowrap;
}

button.canvas-bg-btn:hover {
    border-color: #547CE2;
    color: #FFFFFF;
}

.swatch-mini {
    display: inline-block;
    width: 11px;
    height: 11px;
    border-radius: 2px;
    border: 1px #475569;
    background-color: #0E1117;
    flex-shrink: 0;
}

#ruler-top-row {
    height: 20px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    background-color: #12151D;
    border-bottom: 1px #252A34;
}

#ruler-corner {
    width: 28px;
    height: 20px;
    background-color: #12151D;
    border-right: 1px #252A34;
    font-family: Consolas;
    font-size: 9px;
    color: #738097;
    text-align: center;
    line-height: 19px;
    flex-shrink: 0;
}

#ruler-top {
    flex: 1;
    height: 20px;
    position: relative;
    overflow: hidden;
    background-color: #12151D;
}

#viewport-mid-row {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: row;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

#ruler-left {
    width: 28px;
    height: 100%;
    position: relative;
    overflow: hidden;
    background-color: #12151D;
    border-right: 1px #252A34;
    flex-shrink: 0;
}

.rtick-x {
    position: absolute;
    top: 0px;
    height: 20px;
    border-left: 1px #353B47;
    padding-left: 4px;
    font-family: Consolas;
    font-size: 9px;
    line-height: 19px;
    color: #738097;
}

.rtick-y {
    position: absolute;
    left: 0px;
    width: 28px;
    border-top: 1px #353B47;
    padding-top: 2px;
    padding-left: 2px;
    font-family: Consolas;
    font-size: 9px;
    color: #738097;
}

#viewport {
    flex: 1;
    height: 100%;
    position: relative;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

/* Artboard overlays inside #viewport */
#artboard-box {
    position: absolute;
    left: 0px;
    top: 0px;
    width: 100px;
    height: 100px;
    overflow: hidden;
    background-color: rgba(0, 0, 0, 0);
}

#safe-area-label {
    position: absolute;
    top: 19px;
    left: 20px;
    font-size: 8px;
    font-weight: bold;
    color: rgba(113, 134, 184, 110);
}

#canvas-empty-center {
    position: absolute;
    left: 0px;
    top: 0px;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding-bottom: 18px;
    text-align: center;
}

.empty-icon {
    width: 58px;
    height: 58px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 1px #344264;
    border-radius: 15px;
    background-color: #18213A;
}

.canvas-eyebrow {
    margin-top: 18px;
    color: #8EA7E9;
    font-size: 9px;
    font-weight: bold;
}

.empty-h1 {
    margin: 9px 0px 7px 0px;
    color: #E9EEFB;
    font-size: 23px;
    line-height: 27px;
    font-weight: bold;
    text-align: center;
}

.empty-p {
    margin: 0px 0px 17px 0px;
    color: #909BB1;
    font-size: 11px;
    text-align: center;
}

button.empty-add-btn {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 7px;
    height: 32px;
    padding: 0px 13px;
    border: 1px #4268CE;
    border-radius: 5px;
    background-color: #28478F;
    color: #EDF2FF;
    font-size: 10px;
    font-weight: bold;
}

button.empty-add-btn:hover {
    background-color: #355CB3;
}

#artboard-meta {
    position: absolute;
    left: 0px;
    bottom: 0px;
    right: 0px;
    height: 24px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 9px;
    background-color: rgba(17, 23, 35, 232);
    color: #99A7C4;
    font-family: Consolas;
    font-size: 9px;
}

/* BOTTOM CODE DOCK (190px) */
#split-bottom {
    position: absolute;
    left: 0px;
    width: 100%;
    height: 4px;
}

#bottom-dock {
    position: absolute;
    left: 0px;
    width: 100%;
    height: 190px;
    display: flex;
    flex-direction: column;
    background-color: #0D1016;
    overflow: hidden;
}

.code-tabs {
    height: 37px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 12px;
    border-bottom: 1px #252B36;
}

.code-tab-list {
    height: 100%;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 15px;
}

button.code-tab {
    height: 37px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    padding: 0px 5px;
    border: 0px rgba(0, 0, 0, 0);
    border-bottom: 2px rgba(0, 0, 0, 0);
    background-color: rgba(0, 0, 0, 0);
    color: #828B9B;
    font-size: 10px;
    white-space: nowrap;
}

button.code-tab:hover {
    color: #C7D2E5;
}

button.code-tab.current {
    border-bottom: 2px #5B85FB;
    color: #E4EAF6;
}

button.code-tab.current img.ico {
    image-color: #88AAFF;
}

.tab-ext {
    color: #8B95A8;
    font-family: Consolas;
    font-size: 9px;
}

.code-actions {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 12px;
}

.code-status {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    color: #8B96A7;
    font-size: 9px;
}

button.code-copy-btn {
    height: 25px;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    padding: 0px 8px;
    border: 1px #303744;
    border-radius: 4px;
    background-color: #191D27;
    color: #BEC6D3;
    font-size: 9px;
}

button.code-copy-btn:hover {
    color: #FFFFFF;
    border-color: #4A5670;
}

#code-content {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: row;
    align-items: stretch;
    padding: 0px;
    background-color: #0D1016;
    overflow: hidden;
}

#code-gutter {
    width: 48px;
    flex-shrink: 0;
    height: 100%;
    min-height: 0;
    overflow: hidden;
    background-color: #0A0D13;
    border-right: 1px #19202D;
    padding-top: 8px;
    padding-bottom: 8px;
    box-sizing: border-box;
}

#code-gutter-inner {
    display: block;
    width: 100%;
    padding-right: 9px;
    box-sizing: border-box;
}

.code-lnum {
    display: block;
    height: 16px;
    line-height: 16px;
    font-family: Consolas;
    font-size: 10.5px;
    color: #4E5B74;
    text-align: right;
}

.code-lnum.cur {
    color: #E2E8F0;
    font-weight: bold;
}

.code-cursor-lbl {
    font-size: 10.5px;
    color: #6B7894;
    margin-right: 6px;
}

#code-editor-wrap {
    flex: 1;
    min-width: 0;
    height: 100%;
    min-height: 0;
    position: relative;
    overflow: hidden;
    background-color: #0D1016;
}

#code-hl-clip {
    position: absolute;
    left: 0px;
    top: 0px;
    right: 0px;
    bottom: 0px;
    overflow: hidden;
    pointer-events: none;
}

#code-active-line {
    position: absolute;
    left: 0px;
    right: 0px;
    top: 8px;
    height: 16px;
    background-color: rgba(59, 130, 246, 20);
    border-top: 1px rgba(59, 130, 246, 36);
    border-bottom: 1px rgba(59, 130, 246, 36);
    pointer-events: none;
}

#code-highlight {
    position: relative;
    display: block;
    padding: 8px 12px;
    font-family: Consolas;
    font-size: 11px;
    line-height: 16px;
    white-space: pre;
    pointer-events: none;
    color: #E2E8F0;
}

.ch-line {
    display: block;
    white-space: pre;
    height: 16px;
    line-height: 16px;
}

.tok-tag { color: #569CD6; }
.tok-punct { color: #6E7E99; }
.tok-attr { color: #7DD3FC; }
.tok-str { color: #FDBA74; }
.tok-comm { color: #6A9955; }
.tok-sel { color: #E5C07B; }
.tok-id { color: #F59E0B; }
.tok-num { color: #B5CEA8; }
.tok-hex { color: #F472B6; }
.tok-fn { color: #C084FC; }
.tok-text { color: #E2E8F0; }

textarea.code-editor {
    position: absolute;
    left: 0px;
    top: 0px;
    width: 100%;
    height: 100%;
    box-sizing: border-box;
    padding: 8px 12px;
    border-width: 0px;
    background-color: rgba(0, 0, 0, 0);
    color: rgba(0, 0, 0, 0);
    caret-color: #60A5FA;
    font-family: Consolas;
    font-size: 11px;
    line-height: 16px;
    cursor: text;
    white-space: pre;
    overflow: auto;
}

textarea.code-editor selection {
    background-color: rgba(59, 130, 246, 95);
    color: #FFFFFF;
}

textarea.code-editor:focus {
    background-color: rgba(14, 18, 26, 25);
}

#code-status-dot.dot-amber-sm {
    background-color: #F59E0B;
}

.code-lines {
    font-family: Consolas;
    font-size: 10px;
    line-height: 15px;
    color: #B8C7ED;
}

.code-line {
    display: block;
    white-space: pre;
    height: 15px;
    line-height: 15px;
}

.line-no {
    display: inline-block;
    width: 28px;
    padding-right: 9px;
    text-align: right;
    color: #505A6C;
}

.code-text {
    color: #B5C6EF;
}

.code-comment {
    color: #56C6A4;
}

/* RIGHT PANEL: INSPECTOR (292px) */
#studio-inspector-panel {
    position: absolute;
    top: 0px;
    right: 0px;
    width: 292px;
    height: 100%;
    background-color: #11141B;
    display: flex;
    flex-direction: column;
    overflow: hidden;
}

#inspector-scroll {
    flex: 1;
    width: 100%;
    min-height: 0;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 10px;
    box-sizing: border-box;
}

.selected-component {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 9px;
    padding: 10px 9px;
    margin-bottom: 9px;
    border: 1px #2B3240;
    border-radius: 6px;
    background-color: #171B24;
}

.component-badge {
    width: 29px;
    height: 29px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 6px;
    background-color: #202D4A;
    flex-shrink: 0;
}

.sel-comp-info {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
}

.sel-comp-name {
    color: #E0E5EF;
    font-size: 10px;
    font-weight: bold;
    white-space: nowrap;
}

.sel-comp-sub {
    color: #818A9B;
    font-size: 9px;
    white-space: nowrap;
}

.inspector-section {
    display: block;
    margin-bottom: 9px;
    border: 1px #292F3B;
    border-radius: 6px;
    background-color: #151922;
    overflow: hidden;
}

.section-title {
    height: 30px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 9px;
    border-bottom: 1px #292F3B;
    background-color: #1A1F29;
    color: #CCD3DF;
    font-size: 10px;
    font-weight: bold;
}

.section-tag {
    color: #778296;
    font-family: Consolas;
    font-size: 8px;
    font-weight: normal;
}

.sec-body {
    display: block;
    padding: 8px 9px;
}

.field-col {
    display: flex;
    flex-direction: column;
    gap: 4px;
    margin-bottom: 7px;
    width: 100%;
    min-width: 0;
}

.field-grid2 .field-col {
    width: auto;
    flex: 1;
    margin-bottom: 0px;
}

.field-col.last {
    margin-bottom: 0px;
}

.field-caption {
    color: #929BAC;
    font-size: 9px;
}

.field-grid2 {
    display: flex;
    flex-direction: row;
    gap: 7px;
    width: 100%;
    margin-bottom: 7px;
}

.field-grid2.last {
    margin-bottom: 0px;
}

input.insp-in {
    width: 100%;
    height: 27px;
    line-height: 25px;
    padding: 0px 8px;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #0F1219;
    color: #D7DCE7;
    font-size: 10px;
}

input.insp-in:focus {
    border-color: #547CE2;
}

.input-unit {
    width: 100%;
    height: 27px;
    display: flex;
    flex-direction: row;
    align-items: center;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #0F1219;
    overflow: hidden;
}

input.unit-in {
    flex: 1;
    min-width: 0;
    height: 25px;
    line-height: 25px;
    padding: 0px 7px;
    border: 0px rgba(0, 0, 0, 0);
    background-color: rgba(0, 0, 0, 0);
    color: #D7DCE7;
    font-family: Consolas;
    font-size: 10px;
}

.unit-sfx {
    padding-right: 7px;
    color: #7D8798;
    font-size: 9px;
    flex-shrink: 0;
}

.visibility-row {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 2px 0px 8px 0px;
    color: #9BA4B3;
    font-size: 9px;
}

button.toggle {
    width: 28px;
    height: 16px;
    padding: 2px;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 8px;
    background-color: #414755;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: flex-start;
}

button.toggle.on {
    background-color: #4778F5;
    justify-content: flex-end;
}

.toggle-knob {
    display: block;
    width: 12px;
    height: 12px;
    border-radius: 6px;
    background-color: #D5D9E2;
}

button.toggle.on .toggle-knob {
    background-color: #FFFFFF;
}

.quick-actions {
    display: flex;
    flex-direction: row;
    gap: 5px;
}

button.quick-btn {
    flex: 1;
    height: 24px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: center;
    gap: 4px;
    padding: 0px 5px;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #1B202A;
    color: #AEB7C6;
    font-size: 9px;
    white-space: nowrap;
}

button.quick-btn:hover {
    border-color: #59677F;
    color: #FFFFFF;
}

button.quick-btn:hover img.ico {
    image-color: #FFFFFF;
}

.seg-row {
    width: 100%;
    height: 27px;
    display: flex;
    flex-direction: row;
    background-color: #0F1219;
    border: 1px #303746;
    border-radius: 4px;
    padding: 2px;
    gap: 2px;
}

button.seg-btn {
    flex: 1;
    height: 21px;
    display: flex;
    align-items: center;
    justify-content: center;
    background-color: rgba(0, 0, 0, 0);
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 3px;
    color: #8E98AC;
    font-size: 10px;
}

button.seg-btn:hover {
    color: #FFFFFF;
    background-color: #1B212E;
}

button.seg-btn.active {
    background-color: #252F47;
    color: #DCE6FF;
}

.align-toolbar {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 3px;
    padding: 2px 0px 8px 0px;
    margin-bottom: 6px;
    border-bottom: 1px #1D222D;
}

button.align-btn {
    flex: 1;
    height: 24px;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 0px;
    background-color: #0F1219;
    border: 1px #303746;
    border-radius: 3px;
}

button.align-btn:hover {
    background-color: #1D2434;
    border-color: #547CE2;
}

button.align-btn img.ico {
    image-color: #9BA6BD;
}

button.align-btn:hover img.ico {
    image-color: #FFFFFF;
}

.align-sep {
    width: 1px;
    height: 16px;
    background-color: #2B3240;
    margin: 0px 2px;
    flex-shrink: 0;
}

.seg-row-compact {
    height: 24px;
}

.seg-row-compact button.seg-btn {
    height: 18px;
    font-size: 9px;
    padding: 0px 2px;
}

.constraint-row {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 10px;
}

#constraint-diagram {
    position: relative;
    width: 66px;
    height: 66px;
    flex-shrink: 0;
    background-color: #0F1219;
    border: 1px #303746;
    border-radius: 4px;
}

.cd-inner-box {
    position: absolute;
    left: 20px;
    top: 20px;
    width: 24px;
    height: 24px;
    border: 1px #52617C;
    border-radius: 2px;
    background-color: #171E2E;
}

button.cd-pin {
    position: absolute;
    padding: 0px;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 1px;
    background-color: #313A4E;
}

button.cd-pin:hover {
    background-color: #6B8EF5;
}

button.cd-pin.active {
    background-color: #4778F5;
}

#cpin-top { left: 29px; top: 4px; width: 6px; height: 14px; }
#cpin-bottom { left: 29px; top: 46px; width: 6px; height: 14px; }
#cpin-left { left: 4px; top: 29px; width: 14px; height: 6px; }
#cpin-right { left: 46px; top: 29px; width: 14px; height: 6px; }
#cpin-hcenter { left: 22px; top: 30px; width: 20px; height: 4px; }
#cpin-vcenter { left: 30px; top: 22px; width: 4px; height: 20px; }

.constraint-controls {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
}

.grid-head-row {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 0px 8px 0px;
    color: #9BA4B3;
    font-size: 9px;
}

.color-field {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 7px;
    color: #9BA4B3;
    font-size: 9px;
}

.color-input-grp {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 5px;
    width: 158px;
}

.swatch {
    width: 24px;
    height: 24px;
    border: 1px #424C5E;
    border-radius: 4px;
    background-color: #0B1020;
    cursor: pointer;
    flex-shrink: 0;
}

.swatch:hover {
    border-color: #60A5FA;
}

input.hex-in {
    flex: 1;
    min-width: 0;
    height: 25px;
    line-height: 23px;
    padding: 0px 7px;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #0F1219;
    color: #D7DCE7;
    font-family: Consolas;
    font-size: 10px;
}

input.alpha-in {
    width: 40px;
    height: 25px;
    line-height: 23px;
    padding: 0px 5px;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #0F1219;
    color: #D7DCE7;
    font-family: Consolas;
    font-size: 10px;
    flex-shrink: 0;
}

button.pick-btn {
    height: 27px;
    padding: 0px 9px;
    border: 1px #303746;
    border-radius: 4px;
    background-color: #1B202A;
    color: #CBD5E1;
    font-size: 10px;
    flex-shrink: 0;
}

button.pick-btn:hover {
    border-color: #547CE2;
    color: #FFFFFF;
}

/* POPUP DROPDOWN */
#menu-popup {
    position: absolute;
    top: 46px;
    left: 20px;
    width: 230px;
    background-color: #12151D;
    border: 1px #2B3242;
    border-radius: 6px;
    padding: 5px;
    z-index: 999;
    display: flex;
    flex-direction: column;
    gap: 2px;
}

button.popup-item {
    display: block;
    width: 100%;
    height: 26px;
    line-height: 24px;
    padding: 0px 9px;
    text-align: left;
    background-color: rgba(0, 0, 0, 0);
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 4px;
    color: #D7DCE7;
    font-size: 11px;
    box-sizing: border-box;
}

button.popup-item:hover {
    background-color: #202D4A;
    color: #FFFFFF;
}

.popup-sep {
    height: 1px;
    background-color: #252A35;
    margin: 3px 4px;
}

/* STATUSBAR (23px) */
#status-bar {
    height: 23px;
    width: 100%;
    flex-shrink: 0;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 11px;
    border-top: 1px #252A35;
    background-color: #10131A;
    color: #858E9E;
    font-family: Consolas;
    font-size: 9px;
}

.status-grp {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 7px;
}

.status-sep {
    color: #4D5666;
}

.version-tag {
    padding: 2px 5px;
    border-radius: 3px;
    background-color: #202530;
    color: #9DA6B7;
    font-size: 8px;
}

/* TOAST */
#toast {
    position: absolute;
    right: 18px;
    bottom: 35px;
    z-index: 950;
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 8px;
    padding: 9px 14px;
    border: 1px #315344;
    border-radius: 7px;
    background-color: #17251F;
    color: #BBEBD5;
    font-size: 11px;
}

/* COLOR PICKER POPOVER (Photoshop style) */
#color-picker-win {
    position: absolute;
    left: 600px;
    top: 220px;
    width: 368px;
    background-color: #242832;
    border: 1px #3E4656;
    border-radius: 7px;
    z-index: 985;
    display: flex;
    flex-direction: column;
    overflow: hidden;
}

.cp-header {
    height: 28px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 0px 10px;
    background-color: #191D26;
    border-bottom: 1px #323948;
}

.cp-title {
    font-size: 11px;
    font-weight: bold;
    color: #E2E8F0;
}

button.cp-close-x {
    width: 20px;
    height: 20px;
    border: 0px rgba(0, 0, 0, 0);
    border-radius: 4px;
    background-color: rgba(0, 0, 0, 0);
    color: #94A3B8;
    font-size: 12px;
    text-align: center;
    line-height: 18px;
}

button.cp-close-x:hover {
    background-color: #2D3546;
    color: #FFFFFF;
}

.cp-body {
    display: flex;
    flex-direction: row;
    gap: 10px;
    padding: 10px;
    align-items: stretch;
}

#cp-sv-bg {
    position: relative;
    width: 176px;
    height: 176px;
    background-color: #FF0000;
    border: 1px #475569;
    overflow: hidden;
    flex-shrink: 0;
    cursor: crosshair;
}

.cp-fill {
    position: absolute;
    left: 0px;
    top: 0px;
    width: 100%;
    height: 100%;
    pointer-events: none;
}

#cp-sv-cursor {
    position: absolute;
    left: 80px;
    top: 80px;
    width: 11px;
    height: 11px;
    border-radius: 6px;
    border: 2px #FFFFFF;
    background-color: rgba(0, 0, 0, 35);
    pointer-events: none;
}

#cp-hue-wrap {
    position: relative;
    width: 22px;
    height: 176px;
    border: 1px #475569;
    flex-shrink: 0;
    cursor: pointer;
}

#cp-hue-thumb {
    position: absolute;
    left: -2px;
    right: -2px;
    top: 0px;
    height: 5px;
    border: 1px #FFFFFF;
    background-color: rgba(15, 23, 42, 120);
    pointer-events: none;
}

.cp-right {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
}

.cp-top-row {
    display: flex;
    flex-direction: row;
    gap: 8px;
    align-items: stretch;
}

.cp-preview-box {
    width: 50px;
    height: 46px;
    border: 1px #475569;
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
}

#cp-preview-new {
    width: 100%;
    height: 23px;
    background-color: #FFCC33;
}

#cp-preview-orig {
    width: 100%;
    height: 21px;
    background-color: #FFCC33;
    cursor: pointer;
}

.cp-btns {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 4px;
}

button.cp-btn {
    width: 100%;
    height: 21px;
    border: 1px #475569;
    border-radius: 4px;
    background-color: #303644;
    color: #E2E8F0;
    font-size: 10px;
}

button.cp-btn:hover {
    border-color: #64748B;
    color: #FFFFFF;
}

button.cp-btn.primary {
    background-color: #3B82F6;
    border-color: #60A5FA;
    color: #FFFFFF;
    font-weight: bold;
}

button.cp-btn.primary:hover {
    background-color: #2563EB;
}

.cp-grid {
    display: flex;
    flex-direction: row;
    gap: 6px;
}

.cp-col {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 3px;
}

.cp-f {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 4px;
    font-size: 10px;
    color: #CBD5E1;
}

.cp-lbl {
    width: 13px;
    color: #94A3B8;
    font-family: Consolas;
    font-size: 10px;
    flex-shrink: 0;
}

input.cp-in {
    flex: 1;
    min-width: 0;
    height: 20px;
    line-height: 18px;
    padding: 0px 5px;
    border: 1px #3B4252;
    border-radius: 3px;
    background-color: #13161E;
    color: #E5E7EB;
    font-family: Consolas;
    font-size: 10px;
}

.cp-hex-row {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 5px;
    margin-top: 2px;
}

.cp-swatches {
    display: flex;
    flex-direction: row;
    gap: 4px;
    padding: 0px 10px 10px 10px;
    align-items: center;
}

button.cp-preset-sw {
    width: 20px;
    height: 16px;
    border: 1px #475569;
    border-radius: 3px;
    padding: 0px;
}

button.cp-preset-sw:hover {
    border-color: #FFFFFF;
}

.hidden,
#menu-popup.hidden,
#color-picker-win.hidden,
#bottom-dock.hidden,
#ruler-top-row.hidden,
#ruler-left.hidden,
#split-bottom.hidden,
#canvas-empty-center.hidden,
#toast.hidden {
    display: none;
}
)RCSS";

const char* kEditorRml = R"RML(<rml>
<head>
    <title>RmlUI Studio</title>
    <link type="text/rcss" href="studio_ui.rcss" />
</head>
<body>
<div id="ide-root">
    <!-- TOPBAR (50px) -->
    <div id="topbar">
        <div class="brand-side">
            <div class="brand-mark"><img class="ico ico-17 ico-brand" src="studio_ico_layers" /></div>
            <span class="brand-name">RmlUI</span>
            <span class="brand-product">STUDIO</span>
            <span class="top-divider"></span>
            <span id="menu-filename" class="project-name">Projeto sem t&#237;tulo</span>
            <span class="saved-pill"><span class="dot-green"></span><span id="saved-text">Salvo</span></span>
        </div>

        <div class="menu-nav">
            <button id="m-file" class="menu-item"><span>Arquivo</span><img class="ico ico-12 ico-muted" src="studio_ico_chevron_down" /></button>
            <button id="m-edit" class="menu-item"><span>Editar</span><img class="ico ico-12 ico-muted" src="studio_ico_chevron_down" /></button>
            <button id="m-view" class="menu-item"><span>Exibir</span><img class="ico ico-12 ico-muted" src="studio_ico_chevron_down" /></button>
            <button id="m-vp" class="menu-item"><span>Viewport</span><img class="ico ico-12 ico-muted" src="studio_ico_chevron_down" /></button>
        </div>

        <div class="top-actions">
            <button id="btn-help" class="icon-button" title="Ajuda e Atalhos"><img class="ico ico-16" src="studio_ico_help" /></button>
            <button id="btn-settings" class="icon-button" title="Alternar Safe Area / Fundo"><img class="ico ico-16" src="studio_ico_settings" /></button>
            <div class="avatar">FC</div>
        </div>
    </div>

    <!-- TOOLBAR (45px) -->
    <div id="toolbar">
        <div class="toolbar-group">
            <button id="btn-new" class="toolbar-btn" title="Adicionar Elemento ou Novo Projeto"><img class="ico ico-14" src="studio_ico_plus" /><span>Novo</span></button>
            <button id="btn-imp-menu" class="toolbar-btn" title="Importar .RML ou .RCSS"><img class="ico ico-14" src="studio_ico_upload" /><span>Importar</span></button>
            <span class="toolbar-sep"></span>
            <button id="btn-undo" class="toolbar-btn square" title="Desfazer (Ctrl+Z)"><img class="ico ico-15" src="studio_ico_undo" /></button>
            <button id="btn-redo" class="toolbar-btn square" title="Refazer (Ctrl+Y)"><img class="ico ico-15" src="studio_ico_redo" /></button>
        </div>

        <div class="toolbar-center">
            <div class="device-switch">
                <button id="vp-pc" class="dev-btn active" title="Desktop 1920x1080"><img class="ico ico-13" src="studio_ico_desktop" /><span>Desktop 1920&#215;1080</span></button>
            </div>
            <button id="bg-grid" class="toolbar-btn selected" title="Alternar Grade"><img class="ico ico-14" src="studio_ico_grid" /><span>Grade</span></button>
            <span id="lbl-zoom" class="zoom-label">100%</span>
        </div>

        <div class="toolbar-group">
            <button id="btn-play" class="toolbar-btn" title="Alternar Pre-visualizacao Interativa"><img id="ico-play" class="ico ico-13" src="studio_ico_play" /><span id="lbl-play">Pr&#233;-visualizar</span></button>
            <button id="btn-export" class="export-btn" title="Exportar .RML + .RCSS"><img class="ico ico-14 ico-white" src="studio_ico_code" /><span>Exportar</span></button>
        </div>
    </div>

    <!-- WORKSPACE -->
    <div id="workstation">
        <!-- LEFT PANEL: HIERARQUIA (226px) -->
        <div id="left-panel">
            <div class="panel-heading">
                <div class="panel-heading-left">
                    <img class="ico ico-14 ico-soft" src="studio_ico_layers" />
                    <span>Hierarquia</span>
                </div>
                <button id="btn-hier-add" class="icon-button tiny" title="Adicionar Elemento"><img class="ico ico-14" src="studio_ico_plus" /></button>
            </div>

            <div class="search-wrap">
                <img class="ico ico-14 ico-muted" src="studio_ico_search" />
                <input id="hier-search" type="text" class="search-input" placeholder="Buscar elementos" />
            </div>

            <div class="tree-label-row">
                <span>CAMADAS</span>
                <span id="layers-count" class="tree-count">1</span>
            </div>

            <div id="layers-list"></div>

            <div class="left-footer">
                <span class="dot-green"></span>
                <span id="left-footer-text">Todos os elementos sincronizados</span>
            </div>
        </div>

        <div id="split-left" class="gutter-v"></div>

        <!-- CENTER COLUMN: CANVAS + CODE DOCK -->
        <div id="center-col">
            <div id="viewport-shell">
                <div class="canvas-toolbar">
                    <div class="canvas-breadcrumb">
                        <span>Canvas</span>
                        <img class="ico ico-12 ico-muted" src="studio_ico_chevron_right" />
                        <span id="canvas-res-lbl" class="res-bold">1920 &#215; 1080</span>
                    </div>

                    <div class="canvas-controls">
                        <button id="tool-sel" class="canvas-ctrl-btn control-active" title="Selecionar (V)"><img class="ico ico-13" src="studio_ico_pointer" /></button>
                        <button id="tool-pan" class="canvas-ctrl-btn" title="Mover Canvas (H)"><img class="ico ico-13" src="studio_ico_hand" /></button>
                        <span class="control-divider"></span>
                        <button id="btn-canvas-grid" class="canvas-ctrl-btn control-active" title="Grade"><img class="ico ico-14" src="studio_ico_grid" /></button>
                        <button id="btn-canvas-snap" class="canvas-ctrl-btn control-active" title="Guias Inteligentes e Snap (Ctrl desativa temporariamente)"><img class="ico ico-14" src="studio_ico_magnet" /></button>
                        <button id="btn-canvas-bg" class="canvas-bg-btn" title="Cor de Fundo do Editor (Branco / Custom)"><span id="swatch-canvas-bg" class="swatch-mini"></span><span id="lbl-canvas-bg">Fundo</span></button>
                        <span class="control-divider"></span>
                        <button id="btn-zoom-out" class="canvas-ctrl-btn" title="Diminuir Zoom"><span>&#8722;</span></button>
                        <span id="lbl-zoom-ctrl" class="mono">100%</span>
                        <button id="btn-zoom-in" class="canvas-ctrl-btn" title="Aumentar Zoom"><span>+</span></button>
                        <span class="control-divider"></span>
                        <button id="btn-fit" class="canvas-ctrl-btn" title="Ajustar ao Viewport"><span>Fit</span></button>
                    </div>
                </div>

                <div id="ruler-top-row">
                    <div id="ruler-corner" class="hidden">px</div>
                    <div id="ruler-top"></div>
                </div>

                <div id="viewport-mid-row">
                    <div id="ruler-left" class="hidden"></div>
                    <div id="viewport">
                        <div id="artboard-box">
                            <div id="safe-area-label">&#193;REA SEGURA</div>
                            <div id="canvas-empty-center">
                                <div class="empty-icon"><img class="ico ico-28 ico-hero-blue" src="studio_ico_file_code" /></div>
                                <div class="canvas-eyebrow">CANVAS VAZIO</div>
                                <div class="empty-h1">Sua pr&#243;xima ideia<br/>come&#231;a aqui.</div>
                                <div class="empty-p">Adicione elementos e construa sua interface.</div>
                                <button id="btn-empty-add" class="empty-add-btn"><img class="ico ico-15 ico-light-blue" src="studio_ico_plus" /><span>Adicionar elemento</span></button>
                            </div>
                            <div id="artboard-meta">
                                <span id="artboard-root-name">screen-root</span>
                                <span id="artboard-size-lbl">1920 &#215; 1080 px</span>
                            </div>
                        </div>
                    </div>
                </div>
            </div>

            <div id="split-bottom" class="gutter-h"></div>

            <!-- CODE DOCK (190px) -->
            <div id="bottom-dock">
                <div class="code-tabs">
                    <div class="code-tab-list">
                        <button id="tab-code-rml" class="code-tab current"><img class="ico ico-14" src="studio_ico_code" /><span>Estrutura</span><span class="tab-ext">.RML</span></button>
                        <button id="tab-code-rcss" class="code-tab"><img class="ico ico-14" src="studio_ico_braces" /><span>Estilos</span><span class="tab-ext">.RCSS</span></button>
                    </div>
                    <div class="code-actions">
                        <span id="code-cursor-lbl" class="code-cursor-lbl">Ln 1, Col 1</span>
                        <span class="code-status"><span id="code-status-dot" class="dot-green-sm"></span><span id="code-status-text">Sincronizado</span></span>
                        <button id="btn-expand-code" class="code-copy-btn" title="Expandir / Recolher Editor de Script"><span id="lbl-expand-code">Expandir</span></button>
                        <button id="btn-copy-code" class="code-copy-btn"><img class="ico ico-13" src="studio_ico_copy" /><span>Copiar c&#243;digo</span></button>
                    </div>
                </div>
                <div id="code-content">
                    <div id="code-gutter"><div id="code-gutter-inner"></div></div>
                    <div id="code-editor-wrap">
                        <div id="code-hl-clip">
                            <div id="code-active-line"></div>
                            <div id="code-highlight"></div>
                        </div>
                        <textarea id="code-editor" class="code-editor" wrap="nowrap"></textarea>
                    </div>
                </div>
            </div>
        </div>

        <div id="split-right" class="gutter-v"></div>

        <!-- RIGHT PANEL: INSPECTOR (292px) -->
        <div id="studio-inspector-panel">
            <div class="panel-heading">
                <div class="panel-heading-left">
                    <img class="ico ico-14 ico-soft" src="studio_ico_settings" />
                    <span>Inspector</span>
                </div>
                <button id="btn-insp-more" class="icon-button tiny" title="Acoes do Elemento"><img class="ico ico-15" src="studio_ico_more" /></button>
            </div>

            <div id="inspector-scroll">
                <div class="selected-component">
                    <div class="component-badge"><img class="ico ico-15 ico-blue" src="studio_ico_braces" /></div>
                    <div class="sel-comp-info">
                        <span id="insp-node-pill" class="sel-comp-name">screen root</span>
                        <span id="insp-node-sub" class="sel-comp-sub">Elemento selecionado</span>
                    </div>
                    <button id="btn-comp-more" class="icon-button tiny"><img class="ico ico-15" src="studio_ico_more" /></button>
                </div>

                <!-- Identificacao DOM -->
                <div class="inspector-section">
                    <div class="section-title"><span>Identifica&#231;&#227;o</span><span class="section-tag">DOM</span></div>
                    <div class="sec-body">
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">ID (#)</span>
                                <input id="prop-id" type="text" class="insp-in" />
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Classe (.)</span>
                                <input id="prop-class" type="text" class="insp-in" />
                            </div>
                        </div>
                        <div class="visibility-row">
                            <span>Vis&#237;vel no RmlUi</span>
                            <button id="prop-visible" class="toggle on" title="Alternar Visibilidade"><span class="toggle-knob"></span></button>
                        </div>
                        <div class="quick-actions">
                            <button id="act-dup" class="quick-btn" title="Duplicar (Ctrl+D)"><img class="ico ico-12" src="studio_ico_copy" /><span>Duplicar</span></button>
                            <button id="act-del" class="quick-btn" title="Excluir (Del)"><img class="ico ico-12" src="studio_ico_close" /><span>Excluir</span></button>
                            <button id="act-lock" class="quick-btn" title="Bloquear"><img class="ico ico-12" src="studio_ico_lock" /><span id="lbl-lock">Bloquear</span></button>
                        </div>
                    </div>
                </div>

                <!-- Transformar BOX MODEL -->
                <div class="inspector-section">
                    <div class="section-title"><span>Transformar</span><span class="section-tag">BOX MODEL</span></div>
                    <div class="sec-body">
                        <div class="align-toolbar">
                            <button id="btn-align-left" class="align-btn" title="Alinhar &#224; Esquerda"><img class="ico ico-13" src="studio_ico_al_left" /></button>
                            <button id="btn-align-hcenter" class="align-btn" title="Centralizar Horizontalmente"><img class="ico ico-13" src="studio_ico_al_hcenter" /></button>
                            <button id="btn-align-right" class="align-btn" title="Alinhar &#224; Direita"><img class="ico ico-13" src="studio_ico_al_right" /></button>
                            <button id="btn-align-top" class="align-btn" title="Alinhar ao Topo"><img class="ico ico-13" src="studio_ico_al_top" /></button>
                            <button id="btn-align-vcenter" class="align-btn" title="Centralizar Verticalmente"><img class="ico ico-13" src="studio_ico_al_vcenter" /></button>
                            <button id="btn-align-bottom" class="align-btn" title="Alinhar embaixo"><img class="ico ico-13" src="studio_ico_al_bottom" /></button>
                            <span class="align-sep"></span>
                            <button id="btn-dist-h" class="align-btn" title="Distribuir Horizontalmente (3+ elementos)"><img class="ico ico-13" src="studio_ico_dist_h" /></button>
                            <button id="btn-dist-v" class="align-btn" title="Distribuir Verticalmente (3+ elementos)"><img class="ico ico-13" src="studio_ico_dist_v" /></button>
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Posi&#231;&#227;o X</span>
                                <div class="input-unit"><input id="prop-x" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Posi&#231;&#227;o Y</span>
                                <div class="input-unit"><input id="prop-y" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Largura</span>
                                <div class="input-unit"><input id="prop-w" type="text" class="unit-in" /><span class="unit-sfx">&#8596;</span></div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Altura</span>
                                <div class="input-unit"><input id="prop-h" type="text" class="unit-in" /><span class="unit-sfx">&#8597;</span></div>
                            </div>
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Redim. W</span>
                                <div class="seg-row seg-row-compact">
                                    <button id="rw-fixed" class="seg-btn active" title="Largura fixa"><span>Fix</span></button>
                                    <button id="rw-hug" class="seg-btn" title="Ajustar ao conte&#250;do (Hug)"><span>Hug</span></button>
                                    <button id="rw-fill" class="seg-btn" title="Preencher container (Fill)"><span>Fill</span></button>
                                </div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Redim. H</span>
                                <div class="seg-row seg-row-compact">
                                    <button id="rh-fixed" class="seg-btn active" title="Altura fixa"><span>Fix</span></button>
                                    <button id="rh-hug" class="seg-btn" title="Ajustar ao conte&#250;do (Hug)"><span>Hug</span></button>
                                    <button id="rh-fill" class="seg-btn" title="Preencher container (Fill)"><span>Fill</span></button>
                                </div>
                            </div>
                        </div>
                        <div class="field-col last">
                            <span class="field-caption">Unidade</span>
                            <div class="seg-row">
                                <button id="u-px" class="seg-btn active"><span>px</span></button>
                                <button id="u-pct" class="seg-btn"><span>%</span></button>
                                <button id="u-dp" class="seg-btn"><span>dp</span></button>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Ancoragem CONSTRAINTS -->
                <div class="inspector-section">
                    <div class="section-title"><span>Ancoragem</span><span class="section-tag">CONSTRAINTS</span></div>
                    <div class="sec-body">
                        <div class="constraint-row">
                            <div id="constraint-diagram" title="Clique nos pinos para ancorar">
                                <div class="cd-inner-box"></div>
                                <button id="cpin-top" class="cd-pin" title="Topo"></button>
                                <button id="cpin-bottom" class="cd-pin" title="Embaixo"></button>
                                <button id="cpin-left" class="cd-pin active" title="Esquerda"></button>
                                <button id="cpin-right" class="cd-pin" title="Direita"></button>
                                <button id="cpin-hcenter" class="cd-pin" title="Centralizar H"></button>
                                <button id="cpin-vcenter" class="cd-pin" title="Centralizar V"></button>
                            </div>
                            <div class="constraint-controls">
                                <div class="field-col last">
                                    <span class="field-caption">Horizontal</span>
                                    <div class="seg-row seg-row-compact">
                                        <button id="ch-left" class="seg-btn active" title="Esquerda"><span>Esq</span></button>
                                        <button id="ch-right" class="seg-btn" title="Direita"><span>Dir</span></button>
                                        <button id="ch-lr" class="seg-btn" title="Esquerda + Direita"><span>E+D</span></button>
                                        <button id="ch-center" class="seg-btn" title="Centro"><span>Cen</span></button>
                                        <button id="ch-scale" class="seg-btn" title="Escalar"><span>Esc</span></button>
                                    </div>
                                </div>
                                <div class="field-col last">
                                    <span class="field-caption">Vertical</span>
                                    <div class="seg-row seg-row-compact">
                                        <button id="cv-top" class="seg-btn active" title="Topo"><span>Top</span></button>
                                        <button id="cv-bottom" class="seg-btn" title="Embaixo"><span>Inf</span></button>
                                        <button id="cv-tb" class="seg-btn" title="Topo + Embaixo"><span>T+I</span></button>
                                        <button id="cv-center" class="seg-btn" title="Centro"><span>Cen</span></button>
                                        <button id="cv-scale" class="seg-btn" title="Escalar"><span>Esc</span></button>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Auto Layout FLEXBOX -->
                <div class="inspector-section">
                    <div class="section-title"><span>Auto Layout</span><span class="section-tag">FLEXBOX</span></div>
                    <div class="sec-body">
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Display</span>
                                <div class="seg-row">
                                    <button id="disp-flex" class="seg-btn active"><span>Flex</span></button>
                                    <button id="disp-block" class="seg-btn"><span>Block</span></button>
                                </div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Wrap</span>
                                <div class="seg-row">
                                    <button id="wrap-off" class="seg-btn active"><span>Fixo</span></button>
                                    <button id="wrap-on" class="seg-btn"><span>Quebrar</span></button>
                                </div>
                            </div>
                        </div>
                        <div class="field-col">
                            <span class="field-caption">Dire&#231;&#227;o</span>
                            <div class="seg-row">
                                <button id="dir-col" class="seg-btn active"><span>Coluna</span></button>
                                <button id="dir-row" class="seg-btn"><span>Linha</span></button>
                            </div>
                        </div>
                        <div class="field-col">
                            <span class="field-caption">Justify Content</span>
                            <div class="seg-row seg-row-compact">
                                <button id="jc-start" class="seg-btn active"><span>Ini</span></button>
                                <button id="jc-center" class="seg-btn"><span>Cen</span></button>
                                <button id="jc-end" class="seg-btn"><span>Fim</span></button>
                                <button id="jc-between" class="seg-btn"><span>Esp</span></button>
                            </div>
                        </div>
                        <div class="field-col">
                            <span class="field-caption">Align Items</span>
                            <div class="seg-row seg-row-compact">
                                <button id="ai-start" class="seg-btn active"><span>Ini</span></button>
                                <button id="ai-center" class="seg-btn"><span>Cen</span></button>
                                <button id="ai-end" class="seg-btn"><span>Fim</span></button>
                            </div>
                        </div>
                        <div class="field-grid2 last">
                            <div class="field-col last">
                                <span class="field-caption">Gap</span>
                                <div class="input-unit"><input id="prop-gap" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                            <div class="field-col last">
                                <span class="field-caption">Padding</span>
                                <div class="input-unit"><input id="prop-pad" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Grades de Layout LAYOUT GRIDS -->
                <div class="inspector-section">
                    <div class="section-title"><span>Grades de Layout</span><span class="section-tag">GRID</span></div>
                    <div class="sec-body">
                        <div class="grid-head-row">
                            <span>Exibir Grade no Container</span>
                            <button id="grid-enable" class="toggle" title="Ativar grade de layout no elemento"><span class="toggle-knob"></span></button>
                        </div>
                        <div class="field-col">
                            <span class="field-caption">Tipo</span>
                            <div class="seg-row seg-row-compact">
                                <button id="gtype-col" class="seg-btn active"><span>Colunas</span></button>
                                <button id="gtype-row" class="seg-btn"><span>Linhas</span></button>
                                <button id="gtype-grid" class="seg-btn"><span>Grade</span></button>
                            </div>
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Quantidade</span>
                                <div class="input-unit"><input id="prop-grid-count" type="text" class="unit-in" /><span class="unit-sfx">x</span></div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Tamanho</span>
                                <div class="input-unit"><input id="prop-grid-size" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Gutter</span>
                                <div class="input-unit"><input id="prop-grid-gutter" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Margem</span>
                                <div class="input-unit"><input id="prop-grid-margin" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                        </div>
                        <div class="color-field" style="margin-bottom:0px;">
                            <span>Cor da Grade</span>
                            <div class="color-input-grp">
                                <div id="swatch-grid" class="swatch"></div>
                                <input id="prop-grid-color" type="text" class="hex-in" />
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Aparencia ESTILO -->
                <div class="inspector-section">
                    <div class="section-title"><span>Apar&#234;ncia</span><span class="section-tag">ESTILO</span></div>
                    <div class="sec-body">
                        <div class="color-field">
                            <span>Preenchimento</span>
                            <div class="color-input-grp">
                                <div id="swatch-bg" class="swatch"></div>
                                <input id="prop-bg" type="text" class="hex-in" />
                                <input id="prop-bg-a" type="text" class="alpha-in" title="Alpha 0-255" />
                            </div>
                        </div>
                        <div class="color-field">
                            <span>Borda</span>
                            <div class="color-input-grp">
                                <div id="swatch-border" class="swatch"></div>
                                <input id="prop-border" type="text" class="hex-in" />
                                <input id="prop-border-w" type="text" class="alpha-in" title="Espessura px" />
                            </div>
                        </div>
                        <div class="field-grid2 last">
                            <div class="field-col last">
                                <span class="field-caption">Raio</span>
                                <div class="input-unit"><input id="prop-radius" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                            <div class="field-col last">
                                <span class="field-caption">Alpha Borda</span>
                                <div class="input-unit"><input id="prop-border-a" type="text" class="unit-in" /><span class="unit-sfx">A</span></div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Tipografia TEXTO -->
                <div class="inspector-section">
                    <div class="section-title"><span>Tipografia</span><span class="section-tag">TEXTO</span></div>
                    <div class="sec-body">
                        <div class="field-col">
                            <span class="field-caption">Conte&#250;do</span>
                            <input id="prop-text" type="text" class="insp-in" />
                        </div>
                        <div class="field-grid2">
                            <div class="field-col">
                                <span class="field-caption">Tamanho</span>
                                <div class="input-unit"><input id="prop-font-size" type="text" class="unit-in" /><span class="unit-sfx">px</span></div>
                            </div>
                            <div class="field-col">
                                <span class="field-caption">Alinhar</span>
                                <div class="seg-row">
                                    <button id="al-left" class="seg-btn active"><span>E</span></button>
                                    <button id="al-center" class="seg-btn"><span>C</span></button>
                                    <button id="al-right" class="seg-btn"><span>D</span></button>
                                </div>
                            </div>
                        </div>
                        <div class="color-field">
                            <span>Cor Texto</span>
                            <div class="color-input-grp">
                                <div id="swatch-fg" class="swatch"></div>
                                <input id="prop-fg" type="text" class="hex-in" />
                            </div>
                        </div>
                        <div class="field-col last">
                            <span class="field-caption">Imagem</span>
                            <div class="field-grid2 last">
                                <input id="prop-src" type="text" class="insp-in" style="flex:1; min-width:0;" />
                                <button id="btn-pick-img" class="pick-btn"><span>...</span></button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <!-- STATUSBAR (23px) -->
    <div id="status-bar">
        <div class="status-grp">
            <span class="dot-green-sm"></span>
            <span id="sb-dom">Pronto / screen-root</span>
        </div>
        <div class="status-grp">
            <span id="sb-vp">RML &#183; Canvas em tempo real</span>
        </div>
        <div class="status-grp">
            <span>RmlUI Studio</span>
            <span class="version-tag">v1.0</span>
        </div>
    </div>

    <!-- DROPDOWN POPUP, COLOR PICKER & TOAST -->
    <div id="menu-popup" class="hidden"></div>
    <div id="color-picker-win" class="hidden">
        <div class="cp-header">
            <span id="cp-title" class="cp-title">Seletor de Cores</span>
            <button id="cp-close" class="cp-close-x" title="Fechar (Esc)">&#215;</button>
        </div>
        <div class="cp-body">
            <div id="cp-sv-bg">
                <img class="cp-fill" src="studio_cp_sv_white" />
                <img class="cp-fill" src="studio_cp_sv_black" />
                <div id="cp-sv-cursor"></div>
            </div>
            <div id="cp-hue-wrap">
                <img class="cp-fill" src="studio_cp_hue" />
                <div id="cp-hue-thumb"></div>
            </div>
            <div class="cp-right">
                <div class="cp-top-row">
                    <div class="cp-preview-box">
                        <div id="cp-preview-new" title="Nova cor"></div>
                        <div id="cp-preview-orig" title="Cor original (clique para restaurar)"></div>
                    </div>
                    <div class="cp-btns">
                        <button id="cp-ok" class="cp-btn primary">OK</button>
                        <button id="cp-cancel" class="cp-btn">Cancelar</button>
                    </div>
                </div>
                <div class="cp-grid">
                    <div class="cp-col">
                        <div class="cp-f"><span class="cp-lbl">H</span><input id="cp-in-h" type="text" class="cp-in" /></div>
                        <div class="cp-f"><span class="cp-lbl">S</span><input id="cp-in-s" type="text" class="cp-in" /></div>
                        <div class="cp-f"><span class="cp-lbl">B</span><input id="cp-in-v" type="text" class="cp-in" /></div>
                    </div>
                    <div class="cp-col">
                        <div class="cp-f"><span class="cp-lbl">R</span><input id="cp-in-r" type="text" class="cp-in" /></div>
                        <div class="cp-f"><span class="cp-lbl">G</span><input id="cp-in-g" type="text" class="cp-in" /></div>
                        <div class="cp-f"><span class="cp-lbl">B</span><input id="cp-in-b" type="text" class="cp-in" /></div>
                    </div>
                </div>
                <div class="cp-hex-row">
                    <span class="cp-lbl">#</span>
                    <input id="cp-in-hex" type="text" class="cp-in" />
                </div>
            </div>
        </div>
        <div class="cp-swatches">
            <button id="cp-sw-0" class="cp-preset-sw" style="background-color: #FFFFFF;" title="Branco #FFFFFF"></button>
            <button id="cp-sw-1" class="cp-preset-sw" style="background-color: #E2E8F0;" title="Claro #E2E8F0"></button>
            <button id="cp-sw-2" class="cp-preset-sw" style="background-color: #64748B;" title="Cinza #64748B"></button>
            <button id="cp-sw-3" class="cp-preset-sw" style="background-color: #1E293B;" title="Ard&#243;sia #1E293B"></button>
            <button id="cp-sw-4" class="cp-preset-sw" style="background-color: #0E1117;" title="Dark #0E1117"></button>
            <button id="cp-sw-5" class="cp-preset-sw" style="background-color: #000000;" title="Preto #000000"></button>
            <button id="cp-sw-6" class="cp-preset-sw" style="background-color: #EF4444;" title="Vermelho #EF4444"></button>
            <button id="cp-sw-7" class="cp-preset-sw" style="background-color: #F59E0B;" title="&#194;mbar #F59E0B"></button>
            <button id="cp-sw-8" class="cp-preset-sw" style="background-color: #10B981;" title="Verde #10B981"></button>
            <button id="cp-sw-9" class="cp-preset-sw" style="background-color: #3B82F6;" title="Azul #3B82F6"></button>
            <button id="cp-sw-10" class="cp-preset-sw" style="background-color: #8B5CF6;" title="Roxo #8B5CF6"></button>
        </div>
    </div>
    <div id="toast" class="hidden"><img class="ico ico-15 ico-green" src="studio_ico_check" /><span id="toast-msg">Pronto</span></div>
</div>
</body>
</rml>)RML";

} // namespace

struct StudioUI::Impl {
    HWND hwnd = nullptr;
    RmlDocument* doc = nullptr;
    RmlEngineHost* engine = nullptr;
    Rml::ElementDocument* ui_doc = nullptr;

    int win_w = 1440;
    int win_h = 860;

    // Layout dimensions matching v0-project
    bool show_rulers = true;
    bool show_left_ruler = false;
    bool show_bottom_dock = true;
    std::string active_menu;
    std::string code_tab = "RML";
    std::string search_filter;

    float left_panel_width = 226.0f;
    float right_inspector_width = 292.0f;
    float bottom_dock_height = 190.0f;
    static constexpr float kSplitW = 4.0f;
    static constexpr float kTopBarH = 50.0f;
    static constexpr float kToolbarH = 45.0f;
    static constexpr float kCanvasToolbarH = 39.0f;
    static constexpr float kRulerH = 20.0f;
    static constexpr float kRulerLeftW = 28.0f;
    static constexpr float kStatusBarH = 23.0f;

    enum class SplitterDrag { None, Left, Right, Bottom };
    SplitterDrag splitter_drag = SplitterDrag::None;
    int split_start_mouse = 0;
    float split_start_size = 0.0f;

    float LeftTotal() const { return left_panel_width + kSplitW + ((show_rulers && show_left_ruler) ? kRulerLeftW : 0.0f); }
    float RightTotal() const { return kSplitW + right_inspector_width; }
    float TopTotal() const { return kTopBarH + kToolbarH + kCanvasToolbarH + (show_rulers ? kRulerH : 0.0f); }
    float BottomTotal() const { return (show_bottom_dock ? (kSplitW + bottom_dock_height) : 0.0f) + kStatusBarH; }

    float ViewportWidth() const { return std::max(100.0f, (float)win_w - LeftTotal() - RightTotal()); }
    float ViewportHeight() const { return std::max(100.0f, (float)win_h - TopTotal() - BottomTotal()); }

    void ApplyPanelSizes() { ApplyPanelSizes(win_w, win_h); }

    void ApplyPanelSizes(int w, int h) {
        if (!ui_doc) return;
        win_w = std::max(1, w);
        win_h = std::max(1, h);

        float max_panels_sum = std::max(380.0f, (float)win_w - kSplitW * 2.0f - 140.0f);
        left_panel_width = std::clamp(left_panel_width, 180.0f, std::min(420.0f, max_panels_sum - 240.0f));
        right_inspector_width = std::clamp(right_inspector_width, 250.0f, std::min(460.0f, max_panels_sum - left_panel_width));
        float work_h = std::max(160.0f, (float)win_h - kTopBarH - kToolbarH - kStatusBarH);
        bottom_dock_height = std::clamp(bottom_dock_height, 100.0f, std::min(420.0f, work_h - 110.0f));

        float center_w = std::max(120.0f, (float)win_w - left_panel_width - (kSplitW * 2.0f) - right_inspector_width);
        float vp_shell_h = std::max(80.0f, work_h - (show_bottom_dock ? (kSplitW + bottom_dock_height) : 0.0f));
        float dock_h = show_bottom_dock ? bottom_dock_height : 0.0f;

        auto set_px = [&](const char* id, const char* prop, float val) {
            if (auto* el = ui_doc->GetElementById(id)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%dpx", (int)std::round(val));
                el->SetProperty(prop, buf);
            }
        };
        auto set_box = [&](const char* id, float x, float y, float bw, float bh) {
            set_px(id, "left", x);
            set_px(id, "top", y);
            set_px(id, "width", bw);
            set_px(id, "height", bh);
        };

        const float split_l_x = left_panel_width;
        const float center_x = split_l_x + kSplitW;
        const float split_r_x = center_x + center_w;
        const float right_x = split_r_x + kSplitW;

        set_px("workstation", "height", work_h);
        set_box("left-panel", 0.0f, 0.0f, left_panel_width, work_h);
        set_box("split-left", split_l_x, 0.0f, kSplitW, work_h);
        set_box("center-col", center_x, 0.0f, center_w, work_h);
        set_box("split-right", split_r_x, 0.0f, kSplitW, work_h);
        set_box("studio-inspector-panel", right_x, 0.0f, right_inspector_width, work_h);

        set_box("viewport-shell", 0.0f, 0.0f, center_w, vp_shell_h);
        set_box("split-bottom", 0.0f, vp_shell_h, center_w, kSplitW);
        set_box("bottom-dock", 0.0f, vp_shell_h + kSplitW, center_w, dock_h);

        if (auto* bd = ui_doc->GetElementById("bottom-dock")) bd->SetClass("hidden", !show_bottom_dock);
        if (auto* sb = ui_doc->GetElementById("split-bottom")) sb->SetClass("hidden", !show_bottom_dock);
        if (auto* rc = ui_doc->GetElementById("ruler-corner")) rc->SetClass("hidden", !(show_rulers && show_left_ruler));
        if (auto* rl = ui_doc->GetElementById("ruler-left")) rl->SetClass("hidden", !(show_rulers && show_left_ruler));

        overlay_dirty = true;
    }

    // Multi-selection
    std::vector<int> selected_ids;
    int selection_anchor_id = 0;

    int PrimaryId() const {
        if (!selected_ids.empty()) return selected_ids.back();
        return doc ? doc->root_id : 0;
    }

    bool IsSelected(int id) const {
        return std::find(selected_ids.begin(), selected_ids.end(), id) != selected_ids.end();
    }

    void SetSingleSelection(int id) {
        selected_ids.clear();
        selected_ids.push_back(id);
        selection_anchor_id = id;
    }

    bool play_mode = false;
    bool hand_tool = false;
    bool safe_area = true;
    bool show_grid = true;
    enum class BgStyle { Dark, White, Gray, Gta, Custom };
    BgStyle bg_style = BgStyle::Dark;
    RmlColor canvas_bg_color{14, 17, 23, 255};
    std::string unit_mode = "px";

    bool syncing_ui = false;
    bool suppress_code_sync = false;
    bool code_expanded = false;
    uint64_t last_synced_rev = 0;
    int last_synced_sel = -1;
    size_t last_synced_sel_count = 0;
    std::string last_synced_code_tab;
    uint64_t last_code_undo_ms = 0;
    std::string last_code_undo_tab;
    uint64_t last_prop_undo_ms = 0;
    std::string last_prop_undo_field;
    std::string active_edit_input_id;
    bool force_write_inputs = false;
    int last_gutter_lines = -1;
    int last_gutter_cur_line = -1;
    int last_gutter_col = -1;
    float last_code_scroll_top = -1.0f;
    float last_code_scroll_left = -1.0f;
    int last_cursor_sel_start = -1;
    std::string last_highlighted_code;
    std::string last_highlighted_tab;
    bool overlay_dirty = true;

    enum class ColorTarget { None, NodeBg, NodeBorder, NodeFg, CanvasBg, NodeGrid };
    ColorTarget cp_target = ColorTarget::None;
    bool cp_open = false;
    RmlColor cp_orig_color{255, 255, 255, 255};
    RmlColor cp_curr_color{255, 204, 51, 255};
    float cp_h = 43.0f;
    float cp_s = 0.80f;
    float cp_v = 1.00f;
    enum class CpDrag { None, SV, Hue };
    CpDrag cp_drag = CpDrag::None;

    std::vector<RmlDocument> undo_stack;
    std::vector<RmlDocument> redo_stack;
    static constexpr size_t kMaxHistory = 40;

    float pan_x = 300.0f;
    float pan_y = 180.0f;
    float zoom = 0.40f;

    bool panning = false;
    int pan_start_mx = 0;
    int pan_start_my = 0;
    float pan_start_x = 0.0f;
    float pan_start_y = 0.0f;

    enum class DragMode { None, Move, Resize };
    DragMode drag_mode = DragMode::None;
    bool drag_pending = false;
    int drag_down_mx = 0;
    int drag_down_my = 0;
    bool drag_did_move = false;
    bool drag_transform_ui_dirty = false;
    int drag_handle = -1;
    float drag_start_sx = 0.0f;
    float drag_start_sy = 0.0f;
    float orig_x = 0.0f;
    float orig_y = 0.0f;
    float orig_w = 0.0f;
    float orig_h = 0.0f;
    struct DragOrigNode { int id; float x; float y; float w; float h; };
    std::vector<DragOrigNode> drag_multi_orig;
    std::vector<int> drag_dirty_node_ids;

    bool snap_guides = true;
    struct SmartGuideLine {
        bool vertical = true;
        float coord = 0.0f;
        float span_min = 0.0f;
        float span_max = 0.0f;
    };
    std::vector<SmartGuideLine> active_guides;

    int mouse_x = 0;
    int mouse_y = 0;
    bool space_down = false;

    uint64_t toast_hide_at_ms = 0;
    bool toast_visible = false;

    void ShowToast(const std::string& msg) {
        if (!ui_doc) return;
        if (auto* tm = ui_doc->GetElementById("toast-msg")) {
            tm->SetInnerRML(EscapeXml(msg));
        }
        if (auto* t = ui_doc->GetElementById("toast")) {
            t->SetClass("hidden", false);
        }
        toast_visible = true;
        toast_hide_at_ms = GetTickCount64() + 2300ULL;
    }

    void UpdateToastTimer() {
        if (!toast_visible || !ui_doc) return;
        if (GetTickCount64() >= toast_hide_at_ms) {
            if (auto* t = ui_doc->GetElementById("toast")) {
                t->SetClass("hidden", true);
            }
            toast_visible = false;
        }
    }

    // Deferred Command Queue (avoids use-after-free when listeners mutate DOM)
    struct PendingCmd {
        std::string action;
        int node_id = 0;
        bool ctrl = false;
        bool shift = false;
        std::string field;
        bool from_popup = false;
    };
    std::vector<PendingCmd> pending_cmds;

    struct ClickHandler : public Rml::EventListener {
        Impl* self = nullptr;
        std::string action;
        int node_id = 0;
        bool is_popup_item = false;
        void ProcessEvent(Rml::Event& ev) override {
            if (!self) return;
            if (is_popup_item || action == "select_node" || action == "toggle_collapse" ||
                action == "toggle_vis" || action == "toggle_lock" || action == "dup_node" ||
                action == "del_node" || action == "empty_add") {
                ev.StopPropagation();
            }
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || ev.GetParameter<bool>("ctrl_key", false);
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || ev.GetParameter<bool>("shift_key", false);
            self->pending_cmds.push_back({action, node_id, ctrl, shift, "", is_popup_item});
        }
    };

    struct ChangeHandler : public Rml::EventListener {
        Impl* self = nullptr;
        std::string field;
        void ProcessEvent(Rml::Event& ev) override {
            if (!self || self->syncing_ui) return;
            bool is_commit = (ev.GetId() == Rml::EventId::Blur) || ev.GetParameter<bool>("linebreak", false);
            self->pending_cmds.push_back({"__field_change__", 0, is_commit, false, field, false});
        }
    };

    std::vector<std::unique_ptr<ClickHandler>> click_listeners;
    std::vector<std::unique_ptr<ChangeHandler>> change_listeners;
    std::vector<std::unique_ptr<ClickHandler>> layer_listeners;
    std::vector<std::unique_ptr<ClickHandler>> popup_listeners;

    void PushUndo() {
        if (!doc) return;
        undo_stack.push_back(*doc);
        if (undo_stack.size() > kMaxHistory) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
        if (auto* st = ui_doc ? ui_doc->GetElementById("saved-text") : nullptr) {
            st->SetInnerRML("Editado");
        }
    }

    void Undo() {
        if (!doc || undo_stack.empty()) return;
        redo_stack.push_back(*doc);
        *doc = std::move(undo_stack.back());
        undo_stack.pop_back();
        doc->Touch();
        SyncUIFromDoc(true);
        ShowToast("Ação desfeita");
    }

    void Redo() {
        if (!doc || redo_stack.empty()) return;
        undo_stack.push_back(*doc);
        *doc = std::move(redo_stack.back());
        redo_stack.pop_back();
        doc->Touch();
        SyncUIFromDoc(true);
        ShowToast("Ação refeita");
    }

    bool GetNodeCanvasBounds(int id, float& wx, float& wy, float& ww, float& wh) const {
        if (!doc) return false;
        const auto* n = doc->FindNode(id);
        if (!n) return false;
        if (engine && engine->GetCanvasElementRect(id, wx, wy, ww, wh)) {
            ww = std::max(1.0f, ww);
            wh = std::max(1.0f, wh);
            return true;
        }
        doc->GetWorldPos(id, wx, wy);
        ww = n->width;
        wh = n->height;
        return true;
    }

    void GetParentCanvasOrigin(int parent_id, float& ox, float& oy, int child_id = -1) const {
        if (child_id > 0 && engine && engine->GetCanvasElementOffsetParentOrigin(child_id, ox, oy)) {
            return;
        }
        ox = 0.0f;
        oy = 0.0f;
        if (!doc || parent_id == doc->root_id) return;
        float pw = 0.0f, ph = 0.0f;
        GetNodeCanvasBounds(parent_id, ox, oy, pw, ph);
    }

    bool IsInDragOrigSet(int node_id) const {
        for (const auto& d : drag_multi_orig) {
            if (d.id == node_id) return true;
        }
        return false;
    }

    void RestoreSubtreeForConstraintDrag(const RmlDocument& snapshot, int container_id) {
        if (!doc) return;
        const auto* c = doc->FindNode(container_id);
        if (!c) return;
        std::vector<int> stack = c->children;
        while (!stack.empty()) {
            int cid = stack.back();
            stack.pop_back();
            if (auto* cur = doc->FindNode(cid)) {
                if (const auto* orig = snapshot.FindNode(cid)) {
                    cur->x = orig->x;
                    cur->y = orig->y;
                    cur->width = orig->width;
                    cur->height = orig->height;
                }
                for (int sub : cur->children) stack.push_back(sub);
            }
        }
    }

    void ComputeDragSnapMove(int primary_id, float raw_dx, float raw_dy, bool bypass_snap, float& out_dx, float& out_dy) {
        active_guides.clear();
        out_dx = raw_dx;
        out_dy = raw_dy;
        if (!snap_guides || bypass_snap || !doc) return;
        const auto* pn = doc->FindNode(primary_id);
        if (!pn) return;

        float ox = 0.0f, oy = 0.0f;
        GetParentCanvasOrigin(pn->parent_id, ox, oy, pn->id);
        float px = ox + orig_x + raw_dx;
        float py = oy + orig_y + raw_dy;
        float pw = std::max(1.0f, orig_w);
        float ph = std::max(1.0f, orig_h);
        if (!drag_multi_orig.empty()) {
            for (const auto& d : drag_multi_orig) {
                if (d.id == primary_id) {
                    px = ox + d.x + raw_dx;
                    py = oy + d.y + raw_dy;
                    pw = std::max(1.0f, d.w);
                    ph = std::max(1.0f, d.h);
                    break;
                }
            }
        }

        struct RefTarget { float coord; float s0; float s1; };
        std::vector<RefTarget> refs_x;
        std::vector<RefTarget> refs_y;

        auto push_rect_refs = [&](float rx, float ry, float rw, float rh) {
            refs_x.push_back({rx, ry, ry + rh});
            refs_x.push_back({rx + rw * 0.5f, ry, ry + rh});
            refs_x.push_back({rx + rw, ry, ry + rh});
            refs_y.push_back({ry, rx, rx + rw});
            refs_y.push_back({ry + rh * 0.5f, rx, rx + rw});
            refs_y.push_back({ry + rh, rx, rx + rw});
        };

        // Artboard bounds & centers
        push_rect_refs(0.0f, 0.0f, (float)doc->screen_width, (float)doc->screen_height);
        if (pn->parent_id != doc->root_id) {
            float prx = 0.0f, pry = 0.0f, prw = 0.0f, prh = 0.0f;
            if (GetNodeCanvasBounds(pn->parent_id, prx, pry, prw, prh)) {
                push_rect_refs(prx, pry, prw, prh);
            }
        }
        // Siblings in same parent
        if (const auto* parent = doc->FindNode(pn->parent_id)) {
            for (int sib_id : parent->children) {
                if (sib_id == primary_id || IsInDragOrigSet(sib_id)) continue;
                const auto* sib = doc->FindNode(sib_id);
                if (!sib || !sib->visible) continue;
                float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;
                if (GetNodeCanvasBounds(sib_id, sx, sy, sw, sh)) {
                    push_rect_refs(sx, sy, sw, sh);
                }
            }
        }

        float thresh = std::clamp(6.0f / std::max(0.25f, zoom), 2.5f, 14.0f);
        float edges_x[3] = {px, px + pw * 0.5f, px + pw};
        float best_dx = 0.0f;
        float best_ax = thresh + 0.001f;
        for (float ex : edges_x) {
            for (const auto& rx : refs_x) {
                float diff = rx.coord - ex;
                if (std::fabs(diff) < best_ax) {
                    best_ax = std::fabs(diff);
                    best_dx = diff;
                }
            }
        }
        if (best_ax <= thresh) {
            out_dx += best_dx;
            px += best_dx;
        }

        float edges_y[3] = {py, py + ph * 0.5f, py + ph};
        float best_dy = 0.0f;
        float best_ay = thresh + 0.001f;
        for (float ey : edges_y) {
            for (const auto& ry : refs_y) {
                float diff = ry.coord - ey;
                if (std::fabs(diff) < best_ay) {
                    best_ay = std::fabs(diff);
                    best_dy = diff;
                }
            }
        }
        if (best_ay <= thresh) {
            out_dy += best_dy;
            py += best_dy;
        }

        constexpr float kEps = 0.55f;
        float final_ex[3] = {px, px + pw * 0.5f, px + pw};
        for (float ex : final_ex) {
            bool matched = false;
            float mn = py, mx = py + ph;
            for (const auto& rx : refs_x) {
                if (std::fabs(rx.coord - ex) <= kEps) {
                    matched = true;
                    mn = std::min(mn, rx.s0);
                    mx = std::max(mx, rx.s1);
                }
            }
            if (matched) active_guides.push_back({true, ex, mn, mx});
        }
        float final_ey[3] = {py, py + ph * 0.5f, py + ph};
        for (float ey : final_ey) {
            bool matched = false;
            float mn = px, mx = px + pw;
            for (const auto& ry : refs_y) {
                if (std::fabs(ry.coord - ey) <= kEps) {
                    matched = true;
                    mn = std::min(mn, ry.s0);
                    mx = std::max(mx, ry.s1);
                }
            }
            if (matched) active_guides.push_back({false, ey, mn, mx});
        }
    }

    void ComputeDragSnapResize(int node_id, int handle, bool bypass_snap, float& nx, float& ny, float& nw, float& nh) {
        active_guides.clear();
        if (!snap_guides || bypass_snap || !doc) return;
        const auto* pn = doc->FindNode(node_id);
        if (!pn) return;

        float ox = 0.0f, oy = 0.0f;
        GetParentCanvasOrigin(pn->parent_id, ox, oy, pn->id);
        float px = ox + nx;
        float py = oy + ny;

        struct RefTarget { float coord; float s0; float s1; };
        std::vector<RefTarget> refs_x;
        std::vector<RefTarget> refs_y;
        auto push_rect_refs = [&](float rx, float ry, float rw, float rh) {
            refs_x.push_back({rx, ry, ry + rh});
            refs_x.push_back({rx + rw * 0.5f, ry, ry + rh});
            refs_x.push_back({rx + rw, ry, ry + rh});
            refs_y.push_back({ry, rx, rx + rw});
            refs_y.push_back({ry + rh * 0.5f, rx, rx + rw});
            refs_y.push_back({ry + rh, rx, rx + rw});
        };
        push_rect_refs(0.0f, 0.0f, (float)doc->screen_width, (float)doc->screen_height);
        if (pn->parent_id != doc->root_id) {
            float prx = 0.0f, pry = 0.0f, prw = 0.0f, prh = 0.0f;
            if (GetNodeCanvasBounds(pn->parent_id, prx, pry, prw, prh)) {
                push_rect_refs(prx, pry, prw, prh);
            }
        }
        if (const auto* parent = doc->FindNode(pn->parent_id)) {
            for (int sib_id : parent->children) {
                if (sib_id == node_id) continue;
                const auto* sib = doc->FindNode(sib_id);
                if (!sib || !sib->visible) continue;
                float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;
                if (GetNodeCanvasBounds(sib_id, sx, sy, sw, sh)) {
                    push_rect_refs(sx, sy, sw, sh);
                }
            }
        }

        float thresh = std::clamp(6.0f / std::max(0.25f, zoom), 2.5f, 14.0f);
        bool move_l = (handle == 0 || handle == 6 || handle == 7);
        bool move_r = (handle == 2 || handle == 3 || handle == 4);
        bool move_t = (handle == 0 || handle == 1 || handle == 2);
        bool move_b = (handle == 4 || handle == 5 || handle == 6);

        if (move_l) {
            float best = thresh + 0.001f, delta = 0.0f;
            for (const auto& rx : refs_x) {
                float d = rx.coord - px;
                if (std::fabs(d) < best && (nw - d) >= 12.0f) { best = std::fabs(d); delta = d; }
            }
            if (best <= thresh) { nx += delta; px += delta; nw -= delta; }
        } else if (move_r) {
            float right_x = px + nw;
            float best = thresh + 0.001f, delta = 0.0f;
            for (const auto& rx : refs_x) {
                float d = rx.coord - right_x;
                if (std::fabs(d) < best && (nw + d) >= 12.0f) { best = std::fabs(d); delta = d; }
            }
            if (best <= thresh) { nw += delta; }
        }

        if (move_t) {
            float best = thresh + 0.001f, delta = 0.0f;
            for (const auto& ry : refs_y) {
                float d = ry.coord - py;
                if (std::fabs(d) < best && (nh - d) >= 12.0f) { best = std::fabs(d); delta = d; }
            }
            if (best <= thresh) { ny += delta; py += delta; nh -= delta; }
        } else if (move_b) {
            float bot_y = py + nh;
            float best = thresh + 0.001f, delta = 0.0f;
            for (const auto& ry : refs_y) {
                float d = ry.coord - bot_y;
                if (std::fabs(d) < best && (nh + d) >= 12.0f) { best = std::fabs(d); delta = d; }
            }
            if (best <= thresh) { nh += delta; }
        }

        constexpr float kEps = 0.55f;
        if (move_l) {
            float mn = py, mx = py + nh;
            bool m = false;
            for (const auto& rx : refs_x) {
                if (std::fabs(rx.coord - px) <= kEps) { m = true; mn = std::min(mn, rx.s0); mx = std::max(mx, rx.s1); }
            }
            if (m) active_guides.push_back({true, px, mn, mx});
        }
        if (move_r) {
            float rx_cur = px + nw, mn = py, mx = py + nh;
            bool m = false;
            for (const auto& rx : refs_x) {
                if (std::fabs(rx.coord - rx_cur) <= kEps) { m = true; mn = std::min(mn, rx.s0); mx = std::max(mx, rx.s1); }
            }
            if (m) active_guides.push_back({true, rx_cur, mn, mx});
        }
        if (move_t) {
            float mn = px, mx = px + nw;
            bool m = false;
            for (const auto& ry : refs_y) {
                if (std::fabs(ry.coord - py) <= kEps) { m = true; mn = std::min(mn, ry.s0); mx = std::max(mx, ry.s1); }
            }
            if (m) active_guides.push_back({false, py, mn, mx});
        }
        if (move_b) {
            float by_cur = py + nh, mn = px, mx = px + nw;
            bool m = false;
            for (const auto& ry : refs_y) {
                if (std::fabs(ry.coord - by_cur) <= kEps) { m = true; mn = std::min(mn, ry.s0); mx = std::max(mx, ry.s1); }
            }
            if (m) active_guides.push_back({false, by_cur, mn, mx});
        }
    }

    void AlignSelection(int mode) {
        // mode: 0=Left, 1=HCenter, 2=Right, 3=Top, 4=VCenter, 5=Bottom
        if (!doc) return;
        struct Item { int id; float wx; float wy; float ww; float wh; float ox; float oy; };
        std::vector<Item> items;
        for (int sid : selected_ids) {
            if (sid == doc->root_id) continue;
            const auto* n = doc->FindNode(sid);
            if (!n || n->locked) continue;
            float wx = 0.0f, wy = 0.0f, ww = n->width, wh = n->height;
            GetNodeCanvasBounds(sid, wx, wy, ww, wh);
            float ox = 0.0f, oy = 0.0f;
            GetParentCanvasOrigin(n->parent_id, ox, oy, n->id);
            items.push_back({sid, wx, wy, ww, wh, ox, oy});
        }
        if (items.empty()) {
            ShowToast("Selecione um elemento para alinhar");
            return;
        }
        float ref_x = 0.0f, ref_y = 0.0f, ref_w = (float)doc->screen_width, ref_h = (float)doc->screen_height;
        if (items.size() == 1) {
            const auto* n = doc->FindNode(items[0].id);
            if (n && n->parent_id != doc->root_id) {
                GetNodeCanvasBounds(n->parent_id, ref_x, ref_y, ref_w, ref_h);
            }
        } else {
            float min_x = items[0].wx, min_y = items[0].wy;
            float max_x = items[0].wx + items[0].ww, max_y = items[0].wy + items[0].wh;
            for (size_t i = 1; i < items.size(); ++i) {
                min_x = std::min(min_x, items[i].wx);
                min_y = std::min(min_y, items[i].wy);
                max_x = std::max(max_x, items[i].wx + items[i].ww);
                max_y = std::max(max_y, items[i].wy + items[i].wh);
            }
            ref_x = min_x;
            ref_y = min_y;
            ref_w = max_x - min_x;
            ref_h = max_y - min_y;
        }
        PushUndo();
        for (const auto& it : items) {
            if (auto* n = doc->FindNode(it.id)) {
                if (mode == 0) n->x = std::round(ref_x - it.ox);
                else if (mode == 1) n->x = std::round(ref_x + (ref_w - it.ww) * 0.5f - it.ox);
                else if (mode == 2) n->x = std::round(ref_x + ref_w - it.ww - it.ox);
                else if (mode == 3) n->y = std::round(ref_y - it.oy);
                else if (mode == 4) n->y = std::round(ref_y + (ref_h - it.wh) * 0.5f - it.oy);
                else if (mode == 5) n->y = std::round(ref_y + ref_h - it.wh - it.oy);
                n->modified_transform = true;
            }
        }
        doc->Touch();
    }

    void DistributeSelection(bool horizontal) {
        if (!doc) return;
        struct Item { int id; float wx; float wy; float ww; float wh; float ox; float oy; };
        std::vector<Item> items;
        for (int sid : selected_ids) {
            if (sid == doc->root_id) continue;
            const auto* n = doc->FindNode(sid);
            if (!n || n->locked) continue;
            float wx = 0.0f, wy = 0.0f, ww = n->width, wh = n->height;
            GetNodeCanvasBounds(sid, wx, wy, ww, wh);
            float ox = 0.0f, oy = 0.0f;
            GetParentCanvasOrigin(n->parent_id, ox, oy, n->id);
            items.push_back({sid, wx, wy, ww, wh, ox, oy});
        }
        if (items.size() < 3) {
            ShowToast("Selecione 3 ou mais elementos para distribuir");
            return;
        }
        std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            return horizontal ? ((a.wx + a.ww * 0.5f) < (b.wx + b.ww * 0.5f))
                              : ((a.wy + a.wh * 0.5f) < (b.wy + b.wh * 0.5f));
        });
        float start_edge = horizontal ? items.front().wx : items.front().wy;
        float end_edge = horizontal ? (items.back().wx + items.back().ww) : (items.back().wy + items.back().wh);
        float total_dim = 0.0f;
        for (const auto& it : items) total_dim += (horizontal ? it.ww : it.wh);
        float gap = (end_edge - start_edge - total_dim) / (float)(items.size() - 1);

        PushUndo();
        float cursor = start_edge + (horizontal ? items.front().ww : items.front().wh) + gap;
        for (size_t i = 1; i + 1 < items.size(); ++i) {
            if (auto* n = doc->FindNode(items[i].id)) {
                if (horizontal) n->x = std::round(cursor - items[i].ox);
                else n->y = std::round(cursor - items[i].oy);
                n->modified_transform = true;
            }
            cursor += (horizontal ? items[i].ww : items[i].wh) + gap;
        }
        doc->Touch();
    }

    void BindButton(const char* id, const std::string& action) {
        if (!ui_doc) return;
        if (auto* el = ui_doc->GetElementById(id)) {
            auto h = std::make_unique<ClickHandler>();
            h->self = this;
            h->action = action;
            el->AddEventListener(Rml::EventId::Click, h.get());
            click_listeners.push_back(std::move(h));
        }
    }

    void BindChange(const char* id, const std::string& field) {
        if (!ui_doc) return;
        if (auto* el = ui_doc->GetElementById(id)) {
            auto h = std::make_unique<ChangeHandler>();
            h->self = this;
            h->field = field;
            el->AddEventListener(Rml::EventId::Change, h.get());
            if (std::string(id) != "code-editor") {
                el->AddEventListener(Rml::EventId::Blur, h.get());
            }
            change_listeners.push_back(std::move(h));
        }
    }

    void SetInputValue(const char* id, const std::string& val) {
        if (!ui_doc) return;
        if (!force_write_inputs && id) {
            if (active_edit_input_id == id) return;
            if (auto* ectx = engine ? engine->EditorContext() : nullptr) {
                if (auto* focus = ectx->GetFocusElement()) {
                    if (focus->GetId() == id && std::string(id) != "code-editor") return;
                }
            }
        }
        if (auto* el = ui_doc->GetElementById(id)) {
            if (auto* inp = dynamic_cast<Rml::ElementFormControlInput*>(el)) inp->SetValue(val);
            else if (auto* ta = dynamic_cast<Rml::ElementFormControlTextArea*>(el)) ta->SetValue(val);
        }
    }

    std::string GetInputValue(const char* id) {
        if (!ui_doc) return {};
        if (auto* el = ui_doc->GetElementById(id)) {
            if (auto* inp = dynamic_cast<Rml::ElementFormControlInput*>(el)) return inp->GetValue();
            if (auto* ta = dynamic_cast<Rml::ElementFormControlTextArea*>(el)) return ta->GetValue();
        }
        return {};
    }

    void SyncRulers() {
        if (!ui_doc) return;
        float vw = ViewportWidth();
        float vh = ViewportHeight();
        float origin_x = pan_x - LeftTotal();
        float origin_y = pan_y - TopTotal();
        float step = (zoom < 0.25f) ? 400.0f : 200.0f;

        if (auto* rt = ui_doc->GetElementById("ruler-top")) {
            std::ostringstream ss;
            float start_c = std::floor((-origin_x) / (step * zoom)) * step;
            float end_c = (-origin_x + vw) / zoom;
            for (float c = start_c; c <= end_c; c += step) {
                int sx = (int)std::round(origin_x + c * zoom);
                if (sx >= -40 && sx <= (int)vw) {
                    ss << "<div class=\"rtick-x\" style=\"left:" << sx << "px;\">" << (int)std::round(c) << "</div>";
                }
            }
            rt->SetInnerRML(ss.str());
        }

        if (show_rulers && show_left_ruler) {
            if (auto* rl = ui_doc->GetElementById("ruler-left")) {
                std::ostringstream ss;
                float start_c = std::floor((-origin_y) / (step * zoom)) * step;
                float end_c = (-origin_y + vh) / zoom;
                for (float c = start_c; c <= end_c; c += step) {
                    int sy = (int)std::round(origin_y + c * zoom);
                    if (sy >= -20 && sy <= (int)vh) {
                        ss << "<div class=\"rtick-y\" style=\"top:" << sy << "px;\">" << (int)std::round(c) << "</div>";
                    }
                }
                rl->SetInnerRML(ss.str());
            }
        }
        overlay_dirty = true;
    }

    void SyncArtboardOverlay() {
        if (!overlay_dirty || !ui_doc || !doc) return;
        overlay_dirty = false;

        float cw = (float)doc->screen_width * zoom;
        float ch = (float)doc->screen_height * zoom;
        float rel_x = pan_x - LeftTotal();
        float rel_y = pan_y - TopTotal();

        auto set_px = [&](Rml::Element* el, const char* prop, int val) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%dpx", val);
            el->SetProperty(prop, buf);
        };

        if (auto* ab = ui_doc->GetElementById("artboard-box")) {
            set_px(ab, "left", (int)std::round(rel_x));
            set_px(ab, "top", (int)std::round(rel_y));
            set_px(ab, "width", std::max(1, (int)std::round(cw)));
            set_px(ab, "height", std::max(1, (int)std::round(ch)));
        }

        bool is_empty = (doc->nodes.size() <= 1 && !doc->has_imported_source && !play_mode && cw >= 180.0f && ch >= 140.0f);
        if (auto* ec = ui_doc->GetElementById("canvas-empty-center")) {
            ec->SetClass("hidden", !is_empty);
        }
        if (auto* sl = ui_doc->GetElementById("safe-area-label")) {
            sl->SetClass("hidden", !safe_area && !is_empty);
        }
        if (auto* rn = ui_doc->GetElementById("artboard-root-name")) {
            const RmlNode* r = doc->FindNode(doc->root_id);
            rn->SetInnerRML(EscapeXml(r && !r->name.empty() ? r->name : "screen-root"));
        }
        if (auto* as = ui_doc->GetElementById("artboard-size-lbl")) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d &#215; %d px", doc->screen_width, doc->screen_height);
            as->SetInnerRML(buf);
        }
        if (auto* am = ui_doc->GetElementById("artboard-meta")) {
            am->SetClass("hidden", play_mode || ch < 70.0f);
        }
    }

    void FitViewport() {
        if (!doc) return;
        float vw = ViewportWidth();
        float vh = ViewportHeight();
        float zx = (vw - 60.0f) / std::max(1.0f, (float)doc->screen_width);
        float zy = (vh - 60.0f) / std::max(1.0f, (float)doc->screen_height);
        zoom = std::clamp(std::min(zx, zy), 0.10f, 2.0f);
        pan_x = LeftTotal() + (vw - doc->screen_width * zoom) * 0.5f;
        pan_y = TopTotal() + (vh - doc->screen_height * zoom) * 0.5f;
        UpdateZoomLabel();
        SyncRulers();
    }

    void UpdateZoomLabel() {
        if (!ui_doc) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d%%", (int)std::round(zoom * 100.0f));
        if (auto* el = ui_doc->GetElementById("lbl-zoom")) el->SetInnerRML(buf);
        if (auto* el = ui_doc->GetElementById("lbl-zoom-ctrl")) el->SetInnerRML(buf);
    }

    int ResolveTargetContainer() const {
        if (!doc) return 0;
        const RmlNode* sel = doc->FindNode(PrimaryId());
        if (!sel) return doc->root_id;
        if (sel->tag == "div" || sel->is_body_root) return sel->id;
        if (const RmlNode* p = doc->FindNode(sel->parent_id)) {
            if (p->tag == "div" || p->is_body_root) return p->id;
        }
        return doc->root_id;
    }

    bool IsDescendant(int node_id, int potential_ancestor) const {
        int cur = node_id;
        int guard = 0;
        while (cur >= 0 && guard++ < 256) {
            if (cur == potential_ancestor) return true;
            const RmlNode* n = doc ? doc->FindNode(cur) : nullptr;
            if (!n) break;
            cur = n->parent_id;
        }
        return false;
    }

    int FindBestContainerAt(float wx, float wy, int exclude_id) const {
        if (!doc) return doc ? doc->root_id : 0;
        int best = doc->root_id;
        std::function<bool(int)> visit = [&](int id) -> bool {
            const RmlNode* n = doc->FindNode(id);
            if (!n || !n->visible || n->locked) return false;
            if (id == exclude_id || IsDescendant(id, exclude_id)) return false;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
                if (visit(*it)) return true;
            }
            if (id != doc->root_id && n->tag == "div") {
                float nwx = 0.0f, nwy = 0.0f;
                doc->GetWorldPos(id, nwx, nwy);
                if (wx >= nwx && wx <= nwx + n->width && wy >= nwy && wy <= nwy + n->height) {
                    best = id;
                    return true;
                }
            }
            return false;
        };
        visit(doc->root_id);
        return best;
    }

    void AutoReparentAfterMove(int moved_id) {
        if (!doc || moved_id == doc->root_id) return;
        const RmlNode* n = doc->FindNode(moved_id);
        if (!n) return;
        float wx = 0.0f, wy = 0.0f;
        doc->GetWorldPos(moved_id, wx, wy);
        int target_parent = FindBestContainerAt(wx + n->width * 0.5f, wy + n->height * 0.5f, moved_id);
        if (target_parent != n->parent_id) {
            doc->ReparentNode(moved_id, target_parent);
        }
    }

    void CollectVisibleTreeOrder(int id, std::vector<int>& out) const {
        if (!doc) return;
        const RmlNode* n = doc->FindNode(id);
        if (!n) return;
        out.push_back(n->id);
        if (!n->collapsed) {
            for (int cid : n->children) CollectVisibleTreeOrder(cid, out);
        }
    }

    void SelectNodeWithMods(int id, bool ctrl, bool shift) {
        if (!doc) return;
        if (shift && !selected_ids.empty()) {
            std::vector<int> order;
            CollectVisibleTreeOrder(doc->root_id, order);
            int anchor = (selection_anchor_id > 0) ? selection_anchor_id : PrimaryId();
            auto it_a = std::find(order.begin(), order.end(), anchor);
            auto it_b = std::find(order.begin(), order.end(), id);
            if (it_a != order.end() && it_b != order.end()) {
                size_t ia = (size_t)(it_a - order.begin());
                size_t ib = (size_t)(it_b - order.begin());
                if (ia > ib) std::swap(ia, ib);
                if (!ctrl) selected_ids.clear();
                for (size_t i = ia; i <= ib; ++i) {
                    if (!IsSelected(order[i])) selected_ids.push_back(order[i]);
                }
                SyncUIFromDoc(true);
                return;
            }
        }
        if (ctrl) {
            auto it = std::find(selected_ids.begin(), selected_ids.end(), id);
            if (it != selected_ids.end()) {
                if (selected_ids.size() > 1) selected_ids.erase(it);
            } else {
                if (id != doc->root_id) {
                    selected_ids.erase(std::remove(selected_ids.begin(), selected_ids.end(), doc->root_id), selected_ids.end());
                }
                selected_ids.push_back(id);
                selection_anchor_id = id;
            }
            SyncUIFromDoc(true);
            return;
        }
        SetSingleSelection(id);
        SyncUIFromDoc(true);
    }

    void DrainPendingCommands() {
        if (pending_cmds.empty()) return;
        auto cmds = std::move(pending_cmds);
        pending_cmds.clear();
        for (const auto& cmd : cmds) {
            if (cmd.action == "__close_menu__") {
                CloseMenuPopup();
                continue;
            }
            if (cmd.from_popup) {
                CloseMenuPopup();
            }
            if (cmd.action == "__field_change__") {
                ApplyFieldChange(cmd.field, cmd.ctrl);
            } else if (cmd.action == "select_node") {
                SelectNodeWithMods(cmd.node_id, cmd.ctrl, cmd.shift);
            } else if (cmd.action == "toggle_collapse") {
                ToggleCollapse(cmd.node_id);
            } else if (cmd.action == "toggle_vis") {
                ToggleVisibility(cmd.node_id);
            } else if (cmd.action == "toggle_lock") {
                ToggleLock(cmd.node_id);
            } else if (cmd.action == "dup_node") {
                DuplicateSpecificNode(cmd.node_id);
            } else if (cmd.action == "del_node") {
                DeleteSpecificNode(cmd.node_id);
            } else {
                HandleButton(cmd.action);
            }
        }
    }

    void ToggleCollapse(int id) {
        if (!doc) return;
        if (auto* n = doc->FindNode(id)) {
            n->collapsed = !n->collapsed;
            RebuildLayers();
        }
    }

    static std::string ColorToHex6Local(const RmlColor& c) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
        return std::string(buf);
    }

    bool cp_undo_pushed = false;

    void CloseColorPicker(bool revert) {
        if (!cp_open) return;
        if (revert) {
            cp_curr_color = cp_orig_color;
            ApplyColorPickerCurrent(false);
        }
        cp_open = false;
        cp_drag = CpDrag::None;
        cp_target = ColorTarget::None;
        if (ui_doc) {
            if (auto* win = ui_doc->GetElementById("color-picker-win")) {
                win->SetClass("hidden", true);
            }
        }
    }

    void ApplyColorPickerCurrent(bool record_undo = true) {
        uint8_t nr = 255, ng = 255, nb = 255;
        HsvToRgb(cp_h, cp_s, cp_v, nr, ng, nb);
        cp_curr_color.r = nr;
        cp_curr_color.g = ng;
        cp_curr_color.b = nb;

        if (cp_target == ColorTarget::CanvasBg) {
            canvas_bg_color = {nr, ng, nb, 255};
            if (nr == 255 && ng == 255 && nb == 255) bg_style = BgStyle::White;
            else if (nr == 14 && ng == 17 && nb == 23) bg_style = BgStyle::Dark;
            else bg_style = BgStyle::Custom;
            UpdateHeaderButtonsUI();
            return;
        }
        if (!doc) return;
        auto* n = doc->FindNode(PrimaryId());
        if (!n) return;

        if (record_undo && !cp_undo_pushed) {
            PushUndo();
            cp_undo_pushed = true;
        }
        if (cp_target == ColorTarget::NodeBg) {
            n->bg_color.r = nr;
            n->bg_color.g = ng;
            n->bg_color.b = nb;
            if (n->bg_color.a == 0) n->bg_color.a = 255;
            n->has_bg = (n->bg_color.a > 0);
            n->modified_style = true;
        } else if (cp_target == ColorTarget::NodeBorder) {
            n->border_color.r = nr;
            n->border_color.g = ng;
            n->border_color.b = nb;
            if (n->border_color.a == 0) n->border_color.a = 255;
            n->modified_style = true;
        } else if (cp_target == ColorTarget::NodeFg) {
            n->text_color.r = nr;
            n->text_color.g = ng;
            n->text_color.b = nb;
            n->modified_style = true;
        } else if (cp_target == ColorTarget::NodeGrid) {
            n->grid_color.r = nr;
            n->grid_color.g = ng;
            n->grid_color.b = nb;
            if (n->grid_color.a == 0) n->grid_color.a = 46;
            n->modified_style = true;
        }
        doc->Touch();
        force_write_inputs = true;
        SyncUIFromDoc(true);
        force_write_inputs = false;
    }

    void SyncColorPickerUI() {
        if (!ui_doc || !cp_open) return;

        uint8_t hr = 255, hg = 0, hb = 0;
        HsvToRgb(cp_h, 1.0f, 1.0f, hr, hg, hb);
        char buf[64];
        if (auto* svbg = ui_doc->GetElementById("cp-sv-bg")) {
            std::snprintf(buf, sizeof(buf), "rgb(%u, %u, %u)", hr, hg, hb);
            svbg->SetProperty("background-color", buf);
        }
        if (auto* cur = ui_doc->GetElementById("cp-sv-cursor")) {
            float cx = std::clamp(cp_s, 0.0f, 1.0f) * 174.0f - 5.0f;
            float cy = (1.0f - std::clamp(cp_v, 0.0f, 1.0f)) * 174.0f - 5.0f;
            std::snprintf(buf, sizeof(buf), "%.1fpx", cx);
            cur->SetProperty("left", buf);
            std::snprintf(buf, sizeof(buf), "%.1fpx", cy);
            cur->SetProperty("top", buf);
        }
        if (auto* ht = ui_doc->GetElementById("cp-hue-thumb")) {
            float hy = (1.0f - std::clamp(cp_h / 360.0f, 0.0f, 1.0f)) * 174.0f - 2.0f;
            std::snprintf(buf, sizeof(buf), "%.1fpx", std::clamp(hy, -1.0f, 171.0f));
            ht->SetProperty("top", buf);
        }

        uint8_t r = cp_curr_color.r, g = cp_curr_color.g, b = cp_curr_color.b;
        if (auto* pn = ui_doc->GetElementById("cp-preview-new")) {
            std::snprintf(buf, sizeof(buf), "rgb(%u, %u, %u)", r, g, b);
            pn->SetProperty("background-color", buf);
        }
        if (auto* po = ui_doc->GetElementById("cp-preview-orig")) {
            std::snprintf(buf, sizeof(buf), "rgb(%u, %u, %u)", cp_orig_color.r, cp_orig_color.g, cp_orig_color.b);
            po->SetProperty("background-color", buf);
        }

        bool prev_sync = syncing_ui;
        syncing_ui = true;
        SetInputValue("cp-in-h", std::to_string((int)std::round(cp_h) % 360));
        SetInputValue("cp-in-s", std::to_string((int)std::round(cp_s * 100.0f)));
        SetInputValue("cp-in-v", std::to_string((int)std::round(cp_v * 100.0f)));
        SetInputValue("cp-in-r", std::to_string((int)r));
        SetInputValue("cp-in-g", std::to_string((int)g));
        SetInputValue("cp-in-b", std::to_string((int)b));
        char hbuf[16];
        std::snprintf(hbuf, sizeof(hbuf), "%02X%02X%02X", r, g, b);
        SetInputValue("cp-in-hex", hbuf);
        syncing_ui = prev_sync;
    }

    void OpenColorPicker(ColorTarget target, const char* anchor_id) {
        if (!ui_doc) return;
        CloseMenuPopup();

        if (cp_open && cp_target == target) {
            CloseColorPicker(false);
            return;
        }
        cp_open = true;
        cp_target = target;
        cp_undo_pushed = false;
        cp_drag = CpDrag::None;

        RmlColor initial{255, 255, 255, 255};
        const char* title = "Seletor de Cores";
        if (target == ColorTarget::CanvasBg) {
            initial = canvas_bg_color;
            title = "Cor de Fundo do Canvas";
        } else if (doc) {
            if (const auto* n = doc->FindNode(PrimaryId())) {
                if (target == ColorTarget::NodeBg) { initial = n->bg_color; title = "Cor de Preenchimento"; }
                else if (target == ColorTarget::NodeBorder) { initial = n->border_color; title = "Cor da Borda"; }
                else if (target == ColorTarget::NodeFg) { initial = n->text_color; title = "Cor do Texto"; }
                else if (target == ColorTarget::NodeGrid) { initial = n->grid_color; title = "Cor da Grade de Layout"; }
            }
        }
        cp_orig_color = initial;
        cp_curr_color = initial;
        RgbToHsv(initial.r, initial.g, initial.b, cp_h, cp_s, cp_v);

        if (auto* t = ui_doc->GetElementById("cp-title")) {
            t->SetInnerRML(title);
        }
        if (auto* win = ui_doc->GetElementById("color-picker-win")) {
            float wx = (float)win_w - right_inspector_width - 384.0f;
            float wy = 200.0f;
            if (anchor_id) {
                if (auto* ae = ui_doc->GetElementById(anchor_id)) {
                    Rml::Vector2f off = ae->GetAbsoluteOffset(Rml::BoxArea::Border);
                    if (target == ColorTarget::CanvasBg) {
                        wx = off.x;
                        wy = off.y + 28.0f;
                    } else {
                        wx = (float)win_w - right_inspector_width - 384.0f;
                        wy = off.y - 30.0f;
                    }
                }
            }
            wx = std::clamp(wx, 12.0f, std::max(12.0f, (float)win_w - 380.0f));
            wy = std::clamp(wy, 54.0f, std::max(54.0f, (float)win_h - 275.0f));
            char pbuf[32];
            std::snprintf(pbuf, sizeof(pbuf), "%dpx", (int)wx);
            win->SetProperty("left", pbuf);
            std::snprintf(pbuf, sizeof(pbuf), "%dpx", (int)wy);
            win->SetProperty("top", pbuf);
            win->SetClass("hidden", false);
        }
        SyncColorPickerUI();
    }

    void UpdateColorPickerFromSVMouse(int mx, int my) {
        if (!ui_doc) return;
        auto* sv = ui_doc->GetElementById("cp-sv-bg");
        if (!sv) return;
        float x0 = sv->GetAbsoluteLeft();
        float y0 = sv->GetAbsoluteTop();
        float w = std::max(1.0f, sv->GetOffsetWidth());
        float h = std::max(1.0f, sv->GetOffsetHeight());
        cp_s = std::clamp(((float)mx - x0) / w, 0.0f, 1.0f);
        cp_v = 1.0f - std::clamp(((float)my - y0) / h, 0.0f, 1.0f);
        ApplyColorPickerCurrent(true);
        SyncColorPickerUI();
    }

    void UpdateColorPickerFromHueMouse(int my) {
        if (!ui_doc) return;
        auto* hw = ui_doc->GetElementById("cp-hue-wrap");
        if (!hw) return;
        float y0 = hw->GetAbsoluteTop();
        float h = std::max(1.0f, hw->GetOffsetHeight());
        float t = std::clamp(((float)my - y0) / h, 0.0f, 1.0f);
        cp_h = (1.0f - t) * 359.9f;
        ApplyColorPickerCurrent(true);
        SyncColorPickerUI();
    }

    void ApplyCpInputChange(const std::string& cp_field, bool commit) {
        if (!cp_open) return;
        const std::string id = "cp-in-" + cp_field;
        std::string raw = GetInputValue(id.c_str());
        if (cp_field == "hex") {
            RmlColor parsed{};
            std::string trimmed = raw;
            if (!trimmed.empty() && trimmed[0] == '#') trimmed.erase(trimmed.begin());
            if ((commit || trimmed.size() >= 6) && ParseHex6(raw, parsed)) {
                cp_curr_color.r = parsed.r;
                cp_curr_color.g = parsed.g;
                cp_curr_color.b = parsed.b;
                RgbToHsv(parsed.r, parsed.g, parsed.b, cp_h, cp_s, cp_v);
                ApplyColorPickerCurrent(true);
                SyncColorPickerUI();
            } else if (commit) {
                SyncColorPickerUI();
            }
            return;
        }
        int val = 0;
        if (!TryParseIntStrict(raw, val)) {
            if (commit) SyncColorPickerUI();
            return;
        }
        if (cp_field == "h") {
            cp_h = (float)std::clamp(val, 0, 359);
            ApplyColorPickerCurrent(true);
        } else if (cp_field == "s") {
            cp_s = (float)std::clamp(val, 0, 100) / 100.0f;
            ApplyColorPickerCurrent(true);
        } else if (cp_field == "v") {
            cp_v = (float)std::clamp(val, 0, 100) / 100.0f;
            ApplyColorPickerCurrent(true);
        } else if (cp_field == "r" || cp_field == "g" || cp_field == "b") {
            uint8_t r = cp_curr_color.r, g = cp_curr_color.g, b = cp_curr_color.b;
            uint8_t c8 = (uint8_t)std::clamp(val, 0, 255);
            if (cp_field == "r") r = c8;
            else if (cp_field == "g") g = c8;
            else b = c8;
            cp_curr_color.r = r; cp_curr_color.g = g; cp_curr_color.b = b;
            RgbToHsv(r, g, b, cp_h, cp_s, cp_v);
            ApplyColorPickerCurrent(true);
        }
        SyncColorPickerUI();
    }

    void CloseMenuPopup() {
        active_menu.clear();
        if (ui_doc) {
            if (auto* p = ui_doc->GetElementById("menu-popup")) {
                p->SetClass("hidden", true);
                p->SetInnerRML("");
            }
        }
        popup_listeners.clear();
    }

    void OpenMenuPopup(const std::string& menu, float left_px, float top_px = 46.0f) {
        if (!ui_doc) return;
        if (active_menu == menu) {
            CloseMenuPopup();
            return;
        }
        active_menu = menu;
        auto* popup = ui_doc->GetElementById("menu-popup");
        if (!popup) return;

        struct Item { std::string id; std::string label; std::string action; bool sep = false; };
        std::vector<Item> items;
        if (menu == "file") {
            items = {
                {"pop-new", "Novo Projeto Limpo", "new"},
                {"pop-irm", "Importar .RML...", "import_rml"},
                {"pop-irc", "Importar .RCSS...", "import_rcss"},
                {"", "", "", true},
                {"pop-exp", "Exportar .RML + .RCSS...", "export"},
                {"", "", "", true},
                {"pop-exit", "Sair", "exit_app"}
            };
        } else if (menu == "edit") {
            items = {
                {"pop-undo", "Desfazer (Ctrl+Z)", "undo"},
                {"pop-redo", "Refazer (Ctrl+Y)", "redo"},
                {"", "", "", true},
                {"pop-dup", "Duplicar Elemento (Ctrl+D)", "act_dup"},
                {"pop-del", "Excluir Elemento (Delete)", "act_del"},
                {"pop-lck", "Bloquear / Desbloquear", "act_lock"},
                {"pop-vis", "Ocultar / Mostrar", "act_vis"}
            };
        } else if (menu == "view") {
            items = {
                {"pop-rul", show_rulers ? "Ocultar Réguas" : "Mostrar Réguas", "toggle_rulers"},
                {"pop-rlat", show_left_ruler ? "Ocultar Régua Lateral" : "Mostrar Régua Lateral", "toggle_left_ruler"},
                {"pop-grd", show_grid ? "Ocultar Grade" : "Mostrar Grade", "toggle_grid"},
                {"pop-snap", snap_guides ? "Desativar Guias Inteligentes" : "Ativar Guias Inteligentes", "toggle_snap"},
                {"pop-bot", show_bottom_dock ? "Ocultar Painel de Código" : "Mostrar Painel de Código", "toggle_bottom_dock"},
                {"pop-fs", "Tela Cheia (F11)", "toggle_fullscreen"},
                {"", "", "", true},
                {"pop-z100", "Resetar Zoom (100%)", "z100"},
                {"pop-fit", "Ajustar Tela (Fit)", "fit"}
            };
        } else if (menu == "vp") {
            items = {
                {"pop-pc", "Desktop 1920x1080", "vp_pc"},
                {"pop-hd", "HD 1280x720", "vp_hd"},
                {"", "", "", true},
                {"pop-safe", "Alternar Área Segura", "vp_safe"},
                {"pop-bgd", "Fundo: Escuro (#0E1117)", "bg_dark"},
                {"pop-bgw", "Fundo: Branco (#FFFFFF)", "bg_white"},
                {"pop-bgy", "Fundo: Cinza (#CBD5E1)", "bg_gray"},
                {"pop-bgta", "Fundo: GTA", "bg_gta"},
                {"pop-bgc", "Fundo: Escolher Cor...", "bg_custom"}
            };
        } else if (menu == "import") {
            items = {
                {"pop-irm", "Importar .RML...", "import_rml"},
                {"pop-irc", "Importar .RCSS...", "import_rcss"},
                {"", "", "", true},
                {"pop-exp", "Exportar Pacote (.rml + .rcss)...", "export"}
            };
        } else if (menu == "add") {
            items = {
                {"pop-adiv", "Container (div)", "add_div"},
                {"pop-atxt", "Texto (span)", "add_txt"},
                {"pop-abtn", "Botão (button)", "add_btn"},
                {"pop-ainp", "Campo Input (input)", "add_inp"},
                {"pop-aimg", "Imagem (img)...", "add_img"},
                {"pop-abar", "Barra de Progresso", "add_bar"},
                {"", "", "", true},
                {"pop-plog", "Template: Painel de Login", "pref_login"},
                {"pop-pflx", "Template: Flex Container", "pref_flex"},
                {"pop-plst", "Template: Lista Scroll", "pref_list"},
                {"pop-pgrd", "Template: Grid 2x2", "pref_grid"},
                {"", "", "", true},
                {"pop-new", "Novo Projeto Limpo", "new"}
            };
        }

        std::ostringstream html;
        for (const auto& it : items) {
            if (it.sep) {
                html << "<div class=\"popup-sep\"></div>";
            } else {
                html << "<button id=\"" << it.id << "\" class=\"popup-item\">" << EscapeXml(it.label) << "</button>";
            }
        }
        popup->SetInnerRML(html.str());
        popup_listeners.clear();

        float clamped_left = std::clamp(left_px, 6.0f, std::max(6.0f, (float)win_w - 240.0f));
        float clamped_top = std::clamp(top_px, 24.0f, std::max(24.0f, (float)win_h - 320.0f));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dpx", (int)clamped_left);
        popup->SetProperty("left", buf);
        std::snprintf(buf, sizeof(buf), "%dpx", (int)clamped_top);
        popup->SetProperty("top", buf);
        popup->SetClass("hidden", false);

        for (const auto& it : items) {
            if (it.sep) continue;
            if (auto* el = ui_doc->GetElementById(it.id.c_str())) {
                auto h = std::make_unique<ClickHandler>();
                h->self = this;
                h->action = it.action;
                h->is_popup_item = true;
                el->AddEventListener(Rml::EventId::Click, h.get());
                popup_listeners.push_back(std::move(h));
            }
        }
    }

    void ToggleVisibility(int id) {
        if (!doc) return;
        PushUndo();
        std::vector<int> targets = (IsSelected(id) && selected_ids.size() > 1) ? selected_ids : std::vector<int>{id};
        for (int tid : targets) {
            if (auto* n = doc->FindNode(tid)) {
                n->visible = !n->visible;
                n->modified_style = true;
            }
        }
        doc->Touch();
        SyncUIFromDoc(true);
    }

    void ToggleLock(int id) {
        if (!doc) return;
        std::vector<int> targets = (IsSelected(id) && selected_ids.size() > 1) ? selected_ids : std::vector<int>{id};
        PushUndo();
        for (int tid : targets) {
            if (tid == doc->root_id) continue;
            if (auto* n = doc->FindNode(tid)) n->locked = !n->locked;
        }
        doc->Touch();
        SyncUIFromDoc(true);
    }

    void DuplicateSpecificNode(int id) {
        if (!doc) return;
        std::vector<int> targets = (IsSelected(id) && selected_ids.size() > 1) ? selected_ids : std::vector<int>{id};
        PushUndo();
        std::vector<int> created;
        for (int tid : targets) {
            if (tid == doc->root_id) continue;
            int nid = doc->DuplicateNode(tid);
            if (nid > 0) created.push_back(nid);
        }
        if (!created.empty()) {
            selected_ids = created;
            selection_anchor_id = created.back();
            ShowToast("Elemento duplicado");
        }
        SyncUIFromDoc(true);
    }

    void DeleteSpecificNode(int id) {
        if (!doc) return;
        std::vector<int> targets = (IsSelected(id) && selected_ids.size() > 1) ? selected_ids : std::vector<int>{id};
        PushUndo();
        bool any_deleted = false;
        for (int tid : targets) {
            if (tid == doc->root_id) continue;
            if (doc->DeleteNode(tid)) any_deleted = true;
        }
        selected_ids.erase(
            std::remove_if(selected_ids.begin(), selected_ids.end(), [&](int sid) { return !doc->FindNode(sid); }),
            selected_ids.end()
        );
        if (selected_ids.empty()) SetSingleSelection(doc->root_id);
        if (any_deleted) ShowToast("Elemento removido");
        SyncUIFromDoc(true);
    }

    void UpdateHeaderButtonsUI() {
        if (!ui_doc || !doc) return;
        if (auto* e = ui_doc->GetElementById("vp-pc")) e->SetClass("active", true);
        if (auto* e = ui_doc->GetElementById("bg-grid")) e->SetClass("selected", show_grid);
        if (auto* e = ui_doc->GetElementById("btn-canvas-grid")) e->SetClass("control-active", show_grid);
        if (auto* e = ui_doc->GetElementById("btn-canvas-snap")) e->SetClass("control-active", snap_guides);
        if (auto* sw = ui_doc->GetElementById("swatch-canvas-bg")) {
            RmlColor sw_col = (bg_style == BgStyle::Gta) ? RmlColor{68, 138, 204, 255} : canvas_bg_color;
            sw->SetProperty("background-color", sw_col.ToCss());
        }
        if (auto* lb = ui_doc->GetElementById("lbl-canvas-bg")) {
            const char* name = "Fundo";
            if (bg_style == BgStyle::Dark) name = "Escuro";
            else if (bg_style == BgStyle::White) name = "Branco";
            else if (bg_style == BgStyle::Gray) name = "Cinza";
            else if (bg_style == BgStyle::Gta) name = "GTA";
            else name = "Cor";
            lb->SetInnerRML(name);
        }
        if (auto* e = ui_doc->GetElementById("tool-sel")) e->SetClass("control-active", !hand_tool);
        if (auto* e = ui_doc->GetElementById("tool-pan")) e->SetClass("control-active", hand_tool);
        if (auto* ex = ui_doc->GetElementById("lbl-expand-code")) {
            ex->SetInnerRML(code_expanded ? "Recolher" : "Expandir");
        }

        if (auto* rt = ui_doc->GetElementById("ruler-top-row")) rt->SetClass("hidden", !show_rulers);
        if (auto* cr = ui_doc->GetElementById("canvas-res-lbl")) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d &#215; %d", doc->screen_width, doc->screen_height);
            cr->SetInnerRML(buf);
        }
        if (auto* sbv = ui_doc->GetElementById("sb-vp")) {
            char vp_buf[64];
            std::snprintf(vp_buf, sizeof(vp_buf), "RML &#183; %dx%d &#183; Canvas em tempo real", doc->screen_width, doc->screen_height);
            sbv->SetInnerRML(vp_buf);
        }
        overlay_dirty = true;
    }

    void UpdateDomStatus() {
        if (!ui_doc || !doc) return;
        std::string label = "screen-root";
        if (const RmlNode* n = doc->FindNode(PrimaryId())) {
            if (!n->rml_id.empty()) label = "#" + n->rml_id;
            else if (!n->name.empty()) label = n->name;
            else if (!n->rml_class.empty()) label = "." + n->rml_class;
            else label = n->tag;
        }
        std::string status = "Pronto <span class=\"status-sep\">/</span> " + EscapeXml(label);
        if (selected_ids.size() > 1) {
            status += " (" + std::to_string(selected_ids.size()) + " selecionados)";
        }
        if (auto* el = ui_doc->GetElementById("sb-dom")) el->SetInnerRML(status);
    }

    bool ExecuteImportRcss(const std::string& path) {
        if (!doc || !engine || path.empty()) return false;
        PushUndo();
        std::string err;
        bool ok = engine->ImportRcssAndSync(path, *doc, err);
        SyncUIFromDoc(true);
        ApplyPanelSizes(win_w, win_h);
        if (ok) ShowToast("Estilos .RCSS importados");
        return ok;
    }

    void OpenPopupNearElement(const std::string& menu, const char* anchor_id, float default_x, float default_y) {
        float px = default_x;
        float py = default_y;
        if (ui_doc) {
            if (auto* el = ui_doc->GetElementById(anchor_id)) {
                Rml::Vector2f off = el->GetAbsoluteOffset(Rml::BoxArea::Border);
                Rml::Vector2f sz = el->GetBox().GetSize(Rml::BoxArea::Border);
                px = off.x;
                py = off.y + sz.y + 4.0f;
            }
        }
        OpenMenuPopup(menu, px, py);
    }

    void HandleButton(const std::string& action) {
        if (!doc || !engine) return;
        if (action == "menu_file") { OpenPopupNearElement("file", "m-file", 460.0f, 44.0f); return; }
        if (action == "menu_edit") { OpenPopupNearElement("edit", "m-edit", 530.0f, 44.0f); return; }
        if (action == "menu_view") { OpenPopupNearElement("view", "m-view", 600.0f, 44.0f); return; }
        if (action == "menu_vp")   { OpenPopupNearElement("vp",   "m-vp",   665.0f, 44.0f); return; }
        if (action == "menu_imp")  { OpenPopupNearElement("import", "btn-imp-menu", 88.0f, 92.0f); return; }
        if (action == "menu_add")  { OpenPopupNearElement("add",  "btn-new", 15.0f, 92.0f); return; }
        if (action == "hier_add")  { OpenPopupNearElement("add",  "btn-hier-add", 160.0f, 138.0f); return; }
        if (action == "empty_add") { OpenPopupNearElement("add",  "btn-empty-add", (float)win_w * 0.5f - 115.0f, (float)win_h * 0.45f); return; }
        if (action == "insp_more") { OpenPopupNearElement("edit", "btn-insp-more", (float)win_w - 245.0f, 138.0f); return; }
        if (action == "help") {
            ShowToast("Atalhos: V Selecionar | H/Espaço Pan | Ctrl+Z/Y | Ctrl+D | Del");
            return;
        }
        if (action == "settings") {
            safe_area = !safe_area;
            overlay_dirty = true;
            ShowToast(safe_area ? "Área segura ativada" : "Área segura oculta");
            return;
        }
        if (action == "exit_app") { PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }

        if (action == "new") {
            PushUndo();
            doc->ResetDefaultScreen();
            SetSingleSelection(doc->root_id);
            if (auto* fn = ui_doc->GetElementById("menu-filename")) {
                fn->SetInnerRML("Projeto sem t&#237;tulo");
            }
            if (auto* st = ui_doc->GetElementById("saved-text")) st->SetInnerRML("Salvo");
            FitViewport();
            ShowToast("Novo projeto criado");
        } else if (action == "import_rml" || action == "open") {
            std::string path = OpenFileDialog(hwnd, L"Arquivos RmlUi (*.rml)\0*.rml\0Todos (*.*)\0*.*\0", L"Importar arquivo .rml");
            if (!path.empty()) {
                RmlDocument loaded;
                std::string err;
                if (engine->ImportRmlFile(path, loaded, err)) {
                    PushUndo();
                    *doc = std::move(loaded);
                    SetSingleSelection(doc->root_id);
                    if (auto* fn = ui_doc->GetElementById("menu-filename")) {
                        fn->SetInnerRML(EscapeXml(fs::path(path).filename().string()));
                    }
                    if (auto* st = ui_doc->GetElementById("saved-text")) st->SetInnerRML("Salvo");
                    FitViewport();
                    SyncUIFromDoc(true);
                    ApplyPanelSizes(win_w, win_h);
                    ShowToast("Arquivo .RML importado");
                }
            }
        } else if (action == "import_rcss") {
            std::string path = OpenFileDialog(hwnd, L"Estilos RmlUi (*.rcss;*.css)\0*.rcss;*.css\0Todos (*.*)\0*.*\0", L"Importar arquivo .rcss");
            if (!path.empty()) {
                ExecuteImportRcss(path);
            }
        } else if (action == "act_dup") {
            DuplicateSpecificNode(PrimaryId());
            return;
        } else if (action == "act_del") {
            DeleteSpecificNode(PrimaryId());
            return;
        } else if (action == "act_lock") {
            ToggleLock(PrimaryId());
            return;
        } else if (action == "act_vis") {
            ToggleVisibility(PrimaryId());
            return;
        } else if (action == "toggle_rulers") {
            show_rulers = !show_rulers;
            UpdateHeaderButtonsUI();
            ApplyPanelSizes();
            SyncRulers();
            return;
        } else if (action == "toggle_left_ruler") {
            show_left_ruler = !show_left_ruler;
            ApplyPanelSizes();
            SyncRulers();
            return;
        } else if (action == "toggle_grid") {
            show_grid = !show_grid;
            UpdateHeaderButtonsUI();
            return;
        } else if (action == "toggle_snap") {
            snap_guides = !snap_guides;
            if (!snap_guides) active_guides.clear();
            UpdateHeaderButtonsUI();
            ShowToast(snap_guides ? "Guias inteligentes ativadas" : "Guias inteligentes desativadas");
            return;
        } else if (action == "pick_swatch_grid") {
            OpenColorPicker(ColorTarget::NodeGrid, "swatch-grid");
            return;
        } else if (action == "toggle_fullscreen") {
            PostMessageW(hwnd, WM_APP + 101, 0, 0);
            return;
        } else if (action == "toggle_bottom_dock") {
            show_bottom_dock = !show_bottom_dock;
            UpdateHeaderButtonsUI();
            ApplyPanelSizes();
            return;
        } else if (action == "expand_code") {
            code_expanded = !code_expanded;
            bottom_dock_height = code_expanded ? 380.0f : 190.0f;
            UpdateHeaderButtonsUI();
            ApplyPanelSizes();
            SyncRulers();
            return;
        } else if (action == "pick_swatch_bg") {
            OpenColorPicker(ColorTarget::NodeBg, "swatch-bg");
            return;
        } else if (action == "pick_swatch_border") {
            OpenColorPicker(ColorTarget::NodeBorder, "swatch-border");
            return;
        } else if (action == "pick_swatch_fg") {
            OpenColorPicker(ColorTarget::NodeFg, "swatch-fg");
            return;
        } else if (action == "pick_canvas_bg") {
            OpenColorPicker(ColorTarget::CanvasBg, "btn-canvas-bg");
            return;
        } else if (action == "cp_ok") {
            CloseColorPicker(false);
            return;
        } else if (action == "cp_cancel") {
            CloseColorPicker(true);
            return;
        } else if (action == "cp_revert") {
            cp_curr_color = cp_orig_color;
            RgbToHsv(cp_curr_color.r, cp_curr_color.g, cp_curr_color.b, cp_h, cp_s, cp_v);
            ApplyColorPickerCurrent(true);
            SyncColorPickerUI();
            return;
        } else if (action.rfind("cp_sw_", 0) == 0) {
            static const RmlColor kPresets[11] = {
                {255, 255, 255, 255}, {226, 232, 240, 255}, {100, 116, 139, 255},
                {30, 41, 59, 255},    {14, 17, 23, 255},    {0, 0, 0, 255},
                {239, 68, 68, 255},   {245, 158, 11, 255},  {16, 185, 129, 255},
                {59, 130, 246, 255},  {139, 92, 246, 255}
            };
            int idx = std::atoi(action.c_str() + 6);
            if (idx >= 0 && idx < 11) {
                cp_curr_color = kPresets[idx];
                RgbToHsv(cp_curr_color.r, cp_curr_color.g, cp_curr_color.b, cp_h, cp_s, cp_v);
                ApplyColorPickerCurrent(true);
                SyncColorPickerUI();
            }
            return;
        } else if (action == "export") {
            std::string path = SaveFileDialog(hwnd, L"Arquivos RmlUi (*.rml)\0*.rml\0", L"rml", L"interface.rml", L"Exportar .rml + .rcss");
            if (!path.empty()) {
                std::string err;
                if (doc->ExportToPath(path, err)) {
                    if (auto* st = ui_doc->GetElementById("saved-text")) st->SetInnerRML("Salvo");
                    ShowToast("Pacote .RML + .RCSS exportado");
                }
            }
        } else if (action == "undo") {
            Undo();
            return;
        } else if (action == "redo") {
            Redo();
            return;
        } else if (action == "vp_pc") {
            PushUndo();
            float old_w = (float)doc->screen_width;
            float old_h = (float)doc->screen_height;
            doc->screen_width = 1920;
            doc->screen_height = 1080;
            if (auto* r = doc->FindNode(doc->root_id)) {
                old_w = r->width;
                old_h = r->height;
                r->width = 1920.0f;
                r->height = 1080.0f;
            }
            doc->ApplyConstraintsOnParentResize(doc->root_id, old_w, old_h, 1920.0f, 1080.0f);
            doc->Touch();
            FitViewport();
        } else if (action == "vp_hd") {
            PushUndo();
            float old_w = (float)doc->screen_width;
            float old_h = (float)doc->screen_height;
            doc->screen_width = 1280;
            doc->screen_height = 720;
            if (auto* r = doc->FindNode(doc->root_id)) {
                old_w = r->width;
                old_h = r->height;
                r->width = 1280.0f;
                r->height = 720.0f;
            }
            doc->ApplyConstraintsOnParentResize(doc->root_id, old_w, old_h, 1280.0f, 720.0f);
            doc->Touch();
            FitViewport();
        } else if (action == "vp_safe") {
            safe_area = !safe_area;
            overlay_dirty = true;
        } else if (action == "bg_dark") {
            bg_style = BgStyle::Dark;
            canvas_bg_color = {14, 17, 23, 255};
        } else if (action == "bg_white") {
            bg_style = BgStyle::White;
            canvas_bg_color = {255, 255, 255, 255};
        } else if (action == "bg_gray") {
            bg_style = BgStyle::Gray;
            canvas_bg_color = {203, 213, 225, 255};
        } else if (action == "bg_gta") {
            bg_style = BgStyle::Gta;
        } else if (action == "bg_custom") {
            OpenColorPicker(ColorTarget::CanvasBg, "btn-canvas-bg");
            return;
        } else if (action == "fit") {
            FitViewport();
        } else if (action == "z100") {
            zoom = 1.0f;
            pan_x = LeftTotal() + 30.0f;
            pan_y = TopTotal() + 30.0f;
            UpdateZoomLabel();
            SyncRulers();
        } else if (action == "zoom_in" || action == "zoom_out") {
            float old_zoom = zoom;
            float factor = (action == "zoom_in") ? 1.15f : (1.0f / 1.15f);
            zoom = std::clamp(zoom * factor, 0.10f, 4.0f);
            float cx = LeftTotal() + ViewportWidth() * 0.5f;
            float cy = TopTotal() + ViewportHeight() * 0.5f;
            pan_x = cx - (cx - pan_x) * (zoom / old_zoom);
            pan_y = cy - (cy - pan_y) * (zoom / old_zoom);
            UpdateZoomLabel();
            SyncRulers();
        } else if (action == "play") {
            play_mode = !play_mode;
            drag_mode = DragMode::None;
            overlay_dirty = true;
            if (auto* b = ui_doc->GetElementById("btn-play")) b->SetClass("btn-play-on", play_mode);
            if (auto* lbl = ui_doc->GetElementById("lbl-play")) lbl->SetInnerRML(play_mode ? "Parar" : "Pr&#233;-visualizar");
            if (auto* ico = ui_doc->GetElementById("ico-play")) ico->SetAttribute("src", play_mode ? "studio_ico_stop" : "studio_ico_play");
            ShowToast(play_mode ? "Pré-visualização iniciada" : "Modo editor restaurado");
        } else if (action == "tool_sel") {
            hand_tool = false;
        } else if (action == "tool_pan") {
            hand_tool = true;
        } else if (action == "tab_rml" || action == "tab_rcss") {
            code_tab = (action == "tab_rcss") ? "RCSS" : "RML";
            last_code_undo_ms = 0;
            if (auto* t1 = ui_doc->GetElementById("tab-code-rml")) t1->SetClass("current", code_tab == "RML");
            if (auto* t2 = ui_doc->GetElementById("tab-code-rcss")) t2->SetClass("current", code_tab == "RCSS");
            SyncCodeDock();
            return;
        } else if (action == "copy_code") {
            std::string cur_code = GetInputValue("code-editor");
            if (cur_code.empty()) {
                std::string rml, rcss;
                std::vector<RmlAsset> assets;
                doc->GenerateRmlAndRcss(rml, rcss, assets);
                cur_code = (code_tab == "RCSS") ? rcss : rml;
            }
            CopyToClipboard(hwnd, cur_code);
            ShowToast(code_tab + " copiado para a área de transferência");
            return;
        } else if (action == "copy_rml") {
            std::string rml, rcss;
            std::vector<RmlAsset> assets;
            doc->GenerateRmlAndRcss(rml, rcss, assets);
            CopyToClipboard(hwnd, rml);
            ShowToast("RML copiado para a área de transferência");
            return;
        } else if (action == "copy_rcss") {
            std::string rml, rcss;
            std::vector<RmlAsset> assets;
            doc->GenerateRmlAndRcss(rml, rcss, assets);
            CopyToClipboard(hwnd, rcss);
            ShowToast("RCSS copiado para a área de transferência");
            return;
        } else if (action == "u_px" || action == "u_pct" || action == "u_dp") {
            unit_mode = (action == "u_pct") ? "%" : (action == "u_dp" ? "dp" : "px");
            SyncTransformFieldsOnly();
        } else if (action == "btn_align_left") {
            AlignSelection(0);
        } else if (action == "btn_align_hcenter") {
            AlignSelection(1);
        } else if (action == "btn_align_right") {
            AlignSelection(2);
        } else if (action == "btn_align_top") {
            AlignSelection(3);
        } else if (action == "btn_align_vcenter") {
            AlignSelection(4);
        } else if (action == "btn_align_bottom") {
            AlignSelection(5);
        } else if (action == "btn_dist_h") {
            DistributeSelection(true);
        } else if (action == "btn_dist_v") {
            DistributeSelection(false);
        } else if (action == "rw_fixed" || action == "rw_hug" || action == "rw_fill") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->size_mode_w = (action == "rw_hug") ? 1 : (action == "rw_fill" ? 2 : 0);
                n->modified_style = true;
                n->modified_transform = true;
                doc->Touch();
            }
        } else if (action == "rh_fixed" || action == "rh_hug" || action == "rh_fill") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->size_mode_h = (action == "rh_hug") ? 1 : (action == "rh_fill" ? 2 : 0);
                n->modified_style = true;
                n->modified_transform = true;
                doc->Touch();
            }
        } else if (action == "disp_block" || action == "disp_flex") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->display_flex = (action == "disp_flex");
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "wrap_off" || action == "wrap_on") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->flex_wrap = (action == "wrap_on");
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "dir_row" || action == "dir_col") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->flex_direction = (action == "dir_row") ? 0 : 1;
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "jc_start" || action == "jc_center" || action == "jc_end" || action == "jc_between") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->justify_content = (action == "jc_center") ? 1 : (action == "jc_end" ? 2 : (action == "jc_between" ? 3 : 0));
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "ai_start" || action == "ai_center" || action == "ai_end") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->align_items = (action == "ai_center") ? 1 : (action == "ai_end" ? 2 : 0);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "ch_left" || action == "ch_right" || action == "ch_lr" || action == "ch_center" || action == "ch_scale") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_h = (action == "ch_right") ? 1 : (action == "ch_lr" ? 2 : (action == "ch_center" ? 3 : (action == "ch_scale" ? 4 : 0)));
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cv_top" || action == "cv_bottom" || action == "cv_tb" || action == "cv_center" || action == "cv_scale") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_v = (action == "cv_bottom") ? 1 : (action == "cv_tb" ? 2 : (action == "cv_center" ? 3 : (action == "cv_scale" ? 4 : 0)));
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_left") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_h = (n->anchor_h == 1) ? 2 : (n->anchor_h == 2 ? 1 : 0);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_right") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_h = (n->anchor_h == 0) ? 2 : (n->anchor_h == 2 ? 0 : 1);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_hcenter") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_h = (n->anchor_h == 3) ? 0 : 3;
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_top") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_v = (n->anchor_v == 1) ? 2 : (n->anchor_v == 2 ? 1 : 0);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_bottom") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_v = (n->anchor_v == 0) ? 2 : (n->anchor_v == 2 ? 0 : 1);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "cpin_vcenter") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->anchor_v = (n->anchor_v == 3) ? 0 : 3;
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "toggle_node_grid") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->grid_enabled = !n->grid_enabled;
                doc->Touch();
            }
        } else if (action == "gtype_col" || action == "gtype_row" || action == "gtype_grid") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->grid_type = (action == "gtype_row") ? 1 : (action == "gtype_grid" ? 2 : 0);
                n->grid_enabled = true;
                doc->Touch();
            }
        } else if (action == "al_left" || action == "al_center" || action == "al_right") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                PushUndo();
                n->text_align = (action == "al_center") ? 1 : (action == "al_right" ? 2 : 0);
                n->modified_style = true;
                doc->Touch();
            }
        } else if (action == "pref_login") {
            PushUndo();
            int pid = ResolveTargetContainer();
            int panel = doc->AddDiv(pid, "Painel_Login", 710.0f, 320.0f, 500.0f, 420.0f);
            doc->AddText(panel, "Login SA-MP", 170.0f, 36.0f, 180.0f, 32.0f);
            doc->AddInput(panel, "Usuario", 80.0f, 110.0f, 340.0f, 44.0f);
            doc->AddInput(panel, "Senha", 80.0f, 175.0f, 340.0f, 44.0f);
            doc->AddButton(panel, "Entrar", 150.0f, 260.0f, 200.0f, 48.0f);
            SetSingleSelection(panel);
            doc->Touch();
            ShowToast("Template Login adicionado ao canvas");
        } else if (action == "add_div" || action == "pref_flex") {
            PushUndo();
            int pid = ResolveTargetContainer();
            float ox = (pid == doc->root_id) ? 140.0f : 20.0f;
            float oy = (pid == doc->root_id) ? 140.0f : 20.0f;
            int nid = doc->AddDiv(pid, action == "pref_flex" ? "flex-container" : "container", ox, oy, 320.0f, 200.0f);
            SetSingleSelection(nid);
            if (action == "pref_flex") {
                if (auto* n = doc->FindNode(nid)) {
                    n->display_flex = true;
                    n->flex_direction = 1;
                    n->gap = 8.0f;
                    n->padding[0] = n->padding[1] = n->padding[2] = n->padding[3] = 12.0f;
                }
            }
            doc->Touch();
            ShowToast("Elemento adicionado ao canvas");
        } else if (action == "add_txt") {
            PushUndo();
            int pid = ResolveTargetContainer();
            float ox = (pid == doc->root_id) ? 160.0f : 20.0f;
            float oy = (pid == doc->root_id) ? 160.0f : 20.0f;
            SetSingleSelection(doc->AddText(pid, "Texto RmlUi", ox, oy, 220.0f, 32.0f));
            ShowToast("Texto adicionado ao canvas");
        } else if (action == "add_btn" || action == "pref_btn") {
            PushUndo();
            int pid = ResolveTargetContainer();
            float ox = (pid == doc->root_id) ? 160.0f : 20.0f;
            float oy = (pid == doc->root_id) ? 200.0f : 20.0f;
            SetSingleSelection(doc->AddButton(pid, "Botao", ox, oy, 180.0f, 40.0f));
            ShowToast("Botão adicionado ao canvas");
        } else if (action == "add_inp" || action == "pref_inp") {
            PushUndo();
            int pid = ResolveTargetContainer();
            float ox = (pid == doc->root_id) ? 160.0f : 20.0f;
            float oy = (pid == doc->root_id) ? 250.0f : 70.0f;
            SetSingleSelection(doc->AddInput(pid, "Digite aqui...", ox, oy, 260.0f, 36.0f));
            ShowToast("Input adicionado ao canvas");
        } else if (action == "add_img") {
            std::string path = OpenFileDialog(hwnd, L"Imagens (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0Todos (*.*)\0*.*\0", L"Selecionar Imagem");
            if (!path.empty()) {
                PushUndo();
                int pid = ResolveTargetContainer();
                float ox = (pid == doc->root_id) ? 180.0f : 20.0f;
                float oy = (pid == doc->root_id) ? 180.0f : 20.0f;
                SetSingleSelection(doc->AddImage(pid, fs::path(path).filename().string(), ReadFileBytes(path), ox, oy, 220.0f, 140.0f));
                ShowToast("Imagem adicionada ao canvas");
            }
        } else if (action == "add_bar" || action == "pref_bar") {
            PushUndo();
            int pid = ResolveTargetContainer();
            float ox = (pid == doc->root_id) ? 200.0f : 20.0f;
            float oy = (pid == doc->root_id) ? 200.0f : 20.0f;
            int bar = doc->AddDiv(pid, "progress-bar", ox, oy, 260.0f, 20.0f);
            if (auto* bn = doc->FindNode(bar)) {
                bn->bg_color = {18, 22, 30, 235};
                bn->border_color = {55, 65, 81, 255};
                bn->border_width = 1.0f;
                bn->border_radius = 4.0f;
            }
            int fill = doc->AddDiv(bar, "progress-fill", 2.0f, 2.0f, 170.0f, 16.0f);
            if (auto* fn = doc->FindNode(fill)) {
                fn->bg_color = {54, 200, 145, 255};
                fn->border_radius = 3.0f;
            }
            SetSingleSelection(bar);
            doc->Touch();
            ShowToast("Barra adicionada ao canvas");
        } else if (action == "pref_list") {
            PushUndo();
            int pid = ResolveTargetContainer();
            int list_id = doc->AddDiv(pid, "scroll-list", 200.0f, 200.0f, 280.0f, 210.0f);
            if (auto* ln = doc->FindNode(list_id)) {
                ln->bg_color = {20, 23, 30, 235};
                ln->border_color = {45, 52, 65, 255};
                ln->border_width = 1.0f;
                ln->border_radius = 6.0f;
            }
            for (int i = 0; i < 3; ++i) {
                int item = doc->AddButton(list_id, "Item " + std::to_string(i + 1), 12.0f, 12.0f + i * 50.0f, 256.0f, 40.0f);
                if (auto* in = doc->FindNode(item)) {
                    in->bg_color = {32, 37, 48, 255};
                    in->border_radius = 4.0f;
                }
            }
            SetSingleSelection(list_id);
            doc->Touch();
            ShowToast("Lista adicionada ao canvas");
        } else if (action == "pref_grid") {
            PushUndo();
            int pid = ResolveTargetContainer();
            int grid_id = doc->AddDiv(pid, "grid-2x2", 200.0f, 200.0f, 260.0f, 260.0f);
            if (auto* gn = doc->FindNode(grid_id)) {
                gn->bg_color = {20, 23, 30, 235};
                gn->border_color = {45, 52, 65, 255};
                gn->border_width = 1.0f;
                gn->border_radius = 6.0f;
            }
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    int slot = doc->AddDiv(grid_id, "slot-" + std::to_string(r * 2 + c + 1), 14.0f + c * 120.0f, 14.0f + r * 120.0f, 110.0f, 110.0f);
                    if (auto* sn = doc->FindNode(slot)) {
                        sn->bg_color = {32, 37, 48, 255};
                        sn->border_color = {71, 120, 245, 140};
                        sn->border_width = 1.0f;
                        sn->border_radius = 4.0f;
                    }
                }
            }
            SetSingleSelection(grid_id);
            doc->Touch();
            ShowToast("Grid adicionada ao canvas");
        } else if (action == "pick_img") {
            if (auto* n = doc->FindNode(PrimaryId())) {
                std::string path = OpenFileDialog(hwnd, L"Imagens (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0Todos (*.*)\0*.*\0", L"Selecionar Imagem");
                if (!path.empty()) {
                    PushUndo();
                    n->image_bytes = ReadFileBytes(path);
                    n->image_src = fs::path(path).filename().string();
                    doc->Touch();
                }
            }
        } else if (action == "toggle_visible_switch") {
            ToggleVisibility(PrimaryId());
            return;
        }
        SyncUIFromDoc(true);
    }

    float ToDisplayUnit(float px, float ref_dim) const {
        if (unit_mode == "%") return (ref_dim > 0.0f) ? (px / ref_dim) * 100.0f : 0.0f;
        return px;
    }

    float FromDisplayUnit(float val, float ref_dim) const {
        if (unit_mode == "%") return (val / 100.0f) * ref_dim;
        return val;
    }

    void SyncTransformFieldsOnly() {
        if (!ui_doc || !doc) return;
        const auto* n = doc->FindNode(PrimaryId());
        if (!n) return;
        float pw = (float)doc->screen_width;
        float ph = (float)doc->screen_height;
        if (const auto* p = doc->FindNode(n->parent_id)) { pw = p->width; ph = p->height; }
        syncing_ui = true;
        SetInputValue("prop-x", FormatFloat(ToDisplayUnit(n->x, pw)));
        SetInputValue("prop-y", FormatFloat(ToDisplayUnit(n->y, ph)));
        SetInputValue("prop-w", FormatFloat(ToDisplayUnit(n->width, pw)));
        SetInputValue("prop-h", FormatFloat(ToDisplayUnit(n->height, ph)));
        syncing_ui = false;
    }

    void UpdateCodeStatusIndicator(bool synced, const std::string& label_override = "") {
        if (!ui_doc) return;
        if (auto* dot = ui_doc->GetElementById("code-status-dot")) {
            dot->SetClass("dot-amber-sm", !synced);
        }
        if (auto* txt = ui_doc->GetElementById("code-status-text")) {
            txt->SetInnerRML(EscapeXml(label_override.empty() ? (synced ? "Sincronizado" : "Sintaxe pendente...") : label_override));
        }
    }

    void RecordPropUndoBurst(const std::string& key) {
        uint64_t now_ms = GetTickCount64();
        if (last_prop_undo_field != key || (now_ms - last_prop_undo_ms) > 850ULL) {
            PushUndo();
            last_prop_undo_field = key;
        }
        last_prop_undo_ms = now_ms;
    }

    bool StepNumericPropByWheel(const std::string& input_id, float delta, bool shift) {
        if (!doc) return false;
        auto* n = doc->FindNode(PrimaryId());
        if (!n) return false;

        struct NumWheelSpec {
            const char* id;
            float step;
            float big_step;
        };
        static constexpr NumWheelSpec kSpecs[] = {
            {"prop-x", 1.0f, 10.0f},
            {"prop-y", 1.0f, 10.0f},
            {"prop-w", 1.0f, 10.0f},
            {"prop-h", 1.0f, 10.0f},
            {"prop-gap", 1.0f, 5.0f},
            {"prop-pad", 1.0f, 5.0f},
            {"prop-bg-a", 5.0f, 20.0f},
            {"prop-radius", 1.0f, 5.0f},
            {"prop-border-w", 1.0f, 2.0f},
            {"prop-border-a", 5.0f, 20.0f},
            {"prop-font-size", 1.0f, 4.0f},
            {"prop-grid-count", 1.0f, 4.0f},
            {"prop-grid-size", 2.0f, 8.0f},
            {"prop-grid-gutter", 2.0f, 8.0f},
            {"prop-grid-margin", 2.0f, 8.0f},
        };
        const NumWheelSpec* spec = nullptr;
        for (const auto& s : kSpecs) {
            if (input_id == s.id) {
                spec = &s;
                break;
            }
        }
        if (!spec) return false;

        float dir = (delta >= 0.0f) ? 1.0f : -1.0f;
        float ticks = std::max(1.0f, std::round(std::fabs(delta)));
        float step = dir * ticks * (shift ? spec->big_step : spec->step);

        RecordPropUndoBurst(std::string("wheel:") + input_id);
        float pw = (float)doc->screen_width;
        float ph = (float)doc->screen_height;
        if (const auto* p = doc->FindNode(n->parent_id)) { pw = p->width; ph = p->height; }

        if (input_id == "prop-x") {
            n->x = FromDisplayUnit(ToDisplayUnit(n->x, pw) + step, pw);
            n->modified_transform = true;
        } else if (input_id == "prop-y") {
            n->y = FromDisplayUnit(ToDisplayUnit(n->y, ph) + step, ph);
            n->modified_transform = true;
        } else if (input_id == "prop-w") {
            float old_w = n->width, old_h = n->height;
            n->width = std::max(8.0f, FromDisplayUnit(ToDisplayUnit(n->width, pw) + step, pw));
            n->modified_transform = true;
            if (n->id == doc->root_id) doc->screen_width = (int)std::round(n->width);
            doc->ApplyConstraintsOnParentResize(n->id, old_w, old_h, n->width, n->height);
        } else if (input_id == "prop-h") {
            float old_w = n->width, old_h = n->height;
            n->height = std::max(8.0f, FromDisplayUnit(ToDisplayUnit(n->height, ph) + step, ph));
            n->modified_transform = true;
            if (n->id == doc->root_id) doc->screen_height = (int)std::round(n->height);
            doc->ApplyConstraintsOnParentResize(n->id, old_w, old_h, n->width, n->height);
        } else if (input_id == "prop-gap") {
            n->gap = std::max(0.0f, n->gap + step);
            n->modified_style = true;
        } else if (input_id == "prop-pad") {
            float p = std::max(0.0f, n->padding[0] + step);
            n->padding[0] = n->padding[1] = n->padding[2] = n->padding[3] = p;
            n->modified_style = true;
        } else if (input_id == "prop-bg-a") {
            n->bg_color.a = (uint8_t)std::clamp((int)std::round((float)n->bg_color.a + step), 0, 255);
            n->has_bg = (n->bg_color.a > 0);
            n->modified_style = true;
        } else if (input_id == "prop-radius") {
            n->border_radius = std::max(0.0f, n->border_radius + step);
            n->modified_style = true;
        } else if (input_id == "prop-border-w") {
            n->border_width = std::max(0.0f, n->border_width + step);
            n->modified_style = true;
        } else if (input_id == "prop-border-a") {
            n->border_color.a = (uint8_t)std::clamp((int)std::round((float)n->border_color.a + step), 0, 255);
            n->modified_style = true;
        } else if (input_id == "prop-font-size") {
            n->font_size = std::clamp(n->font_size + step, 6.0f, 160.0f);
            n->modified_style = true;
        } else if (input_id == "prop-grid-count") {
            n->grid_count = std::clamp(n->grid_count + (int)std::round(step), 1, 48);
        } else if (input_id == "prop-grid-size") {
            n->grid_size = std::clamp(n->grid_size + step, 2.0f, 256.0f);
        } else if (input_id == "prop-grid-gutter") {
            n->grid_gutter = std::clamp(n->grid_gutter + step, 0.0f, 200.0f);
        } else if (input_id == "prop-grid-margin") {
            n->grid_margin = std::clamp(n->grid_margin + step, 0.0f, 400.0f);
        }

        doc->Touch();
        active_edit_input_id.clear();
        force_write_inputs = true;
        SyncUIFromDoc(true);
        force_write_inputs = false;
        if (ui_doc && engine) {
            if (auto* el = ui_doc->GetElementById(input_id.c_str())) {
                if (auto* ectx = engine->EditorContext()) {
                    if (ectx->GetFocusElement() != el) {
                        el->Focus();
                    }
                }
            }
        }
        return true;
    }

    void ApplyFieldChange(const std::string& field, bool commit = false) {
        if (field == "hier-search") {
            search_filter = ToLowerAscii(GetInputValue("hier-search"));
            RebuildLayers();
            return;
        }
        if (field.rfind("cp-", 0) == 0) {
            ApplyCpInputChange(field.substr(3), commit);
            return;
        }
        if (!doc) return;
        if (field == "code-editor") {
            if (!engine) return;
            std::string code = GetInputValue("code-editor");
            uint64_t now_ms = GetTickCount64();
            if (last_code_undo_tab != code_tab || (now_ms - last_code_undo_ms) > 900ULL) {
                PushUndo();
                last_code_undo_ms = now_ms;
                last_code_undo_tab = code_tab;
            }
            std::string prev_rml_id;
            std::string prev_tag;
            int prev_id = PrimaryId();
            if (const auto* pn = doc->FindNode(prev_id)) {
                prev_rml_id = pn->rml_id;
                prev_tag = pn->tag;
            }

            std::string err;
            bool ok = (code_tab == "RCSS")
                ? engine->ApplyRcssStringAndSync(code, *doc, err)
                : engine->ApplyRmlStringAndSync(code, *doc, err);

            UpdateCodeStatusIndicator(ok, ok ? "Sincronizado" : (err.empty() ? "Sintaxe pendente..." : err));
            SyncCodeGutter(true);
            if (ok) {
                if (code_tab == "RML") {
                    int next_sel = doc->root_id;
                    bool matched = false;
                    if (!prev_rml_id.empty()) {
                        for (const auto& cand : doc->nodes) {
                            if (cand.rml_id == prev_rml_id) {
                                next_sel = cand.id;
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (!matched) {
                        if (const auto* keep = doc->FindNode(prev_id); keep && keep->tag == prev_tag) {
                            next_sel = keep->id;
                        }
                    }
                    SetSingleSelection(next_sel);
                }
                suppress_code_sync = true;
                SyncUIFromDoc(true);
                suppress_code_sync = false;
            }
            return;
        }
        auto* n = doc->FindNode(PrimaryId());
        if (!n) return;

        last_code_undo_ms = 0;
        const std::string input_id = "prop-" + field;
        const std::string raw = GetInputValue(input_id.c_str());

        float pw = (float)doc->screen_width;
        float ph = (float)doc->screen_height;
        if (const auto* p = doc->FindNode(n->parent_id)) { pw = p->width; ph = p->height; }

        bool changed = false;
        auto push_once = [&]() {
            if (!changed) {
                RecordPropUndoBurst(input_id);
                changed = true;
            }
        };

        if (field == "id") {
            if (n->rml_id != raw) { push_once(); n->rml_id = raw; n->modified_style = true; }
        } else if (field == "class") {
            if (n->rml_class != raw) { push_once(); n->rml_class = raw; n->modified_style = true; }
        } else if (field == "x") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v)) { push_once(); n->x = FromDisplayUnit(v, pw); n->modified_transform = true; }
        } else if (field == "y") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v)) { push_once(); n->y = FromDisplayUnit(v, ph); n->modified_transform = true; }
        } else if (field == "w") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v)) {
                float px = FromDisplayUnit(v, pw);
                if (px >= 8.0f || commit) {
                    push_once();
                    float old_w = n->width, old_h = n->height;
                    n->width = std::max(8.0f, px);
                    n->modified_transform = true;
                    if (n->id == doc->root_id) doc->screen_width = (int)std::round(n->width);
                    doc->ApplyConstraintsOnParentResize(n->id, old_w, old_h, n->width, n->height);
                }
            }
        } else if (field == "h") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v)) {
                float px = FromDisplayUnit(v, ph);
                if (px >= 8.0f || commit) {
                    push_once();
                    float old_w = n->width, old_h = n->height;
                    n->height = std::max(8.0f, px);
                    n->modified_transform = true;
                    if (n->id == doc->root_id) doc->screen_height = (int)std::round(n->height);
                    doc->ApplyConstraintsOnParentResize(n->id, old_w, old_h, n->width, n->height);
                }
            }
        } else if (field == "gap") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) { push_once(); n->gap = std::max(0.0f, v); n->modified_style = true; }
        } else if (field == "pad") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) {
                push_once();
                float p = std::max(0.0f, v);
                n->padding[0] = n->padding[1] = n->padding[2] = n->padding[3] = p;
                n->modified_style = true;
            }
        } else if (field == "bg") {
            RmlColor c = n->bg_color;
            std::string trimmed = raw;
            if (!trimmed.empty() && trimmed[0] == '#') trimmed = trimmed.substr(1);
            if ((commit || trimmed.size() >= 6) && ParseHex6(raw, c)) { push_once(); n->bg_color = c; n->has_bg = true; n->modified_style = true; }
        } else if (field == "bg-a") {
            int v = 0;
            if (TryParseIntStrict(raw, v)) {
                push_once();
                n->bg_color.a = (uint8_t)std::clamp(v, 0, 255);
                n->has_bg = (n->bg_color.a > 0);
                n->modified_style = true;
            }
        } else if (field == "radius") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) { push_once(); n->border_radius = std::max(0.0f, v); n->modified_style = true; }
        } else if (field == "border") {
            RmlColor c = n->border_color;
            std::string trimmed = raw;
            if (!trimmed.empty() && trimmed[0] == '#') trimmed = trimmed.substr(1);
            if ((commit || trimmed.size() >= 6) && ParseHex6(raw, c)) { push_once(); n->border_color = c; n->modified_style = true; }
        } else if (field == "border-w") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) { push_once(); n->border_width = std::max(0.0f, v); n->modified_style = true; }
        } else if (field == "border-a") {
            int v = 0;
            if (TryParseIntStrict(raw, v)) { push_once(); n->border_color.a = (uint8_t)std::clamp(v, 0, 255); n->modified_style = true; }
        } else if (field == "text") {
            if (n->text != raw) { push_once(); n->text = raw; }
        } else if (field == "font-size") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && ((v >= 6.0f && v <= 160.0f) || commit)) {
                push_once();
                n->font_size = std::clamp(v, 6.0f, 160.0f);
                n->modified_style = true;
            }
        } else if (field == "fg") {
            RmlColor c = n->text_color;
            std::string trimmed = raw;
            if (!trimmed.empty() && trimmed[0] == '#') trimmed = trimmed.substr(1);
            if ((commit || trimmed.size() >= 6) && ParseHex6(raw, c)) { push_once(); n->text_color = c; n->modified_style = true; }
        } else if (field == "src") {
            if (n->image_src != raw) { push_once(); n->image_src = raw; }
        } else if (field == "grid-count") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 1.0f || commit)) {
                push_once();
                n->grid_count = std::clamp((int)std::round(v), 1, 48);
            }
        } else if (field == "grid-size") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 2.0f || commit)) {
                push_once();
                n->grid_size = std::clamp(v, 2.0f, 256.0f);
            }
        } else if (field == "grid-gutter") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) {
                push_once();
                n->grid_gutter = std::clamp(v, 0.0f, 200.0f);
            }
        } else if (field == "grid-margin") {
            float v = 0.0f;
            if (TryParseFloatStrict(raw, v) && (v >= 0.0f || commit)) {
                push_once();
                n->grid_margin = std::clamp(v, 0.0f, 400.0f);
            }
        } else if (field == "grid-color") {
            RmlColor c = n->grid_color;
            std::string trimmed = raw;
            if (!trimmed.empty() && trimmed[0] == '#') trimmed = trimmed.substr(1);
            if ((commit || trimmed.size() >= 6) && ParseHex6(raw, c)) {
                c.a = n->grid_color.a > 0 ? n->grid_color.a : 46;
                push_once();
                n->grid_color = c;
            }
        }

        if (changed) doc->Touch();
        if (commit) {
            active_edit_input_id.clear();
            force_write_inputs = true;
            SyncUIFromDoc(true);
            force_write_inputs = false;
        } else if (changed) {
            active_edit_input_id = input_id;
            SyncUIFromDoc(true);
            active_edit_input_id.clear();
        }
    }

    std::string ComputeNodeLabel(const RmlNode& n) const {
        if (!n.name.empty()) return n.name;
        if (!n.rml_id.empty()) return "#" + n.rml_id;
        if (!n.text.empty()) {
            return n.text.size() > 24 ? (n.text.substr(0, 24) + "...") : n.text;
        }
        if (!n.rml_class.empty()) {
            std::istringstream ss(n.rml_class);
            std::string first_c;
            if (ss >> first_c) return "." + first_c;
        }
        return n.tag;
    }

    bool NodeOrSubtreeMatchesFilter(int id) const {
        if (search_filter.empty()) return true;
        const RmlNode* n = doc ? doc->FindNode(id) : nullptr;
        if (!n) return false;
        std::string hay = ToLowerAscii(ComputeNodeLabel(*n) + " " + n->tag + " " + n->rml_id + " " + n->rml_class + " " + n->text);
        if (hay.find(search_filter) != std::string::npos) return true;
        for (int cid : n->children) {
            if (NodeOrSubtreeMatchesFilter(cid)) return true;
        }
        return false;
    }

    void RebuildLayers() {
        if (!ui_doc || !doc) return;
        auto* list = ui_doc->GetElementById("layers-list");
        if (!list) return;

        if (auto* cnt = ui_doc->GetElementById("layers-count")) {
            cnt->SetInnerRML(std::to_string(doc->nodes.size()));
        }

        std::vector<int> rendered_ids;
        std::ostringstream html;
        auto append_node = [&](auto& self_fn, int id, int depth) -> void {
            const RmlNode* n = doc->FindNode(id);
            if (!n) return;
            if (!NodeOrSubtreeMatchesFilter(id)) return;

            const char* ico_src = "studio_ico_box";
            if (n->id == doc->root_id || n->tag == "body") ico_src = "studio_ico_braces";
            else if (n->tag == "button") ico_src = "studio_ico_pointer";
            else if (n->tag == "input" || n->tag == "textarea") ico_src = "studio_ico_input";
            else if (n->tag == "span" || n->tag == "p" || n->tag == "label" || (n->tag.size() == 2 && n->tag[0] == 'h')) ico_src = "studio_ico_align_left";
            else if (n->tag == "img") ico_src = "studio_ico_image";
            else if (n->tag == "progress" || n->tag == "progressbar") ico_src = "studio_ico_bar";

            std::string label = ComputeNodeLabel(*n);
            int pad = depth * 14 + 7;
            rendered_ids.push_back(n->id);

            html << "<div id=\"lr-" << n->id << "\" class=\"layer-row"
                 << (IsSelected(n->id) ? " selected" : "")
                 << (!n->visible ? " hidden-node" : "")
                 << "\" style=\"padding-left:" << pad << "px;\">";
            if (!n->children.empty()) {
                html << "<button id=\"lc-" << n->id << "\" class=\"tree-collapse\"><img class=\"ico ico-12 ico-muted\" src=\""
                     << (n->collapsed ? "studio_ico_chevron_right" : "studio_ico_chevron_down") << "\" /></button>";
            } else {
                html << "<span class=\"tree-spacer\">&#160;</span>";
            }
            html << "<span class=\"node-ico-box\"><img class=\"ico ico-14\" src=\"" << ico_src << "\" /></span>"
                 << "<span class=\"layer-name\">" << EscapeXml(label) << "</span>"
                 << "<button id=\"lv-" << n->id << "\" class=\"tree-vis-btn" << (n->visible ? "" : " off") << "\" title=\"Alternar visibilidade\">"
                 << "<img class=\"ico ico-13\" src=\"" << (n->visible ? "studio_ico_eye" : "studio_ico_eye_off") << "\" /></button>"
                 << "</div>";

            if (!n->collapsed || !search_filter.empty()) {
                for (int cid : n->children) self_fn(self_fn, cid, depth + 1);
            }
        };
        append_node(append_node, doc->root_id, 0);
        list->SetInnerRML(html.str());
        layer_listeners.clear();

        auto bind = [&](const std::string& eid, const std::string& act, int nid) {
            if (auto* el = ui_doc->GetElementById(eid.c_str())) {
                auto h = std::make_unique<ClickHandler>();
                h->self = this;
                h->action = act;
                h->node_id = nid;
                el->AddEventListener(Rml::EventId::Click, h.get());
                layer_listeners.push_back(std::move(h));
            }
        };
        for (int nid : rendered_ids) {
            bind("lr-" + std::to_string(nid), "select_node", nid);
            bind("lv-" + std::to_string(nid), "toggle_vis", nid);
            if (const auto* n = doc->FindNode(nid); n && !n->children.empty()) {
                bind("lc-" + std::to_string(nid), "toggle_collapse", nid);
            }
        }
        ApplyPanelSizes(win_w, win_h);
    }

    static size_t Utf8CharToByteOffset(const std::string& s, int char_idx) {
        if (char_idx <= 0) return 0;
        size_t pos = 0;
        int count = 0;
        while (pos < s.size() && count < char_idx) {
            unsigned char c = static_cast<unsigned char>(s[pos]);
            size_t step = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
            pos = std::min(s.size(), pos + step);
            ++count;
        }
        return pos;
    }

    void SyncCodeGutter(bool force_rebuild) {
        if (!ui_doc) return;
        auto* ta = dynamic_cast<Rml::ElementFormControlTextArea*>(ui_doc->GetElementById("code-editor"));
        auto* inner = ui_doc->GetElementById("code-gutter-inner");
        auto* hl = ui_doc->GetElementById("code-highlight");
        auto* act_line = ui_doc->GetElementById("code-active-line");
        if (!ta || !inner) return;

        float st = ta->GetScrollTop();
        float sl = ta->GetScrollLeft();
        if (force_rebuild || std::fabs(st - last_code_scroll_top) > 0.25f || std::fabs(sl - last_code_scroll_left) > 0.25f) {
            last_code_scroll_top = st;
            last_code_scroll_left = sl;
            char m_buf[32];
            std::snprintf(m_buf, sizeof(m_buf), "%.1fpx", -st);
            inner->SetProperty("margin-top", m_buf);
            if (hl) {
                hl->SetProperty("margin-top", m_buf);
                std::snprintf(m_buf, sizeof(m_buf), "%.1fpx", -sl);
                hl->SetProperty("margin-left", m_buf);
            }
            if (act_line && last_gutter_cur_line >= 1) {
                float top_y = 8.0f + (float)(last_gutter_cur_line - 1) * 16.0f - st;
                std::snprintf(m_buf, sizeof(m_buf), "%.1fpx", top_y);
                act_line->SetProperty("top", m_buf);
            }
        }

        std::string val = ta->GetValue();
        if (hl && (force_rebuild || val != last_highlighted_code || code_tab != last_highlighted_tab)) {
            last_highlighted_code = val;
            last_highlighted_tab = code_tab;
            std::string hl_src = val;
            constexpr size_t kMaxHlBytes = 48000u;
            if (hl_src.size() > kMaxHlBytes) {
                hl_src.resize(kMaxHlBytes);
            }
            hl->SetInnerRML((code_tab == "RCSS") ? BuildHighlightedRcss(hl_src) : BuildHighlightedRml(hl_src));
        }

        int sel_s = 0, sel_e = 0;
        ta->GetSelection(&sel_s, &sel_e, nullptr);
        if (!force_rebuild && sel_s == last_cursor_sel_start) {
            return;
        }
        last_cursor_sel_start = sel_s;

        size_t cursor_byte = Utf8CharToByteOffset(val, sel_s);

        int total_lines = 1;
        int cur_line = 1;
        size_t line_start_byte = 0;
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == '\n') {
                ++total_lines;
                if (i < cursor_byte) {
                    ++cur_line;
                    line_start_byte = i + 1;
                }
            }
        }
        int cur_col = 1;
        for (size_t i = line_start_byte; i < cursor_byte && i < val.size();) {
            unsigned char c = static_cast<unsigned char>(val[i]);
            size_t step = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
            i = std::min(cursor_byte, i + step);
            ++cur_col;
        }

        constexpr int kMaxGutterLines = 1500;
        int shown_lines = std::clamp(total_lines, 1, kMaxGutterLines);

        if (shown_lines != last_gutter_lines) {
            std::ostringstream gss;
            for (int l = 1; l <= shown_lines; ++l) {
                gss << "<div id=\"cl-" << l << "\" class=\"code-lnum" << (l == cur_line ? " cur" : "")
                    << "\">" << l << "</div>";
            }
            inner->SetInnerRML(gss.str());
            last_gutter_lines = shown_lines;
            last_gutter_cur_line = cur_line;
        } else if (cur_line != last_gutter_cur_line) {
            if (last_gutter_cur_line >= 1) {
                if (auto* prev_el = ui_doc->GetElementById("cl-" + std::to_string(last_gutter_cur_line))) {
                    prev_el->SetClass("cur", false);
                }
            }
            if (cur_line >= 1) {
                if (auto* cur_el = ui_doc->GetElementById("cl-" + std::to_string(cur_line))) {
                    cur_el->SetClass("cur", true);
                }
            }
            last_gutter_cur_line = cur_line;
        }

        if (act_line) {
            char m_buf[32];
            float top_y = 8.0f + (float)(cur_line - 1) * 16.0f - st;
            std::snprintf(m_buf, sizeof(m_buf), "%.1fpx", top_y);
            act_line->SetProperty("top", m_buf);
        }

        if (auto* lbl = ui_doc->GetElementById("code-cursor-lbl")) {
            char cbuf[64];
            std::snprintf(cbuf, sizeof(cbuf), "Ln %d, Col %d &#183; %d linha%s",
                          cur_line, cur_col, total_lines, total_lines == 1 ? "" : "s");
            lbl->SetInnerRML(cbuf);
        }
    }

    void SyncCodeDock() {
        if (!ui_doc || !doc) return;
        last_synced_code_tab = code_tab;
        if (suppress_code_sync) return;

        std::string rml, rcss;
        std::vector<RmlAsset> assets;
        doc->GenerateRmlAndRcss(rml, rcss, assets);
        const std::string& src = (code_tab == "RCSS") ? rcss : rml;

        bool prev_sync = syncing_ui;
        syncing_ui = true;
        SetInputValue("code-editor", src);
        UpdateCodeStatusIndicator(true);
        SyncCodeGutter(true);
        syncing_ui = prev_sync;
    }

    void SyncUIFromDoc(bool force = false) {
        if (!ui_doc || !doc) return;
        int cur_primary = PrimaryId();
        if (!force && last_synced_rev == doc->revision && last_synced_sel == cur_primary && last_synced_sel_count == selected_ids.size()) return;

        selected_ids.erase(
            std::remove_if(selected_ids.begin(), selected_ids.end(), [&](int sid) { return !doc->FindNode(sid); }),
            selected_ids.end()
        );
        if (selected_ids.empty()) SetSingleSelection(doc->root_id);
        cur_primary = PrimaryId();

        last_synced_rev = doc->revision;
        last_synced_sel = cur_primary;
        last_synced_sel_count = selected_ids.size();
        syncing_ui = true;

        UpdateHeaderButtonsUI();
        UpdateDomStatus();
        RebuildLayers();

        if (const auto* n = doc->FindNode(cur_primary)) {
            if (auto* pill = ui_doc->GetElementById("insp-node-pill")) {
                std::string ptxt = ComputeNodeLabel(*n);
                if (selected_ids.size() > 1) ptxt += " (+" + std::to_string(selected_ids.size() - 1) + ")";
                pill->SetInnerRML(EscapeXml(ptxt));
            }
            if (auto* sub = ui_doc->GetElementById("insp-node-sub")) {
                std::string stxt = (selected_ids.size() > 1)
                    ? (std::to_string(selected_ids.size()) + " elementos selecionados")
                    : ("&lt;" + EscapeXml(n->tag) + "&gt; selecionado");
                sub->SetInnerRML(stxt);
            }

            SetInputValue("prop-id", n->rml_id.empty() ? n->name : n->rml_id);
            SetInputValue("prop-class", n->rml_class);
            if (auto* vis = ui_doc->GetElementById("prop-visible")) vis->SetClass("on", n->visible);
            if (auto* b = ui_doc->GetElementById("lbl-lock")) b->SetInnerRML(n->locked ? "Destravar" : "Bloquear");

            float pw = (float)doc->screen_width;
            float ph = (float)doc->screen_height;
            if (const auto* p = doc->FindNode(n->parent_id)) { pw = p->width; ph = p->height; }
            SetInputValue("prop-x", FormatFloat(ToDisplayUnit(n->x, pw)));
            SetInputValue("prop-y", FormatFloat(ToDisplayUnit(n->y, ph)));
            SetInputValue("prop-w", FormatFloat(ToDisplayUnit(n->width, pw)));
            SetInputValue("prop-h", FormatFloat(ToDisplayUnit(n->height, ph)));

            if (auto* e = ui_doc->GetElementById("u-px")) e->SetClass("active", unit_mode == "px");
            if (auto* e = ui_doc->GetElementById("u-pct")) e->SetClass("active", unit_mode == "%");
            if (auto* e = ui_doc->GetElementById("u-dp")) e->SetClass("active", unit_mode == "dp");

            if (auto* e = ui_doc->GetElementById("rw-fixed")) e->SetClass("active", n->size_mode_w == 0);
            if (auto* e = ui_doc->GetElementById("rw-hug")) e->SetClass("active", n->size_mode_w == 1);
            if (auto* e = ui_doc->GetElementById("rw-fill")) e->SetClass("active", n->size_mode_w == 2);
            if (auto* e = ui_doc->GetElementById("rh-fixed")) e->SetClass("active", n->size_mode_h == 0);
            if (auto* e = ui_doc->GetElementById("rh-hug")) e->SetClass("active", n->size_mode_h == 1);
            if (auto* e = ui_doc->GetElementById("rh-fill")) e->SetClass("active", n->size_mode_h == 2);

            if (auto* e = ui_doc->GetElementById("ch-left")) e->SetClass("active", n->anchor_h == 0);
            if (auto* e = ui_doc->GetElementById("ch-right")) e->SetClass("active", n->anchor_h == 1);
            if (auto* e = ui_doc->GetElementById("ch-lr")) e->SetClass("active", n->anchor_h == 2);
            if (auto* e = ui_doc->GetElementById("ch-center")) e->SetClass("active", n->anchor_h == 3);
            if (auto* e = ui_doc->GetElementById("ch-scale")) e->SetClass("active", n->anchor_h == 4);

            if (auto* e = ui_doc->GetElementById("cv-top")) e->SetClass("active", n->anchor_v == 0);
            if (auto* e = ui_doc->GetElementById("cv-bottom")) e->SetClass("active", n->anchor_v == 1);
            if (auto* e = ui_doc->GetElementById("cv-tb")) e->SetClass("active", n->anchor_v == 2);
            if (auto* e = ui_doc->GetElementById("cv-center")) e->SetClass("active", n->anchor_v == 3);
            if (auto* e = ui_doc->GetElementById("cv-scale")) e->SetClass("active", n->anchor_v == 4);

            if (auto* e = ui_doc->GetElementById("cpin-left")) e->SetClass("active", n->anchor_h == 0 || n->anchor_h == 2);
            if (auto* e = ui_doc->GetElementById("cpin-right")) e->SetClass("active", n->anchor_h == 1 || n->anchor_h == 2);
            if (auto* e = ui_doc->GetElementById("cpin-hcenter")) e->SetClass("active", n->anchor_h == 3 || n->anchor_h == 4);
            if (auto* e = ui_doc->GetElementById("cpin-top")) e->SetClass("active", n->anchor_v == 0 || n->anchor_v == 2);
            if (auto* e = ui_doc->GetElementById("cpin-bottom")) e->SetClass("active", n->anchor_v == 1 || n->anchor_v == 2);
            if (auto* e = ui_doc->GetElementById("cpin-vcenter")) e->SetClass("active", n->anchor_v == 3 || n->anchor_v == 4);

            if (auto* e = ui_doc->GetElementById("disp-block")) e->SetClass("active", !n->display_flex);
            if (auto* e = ui_doc->GetElementById("disp-flex")) e->SetClass("active", n->display_flex);
            if (auto* e = ui_doc->GetElementById("wrap-off")) e->SetClass("active", !n->flex_wrap);
            if (auto* e = ui_doc->GetElementById("wrap-on")) e->SetClass("active", n->flex_wrap);
            if (auto* e = ui_doc->GetElementById("dir-row")) e->SetClass("active", n->flex_direction == 0);
            if (auto* e = ui_doc->GetElementById("dir-col")) e->SetClass("active", n->flex_direction != 0);
            if (auto* e = ui_doc->GetElementById("jc-start")) e->SetClass("active", n->justify_content == 0);
            if (auto* e = ui_doc->GetElementById("jc-center")) e->SetClass("active", n->justify_content == 1);
            if (auto* e = ui_doc->GetElementById("jc-end")) e->SetClass("active", n->justify_content == 2);
            if (auto* e = ui_doc->GetElementById("jc-between")) e->SetClass("active", n->justify_content == 3);
            if (auto* e = ui_doc->GetElementById("ai-start")) e->SetClass("active", n->align_items == 0);
            if (auto* e = ui_doc->GetElementById("ai-center")) e->SetClass("active", n->align_items == 1);
            if (auto* e = ui_doc->GetElementById("ai-end")) e->SetClass("active", n->align_items == 2);
            SetInputValue("prop-gap", FormatFloat(n->gap));
            SetInputValue("prop-pad", FormatFloat(n->padding[0]));

            if (auto* ge = ui_doc->GetElementById("grid-enable")) ge->SetClass("on", n->grid_enabled);
            if (auto* e = ui_doc->GetElementById("gtype-col")) e->SetClass("active", n->grid_type == 0);
            if (auto* e = ui_doc->GetElementById("gtype-row")) e->SetClass("active", n->grid_type == 1);
            if (auto* e = ui_doc->GetElementById("gtype-grid")) e->SetClass("active", n->grid_type == 2);
            SetInputValue("prop-grid-count", std::to_string(n->grid_count));
            SetInputValue("prop-grid-size", FormatFloat(n->grid_size));
            SetInputValue("prop-grid-gutter", FormatFloat(n->grid_gutter));
            SetInputValue("prop-grid-margin", FormatFloat(n->grid_margin));
            SetInputValue("prop-grid-color", ColorToHex6(n->grid_color));
            if (auto* sw = ui_doc->GetElementById("swatch-grid")) {
                RmlColor swg = n->grid_color;
                swg.a = 255;
                sw->SetProperty("background-color", swg.ToCss());
            }

            SetInputValue("prop-bg", ColorToHex6(n->bg_color));
            SetInputValue("prop-bg-a", std::to_string((int)n->bg_color.a));
            SetInputValue("prop-radius", FormatFloat(n->border_radius));
            if (auto* sw = ui_doc->GetElementById("swatch-bg")) sw->SetProperty("background-color", n->bg_color.ToCss());

            SetInputValue("prop-border", ColorToHex6(n->border_color));
            SetInputValue("prop-border-w", FormatFloat(n->border_width));
            SetInputValue("prop-border-a", std::to_string((int)n->border_color.a));
            if (auto* sw = ui_doc->GetElementById("swatch-border")) sw->SetProperty("background-color", n->border_color.ToCss());

            SetInputValue("prop-text", n->text);
            SetInputValue("prop-font-size", FormatFloat(n->font_size));
            if (auto* e = ui_doc->GetElementById("al-left")) e->SetClass("active", n->text_align == 0);
            if (auto* e = ui_doc->GetElementById("al-center")) e->SetClass("active", n->text_align == 1);
            if (auto* e = ui_doc->GetElementById("al-right")) e->SetClass("active", n->text_align == 2);

            SetInputValue("prop-fg", ColorToHex6(n->text_color));
            if (auto* sw = ui_doc->GetElementById("swatch-fg")) sw->SetProperty("background-color", n->text_color.ToCss());
            SetInputValue("prop-src", n->image_src);
        }

        SyncCodeDock();
        ApplyPanelSizes(win_w, win_h);
        syncing_ui = false;
    }

    bool IsPointOverElement(const char* id, int x, int y) const {
        if (!ui_doc) return false;
        auto* el = ui_doc->GetElementById(id);
        if (!el || el->IsClassSet("hidden")) return false;
        Rml::Vector2f off = el->GetAbsoluteOffset(Rml::BoxArea::Border);
        Rml::Vector2f sz = el->GetBox().GetSize(Rml::BoxArea::Border);
        if (sz.x <= 0.0f || sz.y <= 0.0f) return false;
        return x >= (int)off.x && x <= (int)(off.x + sz.x) && y >= (int)off.y && y <= (int)(off.y + sz.y);
    }

    bool IsPointInCanvasViewport(int x, int y) const {
        if (!active_menu.empty()) return false;
        if (cp_open && IsPointOverElement("color-picker-win", x, y)) return false;
        if (x < (int)LeftTotal() || x >= (int)((float)win_w - RightTotal()) ||
            y < (int)TopTotal() || y >= (int)((float)win_h - BottomTotal())) {
            return false;
        }
        if (IsPointOverElement("btn-empty-add", x, y)) return false;
        return true;
    }

    void ScreenToCanvas(int mx, int my, float& cx, float& cy) const {
        cx = ((float)mx - pan_x) / std::max(0.01f, zoom);
        cy = ((float)my - pan_y) / std::max(0.01f, zoom);
    }

    int HitTestHandle(int mx, int my, const RmlNode& n) const {
        float wx = 0.0f, wy = 0.0f, ww = n.width, wh = n.height;
        if (!engine || !engine->GetCanvasElementRect(n.id, wx, wy, ww, wh)) {
            doc->GetWorldPos(n.id, wx, wy);
            ww = n.width;
            wh = n.height;
        }
        float x0 = pan_x + wx * zoom;
        float y0 = pan_y + wy * zoom;
        float x1 = x0 + ww * zoom;
        float y1 = y0 + wh * zoom;
        float xm = (x0 + x1) * 0.5f;
        float ym = (y0 + y1) * 0.5f;
        float pts[8][2] = {
            {x0, y0}, {xm, y0}, {x1, y0}, {x1, ym},
            {x1, y1}, {xm, y1}, {x0, y1}, {x0, ym}
        };
        for (int i = 0; i < 8; ++i) {
            if (std::fabs((float)mx - pts[i][0]) <= 7.0f && std::fabs((float)my - pts[i][1]) <= 7.0f) return i;
        }
        return -1;
    }
};

StudioUI::StudioUI() : impl_(std::make_unique<Impl>()) {}
StudioUI::~StudioUI() = default;

bool StudioUI::Init(HWND hwnd, RmlDocument& doc, RmlEngineHost& engine) {
    impl_->hwnd = hwnd;
    impl_->doc = &doc;
    impl_->engine = &engine;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    impl_->win_w = std::max(1, (int)(rc.right - rc.left));
    impl_->win_h = std::max(1, (int)(rc.bottom - rc.top));

    impl_->ui_doc = engine.LoadEditorDocument(kEditorRml, kEditorRcss);
    if (!impl_->ui_doc) return false;

    engine.ResizeEditor(impl_->win_w, impl_->win_h);

    // Topbar
    impl_->BindButton("m-file", "menu_file");
    impl_->BindButton("m-edit", "menu_edit");
    impl_->BindButton("m-view", "menu_view");
    impl_->BindButton("m-vp", "menu_vp");
    impl_->BindButton("btn-help", "help");
    impl_->BindButton("btn-settings", "settings");

    // Toolbar
    impl_->BindButton("btn-new", "menu_add");
    impl_->BindButton("btn-imp-menu", "menu_imp");
    impl_->BindButton("btn-undo", "undo");
    impl_->BindButton("btn-redo", "redo");
    impl_->BindButton("vp-pc", "vp_pc");
    impl_->BindButton("bg-grid", "toggle_grid");
    impl_->BindButton("btn-play", "play");
    impl_->BindButton("btn-export", "export");

    // Left hierarchy
    impl_->BindButton("btn-hier-add", "hier_add");
    impl_->BindChange("hier-search", "hier-search");

    // Canvas controls & empty overlay
    impl_->BindButton("tool-sel", "tool_sel");
    impl_->BindButton("tool-pan", "tool_pan");
    impl_->BindButton("btn-canvas-grid", "toggle_grid");
    impl_->BindButton("btn-canvas-snap", "toggle_snap");
    impl_->BindButton("btn-canvas-bg", "pick_canvas_bg");
    impl_->BindButton("btn-zoom-out", "zoom_out");
    impl_->BindButton("btn-zoom-in", "zoom_in");
    impl_->BindButton("btn-fit", "fit");
    impl_->BindButton("btn-empty-add", "empty_add");

    // Code dock
    impl_->BindButton("tab-code-rml", "tab_rml");
    impl_->BindButton("tab-code-rcss", "tab_rcss");
    impl_->BindButton("btn-expand-code", "expand_code");
    impl_->BindButton("btn-copy-code", "copy_code");
    impl_->BindChange("code-editor", "code-editor");

    // Color picker
    impl_->BindButton("cp-ok", "cp_ok");
    impl_->BindButton("cp-cancel", "cp_cancel");
    impl_->BindButton("cp-close", "cp_cancel");
    impl_->BindButton("cp-preview-orig", "cp_revert");
    for (int swi = 0; swi < 11; ++swi) {
        std::string sw_id = "cp-sw-" + std::to_string(swi);
        std::string sw_act = "cp_sw_" + std::to_string(swi);
        impl_->BindButton(sw_id.c_str(), sw_act);
    }
    impl_->BindChange("cp-in-h", "cp-h");
    impl_->BindChange("cp-in-s", "cp-s");
    impl_->BindChange("cp-in-v", "cp-v");
    impl_->BindChange("cp-in-r", "cp-r");
    impl_->BindChange("cp-in-g", "cp-g");
    impl_->BindChange("cp-in-b", "cp-b");
    impl_->BindChange("cp-in-hex", "cp-hex");

    // Inspector swatches & controls
    impl_->BindButton("swatch-bg", "pick_swatch_bg");
    impl_->BindButton("swatch-border", "pick_swatch_border");
    impl_->BindButton("swatch-fg", "pick_swatch_fg");
    impl_->BindButton("swatch-grid", "pick_swatch_grid");
    impl_->BindButton("btn-insp-more", "insp_more");
    impl_->BindButton("btn-comp-more", "insp_more");
    impl_->BindButton("prop-visible", "toggle_visible_switch");
    impl_->BindButton("btn-align-left", "btn_align_left");
    impl_->BindButton("btn-align-hcenter", "btn_align_hcenter");
    impl_->BindButton("btn-align-right", "btn_align_right");
    impl_->BindButton("btn-align-top", "btn_align_top");
    impl_->BindButton("btn-align-vcenter", "btn_align_vcenter");
    impl_->BindButton("btn-align-bottom", "btn_align_bottom");
    impl_->BindButton("btn-dist-h", "btn_dist_h");
    impl_->BindButton("btn-dist-v", "btn_dist_v");
    impl_->BindButton("rw-fixed", "rw_fixed");
    impl_->BindButton("rw-hug", "rw_hug");
    impl_->BindButton("rw-fill", "rw_fill");
    impl_->BindButton("rh-fixed", "rh_fixed");
    impl_->BindButton("rh-hug", "rh_hug");
    impl_->BindButton("rh-fill", "rh_fill");
    impl_->BindButton("ch-left", "ch_left");
    impl_->BindButton("ch-right", "ch_right");
    impl_->BindButton("ch-lr", "ch_lr");
    impl_->BindButton("ch-center", "ch_center");
    impl_->BindButton("ch-scale", "ch_scale");
    impl_->BindButton("cv-top", "cv_top");
    impl_->BindButton("cv-bottom", "cv_bottom");
    impl_->BindButton("cv-tb", "cv_tb");
    impl_->BindButton("cv-center", "cv_center");
    impl_->BindButton("cv-scale", "cv_scale");
    impl_->BindButton("cpin-left", "cpin_left");
    impl_->BindButton("cpin-right", "cpin_right");
    impl_->BindButton("cpin-hcenter", "cpin_hcenter");
    impl_->BindButton("cpin-top", "cpin_top");
    impl_->BindButton("cpin-bottom", "cpin_bottom");
    impl_->BindButton("cpin-vcenter", "cpin_vcenter");
    impl_->BindButton("grid-enable", "toggle_node_grid");
    impl_->BindButton("gtype-col", "gtype_col");
    impl_->BindButton("gtype-row", "gtype_row");
    impl_->BindButton("gtype-grid", "gtype_grid");
    impl_->BindButton("u-px", "u_px");
    impl_->BindButton("u-pct", "u_pct");
    impl_->BindButton("u-dp", "u_dp");
    impl_->BindButton("disp-block", "disp_block");
    impl_->BindButton("disp-flex", "disp_flex");
    impl_->BindButton("wrap-off", "wrap_off");
    impl_->BindButton("wrap-on", "wrap_on");
    impl_->BindButton("dir-row", "dir_row");
    impl_->BindButton("dir-col", "dir_col");
    impl_->BindButton("jc-start", "jc_start");
    impl_->BindButton("jc-center", "jc_center");
    impl_->BindButton("jc-end", "jc_end");
    impl_->BindButton("jc-between", "jc_between");
    impl_->BindButton("ai-start", "ai_start");
    impl_->BindButton("ai-center", "ai_center");
    impl_->BindButton("ai-end", "ai_end");
    impl_->BindButton("al-left", "al_left");
    impl_->BindButton("al-center", "al_center");
    impl_->BindButton("al-right", "al_right");
    impl_->BindButton("btn-pick-img", "pick_img");
    impl_->BindButton("act-dup", "act_dup");
    impl_->BindButton("act-del", "act_del");
    impl_->BindButton("act-lock", "act_lock");

    impl_->BindChange("prop-id", "id");
    impl_->BindChange("prop-class", "class");
    impl_->BindChange("prop-x", "x");
    impl_->BindChange("prop-y", "y");
    impl_->BindChange("prop-w", "w");
    impl_->BindChange("prop-h", "h");
    impl_->BindChange("prop-gap", "gap");
    impl_->BindChange("prop-pad", "pad");
    impl_->BindChange("prop-grid-count", "grid-count");
    impl_->BindChange("prop-grid-size", "grid-size");
    impl_->BindChange("prop-grid-gutter", "grid-gutter");
    impl_->BindChange("prop-grid-margin", "grid-margin");
    impl_->BindChange("prop-grid-color", "grid-color");
    impl_->BindChange("prop-bg", "bg");
    impl_->BindChange("prop-bg-a", "bg-a");
    impl_->BindChange("prop-radius", "radius");
    impl_->BindChange("prop-border", "border");
    impl_->BindChange("prop-border-w", "border-w");
    impl_->BindChange("prop-border-a", "border-a");
    impl_->BindChange("prop-text", "text");
    impl_->BindChange("prop-font-size", "font-size");
    impl_->BindChange("prop-fg", "fg");
    impl_->BindChange("prop-src", "src");

    impl_->SetSingleSelection(doc.root_id);
    impl_->ApplyPanelSizes();
    impl_->FitViewport();
    impl_->SyncUIFromDoc(true);
    return true;
}

void StudioUI::Shutdown() {
    if (impl_->ui_doc) {
        impl_->ui_doc->SetInnerRML("");
    }
    impl_->click_listeners.clear();
    impl_->change_listeners.clear();
    impl_->layer_listeners.clear();
    impl_->popup_listeners.clear();
    impl_->ui_doc = nullptr;
}

void StudioUI::OnResize(int width, int height) {
    impl_->win_w = std::max(1, width);
    impl_->win_h = std::max(1, height);
    if (impl_->engine) impl_->engine->ResizeEditor(impl_->win_w, impl_->win_h);
    impl_->ApplyPanelSizes();
    impl_->SyncRulers();
}

void StudioUI::OnMouseMove(int x, int y) {
    impl_->mouse_x = x;
    impl_->mouse_y = y;
    if (!impl_->engine || !impl_->doc) return;

    if (impl_->cp_drag != Impl::CpDrag::None) {
        if (impl_->cp_drag == Impl::CpDrag::SV) impl_->UpdateColorPickerFromSVMouse(x, y);
        else if (impl_->cp_drag == Impl::CpDrag::Hue) impl_->UpdateColorPickerFromHueMouse(y);
        return;
    }

    if (impl_->splitter_drag != Impl::SplitterDrag::None) {
        if (impl_->splitter_drag == Impl::SplitterDrag::Left) {
            impl_->left_panel_width = impl_->split_start_size + (float)(x - impl_->split_start_mouse);
        } else if (impl_->splitter_drag == Impl::SplitterDrag::Right) {
            impl_->right_inspector_width = impl_->split_start_size - (float)(x - impl_->split_start_mouse);
        } else if (impl_->splitter_drag == Impl::SplitterDrag::Bottom) {
            impl_->bottom_dock_height = impl_->split_start_size - (float)(y - impl_->split_start_mouse);
        }
        impl_->ApplyPanelSizes();
        impl_->SyncRulers();
        return;
    }

    if (impl_->panning) {
        impl_->pan_x = impl_->pan_start_x + (float)(x - impl_->pan_start_mx);
        impl_->pan_y = impl_->pan_start_y + (float)(y - impl_->pan_start_my);
        impl_->SyncRulers();
        return;
    }

    if (impl_->drag_pending) {
        int mdx = std::abs(x - impl_->drag_down_mx);
        int mdy = std::abs(y - impl_->drag_down_my);
        if (mdx < 4 && mdy < 4) return;
        impl_->drag_pending = false;
        impl_->drag_did_move = true;
        impl_->PushUndo();
        impl_->drag_mode = Impl::DragMode::Move;
        for (const auto& orig : impl_->drag_multi_orig) {
            if (auto* n = impl_->doc->FindNode(orig.id)) {
                n->x = orig.x;
                n->y = orig.y;
                n->width = orig.w;
                n->height = orig.h;
            }
        }
    }

    if (impl_->drag_mode != Impl::DragMode::None) {
        float cx = 0.0f, cy = 0.0f;
        impl_->ScreenToCanvas(x, y, cx, cy);
        float dx = cx - impl_->drag_start_sx;
        float dy = cy - impl_->drag_start_sy;
        if (impl_->drag_mode == Impl::DragMode::Move) {
            bool bypass_snap = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            float snapped_dx = dx, snapped_dy = dy;
            impl_->ComputeDragSnapMove(impl_->PrimaryId(), dx, dy, bypass_snap, snapped_dx, snapped_dy);
            impl_->drag_dirty_node_ids.clear();
            for (const auto& orig : impl_->drag_multi_orig) {
                if (auto* n = impl_->doc->FindNode(orig.id)) {
                    n->x = std::round(orig.x + snapped_dx);
                    n->y = std::round(orig.y + snapped_dy);
                    n->modified_transform = true;
                    impl_->drag_dirty_node_ids.push_back(n->id);
                }
            }
            impl_->drag_transform_ui_dirty = true;
        } else if (impl_->drag_mode == Impl::DragMode::Resize) {
            if (auto* n = impl_->doc->FindNode(impl_->PrimaryId())) {
                float nx = impl_->orig_x, ny = impl_->orig_y, nw = impl_->orig_w, nh = impl_->orig_h;
                int h = impl_->drag_handle;
                if (h == 0 || h == 6 || h == 7) { float d = std::min(dx, nw - 12.0f); nx += d; nw -= d; }
                if (h == 2 || h == 3 || h == 4) { nw = std::max(12.0f, nw + dx); }
                if (h == 0 || h == 1 || h == 2) { float d = std::min(dy, nh - 12.0f); ny += d; nh -= d; }
                if (h == 4 || h == 5 || h == 6) { nh = std::max(12.0f, nh + dy); }
                bool bypass_snap = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                impl_->ComputeDragSnapResize(n->id, h, bypass_snap, nx, ny, nw, nh);
                n->x = std::round(nx);
                n->y = std::round(ny);
                n->width = std::round(nw);
                n->height = std::round(nh);
                n->modified_transform = true;
                if (!n->children.empty() && !impl_->undo_stack.empty()) {
                    impl_->RestoreSubtreeForConstraintDrag(impl_->undo_stack.back(), n->id);
                    impl_->doc->ApplyConstraintsOnParentResize(n->id, impl_->orig_w, impl_->orig_h, n->width, n->height);
                }
                impl_->drag_dirty_node_ids.clear();
                impl_->drag_dirty_node_ids.push_back(n->id);
                impl_->drag_transform_ui_dirty = true;
            }
        }
        return;
    }

    if (impl_->play_mode && impl_->IsPointInCanvasViewport(x, y)) {
        float cx = 0.0f, cy = 0.0f;
        impl_->ScreenToCanvas(x, y, cx, cy);
        impl_->engine->CanvasMouseMove((int)std::round(cx), (int)std::round(cy));
    }

    if (auto* ectx = impl_->engine->EditorContext()) {
        ectx->ProcessMouseMove(x, y, GetRmlKeyModifiers());
    }
}

void StudioUI::OnMouseButton(int button, bool down) {
    if (!impl_->engine || !impl_->doc) return;
    int x = impl_->mouse_x;
    int y = impl_->mouse_y;

    if (button == 0 && !down && impl_->cp_drag != Impl::CpDrag::None) {
        impl_->cp_drag = Impl::CpDrag::None;
        ReleaseCapture();
        return;
    }

    if (button == 0 && down && impl_->cp_open) {
        if (impl_->IsPointOverElement("cp-sv-bg", x, y)) {
            impl_->cp_drag = Impl::CpDrag::SV;
            impl_->UpdateColorPickerFromSVMouse(x, y);
            SetCapture(impl_->hwnd);
            return;
        }
        if (impl_->IsPointOverElement("cp-hue-wrap", x, y)) {
            impl_->cp_drag = Impl::CpDrag::Hue;
            impl_->UpdateColorPickerFromHueMouse(y);
            SetCapture(impl_->hwnd);
            return;
        }
    }

    if (button == 0 && !down && impl_->splitter_drag != Impl::SplitterDrag::None) {
        impl_->splitter_drag = Impl::SplitterDrag::None;
        ReleaseCapture();
        return;
    }

    if (button == 0 && down && !impl_->active_menu.empty()) {
        bool inside_popup = impl_->IsPointOverElement("menu-popup", x, y);
        if (!inside_popup && y > (int)Impl::kTopBarH) {
            impl_->pending_cmds.push_back({"__close_menu__", 0, false, false, "", false});
        }
        if (inside_popup) {
            if (auto* ectx = impl_->engine->EditorContext()) {
                ectx->ProcessMouseButtonDown(button, GetRmlKeyModifiers());
            }
            return;
        }
    }

    if (button == 0 && down && impl_->active_menu.empty()) {
        float top_y = Impl::kTopBarH + Impl::kToolbarH;
        float bot_y = (float)impl_->win_h - Impl::kStatusBarH;
        if (y >= (int)top_y && y <= (int)bot_y) {
            float sx_left = impl_->left_panel_width;
            float sx_right = (float)impl_->win_w - impl_->right_inspector_width - Impl::kSplitW;
            if (x >= (int)(sx_left - 2.0f) && x <= (int)(sx_left + Impl::kSplitW + 2.0f)) {
                impl_->splitter_drag = Impl::SplitterDrag::Left;
                impl_->split_start_mouse = x;
                impl_->split_start_size = impl_->left_panel_width;
                SetCapture(impl_->hwnd);
                return;
            }
            if (x >= (int)(sx_right - 2.0f) && x <= (int)(sx_right + Impl::kSplitW + 2.0f)) {
                impl_->splitter_drag = Impl::SplitterDrag::Right;
                impl_->split_start_mouse = x;
                impl_->split_start_size = impl_->right_inspector_width;
                SetCapture(impl_->hwnd);
                return;
            }
            if (impl_->show_bottom_dock) {
                float sy_bot = bot_y - impl_->bottom_dock_height - Impl::kSplitW;
                if (x >= (int)(sx_left + Impl::kSplitW) && x <= (int)sx_right &&
                    y >= (int)(sy_bot - 2.0f) && y <= (int)(sy_bot + Impl::kSplitW + 2.0f)) {
                    impl_->splitter_drag = Impl::SplitterDrag::Bottom;
                    impl_->split_start_mouse = y;
                    impl_->split_start_size = impl_->bottom_dock_height;
                    SetCapture(impl_->hwnd);
                    return;
                }
            }
        }
    }

    if (button == 2 || (button == 0 && (impl_->space_down || impl_->hand_tool))) {
        if (down && impl_->IsPointInCanvasViewport(x, y)) {
            impl_->panning = true;
            impl_->pan_start_mx = x;
            impl_->pan_start_my = y;
            impl_->pan_start_x = impl_->pan_x;
            impl_->pan_start_y = impl_->pan_y;
            SetCapture(impl_->hwnd);
            return;
        }
        if (!down && impl_->panning) {
            impl_->panning = false;
            ReleaseCapture();
            return;
        }
    }

    if (!down && impl_->drag_pending) {
        impl_->drag_pending = false;
        impl_->drag_did_move = false;
        impl_->drag_multi_orig.clear();
        ReleaseCapture();
        return;
    }

    if (!down && impl_->drag_mode != Impl::DragMode::None) {
        auto prev_mode = impl_->drag_mode;
        bool did_move = impl_->drag_did_move;
        impl_->drag_mode = Impl::DragMode::None;
        impl_->drag_did_move = false;
        impl_->drag_transform_ui_dirty = false;
        impl_->active_guides.clear();
        ReleaseCapture();
        if (!impl_->doc->has_imported_source && did_move &&
            prev_mode == Impl::DragMode::Move && impl_->drag_multi_orig.size() == 1) {
            impl_->AutoReparentAfterMove(impl_->drag_multi_orig[0].id);
        }
        impl_->drag_multi_orig.clear();
        impl_->doc->Touch();
        impl_->SyncUIFromDoc(true);
        return;
    }

    if (impl_->IsPointInCanvasViewport(x, y)) {
        float cx = 0.0f, cy = 0.0f;
        impl_->ScreenToCanvas(x, y, cx, cy);

        if (impl_->play_mode) {
            impl_->engine->CanvasMouseMove((int)std::round(cx), (int)std::round(cy));
            impl_->engine->CanvasMouseButton(button, down);
            return;
        }

        if (button == 0 && down) {
            if (impl_->selected_ids.size() == 1) {
                if (auto* sel = impl_->doc->FindNode(impl_->PrimaryId())) {
                    if (sel->id != impl_->doc->root_id && !sel->locked) {
                        int h = impl_->HitTestHandle(x, y, *sel);
                        if (h >= 0) {
                            impl_->PushUndo();
                            impl_->drag_mode = Impl::DragMode::Resize;
                            impl_->drag_handle = h;
                            impl_->drag_start_sx = cx;
                            impl_->drag_start_sy = cy;
                            float rx = sel->x, ry = sel->y, rw = sel->width, rh = sel->height;
                            float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
                            if (impl_->engine->GetCanvasElementRect(sel->id, ax, ay, aw, ah)) {
                                rw = std::max(8.0f, aw);
                                rh = std::max(8.0f, ah);
                                float ox = 0.0f, oy = 0.0f;
                                if (impl_->engine->GetCanvasElementOffsetParentOrigin(sel->id, ox, oy)) {
                                    rx = ax - ox;
                                    ry = ay - oy;
                                } else {
                                    rx = ax;
                                    ry = ay;
                                }
                            }
                            impl_->orig_x = rx;
                            impl_->orig_y = ry;
                            impl_->orig_w = rw;
                            impl_->orig_h = rh;
                            SetCapture(impl_->hwnd);
                            return;
                        }
                    }
                }
            }

            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int hit = impl_->engine->PickNodeAtCanvasPoint(cx, cy);
            if (hit <= 0) hit = impl_->doc->HitTest(cx, cy);
            if (hit > 0) {
                if (ctrl) {
                    impl_->SelectNodeWithMods(hit, true, false);
                } else {
                    if (!impl_->IsSelected(hit)) {
                        impl_->SetSingleSelection(hit);
                        impl_->SyncUIFromDoc(true);
                    }
                }
                impl_->drag_multi_orig.clear();
                for (int sid : impl_->selected_ids) {
                    if (sid == impl_->doc->root_id) continue;
                    if (const auto* sn = impl_->doc->FindNode(sid); sn && !sn->locked) {
                        float rx = sn->x, ry = sn->y, rw = sn->width, rh = sn->height;
                        float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
                        if (impl_->engine->GetCanvasElementRect(sn->id, ax, ay, aw, ah)) {
                            rw = std::max(8.0f, aw);
                            rh = std::max(8.0f, ah);
                            float ox = 0.0f, oy = 0.0f;
                            if (impl_->engine->GetCanvasElementOffsetParentOrigin(sn->id, ox, oy)) {
                                rx = ax - ox;
                                ry = ay - oy;
                            } else {
                                rx = ax;
                                ry = ay;
                            }
                        }
                        impl_->drag_multi_orig.push_back({sid, rx, ry, rw, rh});
                    }
                }
                if (!ctrl && !impl_->drag_multi_orig.empty()) {
                    impl_->drag_pending = true;
                    impl_->drag_did_move = false;
                    impl_->drag_down_mx = x;
                    impl_->drag_down_my = y;
                    impl_->drag_start_sx = cx;
                    impl_->drag_start_sy = cy;
                    SetCapture(impl_->hwnd);
                }
            } else if (!ctrl) {
                impl_->SetSingleSelection(impl_->doc->root_id);
                impl_->SyncUIFromDoc(true);
            }
            return;
        }
    }

    if (auto* ectx = impl_->engine->EditorContext()) {
        int mods = GetRmlKeyModifiers();
        if (down) {
            ectx->ProcessMouseButtonDown(button, mods);
            if (button == 0) {
                Rml::Element* focus = ectx->GetFocusElement();
                Rml::Element* hover = ectx->GetHoverElement();
                for (Rml::Element* cur = hover; cur; cur = cur->GetParentNode()) {
                    if (cur->IsClassSet("input-unit")) {
                        for (int i = 0; i < cur->GetNumChildren(); ++i) {
                            if (auto* ch = cur->GetChild(i)) {
                                if (ch->GetTagName() == "input") {
                                    if (focus != ch) ch->Focus();
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    if (cur->GetId() == "studio-inspector-panel") break;
                }
            }
        } else {
            ectx->ProcessMouseButtonUp(button, mods);
        }
    }
}

static bool IsNumericInspectorInputId(const std::string& eid) {
    return eid.rfind("prop-", 0) == 0 &&
           eid != "prop-id" && eid != "prop-class" &&
           eid != "prop-bg" && eid != "prop-border" && eid != "prop-fg" &&
           eid != "prop-grid-color" &&
           eid != "prop-text" && eid != "prop-src" && eid != "prop-visible";
}

void StudioUI::OnMouseWheel(float delta) {
    if (!impl_->engine) return;
    if (impl_->IsPointInCanvasViewport(impl_->mouse_x, impl_->mouse_y)) {
        if (impl_->play_mode) {
            impl_->engine->CanvasMouseWheel(delta);
            return;
        }
        float old_zoom = impl_->zoom;
        float factor = (delta > 0.0f) ? 1.1f : (1.0f / 1.1f);
        impl_->zoom = std::clamp(impl_->zoom * factor, 0.1f, 4.0f);
        float mx = (float)impl_->mouse_x;
        float my = (float)impl_->mouse_y;
        impl_->pan_x = mx - (mx - impl_->pan_x) * (impl_->zoom / old_zoom);
        impl_->pan_y = my - (my - impl_->pan_y) * (impl_->zoom / old_zoom);
        impl_->UpdateZoomLabel();
        impl_->SyncRulers();
        return;
    }

    if (auto* ectx = impl_->engine->EditorContext()) {
        // Scroll adjusts a numeric counter ONLY after 1 click (when focused) AND while hovering it
        Rml::Element* focus = ectx->GetFocusElement();
        std::string focused_num_id;
        if (focus && IsNumericInspectorInputId(focus->GetId())) {
            focused_num_id = focus->GetId();
        }
        if (!focused_num_id.empty()) {
            auto pt_in_el = [&](Rml::Element* el) -> bool {
                if (!el || el->IsClassSet("hidden")) return false;
                Rml::Vector2f off = el->GetAbsoluteOffset(Rml::BoxArea::Border);
                Rml::Vector2f sz = el->GetBox().GetSize(Rml::BoxArea::Border);
                return sz.x > 0.0f && sz.y > 0.0f &&
                       impl_->mouse_x >= (int)std::floor(off.x) && impl_->mouse_x <= (int)std::ceil(off.x + sz.x) &&
                       impl_->mouse_y >= (int)std::floor(off.y) && impl_->mouse_y <= (int)std::ceil(off.y + sz.y);
            };
            bool over_focused = pt_in_el(focus);
            if (!over_focused) {
                if (auto* par = focus->GetParentNode()) {
                    if (par->IsClassSet("input-unit")) over_focused = pt_in_el(par);
                }
            }
            if (over_focused) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (impl_->StepNumericPropByWheel(focused_num_id, delta, shift)) {
                    return;
                }
            }
        }
        ectx->ProcessMouseWheel(Rml::Vector2f(0.0f, -delta), GetRmlKeyModifiers());
        impl_->SyncCodeGutter(false);
    }
}

void StudioUI::OnKey(UINT vk, bool down, WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    (void)lparam;
    if (vk == VK_SPACE) impl_->space_down = down;

    if (down && vk == VK_ESCAPE && impl_->cp_open) {
        impl_->CloseColorPicker(true);
        return;
    }

    bool editor_input_focused = false;
    bool code_editor_focused = false;
    if (auto* ectx = impl_->engine ? impl_->engine->EditorContext() : nullptr) {
        if (auto* focus = ectx->GetFocusElement()) {
            const Rml::String& tag = focus->GetTagName();
            if (tag == "input" || tag == "textarea") {
                editor_input_focused = true;
                if (focus->GetId() == "code-editor") code_editor_focused = true;
            }
        }
    }

    if (down && code_editor_focused) {
        if (auto* ectx = impl_->engine ? impl_->engine->EditorContext() : nullptr) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            auto* ta = impl_->ui_doc ? dynamic_cast<Rml::ElementFormControlTextArea*>(impl_->ui_doc->GetElementById("code-editor")) : nullptr;

            if (vk == VK_RETURN) {
                std::string indent;
                bool extra_indent = false;
                if (ta) {
                    int sel_s = 0, sel_e = 0;
                    ta->GetSelection(&sel_s, &sel_e, nullptr);
                    std::string val = ta->GetValue();
                    size_t cur_byte = Impl::Utf8CharToByteOffset(val, sel_s);
                    size_t line_start = val.rfind('\n', cur_byte > 0 ? cur_byte - 1 : 0);
                    line_start = (line_start == std::string::npos || cur_byte == 0) ? 0 : (line_start + 1);
                    size_t p = line_start;
                    while (p < cur_byte && p < val.size() && val[p] == ' ') {
                        indent.push_back(' ');
                        ++p;
                    }
                    size_t r = cur_byte;
                    while (r > line_start && (val[r - 1] == ' ' || val[r - 1] == '\t')) --r;
                    if (r > line_start) {
                        if (val[r - 1] == '{') {
                            extra_indent = true;
                        } else if (val[r - 1] == '>' && !(r >= 2 && val[r - 2] == '/')) {
                            size_t lt = val.rfind('<', r - 1);
                            if (lt != std::string::npos && lt + 1 < r && val[lt + 1] != '/' && val[lt + 1] != '!') {
                                extra_indent = true;
                            }
                        }
                    }
                }
                if (extra_indent) indent += "  ";
                ectx->ProcessTextInput(Rml::String("\n") + indent);
                impl_->SyncCodeGutter(true);
                return;
            }
            if (vk == VK_TAB) {
                if (shift && ta) {
                    int sel_s = 0, sel_e = 0;
                    ta->GetSelection(&sel_s, &sel_e, nullptr);
                    std::string val = ta->GetValue();
                    size_t cur_byte = Impl::Utf8CharToByteOffset(val, sel_s);
                    size_t line_start = (cur_byte == 0) ? 0 : val.rfind('\n', cur_byte - 1);
                    line_start = (line_start == std::string::npos) ? 0 : (line_start + 1);
                    int remove_n = 0;
                    while (remove_n < 2 && line_start + remove_n < val.size() && val[line_start + remove_n] == ' ') {
                        ++remove_n;
                    }
                    if (remove_n > 0) {
                        val.erase(line_start, (size_t)remove_n);
                        impl_->syncing_ui = true;
                        ta->SetValue(val);
                        int new_pos = std::max(0, sel_s - remove_n);
                        ta->SetSelectionRange(new_pos, new_pos);
                        impl_->syncing_ui = false;
                        impl_->ApplyFieldChange("code-editor");
                        impl_->SyncCodeGutter(true);
                    }
                    return;
                }
                ectx->ProcessTextInput(Rml::String("  "));
                impl_->SyncCodeGutter(false);
                return;
            }
            if (ctrl && vk == VK_OEM_2 && ta) {
                int sel_s = 0, sel_e = 0;
                ta->GetSelection(&sel_s, &sel_e, nullptr);
                std::string val = ta->GetValue();
                size_t cur_byte = Impl::Utf8CharToByteOffset(val, sel_s);
                size_t line_start = (cur_byte == 0) ? 0 : val.rfind('\n', cur_byte - 1);
                line_start = (line_start == std::string::npos) ? 0 : (line_start + 1);
                size_t line_end = val.find('\n', line_start);
                if (line_end == std::string::npos) line_end = val.size();
                std::string line = val.substr(line_start, line_end - line_start);
                size_t first_ns = line.find_first_not_of(" \t");
                if (first_ns != std::string::npos) {
                    std::string open_c = (impl_->code_tab == "RCSS") ? "/* " : "<!-- ";
                    std::string close_c = (impl_->code_tab == "RCSS") ? " */" : " -->";
                    if (line.compare(first_ns, open_c.size(), open_c) == 0 &&
                        line.size() >= first_ns + open_c.size() + close_c.size() &&
                        line.compare(line.size() - close_c.size(), close_c.size(), close_c) == 0) {
                        std::string uncommented = line.substr(0, first_ns) +
                            line.substr(first_ns + open_c.size(), line.size() - first_ns - open_c.size() - close_c.size());
                        val.replace(line_start, line.size(), uncommented);
                    } else {
                        std::string commented = line.substr(0, first_ns) + open_c + line.substr(first_ns) + close_c;
                        val.replace(line_start, line.size(), commented);
                    }
                    impl_->syncing_ui = true;
                    ta->SetValue(val);
                    ta->SetSelectionRange(sel_s, sel_s);
                    impl_->syncing_ui = false;
                    impl_->ApplyFieldChange("code-editor");
                    impl_->SyncCodeGutter(true);
                }
                return;
            }
            if (ctrl && vk == 'Z') {
                impl_->Undo();
                if (impl_->ui_doc) {
                    if (auto* ce = impl_->ui_doc->GetElementById("code-editor")) ce->Focus();
                }
                return;
            }
            if (ctrl && vk == 'Y') {
                impl_->Redo();
                if (impl_->ui_doc) {
                    if (auto* ce = impl_->ui_doc->GetElementById("code-editor")) ce->Focus();
                }
                return;
            }
        }
    }

    if (down && !editor_input_focused && !impl_->play_mode) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && vk == 'Z') { impl_->Undo(); return; }
        if (ctrl && vk == 'Y') { impl_->Redo(); return; }
        if (ctrl && vk == 'D') { impl_->DuplicateSpecificNode(impl_->PrimaryId()); return; }
        if (vk == 'V') { impl_->hand_tool = false; impl_->UpdateHeaderButtonsUI(); return; }
        if (vk == 'H') { impl_->hand_tool = true; impl_->UpdateHeaderButtonsUI(); return; }
        if (vk == VK_DELETE && impl_->doc) {
            impl_->DeleteSpecificNode(impl_->PrimaryId());
            return;
        }
    }

    if (auto* ectx = impl_->engine ? impl_->engine->EditorContext() : nullptr) {
        auto key = Win32VkToRmlKey(vk);
        if (key != Rml::Input::KI_UNKNOWN) {
            int mods = GetRmlKeyModifiers();
            if (down) ectx->ProcessKeyDown(key, mods);
            else ectx->ProcessKeyUp(key, mods);
        }
    }
}

void StudioUI::OnChar(uint32_t codepoint) {
    if (codepoint < 32 || !impl_->engine) return;
    if (impl_->play_mode && impl_->IsPointInCanvasViewport(impl_->mouse_x, impl_->mouse_y)) {
        impl_->engine->CanvasTextInput(codepoint);
        return;
    }
    if (auto* ectx = impl_->engine->EditorContext()) {
        ectx->ProcessTextInput((Rml::Character)codepoint);
    }
}

void StudioUI::UpdateAndRender(Dx11Renderer2D& dx, ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height) {
    if (!impl_->doc || !impl_->engine) return;

    impl_->DrainPendingCommands();
    impl_->UpdateToastTimer();

    if (impl_->drag_mode == Impl::DragMode::None) {
        impl_->SyncUIFromDoc(false);
    } else if (impl_->drag_transform_ui_dirty) {
        impl_->SyncTransformFieldsOnly();
        impl_->drag_transform_ui_dirty = false;
    }
    impl_->SyncCodeGutter(false);

    if (impl_->play_mode) {
        impl_->engine->RenderCanvasInteractive(device, ctx);
    } else if (impl_->drag_mode != Impl::DragMode::None && !impl_->drag_dirty_node_ids.empty()) {
        bool fallback_full = false;
        for (int nid : impl_->drag_dirty_node_ids) {
            if (const auto* dn = impl_->doc->FindNode(nid)) {
                const auto* dp = impl_->doc->FindNode(dn->parent_id);
                if (!dn->children.empty() || dn->size_mode_w != 0 || dn->size_mode_h != 0 ||
                    dn->anchor_h != 0 || dn->anchor_v != 0 || (dp && dp->display_flex)) {
                    fallback_full = true;
                    break;
                }
                if (!impl_->engine->SetCanvasNodeBoxProps(
                        dn->id, dn->x, dn->y, dn->width, dn->height,
                        impl_->unit_mode == "%",
                        (float)impl_->doc->screen_width, (float)impl_->doc->screen_height)) {
                    fallback_full = true;
                    break;
                }
            }
        }
        if (fallback_full) {
            impl_->engine->SyncAndRenderCanvas(*impl_->doc, device, ctx);
        } else {
            impl_->engine->FlushCanvasRender(device, ctx);
        }
        impl_->drag_dirty_node_ids.clear();
    } else {
        impl_->engine->SyncAndRenderCanvas(*impl_->doc, device, ctx);
    }

    impl_->SyncArtboardOverlay();

    dx.BeginFrame(ctx, width, height);

    float cw = (float)impl_->doc->screen_width * impl_->zoom;
    float ch = (float)impl_->doc->screen_height * impl_->zoom;
    float px = impl_->pan_x;
    float py = impl_->pan_y;

    float vp_x0 = impl_->LeftTotal();
    float vp_y0 = impl_->TopTotal();
    float vp_x1 = (float)width - impl_->RightTotal();
    float vp_y1 = (float)height - impl_->BottomTotal();

    dx.SetScissor(ctx, (int)vp_x0, (int)vp_y0, (int)(vp_x1 - vp_x0), (int)(vp_y1 - vp_y0));

    // Canvas board background (Escuro / Branco / Cinza / Custom)
    RmlColor brd_col{14, 17, 23, 255};
    if (impl_->bg_style == Impl::BgStyle::White) brd_col = {255, 255, 255, 255};
    else if (impl_->bg_style == Impl::BgStyle::Gray) brd_col = {203, 213, 225, 255};
    else if (impl_->bg_style == Impl::BgStyle::Custom) brd_col = impl_->canvas_bg_color;
    else brd_col = {14, 17, 23, 255};

    dx.DrawRectFilled(ctx, vp_x0, vp_y0, vp_x1 - vp_x0, vp_y1 - vp_y0, brd_col.r, brd_col.g, brd_col.b, 255);

    // Artboard soft drop shadow
    dx.DrawRectFilled(ctx, px - 6.0f, py + 4.0f, cw + 12.0f, ch + 14.0f, 0, 0, 0, 55);
    dx.DrawRectFilled(ctx, px - 2.0f, py + 2.0f, cw + 4.0f, ch + 6.0f, 0, 0, 0, 75);

    // Artboard base backdrop inside bounds
    if (impl_->bg_style == Impl::BgStyle::Gta) {
        dx.DrawRectFilled(ctx, px, py, cw, ch * 0.58f, 68, 138, 204, 255);
        dx.DrawRectFilled(ctx, px, py + ch * 0.58f, cw, ch * 0.42f, 44, 52, 62, 255);
        dx.DrawRectFilled(ctx, px + cw * 0.10f, py + ch * 0.36f, cw * 0.08f, ch * 0.22f, 52, 68, 88, 255);
        dx.DrawRectFilled(ctx, px + cw * 0.22f, py + ch * 0.28f, cw * 0.07f, ch * 0.30f, 46, 62, 82, 255);
        dx.DrawRectFilled(ctx, px + cw * 0.70f, py + ch * 0.32f, cw * 0.11f, ch * 0.26f, 50, 66, 86, 255);
    } else {
        dx.DrawRectFilled(ctx, px, py, cw, ch, brd_col.r, brd_col.g, brd_col.b, 255);
        if (const auto* root_node = impl_->doc->FindNode(impl_->doc->root_id)) {
            if (!impl_->doc->has_imported_source && root_node->has_bg) {
                dx.DrawRectFilled(ctx, px, py, cw, ch, root_node->bg_color.r, root_node->bg_color.g, root_node->bg_color.b, root_node->bg_color.a);
            }
        }
    }

    if (auto* canvas_srv = impl_->engine->CanvasSRV()) {
        dx.DrawTexturedQuad(ctx, canvas_srv, px, py, cw, ch);
    }

    // Artboard 32px blueprint grid (adapts dark/light contrast)
    if (impl_->show_grid) {
        float lum = 0.299f * (float)brd_col.r + 0.587f * (float)brd_col.g + 0.114f * (float)brd_col.b;
        uint8_t gr = (lum > 140.0f) ? 0 : 255;
        uint8_t ga = (lum > 140.0f) ? 24 : 11;
        float step = std::max(14.0f, 32.0f * (impl_->zoom < 0.7f ? 1.0f : impl_->zoom));
        for (float gx = px + step; gx < px + cw; gx += step) {
            dx.DrawRectFilled(ctx, gx, py, 1.0f, ch, gr, gr, gr, ga);
        }
        for (float gy = py + step; gy < py + ch; gy += step) {
            dx.DrawRectFilled(ctx, px, gy, cw, 1.0f, gr, gr, gr, ga);
        }
    }

    // Per-container Layout Grids (Columns / Rows / Uniform Grid)
    if (!impl_->play_mode) {
        for (const auto& gn : impl_->doc->nodes) {
            if (!gn.visible || !gn.grid_enabled) continue;
            float nx = px, ny = py, nw = cw, nh = ch;
            if (gn.id != impl_->doc->root_id) {
                float wx = 0.0f, wy = 0.0f, ww = gn.width, wh = gn.height;
                impl_->GetNodeCanvasBounds(gn.id, wx, wy, ww, wh);
                nx = px + wx * impl_->zoom;
                ny = py + wy * impl_->zoom;
                nw = ww * impl_->zoom;
                nh = wh * impl_->zoom;
            }
            if (nw < 8.0f || nh < 8.0f) continue;

            uint8_t cr = gn.grid_color.r, cg = gn.grid_color.g, cb = gn.grid_color.b;
            uint8_t ca = std::max<uint8_t>(gn.grid_color.a, 18);
            uint8_t edge_a = (uint8_t)std::min(200, (int)ca * 2);
            if (gn.grid_type == 0) {
                int cnt = std::clamp(gn.grid_count, 1, 48);
                float margin = std::max(0.0f, gn.grid_margin) * impl_->zoom;
                float gutter = std::max(0.0f, gn.grid_gutter) * impl_->zoom;
                float avail = nw - margin * 2.0f - gutter * (float)(cnt - 1);
                if (avail > 2.0f) {
                    float col_w = avail / (float)cnt;
                    for (int i = 0; i < cnt; ++i) {
                        float x0 = nx + margin + (float)i * (col_w + gutter);
                        dx.DrawRectFilled(ctx, x0, ny, col_w, nh, cr, cg, cb, ca);
                        if (col_w >= 4.0f) {
                            dx.DrawRectFilled(ctx, x0, ny, 1.0f, nh, cr, cg, cb, edge_a);
                            dx.DrawRectFilled(ctx, x0 + col_w - 1.0f, ny, 1.0f, nh, cr, cg, cb, edge_a);
                        }
                    }
                }
            } else if (gn.grid_type == 1) {
                int cnt = std::clamp(gn.grid_count, 1, 48);
                float margin = std::max(0.0f, gn.grid_margin) * impl_->zoom;
                float gutter = std::max(0.0f, gn.grid_gutter) * impl_->zoom;
                float avail = nh - margin * 2.0f - gutter * (float)(cnt - 1);
                if (avail > 2.0f) {
                    float row_h = avail / (float)cnt;
                    for (int i = 0; i < cnt; ++i) {
                        float y0 = ny + margin + (float)i * (row_h + gutter);
                        dx.DrawRectFilled(ctx, nx, y0, nw, row_h, cr, cg, cb, ca);
                        if (row_h >= 4.0f) {
                            dx.DrawRectFilled(ctx, nx, y0, nw, 1.0f, cr, cg, cb, edge_a);
                            dx.DrawRectFilled(ctx, nx, y0 + row_h - 1.0f, nw, 1.0f, cr, cg, cb, edge_a);
                        }
                    }
                }
            } else {
                float step = std::max(6.0f, gn.grid_size * impl_->zoom);
                uint8_t line_a = (uint8_t)std::min(220, (int)ca * 2);
                int guard = 0;
                for (float gx = nx + step; gx < nx + nw && guard < 256; gx += step, ++guard) {
                    dx.DrawRectFilled(ctx, gx, ny, 1.0f, nh, cr, cg, cb, line_a);
                }
                guard = 0;
                for (float gy = ny + step; gy < ny + nh && guard < 256; gy += step, ++guard) {
                    dx.DrawRectFilled(ctx, nx, gy, nw, 1.0f, cr, cg, cb, line_a);
                }
            }
        }
    }

    // Artboard border
    if (const auto* root_node = impl_->doc->FindNode(impl_->doc->root_id); root_node && !impl_->doc->has_imported_source) {
        dx.DrawRectOutline(ctx, px, py, cw, ch, 1.0f, root_node->border_color.r, root_node->border_color.g, root_node->border_color.b, root_node->border_color.a);
    } else {
        dx.DrawRectOutline(ctx, px - 1.0f, py - 1.0f, cw + 2.0f, ch + 2.0f, 1.0f, 59, 130, 246, 120);
    }

    // Dashed Safe Area outline
    bool is_empty_artboard = (impl_->doc->nodes.size() <= 1 && !impl_->doc->has_imported_source && !impl_->play_mode);
    if (impl_->safe_area || is_empty_artboard) {
        float inset = is_empty_artboard ? 14.0f : std::min(cw, ch) * 0.04f;
        if (cw > inset * 2.0f + 16.0f && ch > inset * 2.0f + 16.0f) {
            dx.DrawDashedRectOutline(ctx, px + inset, py + inset, cw - inset * 2.0f, ch - inset * 2.0f, 1.0f, 5.0f, 4.0f, 71, 120, 245, 75);
        }
    }

    // Selection gizmo & corner handles (#4778F5 outline + #A8BEFF square handles)
    if (!impl_->play_mode) {
        int primary = impl_->PrimaryId();
        for (int sid : impl_->selected_ids) {
            if (const auto* sel = impl_->doc->FindNode(sid)) {
                if (sel->id == impl_->doc->root_id) {
                    // Artboard subtle selection corners like .selection-outline:before/:after
                    dx.DrawRectOutline(ctx, px, py, cw, ch, 1.0f, 71, 120, 245, 110);
                    dx.DrawRectFilled(ctx, px - 3.0f, py - 3.0f, 6.0f, 6.0f, 71, 120, 245, 255);
                    dx.DrawRectOutline(ctx, px - 3.0f, py - 3.0f, 6.0f, 6.0f, 1.0f, 168, 190, 255, 255);
                    dx.DrawRectFilled(ctx, px + cw - 3.0f, py + ch - 3.0f, 6.0f, 6.0f, 71, 120, 245, 255);
                    dx.DrawRectOutline(ctx, px + cw - 3.0f, py + ch - 3.0f, 6.0f, 6.0f, 1.0f, 168, 190, 255, 255);
                    continue;
                }
                float wx = 0.0f, wy = 0.0f, ww = sel->width, wh = sel->height;
                if (!impl_->engine->GetCanvasElementRect(sel->id, wx, wy, ww, wh)) {
                    impl_->doc->GetWorldPos(sel->id, wx, wy);
                    ww = sel->width;
                    wh = sel->height;
                }
                float sx = px + wx * impl_->zoom;
                float sy = py + wy * impl_->zoom;
                float sw = ww * impl_->zoom;
                float sh = wh * impl_->zoom;

                dx.DrawRectOutline(ctx, sx, sy, sw, sh, (sid == primary) ? 1.5f : 1.0f, 71, 120, 245, 255);
                if (impl_->selected_ids.size() == 1 && !sel->locked) {
                    float xm = sx + sw * 0.5f;
                    float ym = sy + sh * 0.5f;
                    float pts[8][2] = {
                        {sx, sy}, {xm, sy}, {sx + sw, sy}, {sx + sw, ym},
                        {sx + sw, sy + sh}, {xm, sy + sh}, {sx, sy + sh}, {sx, ym}
                    };
                    for (int i = 0; i < 8; ++i) {
                        dx.DrawRectFilled(ctx, pts[i][0] - 4.0f, pts[i][1] - 4.0f, 8.0f, 8.0f, 168, 190, 255, 255);
                        dx.DrawRectOutline(ctx, pts[i][0] - 4.0f, pts[i][1] - 4.0f, 8.0f, 8.0f, 1.2f, 71, 120, 245, 255);
                    }
                }
            }
        }

        // Smart Guides overlay (magenta alignment lines & endpoint markers)
        if (!impl_->active_guides.empty()) {
            for (const auto& g : impl_->active_guides) {
                if (g.vertical) {
                    float gx = px + g.coord * impl_->zoom;
                    float gy0 = py + g.span_min * impl_->zoom - 6.0f;
                    float gy1 = py + g.span_max * impl_->zoom + 6.0f;
                    dx.DrawRectFilled(ctx, gx, gy0, 1.0f, std::max(2.0f, gy1 - gy0), 255, 46, 147, 235);
                    dx.DrawRectFilled(ctx, gx - 2.5f, gy0, 6.0f, 1.0f, 255, 46, 147, 235);
                    dx.DrawRectFilled(ctx, gx - 2.5f, gy1 - 1.0f, 6.0f, 1.0f, 255, 46, 147, 235);
                } else {
                    float gy = py + g.coord * impl_->zoom;
                    float gx0 = px + g.span_min * impl_->zoom - 6.0f;
                    float gx1 = px + g.span_max * impl_->zoom + 6.0f;
                    dx.DrawRectFilled(ctx, gx0, gy, std::max(2.0f, gx1 - gx0), 1.0f, 255, 46, 147, 235);
                    dx.DrawRectFilled(ctx, gx0, gy - 2.5f, 1.0f, 6.0f, 255, 46, 147, 235);
                    dx.DrawRectFilled(ctx, gx1 - 1.0f, gy - 2.5f, 1.0f, 6.0f, 255, 46, 147, 235);
                }
            }
        }
    }

    dx.ClearScissor(ctx);

    impl_->engine->RenderEditor(device, ctx);
}

void StudioUI::SetStatus(const std::string& msg) {
    if (impl_) impl_->ShowToast(msg);
}

bool StudioUI::ImportRcssPath(const std::string& path) {
    return impl_ ? impl_->ExecuteImportRcss(path) : false;
}
