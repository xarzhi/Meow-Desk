// meow-desk — 轻量桌面分区工具
//
// 架构：
//   · 桌面层（DesktopSurface）：覆盖全屏的 per-pixel alpha 分层窗口，
//     枚举真实桌面图标并按原位置重画；空白处完全透明，透出壁纸
//   · 盒子（FenceWindow）：同样是分层窗口，GDI/GDI+ 自绘圆角半透明卡片
//   · 拖拽浮层（DragLayer）：盒子之间 / 盒子与桌面之间拖动图标时的跟随图标
//   · 层级：以上三者都紧贴「桌面窗口正上方」，平时位于所有应用窗口之下
//     （见 zorder.h，抄自 my-desktop 的 DesktopHost.KeepAboveDesktop）
//   · 图标只存在一份（DesktopIconView），归属用 owner 表示：
//       owner == -1 → 桌面；owner == i → 第 i 个盒子
//     "拖进盒子""盒子间互拖""拖回桌面"都只是改这个整数
//   · 启动时隐藏资源管理器的桌面图标视图，退出时恢复
#include "config.h"
#include "desktop_icons.h"
#include "desktop_surface.h"
#include "drag_layer.h"
#include "fence_window.h"
#include "settings_window.h"
#include "shell_icons.h"
#include "tray.h"
#include "zorder.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static const int kIconPx = 48;

static std::vector<std::unique_ptr<DesktopIconView>> g_iconStore;
static std::vector<DesktopIconView*> g_icons;

static DesktopSurface g_surface;
static DragLayer g_drag;
static std::vector<std::unique_ptr<FenceWindow>> g_fences;

static TrayIcon g_tray;
static SettingsWindow g_settings;

static bool g_systemIconsHidden = false;
static bool g_surfaceVisible = true;
static int g_lastArrow = 1;
static int g_lastAcrylic = 0;
static int g_cellW = 75;
static int g_cellH = 98;
static DesktopIconView* g_draggingIcon = nullptr;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static void LogStep(const char* step) {
    FILE* f = nullptr;
    fopen_s(&f, "meow-desk.log", "a");
    if (f) { fprintf(f, "%s\n", step); fclose(f); }
}

static void LogStep(const std::string& s) { LogStep(s.c_str()); }

static int SnapTo(int v, int step) {
    if (step <= 0) return v;
    return (int)std::lround((double)v / (double)step) * step;
}

static void OpenPath(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void DumpIconPositions() {
    FILE* f = nullptr;
    fopen_s(&f, "icons.txt", "w, ccs=UTF-8");
    if (!f) return;
    for (auto* e : g_icons) {
        if (!e) continue;
        fwprintf(f, L"%s\t%ld\t%ld\n", e->name.c_str(), e->pos.x, e->pos.y);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// 层级：桌面 → 图标层 → 盒子们 → 拖拽浮层，全部位于所有应用窗口之下
// ---------------------------------------------------------------------------
static HWND SurfaceAnchor() { return FindDesktopWindow(); }

static HWND FenceAnchor(int i) {
    HWND s = g_surfaceVisible && g_surface.hwnd() && IsWindowVisible(g_surface.hwnd())
                 ? g_surface.hwnd()
                 : nullptr;
    if (i <= 0) return s ? s : FindDesktopWindow();
    for (int k = i - 1; k >= 0; --k) {
        if (g_fences[k] && g_fences[k]->hwnd() && IsWindow(g_fences[k]->hwnd()))
            return g_fences[k]->hwnd();
    }
    return s ? s : FindDesktopWindow();
}

static HWND DragAnchor() {
    for (int k = (int)g_fences.size() - 1; k >= 0; --k) {
        if (g_fences[k] && g_fences[k]->hwnd() && IsWindow(g_fences[k]->hwnd()))
            return g_fences[k]->hwnd();
    }
    if (g_surface.hwnd() && IsWindowVisible(g_surface.hwnd())) return g_surface.hwnd();
    return FindDesktopWindow();
}

static void RestoreZOrder() {
    if (g_surfaceVisible && g_surface.hwnd()) PlaceAbove(g_surface.hwnd(), SurfaceAnchor());
    for (size_t i = 0; i < g_fences.size(); ++i) {
        if (g_fences[i] && g_fences[i]->hwnd())
            PlaceAbove(g_fences[i]->hwnd(), FenceAnchor((int)i));
    }
    if (g_drag.hwnd()) PlaceAbove(g_drag.hwnd(), DragAnchor());
}

// ---------------------------------------------------------------------------
// 图标 → 盒子 / 桌面
// ---------------------------------------------------------------------------
static int FenceIndexAt(POINT pt) {
    for (size_t i = 0; i < g_fences.size(); ++i) {
        if (g_fences[i] && g_fences[i]->HitTest(pt)) return (int)i;
    }
    return -1;
}

static void RedrawAll() {
    g_surface.Redraw();
    for (auto& f : g_fences) if (f) f->Redraw();
}

// 把拖动/松手后的位置写回资源管理器（真实桌面同步挪动）
static void CommitPositions(const std::vector<DesktopIconView*>& list) {
    std::vector<void*> pidls;
    std::vector<POINT> pts;
    for (auto* e : list) {
        if (!e || !e->pidl) continue;
        pidls.push_back(e->pidl);
        pts.push_back(e->pos);
    }
    if (pidls.empty()) return;
    if (!SetDesktopIconPositions(pidls.size(), pidls.data(), pts.data())) {
        LogStep(LastEnumDiag());
    }
}

// 把图标移交给 owner（-1 = 桌面），dropPt 为屏幕坐标
static void MoveIconTo(DesktopIconView* e, int owner, POINT dropPt) {
    if (!e) return;
    const int old = e->owner;
    if (old == owner) return;

    if (owner < 0) {
        // 回到桌面：以光标为中心落位，并对齐到格子
        int x = SnapTo(dropPt.x - g_cellW / 2, g_cellW);
        int y = SnapTo(dropPt.y - g_cellH / 2, g_cellH);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        e->pos = POINT{x, y};
        e->dragPos = e->pos;
    }
    e->owner = owner;
    e->selected = false;

    if (owner < 0) {
        std::vector<DesktopIconView*> one{e};
        CommitPositions(one);
    }
    RedrawAll();
}

// ---------------------------------------------------------------------------
// 图标加载
// ---------------------------------------------------------------------------
static void ApplyIconAppearance() {
    for (auto* e : g_icons) {
        if (e) LoadShellIcon(e->path, kIconPx, e->bmp, g_config.shortcutArrow != 0);
    }
    g_surface.InvalidateCache();
    for (auto& f : g_fences) if (f) f->Redraw();
}

// 重新枚举真实桌面（位置可能被用户在外部改了）
static void ReloadDesktopIcons() {
    auto items = EnumerateDesktopIcons();
    LogStep("reload icons: " + std::to_string(items.size()) + " diag: " + LastEnumDiag());

    std::vector<DesktopIconView*> seen;
    for (auto& it : items) {
        DesktopIconView* found = nullptr;
        for (auto* e : g_icons) {
            if (e && e->path == it.path && std::find(seen.begin(), seen.end(), e) == seen.end()) {
                found = e;
                break;
            }
        }
        if (found) {
            if (found->pidl) FreeDesktopPidl(found->pidl);
            found->pidl = it.pidl;
            found->name = it.name;
            if (found->OnDesktop()) {
                found->pos = it.pos;
                found->dragPos = it.pos;
            }
            seen.push_back(found);
            continue;
        }
        auto e = std::make_unique<DesktopIconView>();
        e->path = it.path;
        e->name = it.name;
        e->pos = it.pos;
        e->dragPos = it.pos;
        LoadShellIcon(e->path, kIconPx, e->bmp, g_config.shortcutArrow != 0);
        e->pidl = it.pidl;
        seen.push_back(e.get());
        g_icons.push_back(e.get());
        g_iconStore.push_back(std::move(e));
    }
    // 系统里已经没有的图标：从桌面/盒子移除
    for (size_t i = 0; i < g_icons.size();) {
        DesktopIconView* e = g_icons[i];
        if (std::find(seen.begin(), seen.end(), e) == seen.end()) {
            if (e && e->pidl) FreeDesktopPidl(e->pidl);
            auto it = std::find_if(g_iconStore.begin(), g_iconStore.end(),
                                   [&](const std::unique_ptr<DesktopIconView>& p) {
                                       return p.get() == e;
                                   });
            if (it != g_iconStore.end()) g_iconStore.erase(it);
            g_icons.erase(g_icons.begin() + i);
            continue;
        }
        ++i;
    }
    DumpIconPositions();
    if (g_surfaceVisible) g_surface.Redraw();
    for (auto& f : g_fences) if (f) f->Redraw();
}

// ---------------------------------------------------------------------------
// 设置变化
// ---------------------------------------------------------------------------
static void ApplySettings() {
    if (g_config.shortcutArrow != g_lastArrow) {
        g_lastArrow = g_config.shortcutArrow;
    g_lastAcrylic = g_config.fences.empty() ? 0 : g_config.fences[0].acrylic;
        ApplyIconAppearance();
    }
    const int acrylic = g_config.fences.empty() ? 0 : g_config.fences[0].acrylic;
    const bool recapture = (acrylic != g_lastAcrylic);
    g_lastAcrylic = acrylic;

    g_surface.SetTextShadow(g_config.textShadow != 0);
    for (auto& f : g_fences) if (f) f->ApplyConfig(recapture);
}

// ---------------------------------------------------------------------------
// 显示 / 隐藏系统桌面图标
// ---------------------------------------------------------------------------
static void SetDesktopIconsShown(bool shown) {
    SetSystemDesktopIconsVisible(shown);
    g_systemIconsHidden = !shown;

    g_surfaceVisible = !shown;
    if (g_surface.hwnd()) {
        ShowWindow(g_surface.hwnd(), shown ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
    if (!shown) {
        g_surface.Redraw();
        RestoreZOrder();
    }
    g_tray.SetDesktopIconsVisible(shown);
}

// ---------------------------------------------------------------------------
// 退出
// ---------------------------------------------------------------------------
static void Cleanup() {
    g_tray.Destroy();
    g_settings.Destroy();
    g_drag.Destroy();
    for (auto& f : g_fences) if (f) f->Destroy();
    g_fences.clear();
    g_surface.Destroy();

    for (auto* e : g_icons) {
        if (e && e->pidl) { FreeDesktopPidl(e->pidl); e->pidl = nullptr; }
    }
    g_iconStore.clear();
    g_icons.clear();

    SetSystemDesktopIconsVisible(true);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 物理像素坐标系：不清这个，DWM 会把分层窗口整体拉伸，图标必然发虚
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogStep("--- start ---");

    // 上一次异常退出可能把系统桌面图标留在隐藏状态，先自愈
    if (!IsSystemDesktopIconsVisible()) {
        SetSystemDesktopIconsVisible(true);
        LogStep("self-heal: desktop icons were hidden");
    }

    // 1. 配置
    g_config.Load();
    ConfigEnsureDefaults();
    g_lastArrow = g_config.shortcutArrow;
    g_lastAcrylic = g_config.fences.empty() ? 0 : g_config.fences[0].acrylic;

    // 2. 图标
    auto items = EnumerateDesktopIcons();
    LogStep("enum icons: " + std::to_string(items.size()) + " diag: " + LastEnumDiag());
    // 诊断用：跳过位图加载，用来分离「图标数据」占的内存
    const bool probeNoIcon = GetEnvironmentVariableW(L"MEOW_PROBE_NOICON", nullptr, 0) > 0;
    g_iconStore.reserve(items.size());
    for (auto& it : items) {
        auto e = std::make_unique<DesktopIconView>();
        e->path = it.path;
        e->name = it.name;
        e->pos = it.pos;
        e->dragPos = it.pos;
        if (!probeNoIcon) LoadShellIcon(e->path, kIconPx, e->bmp, g_config.shortcutArrow != 0);
        e->pidl = it.pidl;
        g_icons.push_back(e.get());
        g_iconStore.push_back(std::move(e));
    }
    LogStep("icons ready: " + std::to_string(g_icons.size()));
    {
        size_t totalBytes = 0, maxW = 0, maxH = 0, nonStd = 0;
        for (auto* e : g_icons) {
            if (!e || !e->bmp.valid()) continue;
            totalBytes += e->bmp.pixels.size();
            if ((size_t)e->bmp.width > maxW) maxW = e->bmp.width;
            if ((size_t)e->bmp.height > maxH) maxH = e->bmp.height;
            if (e->bmp.width != kIconPx || e->bmp.height != kIconPx) ++nonStd;
        }
        LogStep("icon mem: " + std::to_string(totalBytes / 1024) + " KB, max=" +
                std::to_string(maxW) + "x" + std::to_string(maxH) + ", nonStd=" +
                std::to_string(nonStd));
    }
    DumpIconPositions();

    POINT sp = LastEnumSpacing();
    g_cellW = sp.x > 0 ? sp.x : 75;
    g_cellH = sp.y > 0 ? sp.y : 98;

    // 3. 隐藏系统桌面图标，交给我们的桌面层
    g_systemIconsHidden = SetSystemDesktopIconsVisible(false);
    LogStep(g_systemIconsHidden ? "system icons hidden" : "hide system icons FAILED");

    // 4. 拖拽浮层（最先创建，层级最后摆）
    g_drag.Create();

    // 5. 桌面层
    g_surface.SetIcons(&g_icons);
    g_surface.SetCellSize(g_cellW, g_cellH);
    g_surface.SetIconPx(kIconPx);
    g_surface.SetTextShadow(g_config.textShadow != 0);
    g_surface.onOpen = [](DesktopIconView* e) { OpenPath(e->path); };
    g_surface.overTarget = [](POINT pt) { return FenceIndexAt(pt) >= 0; };
    g_surface.onDrop = [](const std::vector<DesktopIconView*>& list, POINT pt) -> bool {
        const int fi = FenceIndexAt(pt);
        if (fi < 0) return false;
        for (auto* e : list) MoveIconTo(e, fi, pt);
        return true;
    };
    g_surface.onMoved = [](const std::vector<DesktopIconView*>& list) { CommitPositions(list); };
    g_surface.onDragUpdate = [](const std::vector<DesktopIconView*>& list) {
        std::vector<DragGhost> ghosts;
        ghosts.reserve(list.size());
        for (auto* e : list) {
            if (!e || !e->bmp.valid()) continue;
            DragGhost gh;
            gh.bmp = &e->bmp;
            gh.x = e->dragPos.x + (g_cellW - e->bmp.width) / 2;
            gh.y = e->dragPos.y + DesktopSurface::kIconTopPad;
            ghosts.push_back(gh);
        }
        g_drag.Show(ghosts);
    };
    g_surface.onDragEnd = [] { g_drag.Hide(); };

    if (!g_surface.Create()) {
        LogStep("surface create FAILED");
    } else {
        InstallKeepAbove(g_surface.hwnd(), [] { return SurfaceAnchor(); });
        PlaceAbove(g_surface.hwnd(), SurfaceAnchor());
        LogStep("surface created");
    }

    // 6. 盒子
    for (size_t i = 0; i < g_config.fences.size(); ++i) {
        auto f = std::make_unique<FenceWindow>();
        const FenceConfig& c = g_config.fences[i];
        f->SetIcons(&g_icons);
        f->onOpen = [](DesktopIconView* e) { OpenPath(e->path); };
        f->onGeometryChanged = [i] {
            if (i >= g_fences.size()) return;
            RECT r = g_fences[i]->rect();
            if (i < g_config.fences.size()) {
                g_config.fences[i].x = r.left;
                g_config.fences[i].y = r.top;
                g_config.Save();
            }
            g_fences[i]->ApplyConfig(true);   // 亚克力底图要跟着新位置重抓
        };
        f->onItemDragStart = [](DesktopIconView* e, POINT pt) {
            g_draggingIcon = e;
            std::vector<DragGhost> ghosts;
            DragGhost gh;
            gh.bmp = &e->bmp;
            gh.x = pt.x - e->bmp.width / 2;
            gh.y = pt.y - e->bmp.height / 2;
            ghosts.push_back(gh);
            g_drag.Show(ghosts);
            RestoreZOrder();
        };
        f->onItemDragMove = [](DesktopIconView*, POINT pt) {
            const IconBitmap* bm = (g_draggingIcon) ? &g_draggingIcon->bmp : nullptr;
            if (!bm) return;
            std::vector<DragGhost> ghosts;
            DragGhost gh;
            gh.bmp = bm;
            gh.x = pt.x - bm->width / 2;
            gh.y = pt.y - bm->height / 2;
            ghosts.push_back(gh);
            g_drag.Show(ghosts);
        };
        f->onItemDrop = [](DesktopIconView* e, POINT pt) {
            g_drag.Hide();
            g_draggingIcon = nullptr;
            const int fi = FenceIndexAt(pt);
            if (fi >= 0) {
                MoveIconTo(e, fi, pt);   // 盒子 → 盒子
            } else {
                MoveIconTo(e, -1, pt);   // 盒子 → 桌面
            }
            RedrawAll();
        };

        if (!f->Create((int)i, c.x, c.y, c.w, c.h))
            LogStep("fence create FAILED " + std::to_string(i));
        // 即使创建失败也占位，保证 g_fences 下标 == 配置下标
        g_fences.push_back(std::move(f));
    }
    for (size_t i = 0; i < g_fences.size(); ++i) {
        InstallKeepAbove(g_fences[i]->hwnd(), [i] { return FenceAnchor((int)i); });
        PlaceAbove(g_fences[i]->hwnd(), FenceAnchor((int)i));
    }
    LogStep("fences created: " + std::to_string(g_fences.size()));

    // 7. 设置窗口 + 托盘
    g_settings.Create(nullptr);
    g_settings.onChanged = [] { ApplySettings(); };

    g_tray.onSettings = [] {
        if (g_settings.hwnd()) g_settings.Show();
    };
    g_tray.onReloadIcons = [] { ReloadDesktopIcons(); };
    g_tray.onToggleDesktopIcons = [] { SetDesktopIconsShown(g_systemIconsHidden); };
    g_tray.onExit = [] { PostQuitMessage(0); };
    if (!g_tray.Create(nullptr)) LogStep("tray create FAILED");
    g_tray.SetDesktopIconsVisible(false);

    // 8. 心跳：把层级摆回来（Win+D、Explorer 重启等）
    g_surface.onTick = [] { RestoreZOrder(); };

    RestoreZOrder();
    // 便于自动化测试：MEOW_SHOW_SETTINGS=1 时启动即打开设置面板
    if (GetEnvironmentVariableW(L"MEOW_SHOW_SETTINGS", nullptr, 0) > 0) g_settings.Show();
    LogStep("ready");

    // 9. 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogStep("message loop exited");

    Cleanup();
    CoUninitialize();
    return 0;
}
