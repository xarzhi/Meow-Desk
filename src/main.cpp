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
#include "confirm_dialog.h"
#include "desktop_icons.h"
#include "desktop_surface.h"
#include "drag_layer.h"
#include "fence_window.h"
#include "layout_store.h"
#include "popup_menu.h"
#include "search_window.h"
#include "settings_window.h"
#include "shell_icons.h"
#include "storage.h"
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
#include <unordered_set>
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

// 盒子的右键菜单（自绘，不是系统菜单）
static PopupMenu g_fenceMenu;
// 「删除盒子 / 删除分组」的确认框（同样是自绘，点确定才真删）
static ConfirmBox g_confirm;

// 全局快捷键：Ctrl+Alt+D 唤出搜索面板
static const int kHotkeyId = 1;

static bool g_systemIconsHidden = false;
static bool g_surfaceVisible = true;
static int g_lastArrow = 1;
static int g_lastAcrylic = 0;
static int g_cellW = 75;
static int g_cellH = 98;
// 资源管理器真实的桌面行距。g_cellH 是「实际用」的行距：关掉图标标题后
// 会把标签那一块的高度减掉，行与行之间立刻变紧。
static int g_cellHFull = 98;
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

// 路径转 UTF-8 只是给日志用
static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void OpenPath(const std::wstring& path) {
    if (path.empty()) return;
    const HINSTANCE r =
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute 的返回值 <= 32 就是失败码（SE_ERR_*），排障时很有用
    const INT_PTR rc = reinterpret_cast<INT_PTR>(r);
    if (rc <= 32) {
        LogStep("open failed rc=" + std::to_string((long long)rc) + " " + Narrow(path));
    } else {
        LogStep("open ok " + Narrow(path));
    }
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

// 图标的位图尺寸跟着「所属盒子」的查看档位走：
// 盒子里是「大图标」就真的加载 64px 的位图，回到桌面再换回 48px。
// 只对盒子里的图标做（数量有限），桌面上 200 个图标始终是 48px。
static void FitIconToOwner(DesktopIconView* e) {
    if (!e) return;
    int px = kIconPx;
    if (e->owner >= 0 && e->owner < (int)g_fences.size() && g_fences[e->owner])
        px = g_fences[e->owner]->iconPx();
    if (e->bmp.width == px && e->bmp.height == px) return;
    LoadShellIcon(e->path, px, e->bmp, g_config.shortcutArrow != 0);
}

// 某个盒子换了查看档位：把它的图标位图统一换到新尺寸
static void EnsureFenceIconSize(int fi) {
    for (auto* e : g_icons)
        if (e && e->owner == fi) FitIconToOwner(e);
}

// 把图标插到盒子 fi 的第 vis 个可见位置（vis < 0 = 追加到末尾）。
// 盒子的显示顺序 = g_icons 里过滤 owner 后的顺序，所以这里只挪指针的位置。
static void InsertIntoFence(DesktopIconView* e, int fi, int vis) {
    if (!e || fi < 0 || fi >= (int)g_fences.size()) return;

    // 落到哪个分组：就是盒子当前显示的那个分组（拖进来的图标跟当前标签页走）
    const int grp = g_fences[fi] ? g_fences[fi]->activeGroup() : 0;

    // 拖拽期间布局不再排除被拖图标（原位保留、画暗当空位），所以 DropHint 给的
    // 插入下标是按「完整列表」算的。删掉自己之后，它后面的下标都要减 1。
    // 只有「本来就在同一个盒子的同一个分组里」才需要减 —— 换组相当于新插入。
    const bool sameFence = (e->owner == fi && e->group == grp);
    int srcIdx = -1;
    if (sameFence) {
        srcIdx = 0;
        for (auto* o : g_icons) {
            if (o == e) break;
            if (o && o->owner == fi && o->group == grp) ++srcIdx;
        }
    }

    auto it = std::find(g_icons.begin(), g_icons.end(), e);
    if (it != g_icons.end()) {
        g_surface.ForgetHover();   // 悬停指针可能正指着 e，搬列表之前先丢掉
        g_icons.erase(it);
    }
    e->owner = fi;
    e->group = grp;
    e->selected = false;
    e->dragPos = e->pos;

    if (vis >= 0 && sameFence && vis > srcIdx) --vis;

    if (vis < 0) {
        g_icons.push_back(e);
        FitIconToOwner(e);
        return;
    }
    std::vector<DesktopIconView*> mine;
    for (auto* o : g_icons)
        if (o && o->owner == fi && o->group == grp) mine.push_back(o);
    if (vis >= (int)mine.size()) {
        g_icons.push_back(e);
        FitIconToOwner(e);
        return;
    }
    auto anchor = std::find(g_icons.begin(), g_icons.end(), mine[vis]);
    g_icons.insert(anchor, e);
    FitIconToOwner(e);
}

// ---------------------------------------------------------------------------
// 布局持久化
//
// owner（图标属于哪个盒子）以前只活在内存里，重启后盒子里空空如也。
// 这里把「归属 + 盒内顺序 + 桌面坐标」写进 %APPDATA%\meow-desk\meow-desk.db
// 的 icon_layout 表（老版本是 layout.ini，只用于迁移），启动时按 path 还原。
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
    e->group = 0;   // 回到桌面就不属于任何分组了
    e->selected = false;
    FitIconToOwner(e);   // 回桌面 → 位图换回 48px
    std::vector<DesktopIconView*> one{e};
    CommitPositions(one);
    SaveIconLayout();
}

// 把一批图标从盒子里倒回桌面（删除盒子 / 删除分组时用）。
// 落点从盒子原来所在的位置开始铺开，撞上已有图标会自动挪到最近的空格子；
// 整批只提交一次给资源管理器，比一个个走 PlaceOnDesktop 快得多。
static void SendIconsToDesktop(int fi, const std::vector<DesktopIconView*>& list) {
    if (list.empty()) return;
    RECT r{120, 120, 460, 600};
    if (fi >= 0 && fi < (int)g_fences.size() && g_fences[fi]) r = g_fences[fi]->rect();

    int i = 0;
    for (auto* e : list) {
        if (!e) continue;
        const POINT pt{r.left + g_cellW / 2 + (i % 5) * g_cellW,
                       r.top + 40 + (i / 5) * g_cellH};
        e->pos = g_surface.CellForCursor(pt, e->bmp.valid() ? &e->bmp : nullptr);
        e->dragPos = e->pos;
        e->owner = -1;
        e->group = 0;
        e->selected = false;
        FitIconToOwner(e);   // 回桌面 → 位图换回 48px
        ++i;
    }
    CommitPositions(list);
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
        if (le.owner >= 0) {
            le.order = nextOrder[le.owner]++;
            le.group = e->group;   // 图标属于盒子里的哪个分组
        }
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
            // 分组可能被删过，越界的一律退回第 0 组（否则图标会「消失」）
            const int gn = g_fences[le->owner] ? g_fences[le->owner]->groupCount() : 1;
            e->group = (le->group >= 0 && le->group < gn) ? le->group : 0;
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
// 「可见」= 属于盒子当前分组，和 FenceWindow::CollectVisible 的口径保持一致，
// 否则 DropHint 里的下标会指到别的分组去。
static DesktopIconView* FenceItemAt(int fi, int idx, DesktopIconView* skip) {
    if (fi < 0 || idx < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return nullptr;
    const int grp = g_fences[fi]->activeGroup();
    int k = 0;
    for (auto* o : g_icons) {
        if (!o || o->owner != fi || o->group != grp || o == skip) continue;
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
    // 盒子里可能是「大图标」，刚被统一成 48px 的得换回去
    for (int i = 0; i < (int)g_fences.size(); ++i) EnsureFenceIconSize(i);
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
            g_surface.ForgetHover();   // 马上要析构 e，先丢掉可能指着它的悬停指针
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
// 桌面格子尺寸
//
// 行距来自资源管理器的 GetSpacing（g_cellHFull）。「显示图标标题」关掉之后，
// 标签那一块就不占了，格子高度要把它的高度整块减掉（图标本身不变小）；
// 重新打开就加回来。拖动浮层的标签排版也走同一份尺寸。
// ---------------------------------------------------------------------------
static void ApplyCellSize() {
    const int iconBlock = DesktopSurface::kIconTopPad + kIconPx + DesktopSurface::kTextGap;
    const int labelH = std::max(14, g_cellHFull - iconBlock);
    g_cellH = (g_config.showLabels != 0) ? g_cellHFull : (g_cellHFull - labelH);
    if (g_cellH < iconBlock) g_cellH = iconBlock;

    g_surface.SetCellSize(g_cellW, g_cellH);
    const int textH = std::max(14, g_cellH - iconBlock);
    g_drag.SetLabelLayout(std::max(40, g_cellW - 10), textH, g_config.textShadow != 0,
                          g_config.showLabels != 0);
    // SetCellSize 只是改数字，得自己触发一次重画（悬停/选中框都按格子尺寸算）
    g_surface.Redraw();
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
    ApplyCellSize();   // 标题开关会改桌面格子高度
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
// 盒子右键菜单（自绘）——「新建盒子 / 新建文件夹 / 排序方式 / 查看 /
// 打开设置面板 / 重命名分区」。排序方式、查看带二级菜单，和资源管理器一致。
// ---------------------------------------------------------------------------
enum {
    kCmdNewFence = 4001,
    kCmdNewFolder = 4002,
    kCmdSortName = 4010,    // 4010..4014：名称 / 类型 / 大小 / 修改日期 / 自定义
    kCmdSortAsc = 4020,
    kCmdSortDesc = 4021,
    kCmdViewSmall = 4030,   // 4030..4035：小 / 中 / 大 / 超大 / 列表 / 详情
    kCmdSettings = 4040,
    kCmdRename = 4041,
    kCmdLock = 4042,
    kCmdNewGroup = 4043,      // 在头部新建一个分组标签
    kCmdRenameGroup = 4044,   // 重命名当前分组
    kCmdDeleteGroup = 4045,   // 删除当前分组（图标回桌面）
    kCmdDeleteFence = 4046,   // 删除整个盒子（图标回桌面）
    kCmdMoveToGroup = 4100,   // 4100..4127：盒内图标「移动到第 N 个分组」
};

static void AddFenceWindow(int i);
static void ShowFenceMenu(int fi, POINT sp, int item = -1);
static void BeginRenameGroup(int fi, int gi);
static void DeleteFence(int fi);
static void DeleteActiveGroup(int fi);
static void BeginRenameFence(int fi);

// 新建盒子 / 一键整理建的盒子都要继承「已有盒子的外观」。
// 外观（颜色、透明度、圆角、磨砂、查看方式、排序方式）在设置页是全局一起改的，
// 所以新盒子必须照着现有盒子来，不能留 FenceConfig 的默认值 ——
// 否则改完样式再一键整理，新出来的盒子跟原来的长得不一样。
static void InheritFenceStyle(FenceConfig& c) {
    if (g_config.fences.empty()) return;
    const FenceConfig& s = g_config.fences.front();
    c.color = s.color;
    c.alpha = s.alpha;
    c.radius = s.radius;
    c.acrylic = s.acrylic;
    c.viewMode = s.viewMode;
    c.sortKey = s.sortKey;
    c.sortAsc = s.sortAsc;
}

// 新盒子的尺寸也跟着现有盒子走（没有盒子时才用 340×380）。
// 一键整理一次可能建出好几个盒子，大小不一会显得很乱。
static void InheritFenceSize(int& w, int& h) {
    if (g_config.fences.empty()) return;
    const FenceConfig& s = g_config.fences.front();
    if (s.w >= 120) w = s.w;
    if (s.h >= 120) h = s.h;
}

// 新建盒子：摆在当前盒子右边，超出工作区就换到下一行
static void NewFenceNear(int fi) {
    int w = 340, h = 380;
    InheritFenceSize(w, h);
    RECT base{200, 200, 200 + w, 200 + h};
    if (fi >= 0 && fi < (int)g_fences.size() && g_fences[fi]) base = g_fences[fi]->rect();

    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(POINT{base.left, base.top}, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;

    int x = base.right + 20;
    int y = base.top;
    if (x + w > lim.right) {
        x = lim.left + 40;
        y = base.bottom + 20;
    }
    if (y + h > lim.bottom) y = lim.bottom - h;
    if (x < lim.left) x = lim.left;
    if (y < lim.top) y = lim.top;

    FenceConfig c;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.title = L"新盒子";
    InheritFenceStyle(c);
    g_config.fences.push_back(c);
    // 容器可能重新分配：先把已有盒子手里的配置指针重绑一遍，再建新的那个
    for (auto& f : g_fences)
        if (f) f->RebindConfig();
    AddFenceWindow((int)g_config.fences.size() - 1);
    g_config.Save();
    LogStep("new fence #" + std::to_string((int)g_config.fences.size() - 1) + " at " +
            std::to_string(x) + "," + std::to_string(y));
}

// ---------------------------------------------------------------------------
// 整理规则：一键整理 / 自动整理
//
// 参考 my-desktop 的行为：只改「归属」（图标进哪个盒子），文件始终留在桌面上，
// 所以整理是瞬间完成的，也不受跨盘、大文件影响。规则顺序就是优先级，
// 第一条命中的生效；isFallback 的兜底规则吃下谁都没匹配到的项目。
// ---------------------------------------------------------------------------
static std::wstring ExtLowerOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return std::wstring();
    if (sep != std::wstring::npos && dot < sep) return std::wstring();
    std::wstring ext = path.substr(dot);
    for (auto& ch : ext)
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
    return ext;
}

static bool IsDirectoryPath(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 按规则顺序取第一条命中的；都不命中返回兜底规则下标（没有兜底则 -1）
static int MatchRuleIndex(const std::wstring& path, bool isDir, bool& matched) {
    const std::wstring ext = ExtLowerOf(path);
    int fallback = -1;
    matched = false;
    for (size_t i = 0; i < g_config.rules.size(); ++i) {
        const OrganizeRule& r = g_config.rules[i];
        if (r.isFallback) {
            if (fallback < 0) fallback = (int)i;
            continue;
        }
        if (OrganizeRuleMatches(r, ext, isDir)) {
            matched = true;
            return (int)i;
        }
    }
    return fallback;
}

static int FindFenceByTitle(const std::wstring& t) {
    for (size_t i = 0; i < g_fences.size() && i < g_config.fences.size(); ++i) {
        if (g_fences[i] && g_config.fences[i].title == t) return (int)i;
    }
    return -1;
}

// 给某条规则新建盒子：从现有盒子往右摆，超出工作区换到下一行（和「新建盒子」一致）
static int CreateRuleFence(const std::wstring& title) {
    int w = 340, h = 380;
    InheritFenceSize(w, h);
    RECT base{200, 200, 200 + w, 200 + h};
    for (int i = (int)g_fences.size() - 1; i >= 0; --i) {
        if (g_fences[i] && g_fences[i]->hwnd()) {
            base = g_fences[i]->rect();
            break;
        }
    }
    RECT lim{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(POINT{base.left, base.top}, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;

    int x = base.right + 20;
    int y = base.top;
    if (x + w > lim.right) {
        x = lim.left + 40;
        y = base.bottom + 20;
    }
    if (y + h > lim.bottom) y = lim.bottom - h;
    if (x < lim.left) x = lim.left;
    if (y < lim.top) y = lim.top;

    FenceConfig c;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.title = title;
    InheritFenceStyle(c);
    g_config.fences.push_back(c);
    for (auto& f : g_fences)
        if (f) f->RebindConfig();   // 容器可能重分配，先把已有盒子重绑
    const int idx = (int)g_config.fences.size() - 1;
    AddFenceWindow(idx);
    LogStep("rule fence \"" + Narrow(title) + "\" #" + std::to_string(idx));
    return idx;
}

// 把一批图标按规则归入盒子；quiet = 自动整理（不弹提示、不报错到界面）
static void OrganizeIcons(const std::vector<DesktopIconView*>& cand, bool quiet) {
    (void)quiet;
    if (cand.empty() || g_config.rules.empty()) return;

    std::vector<std::vector<DesktopIconView*>> groups(g_config.rules.size());
    size_t total = 0;
    for (auto* e : cand) {
        if (!e) continue;
        bool matched = false;
        const int ri = MatchRuleIndex(e->path, IsDirectoryPath(e->path), matched);
        if (ri < 0 || ri >= (int)groups.size()) continue;
        groups[ri].push_back(e);
        ++total;
    }
    if (!total) {
        LogStep("organize: nothing matched");
        return;
    }

    bool changed = false;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].empty()) continue;
        std::wstring name = g_config.rules[i].name;
        if (name.empty()) name = L"\u672a\u547d\u540d";   // 未命名
        int fi = FindFenceByTitle(name);
        if (fi < 0) fi = CreateRuleFence(name);
        if (fi < 0) continue;
        for (auto* e : groups[i]) InsertIntoFence(e, fi, -1);
        changed = true;
        LogStep("organize: " + Narrow(name) + " <- " + std::to_string(groups[i].size()));
    }
    if (changed) {
        SaveIconLayout();
        g_config.Save();
        RedrawAll();
    }
}

// 立即整理：桌面上还没进盒子的图标全部按规则归类
static void OrganizeDesktopNow() {
    std::vector<DesktopIconView*> cand;
    for (auto* e : g_icons)
        if (e && e->OnDesktop()) cand.push_back(e);
    if (cand.empty()) {
        LogStep("organize: no desktop icons");
        return;
    }
    OrganizeIcons(cand, false);
}

// 在真实桌面上新建文件夹（和资源管理器一样挑一个不重名的名字）
static void NewDesktopFolder() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
        dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) return;

    std::wstring made;
    for (int i = 1; i <= 200 && made.empty(); ++i) {
        std::wstring path = dir + L"\\\u65b0\u5efa\u6587\u4ef6\u5939";
        if (i > 1) path += L" (" + std::to_wstring(i) + L")";
        if (CreateDirectoryW(path.c_str(), nullptr))
            made = path;
        else if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    if (made.empty()) return;
    LogStep("new folder " + Narrow(made));
    // 不依赖 shell 通知，自己重新枚举一次，图标立刻出现
    ReloadDesktopIcons();
}

// ---- 重命名分区 ----
// 盒子是 WS_EX_NOACTIVATE 的分层窗口，拿不到键盘焦点，所以用一个能激活的小弹窗
// 贴着盒子头部显示（回车提交 / Esc 取消 / 失去焦点也提交）。
static HWND g_renameWnd = nullptr;
static HWND g_renameEdit = nullptr;
static WNDPROC g_renameEditProc = nullptr;
static int g_renameFence = -1;
static int g_renameGroup = -1;   // >= 0 = 改的是这个分组标签；-1 = 改盒子标题

static void FinishRename(bool commit) {
    if (!g_renameWnd) return;
    const int fi = g_renameFence;
    const int gi = g_renameGroup;
    std::wstring text;
    if (commit && g_renameEdit) {
        // 用 WM_GETTEXT 而不是 GetWindowTextW：输入框的文本可能是别的进程
        // （自动化脚本）通过 WM_SETTEXT 写进来的，让 EDIT 自己回答更可靠
        wchar_t buf[256]{};
        SendMessageW(g_renameEdit, WM_GETTEXT, (WPARAM)255, (LPARAM)buf);
        text = buf;
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == L' ' || text[a] == L'\t')) ++a;
        while (b > a && (text[b - 1] == L' ' || text[b - 1] == L'\t')) --b;
        text = text.substr(a, b - a);
    }
    HWND wnd = g_renameWnd;
    g_renameWnd = nullptr;
    g_renameEdit = nullptr;
    g_renameFence = -1;
    g_renameGroup = -1;
    DestroyWindow(wnd);

    if (commit && !text.empty() && fi >= 0 && fi < (int)g_fences.size() && g_fences[fi]) {
        // SetTitle / SetGroupName 内部都会存盘
        if (gi >= 0) g_fences[fi]->SetGroupName(gi, text);
        else g_fences[fi]->SetTitle(text);
        LogStep("rename fence #" + std::to_string(fi) + (gi >= 0 ? " group " : " -> ") +
                (gi >= 0 ? std::to_string(gi) + " -> " : std::string()) + Narrow(text));
    }
}

static LRESULT CALLBACK RenameEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            FinishRename(true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            FinishRename(false);
            return 0;
        }
    }
    // 回车/ESC 已经被吃掉，别再走 WM_CHAR，否则每敲一下都 ding
    if (m == WM_CHAR && (w == L'\r' || w == L'\n' || w == 27)) return 0;
    return CallWindowProcW(g_renameEditProc, h, m, w, l);
}

static LRESULT CALLBACK RenameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) FinishRename(true);
        return 0;
    case WM_CLOSE:
        FinishRename(false);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// 真正建输入框：x 是屏幕坐标（改分组时贴在对应标签上），w 是输入框宽度
static void BeginRenameBoxAt(int fi, int group, const std::wstring& initial, int x, int w,
                             int headH) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    RECT r = g_fences[fi]->rect();
    const int h = std::max(24, headH - 14);
    if (w < 40) return;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RenameProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MeowDeskRename";
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    g_renameFence = fi;
    g_renameGroup = group;
    g_renameWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"MeowDeskRename", L"",
                                  WS_POPUP, x, r.top + (headH - h) / 2, w, h, nullptr, nullptr,
                                  inst, nullptr);
    if (!g_renameWnd) {
        g_renameFence = -1;
        g_renameGroup = -1;
        return;
    }

    g_renameEdit = CreateWindowExW(0, L"EDIT", initial.c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 3, 2,
                                   w - 6, h - 4, g_renameWnd, (HMENU)1, inst, nullptr);
    if (g_renameEdit) {
        SendMessageW(g_renameEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(g_renameEdit, EM_SETSEL, 0, -1);
        g_renameEditProc = (WNDPROC)SetWindowLongPtrW(g_renameEdit, GWLP_WNDPROC,
                                                      (LONG_PTR)RenameEditProc);
    }
    ShowWindow(g_renameWnd, SW_SHOW);
    SetForegroundWindow(g_renameWnd);
    if (g_renameEdit) SetFocus(g_renameEdit);
}

// group < 0 = 改盒子标题；group >= 0 = 改第 group 个分组标签
static void BeginRenameBox(int fi, int group, const std::wstring& initial) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    FenceWindow* f = g_fences[fi].get();
    if (g_renameWnd) FinishRename(false);

    RECT r = f->rect();
    const int headH = f->headerH();
    int x = r.left + 12;
    int w = std::min<int>((int)(r.right - r.left) - 24, 260);
    // 改分组时输入框摆到那个标签上（宽度也跟着标签走），改标题就贴头部左边
    if (group >= 0) {
        const auto tabs = f->TabRects();
        if (group < (int)tabs.size() && tabs[group].right > tabs[group].left) {
            x = r.left + tabs[group].left;
            const int avail = (int)(r.right - r.left) - (int)(x - r.left) - 8;
            const int tabW = (int)(tabs[group].right - tabs[group].left) + 48;
            w = std::max(60, std::min(avail, tabW));
        }
    }
    BeginRenameBoxAt(fi, group, initial, x, w, headH);
}

static void BeginRenameFence(int fi) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    BeginRenameBox(fi, -1, g_fences[fi]->title());
}

static void BeginRenameGroup(int fi, int gi) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    BeginRenameBox(fi, gi, g_fences[fi]->GroupName(gi));
}

static void ShowFenceMenu(int fi, POINT sp, int item) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    FenceWindow* f = g_fences[fi].get();

    const int curSort = f->sortKey();
    const bool asc = f->sortAsc();
    const int curView = f->viewMode();

    std::vector<PopupMenuItem> sortSub;
    static const wchar_t* kSortNames[] = {L"\u540d\u79f0", L"\u7c7b\u578b", L"\u5927\u5c0f",
                                          L"\u4fee\u6539\u65e5\u671f",
                                          L"\u81ea\u5b9a\u4e49\u6392\u5e8f"};
    for (int i = 0; i < 5; ++i) {
        PopupMenuItem m(kSortNames[i], kCmdSortName + i);
        m.checked = (curSort == i);
        sortSub.push_back(std::move(m));
    }
    sortSub.push_back(PopupMenuItem::Sep());
    {
        PopupMenuItem m(L"\u6b63\u5e8f", kCmdSortAsc);   // 正序
        m.checked = asc;
        sortSub.push_back(std::move(m));
    }
    {
        PopupMenuItem m(L"\u5012\u5e8f", kCmdSortDesc);  // 倒序
        m.checked = !asc;
        sortSub.push_back(std::move(m));
    }

    std::vector<PopupMenuItem> viewSub;
    static const wchar_t* kViewNames[] = {
        L"\u5c0f\u56fe\u6807",       // 小图标
        L"\u4e2d\u7b49\u56fe\u6807",  // 中等图标
        L"\u5927\u56fe\u6807",       // 大图标
        L"\u8d85\u5927\u56fe\u6807",  // 超大图标
        L"\u5217\u8868",             // 列表
        L"\u8be6\u7ec6\u4fe1\u606f"};  // 详细信息
    for (int i = 0; i < 6; ++i) {
        PopupMenuItem m(kViewNames[i], kCmdViewSmall + i);
        m.checked = (curView == i);
        viewSub.push_back(std::move(m));
    }

    std::vector<PopupMenuItem> items;
    // 右键点在某张图标上 + 盒子有分组：给一条「把这个图标挪到别的分组」的路。
    // （标签页只显示一个分组，拖拽没法跨分组，只能走菜单。）
    if (item >= 0 && f->hasTabs()) {
        std::vector<PopupMenuItem> moveSub;
        const int n = std::min(28, f->groupCount());
        for (int g = 0; g < n; ++g) {
            PopupMenuItem m(f->GroupName(g), kCmdMoveToGroup + g);
            m.checked = (g == f->activeGroup());
            moveSub.push_back(std::move(m));
        }
        PopupMenuItem m(L"\u79fb\u52a8\u5230\u5206\u7ec4", 0);   // 移动到分组
        m.sub = std::move(moveSub);
        items.push_back(std::move(m));
        items.push_back(PopupMenuItem::Sep());
    }
    items.push_back(PopupMenuItem(L"\u65b0\u5efa\u76d2\u5b50", kCmdNewFence));      // 新建盒子
    items.push_back(PopupMenuItem(L"\u65b0\u5efa\u6587\u4ef6\u5939", kCmdNewFolder));  // 新建文件夹
    items.push_back(PopupMenuItem::Sep());
    {
        PopupMenuItem m(L"\u6392\u5e8f\u65b9\u5f0f", 0);   // 排序方式
        m.sub = std::move(sortSub);
        items.push_back(std::move(m));
    }
    {
        PopupMenuItem m(L"\u67e5\u770b", 0);                // 查看
        m.sub = std::move(viewSub);
        items.push_back(std::move(m));
    }
    items.push_back(PopupMenuItem::Sep());
    items.push_back(
        PopupMenuItem(L"\u6253\u5f00\u8bbe\u7f6e\u9762\u677f", kCmdSettings));  // 打开设置面板
    items.push_back(PopupMenuItem(L"\u91cd\u547d\u540d\u5206\u533a", kCmdRename));  // 重命名分区
    // 分组：头部的标签页。切分组 = 换一批图标，所以「新建分组」进来是空的。
    items.push_back(PopupMenuItem(L"\u521b\u5efa\u5206\u7ec4", kCmdNewGroup));  // 创建分组
    if (f->hasTabs()) {
        items.push_back(PopupMenuItem(L"\u91cd\u547d\u540d\u5206\u7ec4", kCmdRenameGroup));
    }
    // 头部右侧的锁按钮在「显示盒子标题」关掉时是看不见的，这里留一个出口，
    // 免得锁上之后没法再解开。
    items.push_back(PopupMenuItem(f->locked() ? L"\u89e3\u9501\u4f4d\u7f6e"    // 解锁位置
                                              : L"\u9501\u5b9a\u4f4d\u7f6e",   // 锁定位置
                                  kCmdLock));
    // 删除类放最下面，和上面隔开，免得误点。
    // 「删除分组」始终占一行（灰掉比消失更好懂）：第 0 个标签就是盒子标题本身，
    // 它不能被删 —— 要删盒子请用下面的「删除盒子」。
    items.push_back(PopupMenuItem::Sep());
    {
        PopupMenuItem m(L"\u5220\u9664\u5206\u7ec4", kCmdDeleteGroup);   // 删除分组
        m.disabled = !f->hasTabs() || f->activeGroup() <= 0;
        items.push_back(std::move(m));
    }
    items.push_back(PopupMenuItem(L"\u5220\u9664\u76d2\u5b50", kCmdDeleteFence));  // 删除盒子

    if (!g_fenceMenu.Create()) return;
    g_fenceMenu.SetOwner(f->hwnd());
    g_fenceMenu.onCommand = [fi, item](int id) {
        FenceWindow* w = (fi < (int)g_fences.size()) ? g_fences[fi].get() : nullptr;
        if (!w) return;
        if (id >= kCmdMoveToGroup && id < kCmdMoveToGroup + 28) {
            const int g = id - kCmdMoveToGroup;
            DesktopIconView* e = FenceItemAt(fi, item, nullptr);
            if (e && g >= 0 && g < w->groupCount() && g != e->group) {
                // 从当前位置摘出来再追加到目标分组末尾：分组内的顺序就是
                // g_icons 里的相对顺序，所以「追加」= 排到那一组的最后。
                auto it = std::find(g_icons.begin(), g_icons.end(), e);
                if (it != g_icons.end()) {
                    g_surface.ForgetHover();
                    g_icons.erase(it);
                }
                e->group = g;
                g_surface.ForgetHover();
                g_icons.push_back(e);
                SaveIconLayout();
                RedrawAll();
            }
            return;
        }
        if (id == kCmdNewFence) { NewFenceNear(fi); return; }
        if (id == kCmdNewFolder) { NewDesktopFolder(); return; }
        if (id == kCmdSettings) { g_settings.Show(); return; }
        if (id == kCmdRename) { BeginRenameFence(fi); return; }
        if (id == kCmdNewGroup) {
            // 先建出来（默认名「分组 N」并切过去），再直接打开输入框让人改名 ——
            // 空标签既没法点也不好认，不如一步到位。
            const int gi = w->AddGroup(std::wstring());
            RedrawAll();
            if (gi >= 0) BeginRenameGroup(fi, gi);
            return;
        }
        if (id == kCmdRenameGroup) { BeginRenameGroup(fi, w->activeGroup()); return; }
        if (id == kCmdDeleteGroup) {
            if (w->activeGroup() <= 0) return;   // 第一个标签是盒子标题，删不掉
            int cnt = 0;
            for (auto* e : g_icons)
                if (e && e->owner == fi && e->group == w->activeGroup()) ++cnt;
            const std::wstring gname = w->activeGroupName();
            g_confirm.Show(w->hwnd(), L"\u5220\u9664\u5206\u7ec4",   // 删除分组
                           L"\u201c" + gname + L"\u201d\u91cc\u7684 " + std::to_wstring(cnt) +
                               L" \u4e2a\u56fe\u6807\u4f1a\u56de\u5230\u684c\u9762\u3002\n"
                               L"\u5206\u7ec4\u6807\u7b7e\u5c06\u88ab\u5220\u9664\uff0c"
                               L"\u6587\u4ef6\u672c\u8eab\u4e0d\u53d7\u5f71\u54cd\u3002",
                           L"\u5220\u9664", [fi] { DeleteActiveGroup(fi); });
            return;
        }
        if (id == kCmdDeleteFence) {
            int cnt = 0;
            for (auto* e : g_icons)
                if (e && e->owner == fi) ++cnt;
            const std::wstring t = w->title();
            g_confirm.Show(w->hwnd(), L"\u5220\u9664\u76d2\u5b50",   // 删除盒子
                           L"\u201c" + t + L"\u201d\u548c\u91cc\u9762\u7684 " +
                               std::to_wstring(cnt) +
                               L" \u4e2a\u56fe\u6807\u90fd\u4f1a\u56de\u5230\u684c\u9762\u3002\n"
                               L"\u76d2\u5b50\u672c\u8eab\u4f1a\u88ab\u5220\u9664\uff0c"
                               L"\u6587\u4ef6\u4e0d\u4f1a\u4e22\u3002",
                           L"\u5220\u9664", [fi] { DeleteFence(fi); });
            return;
        }
        if (id == kCmdLock) { w->ToggleLock(); return; }
        if (id >= kCmdSortName && id < kCmdSortName + 5) {
            // 换排序字段时保留当前的正序/倒序
            w->SetSort(id - kCmdSortName, w->sortAsc());
            return;
        }
        if (id == kCmdSortAsc) { w->SetSort(w->sortKey(), true); return; }
        if (id == kCmdSortDesc) { w->SetSort(w->sortKey(), false); return; }
        if (id >= kCmdViewSmall && id < kCmdViewSmall + 6) {
            w->SetViewMode(id - kCmdViewSmall);
            return;
        }
    };
    g_fenceMenu.Show(sp, std::move(items));
    LogStep("fence menu #" + std::to_string(fi));
}

// ---------------------------------------------------------------------------
// 盒子（动态增删：菜单里的「新建盒子」要能加）
// ---------------------------------------------------------------------------
static void AddFenceWindow(int i) {
    if (i < 0 || i >= (int)g_config.fences.size()) return;
    auto f = std::make_unique<FenceWindow>();
    const FenceConfig c = g_config.fences[i];
    f->SetIcons(&g_icons);
    f->onOpen = [](DesktopIconView* e) { OpenPath(e->path); };
    f->onGeometryChanged = [i] {
        if (i >= (int)g_fences.size()) return;
        RECT r = g_fences[i]->rect();
        if (i < (int)g_config.fences.size()) {
            g_config.fences[i].x = r.left;
            g_config.fences[i].y = r.top;
            g_config.fences[i].w = r.right - r.left;
            g_config.fences[i].h = r.bottom - r.top;
            g_config.Save();
        }
        g_fences[i]->ApplyConfig(true);   // 亚克力底图要跟着新位置重抓
    };
    f->onContextMenu = [i](int item, POINT sp) { ShowFenceMenu(i, sp, item); };
    // 「查看 / 排序 / 重命名」改完之后存盘：配置（viewMode 等）+ 图标顺序
    f->onStateChanged = [i] {
        EnsureFenceIconSize(i);
        g_config.Save();
        SaveIconLayout();
        RedrawAll();
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

    if (!f->Create(i, c.x, c.y, c.w, c.h))
        LogStep("fence create FAILED " + std::to_string(i));
    // 即使创建失败也占位，保证 g_fences 下标 == 配置下标
    g_fences.push_back(std::move(f));

    if (g_fences[i] && g_fences[i]->hwnd()) {
        InstallKeepAbove(g_fences[i]->hwnd(), [i] { return FenceAnchor(i); });
        PlaceAbove(g_fences[i]->hwnd(), FenceAnchor(i));
        // 拖动时避让其他盒子（用实时窗口矩形，收起状态的盒子只占一条头部）
        g_fences[i]->SetObstacles([i] {
            std::vector<RECT> v;
            for (size_t k = 0; k < g_fences.size(); ++k) {
                if ((int)k == i) continue;
                if (g_fences[k] && g_fences[k]->hwnd()) v.push_back(g_fences[k]->rect());
            }
            return v;
        });
    }
}

// ---------------------------------------------------------------------------
// 删除盒子 / 删除分组
//
// 盒子下标是「数组下标」，删掉中间一个会把后面所有盒子的下标往前挪一格：
//   · FenceWindow::index_ / cfg_
//   · 各种 [i] 捕获的回调（onGeometryChanged / onContextMenu / onStateChanged /
//     onItemDrag* / SetObstacles / InstallKeepAbove 的锚点）
//   · 图标的 owner
// 原地改这些太容易漏，所以先把窗口全销毁、再按新下标整批重建一遍。
// ---------------------------------------------------------------------------
static void RebuildFences();

// 删掉当前分组：组里的图标回到桌面，剩下的组各回各位
static void DeleteActiveGroup(int fi) {
    if (fi < 0 || fi >= (int)g_fences.size() || !g_fences[fi]) return;
    FenceWindow* w = g_fences[fi].get();
    const int g = w->activeGroup();
    if (g <= 0) return;   // 第一个标签就是盒子标题，删不掉

    std::vector<DesktopIconView*> moved;
    for (auto* e : g_icons)
        if (e && e->owner == fi && e->group == g) moved.push_back(e);
    SendIconsToDesktop(fi, moved);

    w->RemoveActiveGroup();   // 剩下的组内图标由它负责重新编号
    g_config.Save();
    SaveIconLayout();
    RedrawAll();
    LogStep("delete group #" + std::to_string(g) + " of fence " + std::to_string(fi) +
            " (" + std::to_string(moved.size()) + " icons to desktop)");
}

// 删掉整个盒子：盒子里所有分组的所有图标回到桌面
static void DeleteFence(int fi) {
    if (fi < 0 || fi >= (int)g_fences.size() || fi >= (int)g_config.fences.size()) return;

    std::vector<DesktopIconView*> moved;
    for (auto* e : g_icons)
        if (e && e->owner == fi) moved.push_back(e);
    SendIconsToDesktop(fi, moved);

    // 摘掉窗口
    if (g_fences[fi]) g_fences[fi]->Destroy();
    g_fences.erase(g_fences.begin() + fi);
    g_config.fences.erase(g_config.fences.begin() + fi);

    // 后面的盒子整体前移一格
    for (auto* e : g_icons)
        if (e && e->owner > fi) --e->owner;

    RebuildFences();
    g_config.Save();
    SaveIconLayout();
    RedrawAll();
    LogStep("delete fence " + std::to_string(fi) + " (" + std::to_string(moved.size()) +
            " icons to desktop)");
}

// 按当前 g_config.fences 把所有盒子窗口重建一遍（下标变了之后必须走这条路）
static void RebuildFences() {
    for (auto& f : g_fences)
        if (f) f->Destroy();
    g_fences.clear();
    for (int i = 0; i < (int)g_config.fences.size(); ++i) AddFenceWindow(i);
    RestoreZOrder();
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

    // 盒子/桌面层都已 Destroy，这里必须先把悬停指针丢掉再析构图标对象
    g_surface.ForgetHover();
    for (auto* e : g_icons) {
        if (e && e->pidl) { FreeDesktopPidl(e->pidl); e->pidl = nullptr; }
    }
    g_iconStore.clear();
    g_icons.clear();

    SetSystemDesktopIconsVisible(true);

    storage::Close();   // 关掉数据库，释放 page cache / 文件句柄
}

// ---------------------------------------------------------------------------
// 自动整理：桌面上新出现的文件几秒后自动按规则归类
//
// 没有用 ReadDirectoryChangesW / FileSystemWatcher：只要 4 秒扫一次桌面文件夹
// （几十个条目，开销可以忽略）就够了，还不会被 Explorer 重启、OneDrive 迁移目录
// 之类的事情搞坏。判「新」用创建时间 —— 同盘移动会保留创建时间，所以从别处
// 移过来的旧文件不会被误收。
// ---------------------------------------------------------------------------
static std::unordered_set<std::wstring> g_knownDesk;
static ULONGLONG g_procStartFt = 0;
static DWORD g_lastAutoCheck = 0;

static ULONGLONG FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// 把当前认识的桌面项目全部记下来（启动时、手动整理后各调一次）
static void NoteKnownDesktopPaths() {
    g_knownDesk.clear();
    for (auto* e : g_icons)
        if (e && !e->path.empty()) g_knownDesk.insert(e->path);
}

static void CheckAutoOrganize() {
    if (!g_config.autoOrganize) return;
    const DWORD now = GetTickCount();
    if (g_lastAutoCheck && now - g_lastAutoCheck < 4000) return;
    g_lastAutoCheck = now;

    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) || !p) return;
    std::wstring dir = p;
    CoTaskMemFree(p);

    bool fresh = false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (g_knownDesk.count(full)) continue;
        if (FileTimeToU64(fd.ftCreationTime) < g_procStartFt) {
            g_knownDesk.insert(full);   // 老文件，只是我们不认识：记下来，别再扫它
            continue;
        }
        fresh = true;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!fresh) return;

    // 有新项目 → 重新枚举一次图标（得先有图标对象才能归类），再把「不认识的那些」收进去
    ReloadDesktopIcons();
    std::vector<DesktopIconView*> added;
    for (auto* e : g_icons) {
        if (!e || !e->OnDesktop() || e->path.empty()) continue;
        if (g_knownDesk.count(e->path)) continue;
        g_knownDesk.insert(e->path);
        added.push_back(e);
    }
    if (!added.empty()) OrganizeIcons(added, true);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 物理像素坐标系：不清这个，DWM 会把分层窗口整体拉伸，图标必然发虚
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        g_procStartFt = FileTimeToU64(ft);
    }

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
    g_cellHFull = sp.y > 0 ? sp.y : 98;
    g_cellH = g_cellHFull;

    // 3. 隐藏系统桌面图标，交给我们的桌面层
    g_systemIconsHidden = SetSystemDesktopIconsVisible(false);
    LogStep(g_systemIconsHidden ? "system icons hidden" : "hide system icons FAILED");

    // 4. 拖拽浮层（最先创建，层级最后摆）
    g_drag.Create();

    // 5. 桌面层
    g_surface.SetIcons(&g_icons);
    ApplyCellSize();   // 格子尺寸（含「显示标题」开关的影响）+ 拖拽浮层标签排版
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
    for (int i = 0; i < (int)g_config.fences.size(); ++i) AddFenceWindow(i);
    LogStep("fences created: " + std::to_string(g_fences.size()));

    // 盒子建好之后才能校验盒号 → 在这里还原上次的图标布局
    ApplyIconLayout();
    // 还原完归属才知道哪些图标在盒子里，这时再把它们换成「查看」档位对应的尺寸
    for (int i = 0; i < (int)g_fences.size(); ++i) EnsureFenceIconSize(i);
    // 自动整理只认「启动之后新出现的」项目，先把现有的都记下来
    NoteKnownDesktopPaths();
    RedrawAll();   // 盒子创建时就已经画过一次（那时还是空的），还原后必须重画

    // 7. 设置窗口 + 托盘 + 搜索面板
    g_settings.Create(nullptr);
    g_settings.onChanged = [] { ApplySettings(); };
    g_settings.onReloadIcons = [] { ReloadDesktopIcons(); };
    g_settings.getSystemIconsShown = [] { return !g_systemIconsHidden; };
    g_settings.setSystemIconsShown = [](bool shown) { SetDesktopIconsShown(shown); };
    g_settings.onOrganize = [] { OrganizeDesktopNow(); };

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

    // 8. 心跳：把层级摆回来（Win+D、Explorer 重启等）+ 自动整理巡检
    g_surface.onTick = [] {
        RestoreZOrder();
        CheckAutoOrganize();
    };

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
