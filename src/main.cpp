#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdlib>
#endif

#include "gl_renderer.h"
#include "platform_utils.h"
#include "rml_doc.h"
#include "rml_engine.h"
#include "studio_ui.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>

namespace {

struct AppState {
    GLFWwindow* window = nullptr;
    StudioUI* ui = nullptr;
    bool fullscreen = false;
    int saved_x = 80;
    int saved_y = 60;
    int saved_w = 1440;
    int saved_h = 860;

    void ToggleFullscreen() {
        if (!window) return;
        fullscreen = !fullscreen;
        if (fullscreen) {
            glfwGetWindowPos(window, &saved_x, &saved_y);
            glfwGetWindowSize(window, &saved_w, &saved_h);
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
            if (mon && mode) {
                glfwSetWindowMonitor(window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
            }
        } else {
            glfwSetWindowMonitor(window, nullptr, saved_x, saved_y, std::max(640, saved_w), std::max(480, saved_h), 0);
        }
    }

    void ToFramebufferCoords(double wx, double wy, int& out_x, int& out_y) const {
        int win_w = 1, win_h = 1, fb_w = 1, fb_h = 1;
        glfwGetWindowSize(window, &win_w, &win_h);
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        double sx = (win_w > 0) ? (static_cast<double>(fb_w) / static_cast<double>(win_w)) : 1.0;
        double sy = (win_h > 0) ? (static_cast<double>(fb_h) / static_cast<double>(win_h)) : 1.0;
        out_x = static_cast<int>(wx * sx);
        out_y = static_cast<int>(wy * sy);
    }
};

} // namespace

int main(int, char**) {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW.\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1440, 860, "RmlUI Studio", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window (OpenGL 3.3 Core).\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    Platform::ApplyDarkTitlebar(window);

    GlRenderer2D gl_renderer;
    if (!gl_renderer.Init()) {
        std::fprintf(stderr, "Failed to initialize OpenGL 3.3 renderer.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    RmlDocument doc;
    doc.ResetDefaultScreen();

    RmlEngineHost engine;
    if (!engine.Init(window)) {
        std::fprintf(stderr, "Failed to initialize RmlUi engine.\n");
        gl_renderer.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    StudioUI ui;
    if (!ui.Init(window, doc, engine)) {
        std::fprintf(stderr, "Failed to initialize Studio UI.\n");
        engine.Shutdown();
        gl_renderer.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    AppState app;
    app.window = window;
    app.ui = &ui;
    ui.SetToggleFullscreenCallback([&app]() { app.ToggleFullscreen(); });

    glfwSetWindowUserPointer(window, &app);

    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (st && st->ui && width > 0 && height > 0) {
            st->ui->OnResize(width, height);
        }
    });

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double xpos, double ypos) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (st && st->ui) {
            int fx = 0, fy = 0;
            st->ToFramebufferCoords(xpos, ypos, fx, fy);
            st->ui->OnMouseMove(fx, fy);
        }
    });

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int /*mods*/) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (!st || !st->ui) return;
        int rml_btn = -1;
        if (button == GLFW_MOUSE_BUTTON_LEFT) rml_btn = 0;
        else if (button == GLFW_MOUSE_BUTTON_RIGHT) rml_btn = 1;
        else if (button == GLFW_MOUSE_BUTTON_MIDDLE) rml_btn = 2;
        if (rml_btn >= 0) {
            st->ui->OnMouseButton(rml_btn, action == GLFW_PRESS);
        }
    });

    glfwSetScrollCallback(window, [](GLFWwindow* w, double /*xoffset*/, double yoffset) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (st && st->ui) {
            st->ui->OnMouseWheel(static_cast<float>(yoffset));
        }
    });

    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int /*scancode*/, int action, int mods) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (!st || !st->ui) return;
        if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
            st->ToggleFullscreen();
            return;
        }
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            st->ui->OnKey(key, true, mods);
        } else if (action == GLFW_RELEASE) {
            st->ui->OnKey(key, false, mods);
        }
    });

    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int codepoint) {
        auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        if (st && st->ui) {
            st->ui->OnChar(static_cast<uint32_t>(codepoint));
        }
    });

    int init_w = 1440, init_h = 860;
    glfwGetFramebufferSize(window, &init_w, &init_h);
    if (init_w > 0 && init_h > 0) {
        ui.OnResize(init_w, init_h);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0) {
            ui.UpdateAndRender(gl_renderer, fb_w, fb_h);
            glfwSwapBuffers(window);
        } else {
            glfwWaitEventsTimeout(0.05);
        }
    }

    ui.Shutdown();
    engine.Shutdown();
    gl_renderer.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
