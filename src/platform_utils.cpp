#include "platform_utils.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#endif

#include <GLFW/glfw3.h>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#pragma comment(lib, "dwmapi.lib")

namespace {

std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string Utf16ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring BuildWinFilter(const std::vector<Platform::FileFilter>& filters) {
    std::wstring out;
    for (const auto& f : filters) {
        out += Utf8ToUtf16(f.label);
        out.push_back(L'\0');
        out += Utf8ToUtf16(f.pattern);
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

} // namespace

namespace Platform {

std::string OpenFileDialog(GLFWwindow* owner, const std::string& title, const std::vector<FileFilter>& filters) {
    wchar_t buf[MAX_PATH * 4]{};
    std::wstring wfilter = BuildWinFilter(filters);
    std::wstring wtitle = Utf8ToUtf16(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return Utf16ToUtf8(buf);
    return {};
}

std::string SaveFileDialog(GLFWwindow* owner, const std::string& title, const std::string& default_ext, const std::vector<FileFilter>& filters) {
    wchar_t buf[MAX_PATH * 4]{};
    std::wstring wfilter = BuildWinFilter(filters);
    std::wstring wtitle = Utf8ToUtf16(title);
    std::wstring wext = Utf8ToUtf16(default_ext);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = wext.empty() ? nullptr : wext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) return Utf16ToUtf8(buf);
    return {};
}

void ApplyDarkTitlebar(GLFWwindow* window) {
    if (!window) return;
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF cap_col = RGB(16, 19, 26);
    DwmSetWindowAttribute(hwnd, 35, &cap_col, sizeof(cap_col)); // DWMWA_CAPTION_COLOR
}

} // namespace Platform

#else // POSIX / Linux

#include <cstdlib>

#if defined(__linux__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/Xrandr.h>)
#define STUDIO_HAVE_GLFW_X11 1
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#endif
#endif

namespace {

unsigned long GetX11WindowId(GLFWwindow* owner) {
#if defined(STUDIO_HAVE_GLFW_X11)
    if (!owner) return 0;
#ifdef GLFW_PLATFORM_X11
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) return 0;
#endif
    return static_cast<unsigned long>(glfwGetX11Window(owner));
#else
    (void)owner;
    return 0;
#endif
}

std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string RunCommandOutput(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += buf;
    }
    int rc = pclose(pipe);
    if (rc != 0) return {};
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool HasCommand(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string BuildZenityFilters(const std::vector<Platform::FileFilter>& filters) {
    std::ostringstream oss;
    for (const auto& f : filters) {
        std::string pat = f.pattern;
        for (char& c : pat) {
            if (c == ';') c = ' ';
        }
        oss << " --file-filter=" << ShellQuote(f.label + " | " + pat);
    }
    return oss.str();
}

std::string BuildKdialogFilter(const std::vector<Platform::FileFilter>& filters) {
    std::ostringstream oss;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i > 0) oss << "\n";
        std::string pat = filters[i].pattern;
        for (char& c : pat) {
            if (c == ';') c = ' ';
        }
        oss << filters[i].label << " (" << pat << ")";
    }
    return oss.str();
}

} // namespace

namespace Platform {

std::string OpenFileDialog(GLFWwindow* owner, const std::string& title, const std::vector<FileFilter>& filters) {
    unsigned long xwin = GetX11WindowId(owner);
    if (HasCommand("zenity")) {
        std::string cmd;
        if (xwin != 0) cmd += "WINDOWID=" + std::to_string(xwin) + " ";
        cmd += "zenity --file-selection --title=" + ShellQuote(title) + BuildZenityFilters(filters) + " 2>/dev/null";
        return RunCommandOutput(cmd);
    }
    if (HasCommand("kdialog")) {
        std::string cmd = "kdialog --getopenfilename . " + ShellQuote(BuildKdialogFilter(filters)) + " --title " + ShellQuote(title);
        if (xwin != 0) cmd += " --attach " + std::to_string(xwin);
        cmd += " 2>/dev/null";
        return RunCommandOutput(cmd);
    }
    return {};
}

std::string SaveFileDialog(GLFWwindow* owner, const std::string& title, const std::string& default_ext, const std::vector<FileFilter>& filters) {
    unsigned long xwin = GetX11WindowId(owner);
    std::string result;
    if (HasCommand("zenity")) {
        std::string cmd;
        if (xwin != 0) cmd += "WINDOWID=" + std::to_string(xwin) + " ";
        cmd += "zenity --file-selection --save --confirm-overwrite --title=" + ShellQuote(title) + BuildZenityFilters(filters) + " 2>/dev/null";
        result = RunCommandOutput(cmd);
    } else if (HasCommand("kdialog")) {
        std::string cmd = "kdialog --getsavefilename . " + ShellQuote(BuildKdialogFilter(filters)) + " --title " + ShellQuote(title);
        if (xwin != 0) cmd += " --attach " + std::to_string(xwin);
        cmd += " 2>/dev/null";
        result = RunCommandOutput(cmd);
    }
    if (!result.empty() && !default_ext.empty()) {
        std::string ext = "." + default_ext;
        if (result.size() < ext.size() || result.compare(result.size() - ext.size(), ext.size(), ext) != 0) {
            // Append default extension only if no extension was provided in the filename
            auto slash = result.find_last_of('/');
            auto dot = result.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
                result += ext;
            }
        }
    }
    return result;
}

void ApplyDarkTitlebar(GLFWwindow* /*window*/) {
}

} // namespace Platform

#endif

namespace Platform {

void CopyToClipboard(GLFWwindow* window, const std::string& text) {
    if (window) {
        glfwSetClipboardString(window, text.c_str());
    }
}

} // namespace Platform
