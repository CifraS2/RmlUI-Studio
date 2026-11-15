# RmlUI Studio

> 🇺🇸 [English](README.md) | 🇧🇷 **Português (Brasil)**

Editor visual de interfaces para **[RmlUi](https://github.com/mikke89/RmlUi)** (estilo Figma), construído em **C++17**, **RmlUi**, **FreeType**, **GLFW** e **OpenGL 3.3 Core** para **Windows** e **Linux**.

![Demonstração do RmlUI Studio](assets/preview.png)

---

## Como o projeto funciona

O **RmlUI Studio** usa o próprio **RmlUi** tanto para renderizar o documento do usuário quanto para desenhar a interface do editor, sobre um backend multiplataforma **OpenGL 3.3 Core + GLFW** para **Windows** e **Linux**:

### 1. Arquitetura de Dois Contextos (`RmlEngineHost` — `src/rml_engine.h`)
- **Contexto do Canvas (`CanvasContext`):** Renderiza o documento `.rml`/`.rcss` do usuário em uma textura *offscreen* na GPU (OpenGL FBO), permitindo zoom, pan, seleção direta de elementos (*hit-testing* no DOM real do RmlUi), manipulação visual e interação ao vivo.
- **Contexto do Editor (`EditorContext`):** Renderiza toda a interface do estúdio (painéis, árvore de camadas, inspetor de propriedades e barra de ferramentas) diretamente no *backbuffer* via RmlUi, sem dependência de ImGui.

### 2. Modelo de Documento e Geração de RML/RCSS (`RmlDocument` — `src/rml_doc.h`)
- **Árvore de nós (`RmlNode`):** Representa elementos RmlUi (`div`, `button`, `input`, `span`, `img`, `progress`) com propriedades de:
  - **Geometria e Layout:** posição, dimensões, *Flexbox* (`display: flex`, `flex-direction`, `justify-content`, `align-items`, `flex-wrap`, `gap`, `padding`), modos de dimensionamento (`Fixed`, `Hug`, `Fill`), âncoras/constraints e grades visuais (*Layout Grids*: colunas, linhas ou grade uniforme).
  - **Aparência e Tipografia:** cor de fundo, bordas, `border-radius`, opacidade, cor/tamanho/peso/alinhamento de texto e estados `:hover`.
  - **Assets:** suporte a imagens (`<img>`) com resolução relativa ao diretório base do projeto.
- **Importação e Sincronização ao Vivo:**
  - Importa arquivos `.rml` e `.rcss` existentes diretamente pelo DOM do RmlUi (`ImportRmlFile`, `ImportRcssAndSync`).
  - Aplica alterações de código `.rml` e `.rcss` em tempo real no canvas (`ApplyRmlStringAndSync`, `ApplyRcssStringAndSync`), com sanitização de RCSS para compatibilidade com o RmlUi (`SanitizeRcssForRmlUi`).
  - Exporta o projeto de volta para `.rml`, `.rcss` e assets (`GenerateRmlAndRcss`, `ExportToPath`).

### 3. Renderização OpenGL 3.3 e Ícones (`GlRenderer2D` / `studio_icons`)
- **`src/gl_renderer.h` / `.cpp`:** Backend 2D multiplataforma em OpenGL 3.3 Core responsável por exibir a textura do canvas, desenhar *overlays* de seleção, contornos tracejados e guias visuais.
- **`src/platform_utils.h` / `.cpp`:** Utilitários multiplataforma do sistema (diálogos nativos de abrir/salvar no Windows e no Linux via `zenity`/`kdialog`, e área de transferência via GLFW).
- **`src/studio_icons.h` / `.cpp`:** Gera ícones vetoriais 32×32 RGBA pré-multiplicados diretamente em código, coloridos dinamicamente pelo RmlUi via `image-color`.

---

## Estrutura Principal

- `CMakeLists.txt` — Configuração CMake (C++17, FreeType estático, RmlUi Core estático, GLFW, OpenGL 3.3 e executável `RmlUIStudio`).
- `src/main.cpp` — Inicialização multiplataforma da janela GLFW, contexto OpenGL 3.3 Core e loop principal.
- `src/rml_doc.h` / `.cpp` — Modelo de documento, nós, constraints, importação/exportação `.rml` e `.rcss`.
- `src/rml_engine.h` / `.cpp` — Integração com o RmlUi (contextos de Canvas e Editor, DOM, `stb_image` e renderização *offscreen* em OpenGL 3.3).
- `src/gl_renderer.h` / `.cpp` — Renderizador 2D OpenGL 3.3 e gerenciamento de texturas.
- `src/platform_utils.h` / `.cpp` — Diálogos de arquivo e clipboard multiplataforma.
- `src/studio_ui.h` / `.cpp` — Lógica da interface do estúdio, interação com canvas, painéis e inspetor.
- `src/studio_icons.h` / `.cpp` — Ícones vetoriais gerados em código.
- `third_party/RmlUi/` — Biblioteca RmlUi (subprojeto estático + backends GL3/GLFW).
- `third_party/freetype/` — Biblioteca FreeType (motor de fontes estático para o RmlUi).
- `third_party/stb/` — `stb_image.h` para decodificação de imagens multiplataforma.

---

## Como Compilar (Windows e Linux)

### Pré-requisitos
- **CMake** 3.20 ou superior
- **Compilador C++17** (MSVC no Windows; GCC 9+ ou Clang 10+ no Linux)
- **Drivers compatíveis com OpenGL 3.3**
- **Pacotes adicionais no Linux:** headers de desenvolvimento X11/Wayland para o GLFW (ou `libglfw3-dev`), e `zenity` ou `kdialog` em runtime para os diálogos nativos de arquivo.

### Build
```sh
git clone --recursive https://github.com/CifraS2/RmlUI-Studio
cd RmlUI-Studio
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

---

## Créditos e Bibliotecas

Este projeto utiliza as seguintes bibliotecas e APIs:

1. **[RmlUi](https://github.com/mikke89/RmlUi) — The RmlUi Team & Contribuidores**
   - Copyright (c) 2008–2014 CodePoint Ltd, Shift Technology Ltd, and contributors
   - Copyright (c) 2019–2026 The RmlUi Team, and contributors
   - Motor de interface HTML/CSS-like utilizado tanto no canvas quanto na UI do editor (`third_party/RmlUi/`).
   - Licença: **MIT** ([`third_party/RmlUi/LICENSE.txt`](third_party/RmlUi/LICENSE.txt)).

2. **[FreeType 2](https://freetype.org/) — The FreeType Project**
   - Motor de rasterização de fontes utilizado pelo RmlUi (`third_party/freetype/`).
   - Licença: **FreeType License (FTL)** ou **GPLv2** ([`third_party/freetype/LICENSE.TXT`](third_party/freetype/LICENSE.TXT); utilizado sob a **FTL**).

3. **[GLFW](https://github.com/glfw/glfw) — Camilla Löwy & Contribuidores**
   - Biblioteca multiplataforma para criação de janelas, gerenciamento de contexto OpenGL, entrada e área de transferência.
   - Licença: **zlib/libpng**.

4. **[stb_image](https://github.com/nothings/stb) — Sean Barrett**
   - Biblioteca *single-header* de domínio público para decodificação de imagens (`third_party/stb/stb_image.h`).
   - Licença: **MIT / Domínio Público**.

5. **OpenGL 3.3 Core e APIs da Plataforma**
   - API gráfica multiplataforma **OpenGL 3.3 Core**.
   - **Windows:** Win32 API (`comdlg32`, `dwmapi`, `user32`, `shell32`) para diálogos nativos de arquivo e integração com a janela.
   - **Linux:** X11/Wayland e `zenity`/`kdialog` para diálogos nativos de arquivo.
