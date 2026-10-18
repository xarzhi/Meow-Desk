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
#include "layout_store.h"
#include "search_window.h"
#include "settings_window.h"
#include "shell_icons.h"
#include "tray.h"
#include "zorder.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
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
static SearchWindow g_search;

// 全局快捷键：Ctrl+Alt+D 唤出搜索面板
static const int kHotkeyId = 1;

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
        fwprintf(f, L"%s\t%ld\t%ld\t%d\n", e->name.c_str(), e->pos.x, e->pos.y, e->owner);
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

// 把图标插到盒子 fi 的第 vis 个可见位置（vis < 0 = 追加到末尾）。
// 盒子的显示顺序 = g_icons 里过滤 owner 后的顺序，所以这里只挪指针的位置。
static void InsertIntoFence(DesktopIconView* e, int fi, int vis) {
    if (!e || fi < 0 || fi >= (int)g_fences.size()) return;
    auto it = std::find(g_icons.begin(), g_icons.end(), e);
    if (it != g_icons.end()) g_icons.erase(it);
    e->owner = fi;
    e->selected = false;
    e->dragPos = e->pos;

    if (vis < 0) {
        g_icons.push_back(e);
        return;
    }
    std::vector<DesktopIconView*> mine;
    for (auto* o : g_icons)
        if (o && o->owner == fi) mine.push_back(o);
    if (vis >= (int)mine.size()) {
        g_icons.push_back(e);
        return;
    }
    auto anchor = std::find(g_icons.begin(), g_icons.end(), mine[vis]);
    g_icons.insert(anchor, e);
}

// ---------------------------------------------------------------------------
// 布局持久化
//
// owner（图标属于哪个盒子）以前只活在内存里，重启后盒子里空空如也。
// 这里把「归属 + 盒内顺序 + 桌面坐标」写进 %APPDATA%\meow-desk\layout.ini，
// 启动时按 path（解析名，唯一且稳定）还原。
// ---------------------------------------------------------------------------
static void SaveIconLayout();

// 把图标放回桌面：吸附到离光标最近的空闲格子，绝不和已有图标重叠
static void PlaceOnDesktop(DesktopIconView* e, POINT dropPt,
                           const std::vector<DesktopIconView*>& pending) {
    if (!e) return;
    (void)pending;   // 目前只有「盒子 → 桌面」的单图标路径，保留参数给将来的多选拖出
    // 拖拽浮层是把图标中心钉在光标上的（gh.x = pt.x - w/2, gh.y = pt.y - h/2），
    // 落位必须用同一套几何关系，否则松手瞬间图标会「跳」一下，看起来就是落点不准。
    e->pos = g_surface.CellForCursor(dropPt, e->bmp.valid() ? &e->bmp : nullptr);
    e->dragPos = e->pos;
    e->owner = -1;
    e->selected = false;
    std::vector<DesktopIconView*> one{e};
    CommitPositions(one);
    SaveIconLayout();
}

// 把当前「归属 + 盒内顺序 + 桌面坐标」整份写盘
static void SaveIconLayout() {
    if (g_icons.empty()) return;
    std::vector<LayoutEntry> out;
    out.reserve(g_icons.size());
    std::vector<int> nextOrder(g_fences.size() + 1, 0);
    for (auto* e : g_icons) {
        if (!e) continue;
        LayoutEntry le;
        le.path = e->path;
        le.owner = (e->owner >= 0 && e->owner < (int)g_fences.size()) ? e->owner : -1;
        if (le.owner >= 0) le.order = nextOrder[le.owner]++;
        le.pos = e->pos;
        out.push_back(std::move(le));
    }
    if (!SaveLayout(out))
        LogStep("SaveLayout FAILED (" + std::to_string(out.size()) + " entries)");
    DumpIconPositions();   // 顺带刷新调试快照
}

// 启动时按 path 还原归属和位置。必须在盒子创建之后调用（要校验盒号）。
static void ApplyIconLayout() {
    std::vector<LayoutEntry> saved;
    if (!LoadLayout(saved) || saved.empty()) {
        LogStep("layout: none");
        return;
    }
    std::unordered_map<std::wstring, const LayoutEntry*> byPath;
    byPath.reserve(saved.size() * 2);
    for (const auto& le : saved) byPath.emplace(le.path, &le);

    int toFence = 0, onDesk = 0;
    std::unordered_map<DesktopIconView*, int> orderOf;
    for (auto* e : g_icons) {
        if (!e) continue;
        auto it = byPath.find(e->path);
        if (it == byPath.end()) continue;
        const LayoutEntry* le = it->second;
        if (le->owner >= 0 && le->owner < (int)g_fences.size()) {
            e->owner = le->owner;
            orderOf[e] = le->order;
            ++toFence;
        } else {
            e->owner = -1;
            // (0,0) 视为「没记录过位置」，保留枚举出来的原始坐标
            if (le->pos.x != 0 || le->pos.y != 0) {
                e->pos = le->pos;
                e->dragPos = le->pos;
            }
            ++onDesk;
        }
    }

    // 盒内图标的显示顺序 = g_icons 里过滤 owner 后的相对顺序，所以按
    // (是否在盒内, 盒号, 盒内序号) 重排即可；桌面图标保持原有相对顺序。
    auto key = [&orderOf](DesktopIconView* p) {
        if (!p || p->owner < 0) return std::pair<int, int>{0, 0};
        auto it = orderOf.find(p);
        return std::pair<int, int>{1 + p->owner,
                                   it == orderOf.end() ? (std::numeric_limits<int>::max)()
                                                       : it->second};
    };
    std::stable_sort(g_icons.begin(), g_icons.end(),
                     [&key](DesktopIconView* x, DesktopIconView* y) { return key(x) < key(y); });

    LogStep("layout: " + std::to_string(toFence) + " in fences, " + std::to_string(onDesk) +
            " on desktop");
    DumpIconPositions();   // 还原后要刷新调试快照，否则外部看到的是还原前的坐标
}

// 盒子里第 idx 个可见图标（skip 用来跳过正在被拖的那个）
static DesktopIconView* FenceItemAt(int fi, int idx, DesktopIconView* skip) {
    if (fi < 0 || idx < 0) return nullptr;
    int k = 0;
    for (auto* o : g_icons) {
        if (!o || o->owner != fi || o == skip) continue;
        if (k == idx) return o;
        ++k;
    }
    return nullptr;
}

// 落在目标图标的「中间」区域松手：把拖拽的文件交给目标图标的程序打开
static void OpenWithDragged(DesktopIconView* src, DesktopIconView* tgt) {
    if (!src || !tgt || src == tgt) return;
    ShellExecuteW(nullptr, L"open", tgt->path.c_str(), src->path.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

static void ClearAllDropHints() {
    for (auto& f : g_fences)
        if (f) f->ClearDropHint();
}

// 拖拽过程中每帧刷新「插入缝隙线」提示
static void UpdateAllDropHints(POINT pt) {
    const int fi = FenceIndexAt(pt);
    for (size_t i = 0; i < g_fences.size(); ++i) {
        if (!g_fences[i]) continue;
        if ((int)i == fi) g_fences[i]->UpdateDropHint(pt);
        else g_fences[i]->ClearDropHint();
    }
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
        ApplyIconAppearance();
    }
    const int acrylic = g_config.fences.empty() ? 0 : g_config.fences[0].acrylic;
    const bool recapture = (acrylic != g_lastAcrylic);
    g_lastAcrylic = acrylic;

    g_surface.SetTextShadow(g_config.textShadow != 0);
    g_surface.SetShowLabels(g_config.showLabels != 0);
    {
        const int textH = std::max(14, g_cellH - (DesktopSurface::kIconTopPad + kIconPx +
                                                  DesktopSurface::kTextGap));
        g_drag.SetLabelLayout(std::max(40, g_cellW - 10), textH, g_config.textShadow != 0,
                              g_config.showLabels != 0);
    }
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
    UnregisterHotKey(nullptr, kHotkeyId);

    // 退出前收尾：布局写盘 + 把桌面图标的最终坐标同步回资源管理器，
    // 否则我们隐藏了真实图标又摆在自己的位置，一退出图标会整体跳一下。
    SaveIconLayout();
    {
        std::vector<DesktopIconView*> desk;
        for (auto* e : g_icons)
            if (e && e->OnDesktop()) desk.push_back(e);
        if (!desk.empty()) CommitPositions(desk);
    }

    g_tray.Destroy();
    g_search.Destroy();
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
    {
        // 浮层里图标下方也要显示名称：排版盒子和桌面标签保持一致
        const int iconSide = kIconPx;
        const int textH = std::max(14, g_cellH - (DesktopSurface::kIconTopPad + iconSide +
                                                  DesktopSurface::kTextGap));
        g_drag.SetLabelLayout(std::max(40, g_cellW - 10), textH, g_config.textShadow != 0,
                              g_config.showLabels != 0);
    }

    // 5. 桌面层
    g_surface.SetIcons(&g_icons);
    g_surface.SetCellSize(g_cellW, g_cellH);
    g_surface.SetIconPx(kIconPx);
    g_surface.SetTextShadow(g_config.textShadow != 0);
    g_surface.SetShowLabels(g_config.showLabels != 0);
    g_surface.onOpen = [](DesktopIconView* e) { OpenPath(e->path); };
    g_surface.onContextInvoked = [] { ReloadDesktopIcons(); };
    g_surface.overTarget = [](POINT pt) { return FenceIndexAt(pt) >= 0; };
    g_surface.onDragHover = [](POINT pt) { UpdateAllDropHints(pt); };
    g_surface.onDrop = [](const std::vector<DesktopIconView*>& list, POINT pt) -> bool {
        const int fi = FenceIndexAt(pt);
        if (fi < 0) return false;

        FenceWindow::DropHint h;
        if (g_fences[fi]) h = g_fences[fi]->dropHint();
        ClearAllDropHints();

        // 中间区域 + 单个文件 → 用目标图标的程序打开它
        if (h.openWith() && list.size() == 1) {
            OpenWithDragged(list[0], FenceItemAt(fi, h.item, nullptr));
            return true;
        }
        int k = 0;
        for (auto* e : list) {
            InsertIntoFence(e, fi, h.active() ? h.insert + k : -1);
            ++k;
        }
        SaveIconLayout();
        RedrawAll();
        return true;
    };
    g_surface.onMoved = [](const std::vector<DesktopIconView*>& list) {
        CommitPositions(list);
        SaveIconLayout();
    };
    g_surface.onDragUpdate = [](const std::vector<DesktopIconView*>& list) {
        std::vector<DragGhost> ghosts;
        ghosts.reserve(list.size());
        for (auto* e : list) {
            if (!e || !e->bmp.valid()) continue;
            DragGhost gh;
            gh.bmp = &e->bmp;
            gh.x = e->dragPos.x + (g_cellW - e->bmp.width) / 2;
            gh.y = e->dragPos.y + DesktopSurface::kIconTopPad;
            gh.name = &e->name;
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
                g_config.fences[i].w = r.right - r.left;
                g_config.fences[i].h = r.bottom - r.top;
                g_config.Save();
            }
            g_fences[i]->ApplyConfig(true);   // 亚克力底图要跟着新位置重抓
        };
        f->onItemDragStart = [](DesktopIconView* e, POINT pt) {
            g_draggingIcon = e;
            ClearAllDropHints();
            g_surface.SetDropPreview(false);
            std::vector<DragGhost> ghosts;
            DragGhost gh;
            gh.bmp = &e->bmp;
            gh.x = pt.x - e->bmp.width / 2;
            gh.y = pt.y - e->bmp.height / 2;
            gh.name = &e->name;
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
            if (g_draggingIcon) gh.name = &g_draggingIcon->name;
            ghosts.push_back(gh);
            g_drag.Show(ghosts);
            UpdateAllDropHints(pt);
            // 拖离所有盒子 → 在真实桌面上的落点画预览框
            if (FenceIndexAt(pt) < 0)
                g_surface.SetDropPreview(true, g_surface.CellForCursor(pt, bm));
            else
                g_surface.SetDropPreview(false);
        };
        f->onItemDrop = [](DesktopIconView* e, POINT pt) {
            g_drag.Hide();
            g_draggingIcon = nullptr;
            g_surface.SetDropPreview(false);

            const int fi = FenceIndexAt(pt);
            FenceWindow::DropHint h;
            if (fi >= 0 && g_fences[fi]) h = g_fences[fi]->dropHint();
            ClearAllDropHints();

            if (fi >= 0) {
                if (h.openWith())
                    OpenWithDragged(e, FenceItemAt(fi, h.item, e));   // 盒子内：中间 → 打开方式
                else
                    InsertIntoFence(e, fi, h.active() ? h.insert : -1);
            } else {
                std::vector<DesktopIconView*> pending;
                PlaceOnDesktop(e, pt, pending);   // 盒子 → 桌面
            }
            SaveIconLayout();
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
        // 拖动时避让其他盒子（用实时窗口矩形，收起状态的盒子只占一条头部）
        g_fences[i]->SetObstacles([i] {
            std::vector<RECT> v;
            for (size_t k = 0; k < g_fences.size(); ++k) {
                if (k == i) continue;
                if (g_fences[k] && g_fences[k]->hwnd()) v.push_back(g_fences[k]->rect());
            }
            return v;
        });
    }
    LogStep("fences created: " + std::to_string(g_fences.size()));

    // 盒子建好之后才能校验盒号 → 在这里还原上次的图标布局
    ApplyIconLayout();
    RedrawAll();   // 盒子创建时就已经画过一次（那时还是空的），还原后必须重画

    // 7. 设置窗口 + 托盘 + 搜索面板
    g_settings.Create(nullptr);
    g_settings.onChanged = [] { ApplySettings(); };
    g_settings.onReloadIcons = [] { ReloadDesktopIcons(); };
    g_settings.getSystemIconsShown = [] { return !g_systemIconsHidden; };
    g_settings.setSystemIconsShown = [](bool shown) { SetDesktopIconsShown(shown); };

    g_search.SetIcons(&g_icons);
    g_search.onOpen = [](DesktopIconView* e) { OpenPath(e->path); };
    g_search.onFiltered = [](const std::vector<DesktopIconView*>& hits) {
        // 自测用：把当前搜索命中写到 search.txt（前 8 条）
        FILE* f = nullptr;
        fopen_s(&f, "search.txt", "w, ccs=UTF-8");
        if (!f) return;
        fwprintf(f, L"%d\n", (int)hits.size());
        for (size_t i = 0; i < hits.size() && i < 8; ++i)
            fwprintf(f, L"%s\n", hits[i] ? hits[i]->name.c_str() : L"(null)");
        fclose(f);
    };
    if (!g_search.Create()) LogStep("search window create FAILED");
    if (!RegisterHotKey(nullptr, kHotkeyId, MOD_ALT, 'F'))
        LogStep("RegisterHotKey(Alt+F) FAILED");
    else
        LogStep("hotkey Alt+F registered");

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
        // 全局快捷键：RegisterHotKey(nullptr, ...) 投递到线程队列，hwnd == nullptr
        if (msg.message == WM_HOTKEY && msg.hwnd == nullptr) {
            if ((int)msg.wParam == kHotkeyId) g_search.Toggle();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogStep("message loop exited");

    Cleanup();
    CoUninitialize();
    return 0;
}
