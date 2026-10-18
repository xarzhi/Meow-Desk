# CORE UI

Windows 桌面 UI 框架，基于 Direct2D 硬件加速渲染，对齐 Fluent 2 / WinUI 3 设计规范。

主推 **`.uix` 单文件组件**（Vue 3 SFC 风格：`<window>` / `<script>` / `<style>` / `<template>`），用最少的代码构建现代化桌面应用，由 QuickJS-NG 在原生进程内求值脚本。

> 当前稳定能力和历史变更以 [CHANGELOG.md](./CHANGELOG.md) 为准。
> `.uix` 的 `<script>` 必须是 `export default { ... }`（Vue 3 Options API）。

## 特性

- **`.uix` 单文件组件** — `<window>` + `<script>` + `<style>` + `<template>` 全在一个文件，Vue 3 风格 `data() / computed / methods`，QuickJS-NG (ES2020+) 求值
- **响应式系统** — Proxy + WatchEffect，模板里 `{{ expr }}` / `:attr` / `v-if` / `v-for` / `v-model` / `@click` 自动收集依赖、增量重渲染
- **CSS 子集** — `<style>` 支持类 / 标签 / 后代选择器、CSS 变量、伪类（`:hover`、`:disabled`）、Flexbox 布局、`var(--bg)` 主题色引用
- **25+ 内置控件** — `button` / `input` / `textarea` / `toggle` / `progressbar` / `menu` / `TitleBar` / `svg` 等，全部映射到原生 widget
- **Fluent 2 设计** — 色彩、圆角、阴影、动画对齐微软官方 Design Token，深 / 浅色主题运行时一行切换
- **纯 C API** — 400+ 导出函数、`uint64_t` 句柄、POD 结构体，可从任何语言调用
- **高性能渲染** — Direct2D + Direct3D 11 硬件加速，Per-Monitor DPI V2
- **自定义无边框窗口** — `<TitleBar>` 控件内置，支持系统拖拽、贴靠、动画
- **国际化** — `.lang` 文件 + `{{ $t('key') }}`，运行时切换语言
- **调试 / 自动化** — Named Pipe IPC（`ui_debug_server_start`），可外部查询控件树、模拟点击 / 输入 / 截图
- **黄金图回归测试** — `golden_runner.exe` 把 `demo/golden/*.uix` 渲染成 PNG，与基准图对比

## 近期重点能力

- **render-thread 渲染基线** — 普通窗口、弹出层、截图、视觉事务和图片路径统一到 render-thread 管线
- **SVG 兼容性增强** — draw.io / diagrams.net 文本、GaussianBlur / drop-shadow、alpha mask 和渐变阴影层
- **视觉质量改进** — CSS `box-shadow` 真模糊、透明动画阴影边界、窗口实时缩放背景色同步
- **UIX 能力补齐** — 锚点布局辅助函数、窗口 resize 回调、菜单行样式和父子菜单宽度控制

完整 per-build 变更见 CHANGELOG。

## 截图

> *（截图待补充）*

## 快速开始

### 环境要求

- Windows 10 (1709+)
- CMake 3.20+
- **MSVC 2019+ 或 clang-cl**（C++17）— 不支持 MinGW

### 构建

构建必须从 PowerShell 调用项目自带脚本（自动配置 vcvars64、用 llvm-rc 绕过 Windows SDK rc.exe 卡死 bug）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-clang-cl.ps1 -Target core-ui
```

常用 target：

| Target | 产物 |
|---|---|
| `core-ui` | `core-ui.dll` + `core-ui.lib` 导入库（默认） |
| `core-ui-static` | `core-ui-static.lib` 自包含静态归档（含 QuickJS） |
| `ui-demo-uix` | `ui-demo-uix.exe` 单文件 demo（资源烤进 exe） |
| `golden_runner` | `golden_runner.exe` 黄金图回归测试 |

加 `-Clean` 强制重建 build 目录；省略 `-Target` 编全部。

静态链接（单 exe 无 DLL 分发）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-clang-cl.ps1 -Target core-ui -Static
```

### Hello World

**hello.uix**

```vue
<window title="Hello" width="400" height="300" centered="true" theme="light"/>

<script>
export default {
  data()    { return { count: 0 }; },
  computed: { doubled() { return this.count * 2; } },
  methods:  { inc() { this.count++; } }
}
</script>

<style>
  .root   { padding: 24px; gap: 12px; background: var(--bg); }
  .h1     { font-size: 22px; color: var(--fg); font-weight: 600; }
  .lbl    { font-size: 13px; color: var(--fg-2); }
  button  { background: var(--accent); color: #fff;
            padding: 6px 14px; border-radius: 4px; cursor: pointer; }
</style>

<template>
  <div class="root">
    <label class="h1">Hello, Core UI!</label>
    <label class="lbl">count = {{ count }} · doubled = {{ doubled }}</label>
    <button @click="inc">+1</button>
  </div>
</template>
```

**main.cpp**

```cpp
#include <ui_core.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    ui_init_with_theme(UI_THEME_LIGHT);

    UiPage page = ui_page_load_file(L"hello.uix");
    if (!page) return 1;

    UiWindow win = ui_page_open_window(page, NULL);
    if (!win) { ui_page_destroy(page); return 2; }

    int ret = ui_run();
    ui_page_destroy(page);
    return ret;
}
```

`ui_page_open_window` 自动按 `<window>` 标签里的 `title / width / height / theme / frameless / ...` 配窗口、把 `<template>` 的根挂上。`<script>` 里的 `methods` 直接由 `@click` 触发，`data` 字段经 reactive proxy 让所有引用它的 `{{ }}` / `:attr` 自动重算。

## `.uix` 文件结构

```vue
<window title="..." width="800" height="600"
        min-width="400" frameless="true" theme="light"/>

<script>
export default {
  data() { return { page: "home", items: [] }; },

  computed: {
    itemCount() { return this.items.length; }
  },

  methods: {
    nav(p)        { this.page = p; },
    addItem()     { this.items = [...this.items, { id: Date.now() }]; },
    removeItem(i) { this.items = this.items.filter(x => x.id !== i.id); }
  }
}
</script>

<style>
  .shell    { gap: 0; background: var(--bg); }
  .sidebar  { width: 220; background: var(--sidebar-bg); }
  .nav      { padding: 0 12px; height: 36px; cursor: pointer; }
  .nav:hover{ background: var(--sidebar-hover); }
  .nav.on   { background: var(--bg-3); }
  button    { background: var(--accent); color: #fff;
              padding: 6px 14px; border-radius: 4px; }
</style>

<template>
  <div class="shell">
    <TitleBar title="App"/>
    <div class="body" style="flex-direction: row; flex: 1">
      <div class="sidebar">
        <div class="nav" :class="page=='home' ? 'on' : ''" @click="nav('home')">
          {{ $t('nav_home') }}
        </div>
      </div>
      <div class="content" style="padding: 32px; flex: 1">
        <div v-if="page=='home'">
          <label class="h1">{{ $t('app_title') }}</label>
          <div v-for="(item, i) in items" :key="item.id">
            #{{ item.id }} <button @click="removeItem(item)">×</button>
          </div>
          <button @click="addItem">add</button>
        </div>
      </div>
    </div>
  </div>
</template>
```

支持的模板特性：

- **绑定**：`{{ expr }}`、`:attr="expr"`、`:class="cond ? 'a' : 'b'"`、`v-model="state"`
- **控制流**：`v-if` / `v-show`、`v-for="(x, i) in list" :key="x.id"`（支持嵌套 `v-for > v-if > v-for`）
- **事件**：`@click` / `@dblclick` / `@change` / `@focus` / `@blur` / `@mousedown` / `@mouseup` / `@wheel`，回调收 `$event` 事件对象
- **i18n**：`{{ $t('key') }}` 或 `<label>@key</label>` 速记，运行时随 `ui_page_set_locale` 切换
- **CSS 变量**：`var(--bg) / var(--fg) / var(--accent) / var(--card-bg) / var(--sidebar-bg) / ...`，主题切换时由库统一翻译
- **样式 scope**：`<style scoped>` 把选择器限制到当前组件
- **菜单**：`<menu trigger="#id" event="rclick">` + `<menuitem>` / `<separator>` 声明式右键菜单

## 内置控件

| 类别 | 标签 |
|---|---|
| 容器 | `div`（Flexbox），用 CSS `flex-direction / flex / gap / padding` 控制 |
| 文本 | `label`（支持多行、自动换行） |
| 按钮 | `button` |
| 输入 | `input`（type=`text` / `password` / `checkbox` / `radio` / `range` / `number`）、`textarea` |
| 选择 | `toggle`、`select` |
| 状态 | `progressbar` |
| 弹出 | `menu` / `menuitem` / `separator` |
| 图像 | `img`、`svg`（内联），底层 `ImageView` 支持缩放 / 平移 / 裁剪 |
| 窗口 | `TitleBar`（仅 `frameless="true"` 时使用） |

## C API

公共接口全是 C 函数，通过 `uint64_t` 句柄操作。最常用的是 **Page API**：

```c
#include <ui_core.h>

ui_init_with_theme(UI_THEME_LIGHT);

UiPage page = ui_page_load_file(L"app.uix");
ui_page_load_language_file(page, "zh", L"lang/zh.lang");
ui_page_load_language_file(page, "en", L"lang/en.lang");
ui_page_set_locale(page, "zh");

UiWindow win = ui_page_open_window(page, NULL);

/* 双向交换 reactive 状态 */
ui_page_set_int (page, "count", 42);
ui_page_set_text(page, "name",  L"Alice");
ui_page_set_json(page, "items", "[{\"id\":1,\"label\":\"a\"}]");

char* j = ui_page_get_json(page, "items");
/* ... 解析 j ... */
ui_page_free(j);

ui_debug_server_start(win, NULL);   /* 可选：开调试 IPC */

ui_run();
ui_page_destroy(page);
```

底层手搭控件树仍然可用（适合需要完全过程式构造的场景）：

```c
UiWidget root  = ui_vbox();
UiWidget label = ui_label(L"Hello");
UiWidget btn   = ui_button(L"OK");
ui_widget_add_child(root, label);
ui_widget_add_child(root, btn);
ui_widget_on_click(btn, my_callback, NULL);
ui_window_set_root(win, root);
```

## 主题

内置 Fluent 2 深色 / 浅色主题，运行时一行切换：

```c
ui_theme_set_mode(UI_THEME_DARK);
ui_theme_set_mode(UI_THEME_LIGHT);
```

`.uix` 的 `<style>` 用 CSS 变量引用主题色（`var(--bg)` / `var(--fg)` / `var(--accent)` / `var(--card-bg)` / `var(--sidebar-bg)` / `var(--border-subtle)` 等），切主题时由库重新 cascade，所有控件自动响应。

## 单 exe 打包

`.uix` / `.lang` 文件可以在编译时烤进 exe，做到完全自包含、无外部依赖：

```cmake
include(cmake/UiCoreHelpers.cmake)

add_executable(my-app WIN32 main.cpp)
target_link_libraries(my-app PRIVATE core-ui)

ui_core_embed_text(my-app FILE app.uix      OUT app.embed.h     VAR k_app)
ui_core_embed_text(my-app FILE lang/zh.lang OUT lang_zh.embed.h VAR k_lang_zh)
```

```cpp
#include "app.embed.h"
#include "lang_zh.embed.h"

UiPage page = ui_page_load_string(k_app);
ui_page_load_language_string(page, "zh", k_lang_zh);
```

`demo/ui_demo_uix.cpp` 就是这种用法的最小完整例子。

## 项目结构

```
core-ui/
├── include/
│   ├── ui_core.h         # 公共 C API（400+ 函数）
│   └── plugin_api.h
├── src/ui/
│   ├── renderer.*        # Direct2D 渲染引擎
│   ├── widget.*          # 基础控件类 + 布局
│   ├── controls.*        # 所有内置控件
│   ├── ui_window.*       # 窗口管理 + 事件分发
│   ├── ui_api.cpp        # 核心 C API 实现
│   ├── ui_debug_server.* # Named Pipe 调试 IPC
│   ├── animation.*       # 动画系统
│   ├── context_menu.*    # 右键菜单
│   ├── image_*           # 图片解码 / GDI / SVG / GIF
│   ├── uix/              # .uix SFC 解析 + QuickJS 脚本运行时
│   │   ├── sfc_parser.*       # <window>/<script>/<style>/<template> 切块
│   │   ├── template_parser.*  # 模板 AST + 指令
│   │   ├── script_runtime.*   # QuickJS-NG 嵌入 + Vue Options API 适配
│   │   ├── expr_rewriter.*    # this.X 自动绑定改写
│   │   └── value_convert.*    # JS ↔ C 值互转
│   ├── markup/           # .ui XML markup 解析（兼容旧路径）
│   ├── reactive/         # Proxy + WatchEffect 响应式绑定
│   ├── css/              # CSS 解析 / 选择器 / cascade
│   ├── flex/             # Flexbox 布局
│   ├── page/             # Page API（widget_factory / compiler / state）
│   └── expression/       # JSON 互操作
├── demo/
│   ├── ui_demo.uix       # 12 页 SFC demo（响应式 / 列表 / 事件 / i18n / 主题）
│   ├── ui_demo_uix.cpp   # 60 行胶水 + locale poll
│   ├── lang/             # ui_demo_zh.lang / ui_demo_en.lang
│   ├── golden/           # 黄金图回归用例（*.uix + *.expected.png）
│   └── golden_runner.cpp # 渲染对比器
├── docs/                 # 文档（见下表）
├── scripts/              # build-clang-cl.ps1 等
├── cmake/                # UiCoreHelpers.cmake（embed_text）
└── test/                 # 单元测试
```

## 文档

| 文档 | 内容 |
|---|---|
| [快速上手](docs/getting-started.md) | 集成指南（CMake + MSVC / clang-cl） |
| [.uix AI 速查](docs/uix-ai-guide.md) | **给 AI 喂的 1 页 prompt**，写 `.uix` 单文件组件 |
| [.uix 详细指南](docs/uix-guide.md) | Vue 3 SFC 完整参考（`<script>` / CSS 子集 / widget 映射 / cookbook / 限制） |
| [调试 & 自动化](docs/debug-simulation.md) | Named Pipe 事件注入 API，AI 自验证闭环 |
| [API 索引](UI_CORE_API.md) | 全量 C 函数列表，按模块分组 |
| [C API 参考](docs/c-api.md) | 每个函数的参数级说明 |
| [控件](docs/controls.md) | 控件详细说明 |
| [布局](docs/layout.md) | Flexbox / 百分比 / 绝对定位 |
| [设计系统](docs/design-system.md) | Fluent 2 设计规范 |
| [国际化](docs/i18n.md) | `.lang` 文件 + `$t()` |
| [.ui markup AI 速查](docs/ai-guide.md) | 旧 `.ui` XML markup 速查（兼容路径） |
| [.ui markup 详细](docs/markup.md) | `.ui` 文件语法 |
| [Changelog](CHANGELOG.md) | 版本历史（`MAJOR.MINOR.PATCH.BUILD`） |

## 依赖

- **QuickJS-NG v0.14.0** — `.uix` 脚本求值，CMake `FetchContent` 自动拉取
- **Direct2D / DirectWrite / Direct3D 11 / DXGI / WIC / DWM / GDI+** — Windows 系统组件，无需额外安装

## 许可证

MIT License
