#pragma once

#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;

namespace Platform {

struct FileFilter {
    std::string label;   // e.g. "RmlUi Files (*.rml)"
    std::string pattern; // e.g. "*.rml" or "*.png;*.jpg;*.jpeg;*.bmp"
};

std::string OpenFileDialog(GLFWwindow* owner, const std::string& title, const std::vector<FileFilter>& filters);
std::string SaveFileDialog(GLFWwindow* owner, const std::string& title, const std::string& default_ext, const std::vector<FileFilter>& filters);
void CopyToClipboard(GLFWwindow* window, const std::string& text);
void ApplyDarkTitlebar(GLFWwindow* window);

} // namespace Platform
