# RmlUI Studio

> 🇺🇸 **English** | 🇧🇷 [Português (Brasil)](README.pt-BR.md)

Visual UI editor for **[RmlUi](https://github.com/mikke89/RmlUi)**, built with **C++17**, **RmlUi**, **FreeType**, **GLFW**, and **OpenGL 3.3 Core** for **Windows** and **Linux**.

![RmlUI Studio Demo](assets/preview.png)

---

## How It Works

**RmlUI Studio** uses **RmlUi** itself both to render the user's document and to draw the editor's own interface on top of a cross-platform **OpenGL 3.3 Core + GLFW** backend for **Windows** and **Linux**:

### 1. Dual-Context Architecture (`RmlEngineHost` — `src/rml_engine.h`)
- **Canvas Context (`CanvasContext`):** Renders the user's `.rml`/`.rcss` document into an offscreen GPU texture (OpenGL FBO), enabling zoom, pan, direct element selection (real RmlUi DOM *hit-testing*), visual manipulation, and live interaction.
- **Editor Context (`EditorContext`):** Renders the entire studio interface (panels, layer tree, property inspector, and toolbar) directly to the *backbuffer* via RmlUi, with no ImGui dependency.

### 2. Document Model & RML/RCSS Generation (`RmlDocument` — `src/rml_doc.h`)
- **Node Tree (`RmlNode`):** Represents RmlUi elements (`div`, `button`, `input`, `span`, `img`, `progress`) with properties for:
  - **Geometry & Layout:** position, dimensions, *Flexbox* (`display: flex`, `flex-direction`, `justify-content`, `align-items`, `flex-wrap`, `gap`, `padding`), sizing modes (`Fixed`, `Hug`, `Fill`), anchors/constraints, and visual *Layout Grids* (columns, rows, or uniform grid).
  - **Appearance & Typography:** background color, borders, `border-radius`, opacity, text color/size/weight/alignment, and `:hover` states.
  - **Assets:** image support (`<img>`) with paths resolved relative to the project base directory.
- **Live Import & Synchronization:**
  - Imports existing `.rml` and `.rcss` files directly through the RmlUi DOM (`ImportRmlFile`, `ImportRcssAndSync`).
  - Applies `.rml` and `.rcss` code edits in real time on the canvas (`ApplyRmlStringAndSync`, `ApplyRcssStringAndSync`), with RCSS sanitization for RmlUi compatibility (`SanitizeRcssForRmlUi`).
  - Exports the project back to `.rml`, `.rcss`, and assets (`GenerateRmlAndRcss`, `ExportToPath`).

### 3. OpenGL 3.3 Rendering & Icons (`GlRenderer2D` / `studio_icons`)
- **`src/gl_renderer.h` / `.cpp`:** Cross-platform OpenGL 3.3 Core 2D renderer responsible for displaying the canvas texture and drawing selection *overlays*, dashed outlines, and visual guides.
- **`src/platform_utils.h` / `.cpp`:** Cross-platform OS utilities (native file open/save dialogs on Windows and Linux via `zenity`/`kdialog`, clipboard via GLFW).
- **`src/studio_icons.h` / `.cpp`:** Generates premultiplied 32×32 RGBA vector icons directly in code, dynamically tinted by RmlUi via `image-color`.

---

## Project Structure

- `CMakeLists.txt` — CMake configuration (C++17, static FreeType, static RmlUi Core, GLFW, OpenGL 3.3, and `RmlUIStudio` executable).
- `src/main.cpp` — Cross-platform GLFW window initialization, OpenGL 3.3 Core context, and main loop.
- `src/rml_doc.h` / `.cpp` — Document model, nodes, constraints, and `.rml`/`.rcss` import/export.
- `src/rml_engine.h` / `.cpp` — RmlUi integration (Canvas and Editor contexts, DOM, `stb_image`, and OpenGL 3.3 offscreen rendering).
- `src/gl_renderer.h` / `.cpp` — OpenGL 3.3 2D renderer and texture management.
- `src/platform_utils.h` / `.cpp` — Cross-platform file dialogs and clipboard helpers.
- `src/studio_ui.h` / `.cpp` — Studio UI logic, canvas interaction, panels, and inspector.
- `src/studio_icons.h` / `.cpp` — Code-generated vector icons.
- `third_party/RmlUi/` — RmlUi library (static subproject + GL3/GLFW backends).
- `third_party/freetype/` — FreeType library (static font engine for RmlUi).
- `third_party/stb/` — `stb_image.h` for cross-platform image decoding.

---

## Building (Windows & Linux)

### Prerequisites
- **CMake** 3.20 or higher
- **C++17 compiler** (MSVC on Windows; GCC 9+ or Clang 10+ on Linux)
- **OpenGL 3.3** compatible drivers
- **Linux additional packages:** X11/Wayland dev headers for GLFW (or `libglfw3-dev`), and `zenity` or `kdialog` at runtime for native file dialogs.

### Build
```sh
git clone --recursive https://github.com/CifraS2/RmlUI-Studio
cd RmlUI-Studio
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

---

## Credits & Libraries

This project uses the following libraries and APIs:

1. **[RmlUi](https://github.com/mikke89/RmlUi) — The RmlUi Team & Contributors**
   - Copyright (c) 2008–2014 CodePoint Ltd, Shift Technology Ltd, and contributors
   - Copyright (c) 2019–2026 The RmlUi Team, and contributors
   - HTML/CSS-like UI engine used for both the canvas and the editor UI (`third_party/RmlUi/`).
   - License: **MIT** ([`third_party/RmlUi/LICENSE.txt`](third_party/RmlUi/LICENSE.txt)).

2. **[FreeType 2](https://freetype.org/) — The FreeType Project**
   - Font rasterization engine used by RmlUi (`third_party/freetype/`).
   - License: **FreeType License (FTL)** or **GPLv2** ([`third_party/freetype/LICENSE.TXT`](third_party/freetype/LICENSE.TXT); used under **FTL**).

3. **[GLFW](https://github.com/glfw/glfw) — Camilla Löwy & Contributors**
   - Cross-platform library for window creation, OpenGL context management, input handling, and clipboard.
   - License: **zlib/libpng**.

4. **[stb_image](https://github.com/nothings/stb) — Sean Barrett**
   - Single-header public domain image decoding library (`third_party/stb/stb_image.h`).
   - License: **MIT / Public Domain**.

5. **OpenGL 3.3 Core & Platform APIs**
   - Cross-platform **OpenGL 3.3 Core** graphics API.
   - **Windows:** Win32 API (`comdlg32`, `dwmapi`, `user32`, `shell32`) for native file dialogs and window integration.
   - **Linux:** X11/Wayland and `zenity`/`kdialog` for native file dialogs.
