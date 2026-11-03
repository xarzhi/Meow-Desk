#include "desktop_surface.h"

#include "icon_render.h"
#include "file_drop.h"
#include "shell_icons.h"
#include "storage.h"
// SetDesktopIconPositions：新建/粘贴后的图标要把位置写回真实桌面，
// 否则下一次重载就被 Explorer 的旧坐标冲回左上角。
#include "desktop_icons.h"

#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const wchar_t* kClassName = L"MeowDeskSurface";

const int kIconTopPad = DesktopSurface::kIconTopPad;
const int kTextGap = DesktopSurface::kTextGap;
const int kLabelInset = 10;    // 标签左右内缩（对齐资源管理器的折行宽度）
// 框选松手后弹「创建盒子」菜单的最小框尺寸：小于它就是「点了下空白处」，
// 那种情况弹菜单会很烦（用户只是想取消选中）。
const int kMarqueeMenuMin = 30;
// 桌面上一个图标都没有时，用来当网格第一格的那个小留白。
// 有图标时不用它 —— 那种情况能从真实图标反推出资源管理器的准确留白（见
// DesktopSurface::GridOrigin）。这个值只影响「空桌面上第一次摆图标」。
const int kEmptyGridPad = 8;

inline int SnapTo(int v, int step) {
    if (step <= 0) return v;
    return (int)std::lround((double)v / (double)step) * step;
}

// 在预乘 alpha 的 32bpp 缓冲上填一个圆角矩形。radius 像素的四个圆角逐行收缩，
// 收进量 = radius - sqrt(radius^2 - (radius-dy)^2)。
void FillRoundRectAlpha(BYTE* dst, int dstW, int dstH, const RECT& r, int radius, int a,
                        COLORREF color) {
    if (!dst || a <= 0) return;
    if (r.right <= r.left || r.bottom <= r.top) return;
    const int rw = (int)(r.right - r.left);
    const int rh = (int)(r.bottom - r.top);
    const int rad = std::max(0, std::min(radius, std::min(rw, rh) / 2));
    for (int y = r.top; y < r.bottom; ++y) {
        int inset = 0;
        if (rad > 0) {
            const int dy = std::min(y - r.top, (r.bottom - 1) - y);
            if (dy < rad) {
                const int t = rad - dy;
                inset = rad - (int)std::lround(std::sqrt((double)(rad * rad - t * t)));
            }
        }
        RECT line{r.left + inset, y, r.right - inset, y + 1};
        FillRectAlpha(dst, dstW, dstH, line, a, color);
    }
}

// 圆角矩形的「描边环」：只画外圈 th 像素，中间留空。
// 不能先用高 alpha 填满整块再叠低 alpha —— 那会让中间也一样实，看着像实心色块。
void StrokeRoundRectAlpha(BYTE* dst, int dstW, int dstH, const RECT& r, int radius, int th,
                          int a, COLORREF color) {
    if (!dst || a <= 0 || th <= 0) return;
    if (r.right <= r.left || r.bottom <= r.top) return;
    const int rw = (int)(r.right - r.left);
    const int rh = (int)(r.bottom - r.top);
    const int rad = std::max(0, std::min(radius, std::min(rw, rh) / 2));

    auto extent = [&](int y, int& left, int& right) {
        int inset = 0;
        if (rad > 0) {
            const int dy = std::min(y - r.top, (r.bottom - 1) - y);
            if (dy < rad) {
                const int t = rad - dy;
                inset = rad - (int)std::lround(std::sqrt((double)(rad * rad - t * t)));
            }
        }
        left = r.left + inset;
        right = r.right - inset;
    };

    for (int y = r.top; y < r.bottom; ++y) {
        int L = 0, R = 0;
        extent(y, L, R);
        if (R <= L) continue;
        if (y < r.top + th || y >= r.bottom - th || L + th >= R) {
            RECT line{L, y, R, y + 1};   // 上/下边框整行，或太窄时整行
            FillRectAlpha(dst, dstW, dstH, line, a, color);
        } else {
            RECT l1{L, y, L + th, y + 1};
            RECT l2{R - th, y, R, y + 1};
            FillRectAlpha(dst, dstW, dstH, l1, a, color);
            FillRectAlpha(dst, dstW, dstH, l2, a, color);
        }
    }
}

RECT Normalize(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

bool Intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void LogSurface(const char* fmt, ...) {
    FILE* f = nullptr;
    _wfopen_s(&f, storage::LogFile().c_str(), L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

bool TraceOn() {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableW(L"MEOW_TRACE", nullptr, 0) > 0 ? 1 : 0;
    return on == 1;
}

// ---------------------------------------------------------------------------
// 桌面右键菜单：因为整层桌面都被我们接管了（不接管就没法框选），
// 原生桌面的右键菜单不会再出现，这里把资源管理器的原生 IContextMenu 接回来——
// 「查看 / 排序方式 / 刷新 / 新建 / 显示设置 / 个性化」全是系统提供的真菜单。
// ---------------------------------------------------------------------------

// 菜单弹出期间，带图标的项是自绘的：WM_MEASUREITEM / WM_DRAWITEM /
// WM_INITMENUPOPUP 必须转给 IContextMenu2，否则这些项会变成 0 高度看不见
// （现象：只剩「创建快捷方式 / 删除 / 属性」三项）。
IContextMenu2* g_menuOwner = nullptr;

// 估算弹出菜单的像素高度。系统自带的翻转逻辑在「菜单比两个方向都高」时会直接裁掉
// 下半部分（现象：屏幕底部的图标右键只显示 3 项），所以先自己把锚点挪进来。
int EstimateMenuHeight(HMENU menu) {
    const int n = GetMenuItemCount(menu);
    if (n <= 0) return 0;
    const int itemH = GetSystemMetrics(SM_CYMENUCHECK) + 2;  // 单行项高度
    const int sepH = GetSystemMetrics(SM_CYMENUCHECK) / 2 + 2;
    int h = 0;
    for (int i = 0; i < n; ++i) {
        wchar_t txt[2]{};
        GetMenuStringW(menu, i, txt, 1, MF_BYPOSITION);
        const UINT st = GetMenuState(menu, i, MF_BYPOSITION);
        h += (st == 0xFFFFFFFFu || (txt[0] == 0 && st & MF_SEPARATOR)) ? sepH : itemH;
    }
    return h + GetSystemMetrics(SM_CYEDGE) * 2 + 4;
}

// 菜单里的「重命名」由宿主接管（见 desktop_surface.h 的说明）。
std::function<bool(HWND, const std::wstring&)> g_renameHandler;

// 这个命令号是不是「重命名」。
//
// 坑：GetCommandString 的第一个参数是**相对 idCmdFirst 的命令偏移**，不是菜单位置。
// 菜单里一堆分隔符和自绘项占着位置，用位置去问会拿到隔壁项的动词 ——
// 实测按位置问出来是 link/delete/rename/properties 整体错位。
// 所以这里按「偏移」扫一遍，找到动词为 rename 的那个偏移，换算回命令号比较。
bool IsRenameCommand(IContextMenu* cm, HMENU menu, int cmd) {
    if (cmd <= 0) return false;
    const int count = GetMenuItemCount(menu);

    for (int k = 0; k < count + 8; ++k) {
        wchar_t verb[64]{};
        // IContextMenu 的签名是 ANSI 的，但传 GCS_VERBW 时它按宽字符写缓冲区，
        // 所以这里必须强转 —— 这是 shell 接口的老习惯用法。
        if (FAILED(cm->GetCommandString(k, GCS_VERBW, nullptr,
                                        reinterpret_cast<LPSTR>(verb), (UINT)sizeof(verb))))
            continue;
        if (!verb[0]) continue;
        if (_wcsicmp(verb, L"rename") == 0) return cmd == k + 1;
    }

    // 兜底：拿不到动词时按菜单文字对（中文 "重命名" / 英文 "Rename"）。
    // 位置 → 命令号用 GetMenuItemID 反查，不要再拿位置当命令号。
    for (int pos = 0; pos < count; ++pos) {
        if ((int)GetMenuItemID(menu, pos) != cmd) continue;
        wchar_t text[128]{};
        if (GetMenuStringW(menu, pos, text, 128, MF_BYPOSITION) > 0 &&
            (wcsstr(text, L"重命名") || wcsstr(text, L"Rename")))
            return true;
    }
    return false;
}

void InvokeShellMenu(HWND hwnd, IContextMenu* cm, HMENU menu, POINT pt,
                     const std::wstring& path = std::wstring()) {
    IContextMenu2* cm2 = nullptr;
    cm->QueryInterface(IID_IContextMenu2, (void**)&cm2);
    g_menuOwner = cm2;

    // 把锚点钳制到「整张菜单都放得下」的位置
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        const int mh = EstimateMenuHeight(menu);
        if (mh > 0) {
            const int maxY = mi.rcWork.bottom - mh;
            if (pt.y > maxY) pt.y = maxY;
            if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;
        }
    }

    // 菜单是模态的：必须先有前台窗口，否则 TrackPopupMenu 会立刻返回、
    // 菜单一闪就没了（surface 是 WS_EX_NOACTIVATE，天然不满足这个条件）。
    HWND prevFg = GetForegroundWindow();
    SetForegroundWindow(hwnd);

    const DWORD t0 = GetTickCount();
    const int cmd = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd,
        nullptr);
    // 菜单关掉之后必须给自己发一条空消息，否则菜单的鼠标捕获不会释放，
    // 表现为"点一下别的窗口菜单才真正消失"。
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (TraceOn())
        LogSurface("menu cmd=%d elapsed=%lu ms", cmd, (unsigned long)(GetTickCount() - t0));

    g_menuOwner = nullptr;
    if (cm2) cm2->Release();
    if (cmd <= 0) {
        if (prevFg && prevFg != hwnd) SetForegroundWindow(prevFg);
        return;
    }

    if (TraceOn()) {
        wchar_t txt[128]{};
        GetMenuStringW(menu, cmd - 1, txt, 128, MF_BYPOSITION);
        LogSurface("menu pick cmd=%d item=%ls", cmd, txt);
    }

    // 点的是「重命名」→ 交给宿主自己弹输入框。
    // 不拦的话会走下面的 InvokeCommand：shell 的 rename 动词要一个真正的
    // IShellView 才能开原地编辑框，我们没有，点下去毫无反应。
    // 注意这里**不还原前台窗口**：接着要弹输入框并让它拿到键盘焦点，
    // 先把前台还给别人，SetForegroundWindow 就会失败、用户打不了字。
    if (!path.empty() && g_renameHandler && IsRenameCommand(cm, menu, cmd) &&
        g_renameHandler(hwnd, path)) {
        if (TraceOn()) LogSurface("menu pick=rename -> host handled");
        return;
    }

    if (prevFg && prevFg != hwnd) SetForegroundWindow(prevFg);

    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd;
    info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
    info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
    info.nShow = SW_SHOWNORMAL;
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    info.ptInvoke = pt;
    cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
}

// 桌面空白处
//
// 注意这里必须走 IShellView::GetItemObject(SVGIO_BACKGROUND)，
// 而不是 IShellFolder::CreateViewObject(IID_IContextMenu)：
// 后者只给「文件夹扩展」那一堆（新建文件夹 / 显示设置 / 个性化 / 各种 IDE 的注入项），
// 「查看 / 排序方式 / 刷新 / 粘贴」是 shell **视图** 提供的，只有 view 的
// background 菜单才有 —— 这正是之前和原生桌面右键不一样的原因。
void ShowBackgroundMenu(HWND hwnd, POINT pt) {
    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) return;

    IContextMenu* cm = nullptr;
    IShellView* view = nullptr;
    if (SUCCEEDED(desktop->CreateViewObject(hwnd, IID_IShellView, (void**)&view)) && view) {
        view->GetItemObject(SVGIO_BACKGROUND, IID_IContextMenu, (void**)&cm);
    }
    if (!cm) {
        // 兜底：拿不到 view 就退回文件夹级菜单（至少还有「新建 / 显示设置」）
        if (TraceOn()) LogSurface("bg menu: view unavailable, fallback to folder");
        desktop->CreateViewObject(hwnd, IID_IContextMenu, (void**)&cm);
    }
    desktop->Release();

    if (!cm) {
        if (view) view->Release();
        return;
    }

    HMENU menu = CreatePopupMenu();
    HRESULT hrq = E_FAIL;
    if (menu) hrq = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE);
    if (TraceOn()) LogSurface("bg menu hr=0x%08lX count=%d", (unsigned long)hrq,
                              menu ? GetMenuItemCount(menu) : -1);
    if (menu && SUCCEEDED(hrq)) InvokeShellMenu(hwnd, cm, menu, pt);
    if (menu) DestroyMenu(menu);
    cm->Release();
    if (view) view->Release();
}

// 某个图标（文件/文件夹/快捷方式）
// 右键菜单：paths 是**当前选中的那一批**，不是鼠标底下那一个。
//
// 【为什么必须整批交出去】shell 的「删除 / 剪切 / 复制」作用的范围 = 创建这个
// IContextMenu 时传进去的对象个数。以前这里只 SHCreateItemFromParsingName 了
// 鼠标底下那一个，于是框选 10 个图标再点删除，只删掉 1 个（用户报的 bug）。
// 现在统一走 IShellItemArray，单选就是长度 1 的数组，只有一套代码。
void ShowItemMenu(HWND hwnd, const std::vector<std::wstring>& paths, POINT pt) {
    if (paths.empty()) return;
    if (TraceOn())
        LogSurface("ctx item count=%d first=%ls", (int)paths.size(), paths[0].c_str());

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        if (p.empty()) continue;
        PIDLIST_ABSOLUTE full = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &full, 0, nullptr)) && full)
            pidls.push_back(full);
    }

    IContextMenu* cm = nullptr;
    IShellItemArray* arr = nullptr;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(),
                                                    (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(),
                                                    &arr)) &&
        arr) {
        // 走 IShellItemArray → BHID_SFUIObject：和资源管理器里右键「一批文件」
        // 拿到的菜单完全一致（多选时 fixture 会自动把「重命名」那类只对单个
        // 对象有意义的项去掉）。
        arr->BindToHandler(nullptr, BHID_SFUIObject, IID_IContextMenu, (void**)&cm);
        arr->Release();
    }
    for (auto* p : pidls) ILFree(p);
    if (!cm) return;

    HMENU menu = CreatePopupMenu();
    HRESULT hrq = E_FAIL;
    // CMF_CANRENAME：不带这个标志，shell 根本不会往菜单里放「重命名」。
    // 它等于向 shell 承诺「宿主能就地改名」—— 我们确实能，只是走自己的输入框
    // （命令行子在 InvokeShellMenu 里被 find/拦下来）。
    if (menu)
        hrq = cm->QueryContextMenu(menu, 0, 1, 0x7FFF,
                                   CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME);
    if (TraceOn()) LogSurface("item menu hr=0x%08lX count=%d", (unsigned long)hrq,
                              menu ? GetMenuItemCount(menu) : -1);
    // path 只用来拦「重命名」（那是单选才有意义的操作），多选时传空 = 不拦。
    if (menu && SUCCEEDED(hrq))
        InvokeShellMenu(hwnd, cm, menu, pt, paths.size() == 1 ? paths[0] : std::wstring());
    if (menu) DestroyMenu(menu);
    cm->Release();
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外暴露的系统原生菜单（盒子里右键图标也走这里，见 desktop_surface.h）
// ---------------------------------------------------------------------------
void ShowShellItemMenu(HWND owner, const std::wstring& path, POINT screenPt) {
    ShowItemMenu(owner, std::vector<std::wstring>{path}, screenPt);
}

void SetShellMenuRenameHandler(std::function<bool(HWND owner, const std::wstring& path)> fn) {
    g_renameHandler = std::move(fn);
}

bool ForwardShellMenuMessage(HWND /*owner*/, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_menuOwner) return false;
    if (msg != WM_INITMENUPOPUP && msg != WM_MEASUREITEM && msg != WM_DRAWITEM) return false;
    return SUCCEEDED(g_menuOwner->HandleMenuMsg(msg, wp, lp));
}

struct DesktopSurface::Impl {
    std::unordered_map<DesktopIconView*, IconDib> icons;
    std::unordered_map<DesktopIconView*, LabelDib> labels;
    std::unordered_map<DesktopIconView*, std::wstring> labelKeys;
    // 诊断用：跳过 DIB 缓存，用来分离「图标 DIB / 文字 DIB」各自占的内存
    bool noCache = GetEnvironmentVariableW(L"MEOW_PROBE_NOCACHE", nullptr, 0) > 0;

    IconDib* Icon(DesktopIconView* e) {
        if (noCache) return nullptr;
        auto it = icons.find(e);
        if (it != icons.end()) return &it->second;
        IconDib d;
        if (!d.Build(e->bmp)) {
            icons.emplace(e, IconDib{});
            return nullptr;
        }
        auto r = icons.emplace(e, std::move(d));
        return &r.first->second;
    }

    // maxLines：平时 1 行（超出省略），选中（单击）后展开到 2 行。
    // 它必须进缓存 key —— 否则点一下图标，拿到的还是那张单行的旧 DIB。
    LabelDib* Label(DesktopIconView* e, int boxW, int boxH, int glow, int maxLines) {
        if (noCache) return nullptr;
        std::wstring key = e->name + L"\x1|" + std::to_wstring(boxW) + L"x" +
                           std::to_wstring(boxH) + L"g" + std::to_wstring(glow) + L"m" +
                           std::to_wstring(maxLines);
        auto k = labelKeys.find(e);
        if (k != labelKeys.end() && k->second == key) {
            auto l = labels.find(e);
            if (l != labels.end()) return &l->second;
        }
        LabelDib d;
        if (!d.Build(e->name, IconLabelFont(), boxW, boxH, glow, RGB(255, 255, 255), maxLines)) {
            labelKeys[e] = key;
            labels[e] = LabelDib{};
            return nullptr;
        }
        labelKeys[e] = key;
        auto r = labels.insert_or_assign(e, std::move(d));
        return &r.first->second;
    }

    void Clear() {
        icons.clear();
        labels.clear();
        labelKeys.clear();
    }
};

LRESULT CALLBACK DesktopSurface::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    DesktopSurface* self = reinterpret_cast<DesktopSurface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<DesktopSurface*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void DesktopSurface::SetTextShadow(bool on) {
    if (textShadow_ == on) return;
    textShadow_ = on;
    if (impl_) {
        impl_->labels.clear();
        impl_->labelKeys.clear();
    }
    Redraw();
}

void DesktopSurface::SetShowLabels(bool on) {
    if (showLabels_ == on) return;
    showLabels_ = on;
    if (impl_) {
        impl_->labels.clear();
        impl_->labelKeys.clear();
    }
    Redraw();
}

void DesktopSurface::InvalidateCache() {
    if (impl_) impl_->Clear();
    Redraw();
}

void DesktopSurface::RefreshIconBitmaps(const std::vector<DesktopIconView*>& list) {
    if (!impl_ || list.empty()) return;
    RECT dirty{};
    bool any = false;
    for (DesktopIconView* e : list) {
        if (!e) continue;
        impl_->icons.erase(e);        // 位图换了，那张 IconDib 就作废了
        if (!e->OnDesktop()) continue; // 盒子里的由盒子自己 Redraw
        const int ex = e->pos.x - origin_.x;
        const int ey = e->pos.y - origin_.y;
        const RECT r{ex, ey, ex + cellW_, ey + cellH_};
        if (!any) {
            dirty = r;
            any = true;
        } else {
            dirty.left = std::min(dirty.left, r.left);
            dirty.top = std::min(dirty.top, r.top);
            dirty.right = std::max(dirty.right, r.right);
            dirty.bottom = std::max(dirty.bottom, r.bottom);
        }
    }
    if (any) RepaintCells(dirty);
}

POINT DesktopSurface::GridOrigin() const {
    const int cw = cellW_ > 0 ? cellW_ : 75;
    const int ch = cellH_ > 0 ? cellH_ : 98;

    // 找「左上角最靠前」的桌面图标：先比 y 再比 x。
    // 它一定落在资源管理器的网格上，所以可以当作锚点往回倒推。
    const DesktopIconView* first = nullptr;
    if (icons_) {
        for (auto* o : *icons_) {
            if (!o || !o->OnDesktop()) continue;
            if (!first || o->pos.y < first->pos.y ||
                (o->pos.y == first->pos.y && o->pos.x < first->pos.x))
                first = o;
        }
    }
    if (!first) {
        // 桌面上一个图标都没有：没有东西可以反推。给一个保守的小留白 ——
        // 宁可差几个像素，也不要让第一列图标紧贴屏幕边缘（用户报的就是这个）。
        return POINT{origin_.x + kEmptyGridPad, origin_.y + kEmptyGridPad};
    }

    // 沿网格往左上倒退到出界为止，落点就是第一格
    int x = first->pos.x;
    int y = first->pos.y;
    while (x - cw >= origin_.x) x -= cw;
    while (y - ch >= origin_.y) y -= ch;
    return POINT{x, y};
}

POINT DesktopSurface::AutoGridAnchor(POINT nearPt) const {
    // 资源管理器的桌面图标网格有自己的左上留白，格子不是从屏幕 (0,0) 起算的，
    // 直接对绝对坐标 round(x/cw)*cw 会让图标落下时整体偏掉半格甚至更多。
    // 所以拿「离得最近的桌面图标当前位置」当锚点 —— 它一定在网格上。
    // 桌面上没有图标时退回 GridOrigin()（那里面已经处理了留白）。
    // 注意参数不能叫 near / far：windef.h 里那两个是空宏，会把这行拆成 ".x"。
    POINT best = GridOrigin();
    long long bestD = -1;
    if (icons_) {
        for (auto* o : *icons_) {
            if (!o || !o->OnDesktop()) continue;
            const long long dx = (long long)o->pos.x - (long long)nearPt.x;
            const long long dy = (long long)o->pos.y - (long long)nearPt.y;
            const long long d = dx * dx + dy * dy;
            if (bestD < 0 || d < bestD) {
                bestD = d;
                best = o->pos;
            }
        }
    }
    return best;
}

bool DesktopSurface::CellIsFree(int x, int y, int cw, int ch,
                                const std::vector<DesktopIconView*>& exclude) const {
    RECT a{x, y, x + cw, y + ch};
    if (!icons_) return true;
    for (auto* o : *icons_) {
        if (!o || !o->OnDesktop()) continue;
        if (std::find(exclude.begin(), exclude.end(), o) != exclude.end()) continue;
        RECT b{o->pos.x, o->pos.y, o->pos.x + cw, o->pos.y + ch};
        if (a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom)
            return false;
    }
    return true;
}

// 从桌面左上角开始按行扫（先从左到右扫完一行，再下移一行），
// 找出第一个空闲格子。删除盒子 / 删除分组时图标从这里开始一个挨一个排。
// 从桌面左上角开始**按列**扫（先从上到下扫完一列，再往右换下一列），
// 找出第一个空闲格子。删除盒子 / 删除分组时图标从这里开始一个挨一个往下排。
// 注意别写成「先扫完一行再下移」—— 桌面图标的排列顺序本来就是一列列往下长，
// 横着排会让删完后图标横躺在屏幕顶上，和用户预期相反。
POINT DesktopSurface::FirstFreeCell(const std::vector<DesktopIconView*>& exclude) const {
    const int cw = cellW_ > 0 ? cellW_ : 75;
    const int ch = cellH_ > 0 ? cellH_ : 98;
    // 起点必须是**资源管理器网格的第一格**（带左上留白），不能是屏幕原点：
    // 「保存文件到桌面」时新文件没有视图位置，就是从这里开始找空位的，
    // 从 origin_ 起算会让它贴死在屏幕左上角。
    const POINT org = GridOrigin();
    const int maxX = origin_.x + width_ - cw;
    const int maxY = origin_.y + height_ - ch;
    for (int x = org.x; x <= maxX; x += cw) {
        for (int y = org.y; y <= maxY; y += ch) {
            if (CellIsFree(x, y, cw, ch, exclude)) return POINT{x, y};
        }
    }
    // 整屏都排满了：退回网格第一格（宁可叠一点，也别把图标藏到屏幕外面去）
    return org;
}

POINT DesktopSurface::SnapToFreeCell(POINT want, const std::vector<DesktopIconView*>& exclude) const {
    return SnapToFreeCellAt(want, AutoGridAnchor(want), exclude);
}

void DesktopSurface::PlaceFreshItemsAt(POINT screenPt, const std::vector<std::wstring>& before) {
    if (!icons_) return;

    // 差集：重载之后才出现、弹菜单之前没有的项。
    // 路径比对就够了 —— 新建/粘贴出来的一定是文件系统里的新路径。
    std::vector<DesktopIconView*> pending;
    for (auto* o : *icons_) {
        if (!o || !o->OnDesktop() || o->path.empty() || !o->pidl) continue;
        if (std::find(before.begin(), before.end(), o->path) == before.end())
            pending.push_back(o);
    }
    if (pending.empty()) return;

    const int ch = cellH_ > 0 ? cellH_ : 98;
    // 从「右键点落下的那一格」开始摆；一次建出好几个（粘贴）就依次往下排。
    POINT want = screenPt;
    while (!pending.empty()) {
        DesktopIconView* o = pending.front();
        // exclude 传还没摆的那几个（含它自己）：免得它自己的**旧**位置把目标格判成占用。
        // 已经摆好的不在 exclude 里，于是它们的新位置会正常挡住后面的。
        const POINT cell = SnapToFreeCell(want, pending);
        void* pidl = o->pidl;
        const bool ok = SetDesktopIconPositions(1, &pidl, &cell);
        if (TraceOn())
            LogSurface("new item place %ls -> %ld,%ld %s", o->name.c_str(), cell.x, cell.y,
                       ok ? "ok" : "FAIL");
        // 写回失败也先把这一帧的内存位置改掉 —— 至少这次看到的是对的，
        // 下次重载会被 Explorer 的旧坐标盖回去，但总比当场跳回左上角好。
        o->pos = cell;
        o->dragPos = cell;
        pending.erase(pending.begin());
        want = POINT{cell.x, cell.y + ch};
    }
    Redraw();
}

POINT DesktopSurface::SnapToFreeCellAt(POINT want, POINT anchor,
                                       const std::vector<DesktopIconView*>& exclude) const {
    const int cw = cellW_ > 0 ? cellW_ : 75;
    const int ch = cellH_ > 0 ? cellH_ : 98;
    const int maxX = origin_.x + width_ - cw;
    const int maxY = origin_.y + height_ - ch;

    auto clampX = [&](int v) {
        if (v < origin_.x) v = origin_.x;
        if (v > maxX && maxX >= origin_.x) v = maxX;
        return v;
    };
    auto clampY = [&](int v) {
        if (v < origin_.y) v = origin_.y;
        if (v > maxY && maxY >= origin_.y) v = maxY;
        return v;
    };

    // 相对锚点取整：落在网格上的锚点 + 整数个格子间距 → 仍在网格上
    const int baseX = clampX(anchor.x + (int)std::lround((double)(want.x - anchor.x) / cw) * cw);
    const int baseY = clampY(anchor.y + (int)std::lround((double)(want.y - anchor.y) / ch) * ch);

    auto freeAt = [&](int x, int y) { return CellIsFree(x, y, cw, ch, exclude); };

    if (freeAt(baseX, baseY)) return POINT{baseX, baseY};

    // 由近及远逐环搜索最近的空闲格子
    for (int ring = 1; ring <= 32; ++ring) {
        int bestX = 0, bestY = 0, bestD = 0x7fffffff;
        bool found = false;
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
                const int x = clampX(baseX + dx * cw);
                const int y = clampY(baseY + dy * ch);
                if (x == baseX && y == baseY) continue;
                if (!freeAt(x, y)) continue;
                const int d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; bestX = x; bestY = y; found = true; }
            }
        }
        if (found) return POINT{bestX, bestY};
    }
    return POINT{baseX, baseY};
}

// 盒子里拖出来的图标没有「原来的桌面格子」，只能按光标位置反推：
// 拖拽浮层是把图标中心钉在光标上的，所以格子左上角 = 光标 - (半格宽, 图标中心高)。
POINT DesktopSurface::CellForCursor(POINT cursor, const IconBitmap* icon) const {
    const int ih = (icon && icon->valid()) ? icon->height : iconPx_;
    POINT want;
    want.x = cursor.x - cellW_ / 2;
    want.y = cursor.y - kIconTopPad - ih / 2;
    return SnapToFreeCell(want, {});
}

// 「某个格子上站着谁」——只算桌面上、且不在 exclude 里的图标
static DesktopIconView* IconAtCellOf(const std::vector<DesktopIconView*>* icons, int x, int y,
                                     const std::vector<DesktopIconView*>& exclude) {
    if (!icons) return nullptr;
    for (auto* o : *icons) {
        if (!o || !o->OnDesktop()) continue;
        if (std::find(exclude.begin(), exclude.end(), o) != exclude.end()) continue;
        if (o->pos.x == x && o->pos.y == y) return o;
    }
    return nullptr;
}

// 把「想要的左上角」吸附到网格上的格子（不做占位判断）
POINT DesktopSurface::SnapCell(POINT want) const {
    const int cw = cellW_ > 0 ? cellW_ : 75;
    const int ch = cellH_ > 0 ? cellH_ : 98;
    const POINT anchor = AutoGridAnchor(want);
    const int maxX = origin_.x + width_ - cw;
    const int maxY = origin_.y + height_ - ch;
    int bx = anchor.x + (int)std::lround((double)(want.x - anchor.x) / cw) * cw;
    int by = anchor.y + (int)std::lround((double)(want.y - anchor.y) / ch) * ch;
    if (bx < origin_.x) bx = origin_.x;
    if (bx > maxX && maxX >= origin_.x) bx = maxX;
    if (by < origin_.y) by = origin_.y;
    if (by > maxY && maxY >= origin_.y) by = maxY;
    return POINT{bx, by};
}

POINT DesktopSurface::ResolveDropCell(POINT want, DesktopIconView* mover,
                                      const std::vector<DesktopIconView*>& exclude,
                                      std::vector<DesktopIconView*>* swapped) const {
    const POINT cellPos = SnapCell(want);
    DesktopIconView* occ = IconAtCellOf(icons_, cellPos.x, cellPos.y, exclude);
    if (!occ || occ == mover) return cellPos;

    // 和目标格上的图标交换：它回到 mover 原来的格子。
    // 前提是那个格子还没被别的拖拽图标占掉（多选拖动时可能已经不空了）。
    if (mover) {
        const POINT old = mover->pos;
        if (!(old.x == cellPos.x && old.y == cellPos.y)) {
            std::vector<DesktopIconView*> ex2 = exclude;
            ex2.push_back(mover);
            if (!IconAtCellOf(icons_, old.x, old.y, ex2)) {
                occ->pos = old;
                occ->dragPos = old;
                occ->selected = false;
                if (swapped) swapped->push_back(occ);
                return cellPos;
            }
        }
    }
    // 换不了就退回环形搜索（不会重叠，只是可能跳几格）
    return SnapToFreeCellAt(want, AutoGridAnchor(want), exclude);
}

void DesktopSurface::SetDropPreview(bool on, POINT cellTopLeft) {
    const bool same = on && previewOn_ && cellTopLeft.x == previewCell_.x &&
                      cellTopLeft.y == previewCell_.y;
    if (on == previewOn_ && (same || !on)) return;
    previewOn_ = on;
    if (on) previewCell_ = cellTopLeft;
    Redraw();
}

void DesktopSurface::SetDropSeam(bool on, int screenX, int top, int h) {
    const bool same = on && seamOn_ && screenX == seamX_ && top == seamTop_ && h == seamH_;
    if (on == seamOn_ && (same || !on)) return;
    seamOn_ = on;
    seamX_ = screenX;
    seamTop_ = top;
    seamH_ = h;
    Redraw();
}

RECT DesktopSurface::CellRect(const DesktopIconView* e) const {
    RECT r{};
    r.left = e->pos.x - origin_.x;
    r.top = e->pos.y - origin_.y;
    r.right = r.left + cellW_;
    r.bottom = r.top + cellH_;
    return r;
}

// 每次绘制前算一次「谁有资格展开标签」：恰好一个图标被选中时才是它。
// 多选（框选 / Ctrl 加选 / 全选）时一个都不展开 —— 否则一片图标同时往下长
// 好几行，桌面直接变成一堵字墙。
void DesktopSurface::ResolveLabelExpand() {
    labelExpand_ = nullptr;
    if (!icons_) return;
    // 拖拽 / 框选过程中谁都不展开：那时候图标在动，脏矩形只按格子算，
    // 多出来的那几行既会跟手乱跳、又会在局部重画时被擦掉一截。
    if (mode_ != Mode::None) return;
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop() || !e->selected) continue;
        if (labelExpand_) return;   // 第二个 → 谁都不展开
        labelExpand_ = e;
    }
}

// 图标实际画到的范围。选中（聚焦）时标签不限行，高度得按「标签实际多高」算 ——
// 直接问那枚已经缓存的 LabelDib（和 DrawOneLayered 用同一个 key，命中缓存不重建）。
// 拿不到 DIB（还没画过 / 关掉了标题）就退回格子本身。
//
// 只有**聚焦**（恰好一个选中，`IsLabelExpand`）才撑高：悬停底色永远是「图标所占
// 的一格」，不跟着标签往下长（用户要求：只有 active 的高度才增加）。
RECT DesktopSurface::IconExtent(const DesktopIconView* e) const {
    RECT r{};
    if (!e) return r;
    r = CellRect(e);
    if (!IsLabelExpand(e) || !showLabels_ || e->renaming || e->name.empty() || !impl_)
        return r;

    const int ih = e->bmp.valid() ? e->bmp.height : iconPx_;
    const int lineH = IconLabelLineHeight();
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH > lineH * 2) textH = lineH * 2;
    if (textH < 14) textH = 14;

    // 这就是 DrawOneLayered 里那次 Build 的参数（key 里含 maxLines=0，会命中同一条缓存）
    const LabelDib* lab = impl_->Label(const_cast<DesktopIconView*>(e), cellW_ - kLabelInset,
                                       textH, textShadow_ ? 2 : 0, 0);
    if (!lab || !lab->valid()) return r;

    const int textTop = r.top + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    const int bottom = textTop - lab->pad + lab->h;
    if (bottom > r.bottom) r.bottom = bottom;
    return r;
}

// 放弃这次拖动、图标回原位（交棒给系统级拖放之前调）。
// 和鼠标松开时「没落在盒子上、也没真的移动」那条分支同一套收尾，只是位置直接从
// dragStartPos_ 还原，不再吸附格子。
void DesktopSurface::AbortDrag() {
    if (mode_ != Mode::Move) return;
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    for (size_t i = 0; i < dragSet_.size() && i < dragStartPos_.size(); ++i) {
        if (!dragSet_[i]) continue;
        dragSet_[i]->pos = dragStartPos_[i];
        dragSet_[i]->dragPos = dragStartPos_[i];
    }
    dragSet_.clear();
    dragStartPos_.clear();
    moved_ = false;
    dragLayerShown_ = false;
    previewOn_ = false;
    seamOn_ = false;
    seamCellValid_ = false;
    mode_ = Mode::None;
    if (onDragEnd) onDragEnd();
    Redraw();
}

void DesktopSurface::SuspendDrag(POINT cursorScreen) {
    suspended_ = false;
    suspendIcons_.clear();
    suspendStart_.clear();
    suspendPos_.clear();
    // 只有「真的拖起来了」才值得挂起：按下没动的单击不该被接着拖
    if (mode_ != Mode::Move || !moved_ || dragSet_.empty()) {
        AbortDrag();
        return;
    }
    suspendIcons_ = dragSet_;
    suspendStart_ = dragStartPos_;
    for (size_t i = 0; i < dragSet_.size(); ++i)
        suspendPos_.push_back(dragSet_[i] ? dragSet_[i]->dragPos : dragStartPos_[i]);
    suspendCursor_ = cursorScreen;
    suspended_ = true;
    AbortDrag();   // 摆回原位 + 撤浮层 + 清 mode_（和以前交棒前的收尾完全一致）
}

bool DesktopSurface::ResumeDrag(POINT cursorScreen) {
    if (!suspended_) return false;
    suspended_ = false;
    if (suspendIcons_.empty() || suspendIcons_.size() != suspendStart_.size() ||
        suspendStart_.size() != suspendPos_.size())
        return false;
    // 期间可能重新枚举过图标（文件被目标搬走 / 桌面监视触发）→ 认不回来就算了
    for (auto* e : suspendIcons_)
        if (!e || !e->OnDesktop()) return false;

    dragSet_ = suspendIcons_;
    dragStartPos_ = suspendStart_;
    // pressScreen_ 要反推出「让 dragPos 等于 挂起时的跟手位置 + 这一段光标位移」的值，
    // 否则图标会瞬间跳回原位或者跳到光标正下方。取第一个图标即可（整批共享同一个位移）。
    const POINT rel{suspendPos_[0].x - suspendCursor_.x, suspendPos_[0].y - suspendCursor_.y};
    pressScreen_ = POINT{suspendStart_[0].x - rel.x, suspendStart_[0].y - rel.y};
    curScreen_ = cursorScreen;
    moved_ = true;
    dragLayerShown_ = false;   // 让下一帧把浮层重新画出来
    mode_ = Mode::Move;
    if (hwnd_) SetCapture(hwnd_);

    const int dx = cursorScreen.x - pressScreen_.x;
    const int dy = cursorScreen.y - pressScreen_.y;
    for (size_t i = 0; i < dragSet_.size(); ++i) {
        if (!dragSet_[i]) continue;
        dragSet_[i]->dragPos = POINT{dragStartPos_[i].x + dx, dragStartPos_[i].y + dy};
    }
    suspendIcons_.clear();
    suspendStart_.clear();
    suspendPos_.clear();

    if (onDragUpdate) onDragUpdate(dragSet_);   // 浮层立刻回来，不用等下一次移动
    if (onDragHover) onDragHover(cursorScreen); // 落点提示 / 盒子缝隙线
    Redraw();
    return true;
}

DesktopIconView* DesktopSurface::HitTest(POINT pt) const {
    if (!icons_) return nullptr;
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop()) continue;
        RECT r = CellRect(e);
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return e;
    }
    return nullptr;
}

void DesktopSurface::ClearSelection() {
    if (!icons_) return;
    for (auto* e : *icons_) if (e) e->selected = false;
}

void DesktopSurface::SelectOnly(DesktopIconView* e) {
    ClearSelection();
    if (e) e->selected = true;
}

void DesktopSurface::ToggleSelection(DesktopIconView* e) {
    if (e) e->selected = !e->selected;
}

void DesktopSurface::UpdateMarqueeSelection() {
    if (!icons_) return;
    RECT m = Normalize(marqueeA_, marqueeB_);
    for (auto* e : *icons_) {
        if (!e || !e->OnDesktop()) continue;
        RECT r = CellRect(e);
        if (Intersects(r, m)) e->selected = true;
        else if (!marqueeAdditive_) e->selected = false;
    }
}

LRESULT DesktopSurface::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    // 单例：第二个进程启动后发过来的「让一让」。这里只让外部把界面叫出来，
    // 不抢焦点、不动图标布局。
    case kMsgActivateInstance:
        if (onActivateRequest) onActivateRequest();
        return 0;

    case WM_ERASEBKGND:
        // 不透明路径由 WM_PAINT 自己画壁纸，别让系统再刷一遍底色
        return opaque_ ? 1 : 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (dc) {
            if (opaque_) {
                PaintOpaque(dc);
            } else if (memDC_) {
                // 分层路径实际不靠 WM_PAINT，这里只做一次整窗更新
                EndPaint(h, &ps);
                Redraw();
                return 0;
            }
            EndPaint(h, &ps);
        }
        dirtyValid_ = false;
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DISPLAYCHANGE:
        // 壁纸 / 主题 / 分辨率变了，整屏重画一遍壁纸
        if (opaque_) {
            dirtyValid_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        // 换壁纸（含幻灯片轮播）→ 让主程序重新抓盒子的磨砂底图。
        // 只认 SPI_SETDESKWALLPAPER 和分辨率变化：WM_SETTINGCHANGE 广播得很勤，
        // 别的参数一律去重抓一次底图（每个盒子几十毫秒）没必要。
        if (onShellChanged &&
            (msg == WM_DISPLAYCHANGE || (msg == WM_SETTINGCHANGE && wp == SPI_SETDESKWALLPAPER)))
            onShellChanged();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
        curScreen_ = pressScreen_;
        moved_ = false;
        dragLayerShown_ = false;
        dragSet_.clear();
        dragStartPos_.clear();
        previewOn_ = false;
        seamOn_ = false;
        seamCellValid_ = false;
        ForgetHover();   // 按下之后要么进拖拽、要么进框选，悬停底色都该收掉

        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        DesktopIconView* hit = HitTest(pt);

        if (hit) {
            if (ctrl) ToggleSelection(hit);
            else if (!hit->selected) SelectOnly(hit);
            if (hit->selected) {
                mode_ = Mode::Move;
                if (icons_) {
                    for (auto* e : *icons_) {
                        if (!e || !e->OnDesktop() || !e->selected) continue;
                        e->dragPos = e->pos;
                        dragSet_.push_back(e);
                        dragStartPos_.push_back(e->pos);
                    }
                }
            } else {
                mode_ = Mode::None;
            }
        } else {
            marqueeAdditive_ = ctrl;
            if (!ctrl) ClearSelection();
            mode_ = Mode::Marquee;
            marqueeA_ = marqueeB_ = pt;
        }
        SetCapture(h);
        if (TraceOn()) {
            LogSurface("LBUTTONDOWN pt=%ld,%ld hit=%d mode=%d sel=%zu", (long)pt.x, (long)pt.y,
                       hit ? 1 : 0, (int)mode_, dragSet_.size());
        }
        Redraw();
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (mode_ == Mode::Move) {
            // 桌面层覆盖全屏且不移动，客户区坐标 + origin 就是屏幕坐标
            curScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
            int rawDx = curScreen_.x - pressScreen_.x;
            int rawDy = curScreen_.y - pressScreen_.y;
            if (std::abs(rawDx) > 3 || std::abs(rawDy) > 3) moved_ = true;

            // 图标严格跟着鼠标走，不再吸附格子（吸附会让图标在目标格子之间跳）
            if (!opaque_) {
                // 进入拖拽的那一帧把图标从桌面层擦掉，之后桌面层内容不再变化，
                // 跟手全交给拖拽浮层 → 拖动过程零重合成，一点都不卡
                if (moved_ && !dragLayerShown_) {
                    dragLayerShown_ = true;
                    Redraw();
                }
            } else {
                for (auto* e : dragSet_) InvalidateCell(e, e->dragPos);  // 擦掉旧位置
            }

            for (size_t i = 0; i < dragSet_.size(); ++i) {
                dragSet_[i]->dragPos = POINT{dragStartPos_[i].x + rawDx,
                                             dragStartPos_[i].y + rawDy};
            }
            if (opaque_) {
                for (auto* e : dragSet_) InvalidateCell(e, e->dragPos);
                FlushDirty();
            }

            // 落点提示，两种形态只出现一个，避免抢戏：
            //   · 压到别的桌面图标上 → 竖向缝隙线，告诉用户「松手会插到它左/右边」
            //     （几何和盒子内的缝隙线一致：线落在两格之间的中线上）
            //   · 落在空地上 → 整格预览框，告诉用户「会落到这一格」
            if (moved_ && dragSet_.size() == 1) {
                const bool over = overTarget && overTarget(curScreen_);
                DesktopIconView* tgt = nullptr;
                if (!over) {
                    DesktopIconView* h0 = HitTest(pt);
                    if (h0 && std::find(dragSet_.begin(), dragSet_.end(), h0) == dragSet_.end())
                        tgt = h0;
                }
                if (tgt) {
                    // 以格子中线为界：光标在左半边 → 插到它左边，右半边 → 右边
                    const int cx = tgt->pos.x + cellW_ / 2;
                    const bool left = curScreen_.x < cx;
                    const int sx = left ? (cx - cellW_ / 2) : (cx + cellW_ / 2);
                    SetDropPreview(false);
                    SetDropSeam(true, sx, tgt->pos.y, cellH_);
                    seamCell_ = POINT{tgt->pos.x + (left ? -cellW_ : cellW_), tgt->pos.y};
                    seamCellValid_ = true;
                } else {
                    SetDropSeam(false, 0, 0, 0);
                    seamCellValid_ = false;
                    const bool show = !over;
                    POINT cell{0, 0};
                    // 用 SnapCell（而不是 SnapToFreeCell）：落点被占时是「交换」，
                    // 图标仍然落在光标下的格子，预览框也必须画在那个格子上。
                    if (show) cell = SnapCell(dragSet_[0]->dragPos);
                    SetDropPreview(show, cell);   // 没变化时它自己会跳过重画
                }
            } else {
                SetDropPreview(false);
                SetDropSeam(false, 0, 0, 0);
                seamCellValid_ = false;
            }

            if (onDragUpdate) onDragUpdate(dragSet_);
            if (onDragHover) onDragHover(curScreen_);
            return 0;
        }

        if (mode_ == Mode::Marquee) {
            if (opaque_) {
                // 旧框 + 新框（外扩一个格子，覆盖选中态的增减）
                RECT oldM = Normalize(marqueeA_, marqueeB_);
                InflateRect(&oldM, cellW_, cellH_);
                Dirty(oldM);
            }
            marqueeB_ = pt;
            UpdateMarqueeSelection();
            if (opaque_) {
                RECT newM = Normalize(marqueeA_, marqueeB_);
                InflateRect(&newM, cellW_, cellH_);
                Dirty(newM);
                FlushDirty();
            } else {
                Redraw();
            }
            return 0;
        }

        // 空闲状态：跟踪悬停高亮。鼠标在同一格子里移动不会触发重画。
        UpdateHover(POINT{origin_.x + pt.x, origin_.y + pt.y});
        if (!mouseTracking_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            mouseTracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        // 光标离开了桌面层（比如移进了某个盒子或别的窗口）→ 收掉悬停底色
        mouseTracking_ = false;
        ClearHover();
        return 0;

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        curScreen_ = POINT{origin_.x + pt.x, origin_.y + pt.y};
        if (GetCapture() == h) ReleaseCapture();
        if (onDragEnd) onDragEnd();

        if (mode_ == Mode::Move) {
            // 松手压在别人的程序窗口上、又没交棒（停的时间不够）→ 当取消。
            // 否则图标会「瞬移」到那个窗口底下的格子里：看不见、像被删了。
            if (moved_ && !dragSet_.empty() && overForeignWindow &&
                overForeignWindow(curScreen_)) {
                AbortDrag();   // 图标回原位 + 撤浮层，和「按 Esc 取消」同一条路
                return 0;
            }
            bool over = overTarget && overTarget(curScreen_);
            if (TraceOn()) {
                LogSurface("LBUTTONUP pt=%ld,%ld over=%d moved=%d n=%zu", (long)curScreen_.x,
                           (long)curScreen_.y, over ? 1 : 0, moved_ ? 1 : 0, dragSet_.size());
            }
            if (over && onDrop && !dragSet_.empty()) {
                if (onDrop(dragSet_, curScreen_)) {
                    for (auto* e : dragSet_) e->selected = false;
                    dragSet_.clear();
                    moved_ = false;
                    dragLayerShown_ = false;
                    mode_ = Mode::None;
                    Redraw();
                    return 0;
                }
            }
            if (moved_ && !dragSet_.empty()) {
                // 松手才落位：吸附到最近的格子。目标格被占就和占位图标交换，
                // 绝不重叠，也不会因为桌面排满而把图标甩到很远。
                std::vector<DesktopIconView*> pending = dragSet_;
                std::vector<DesktopIconView*> swapped;
                for (auto* e : dragSet_) {
                    pending.erase(std::remove(pending.begin(), pending.end(), e), pending.end());
                    POINT cell = (dragSet_.size() == 1)
                                     ? ResolveDropCell(seamCellValid_ ? seamCell_ : e->dragPos, e,
                                                       pending, &swapped)
                                     : SnapToFreeCell(e->dragPos, pending);
                    e->pos = cell;
                    e->dragPos = cell;
                }
                if (onMoved) onMoved(dragSet_);
                if (!swapped.empty() && onMoved) onMoved(swapped);   // 被交换走的也要写回
            } else {
                for (auto* e : dragSet_) e->dragPos = e->pos;
            }
        }
        // 框选松手：先趁 marqueeA_/B_ 还在，把蓝框换算成屏幕矩形。
        // 只有拉出了足够大的一块才弹菜单 —— 单击空白处（框面积≈0）照旧只是取消选中。
        RECT mq{0, 0, 0, 0};
        if (mode_ == Mode::Marquee) {
            const RECT m = Normalize(marqueeA_, marqueeB_);
            if (m.right - m.left >= kMarqueeMenuMin && m.bottom - m.top >= kMarqueeMenuMin) {
                mq = RECT{origin_.x + m.left, origin_.y + m.top,
                          origin_.x + m.right, origin_.y + m.bottom};
            }
        }

        dragSet_.clear();
        dragStartPos_.clear();
        moved_ = false;
        dragLayerShown_ = false;
        previewOn_ = false;
        seamOn_ = false;
        seamCellValid_ = false;
        mode_ = Mode::None;
        Redraw();
        if (mq.right > mq.left && onMarqueeMenu) onMarqueeMenu(mq, curScreen_);
        return 0;
    }
    case WM_MEASUREITEM:
    case WM_DRAWITEM:
    case WM_INITMENUPOPUP:
        // 右键菜单里的自绘项（带图标的那几个）交给 shell 自己画
        if (ForwardShellMenuMessage(h, msg, wp, lp)) return TRUE;
        break;

    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT sp{origin_.x + pt.x, origin_.y + pt.y};
        if (TraceOn()) LogSurface("rbuttonup client=%ld,%ld screen=%ld,%ld", pt.x, pt.y, sp.x, sp.y);
        DesktopIconView* hit = HitTest(pt);
        // 背景菜单要记「弹之前桌面上有什么」：关掉之后靠它认出新建/粘贴了哪几个，
        // 再把它们摆到点击位置（见 PlaceFreshItemsAt）。
        std::vector<std::wstring> before;
        bool bgMenu = false;
        if (hit) {
            // 图标上：打开文件的右键菜单（打开方式 / 属性 / 删除 …）
            if (!hit->selected) SelectOnly(hit);
            Redraw();
            // 点的是**已选中**的一员就保留整批选择，把这一批全交给 shell ——
            // 只传 hit 一个的话，框选 10 个再点删除只会删掉它自己。
            std::vector<std::wstring> sel;
            if (icons_) {
                for (auto* o : *icons_) {
                    if (o && o->OnDesktop() && o->selected && !o->path.empty())
                        sel.push_back(o->path);
                }
            }
            if (sel.empty()) sel.push_back(hit->path);
            ShowItemMenu(h, sel, sp);
        } else {
            ClearSelection();
            Redraw();
            if (icons_) {
                for (auto* o : *icons_)
                    if (o && o->OnDesktop() && !o->path.empty()) before.push_back(o->path);
            }
            ShowBackgroundMenu(h, sp);
            bgMenu = true;
        }
        // 菜单里可能改了名字/删了东西，回来重新枚举一次
        if (onContextInvoked) onContextInvoked();
        // 新建 / 粘贴出来的项摆到右键点的那个格子。
        // **必须在重载之后**：新项要先进 icons_ 才有 pidl 可以写回去。
        if (bgMenu) PlaceFreshItemsAt(sp, before);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        DesktopIconView* item = HitTest(pt);
        if (GetCapture() == h) ReleaseCapture();
        dragSet_.clear();
        dragStartPos_.clear();
        moved_ = false;
        mode_ = Mode::None;
        if (item && onOpen) onOpen(item);
        return 0;
    }

    case WM_TIMER:
        // 搜索「定位」的闪烁：只重画那一格
        if (wp == 2) {
            DesktopIconView* e = flash_;
            if (!e) {
                KillTimer(h, 2);
                return 0;
            }
            ++flashStep_;
            if (flashStep_ >= 4) {
                flash_ = nullptr;   // 亮相 0/2，灭相 1/3 —— 数到 4 正好两下
                KillTimer(h, 2);
            }
            if (e->OnDesktop()) {
                RECT r{e->pos.x - origin_.x - 4, e->pos.y - origin_.y - 4,
                       e->pos.x - origin_.x + cellW_ + 4,
                       e->pos.y - origin_.y + cellH_ + 4};
                RepaintCells(r);
            }
            return 0;
        }
        if (wp == 1) {
            // 每 500ms 拿「当前光标位置」校准一次悬停。正常情况下这里什么都不做
            // （命中的还是同一个图标，UpdateHover 直接返回），但万一某次
            // WM_MOUSEMOVE / WM_MOUSELEAVE 丢了（别的程序 SetCursorPos、远程桌面、
            // 光标被瞬间甩过整个屏幕），底色不会一直挂在某个图标上。
            POINT cur{};
            if (GetCursorPos(&cur)) UpdateHover(cur);
            if (onTick) onTick();
            return 0;
        }
        break;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool DesktopSurface::EnsureBackbuffer(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (memDC_ && bufW_ == w && bufH_ == h) return true;

    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;

    if (!CreateDib(w, h, memDC_, dib_, bits_)) {
        bufW_ = bufH_ = 0;
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

// 图标高亮（悬停 / 选中）的统一数值。
// 用户明确要求「active 高亮和鼠标 hover 的背景颜色一致」—— 所以两个状态共用
// 同一块底色（kHlFill），只靠边框深浅区分。以前选中是 48、悬停是 26，同一块
// 蓝色深浅不同，看着像两种颜色。分层和不透明两条路径都从这里取值，别再写字面量。
namespace {
constexpr int kHlFill = 26;         // 底色 alpha（悬停 / 选中一样）
constexpr int kHlEdgeHover = 72;    // 悬停：淡边
constexpr int kHlEdgeSelect = 150;  // 选中：实边
constexpr COLORREF kHlColor = RGB(0, 120, 215);
}  // namespace

void DesktopSurface::DrawOneLayered(DesktopIconView* e, bool dragging, bool ghost) {
    int x = (dragging ? e->dragPos.x : e->pos.x) - origin_.x;
    int y = (dragging ? e->dragPos.y : e->pos.y) - origin_.y;

    // 被拖走的原件留在原位的「灰占位」：变淡 + 变暗（盒子里是同一套数值，
    // 见 fence_window.cpp 的 BlendIntoDim(ga=110, gd=120)）。
    const int ga = ghost ? 110 : 255;   // 不透明度
    const int gd = ghost ? 120 : 255;   // 额外压暗

    // 悬停：一层很淡的半透明底 + 更淡的描边（选中态另有一套，颜色更深还带实边，
    // 两者一眼能分开）。拖动中的图标不画，否则跟着鼠标跑一块色斑。
    // 直角：和盒子里的悬停底色、资源管理器的选中框保持一致（圆角在这个尺寸下
    // 没有抗锯齿，边缘会明显发毛）。
    if (e->hover && !e->selected && !dragging) {
        // 悬停底就是**图标所占的区域**（一格），不跟着标签往下长 ——
        // 只有 active（选中）才会撑高。以前这里跟选中一样撑到完整标签，
        // 长名字的图标一悬停就顶着个下半截空着的高框，用户要求改回一格。
        const int hb = y + cellH_ - 2;
        RECT hf{x + 2, y + 2, x + cellW_ - 2, hb};
        auto* b = reinterpret_cast<BYTE*>(bits_);
        FillRectAlpha(b, bufW_, bufH_, hf, kHlFill, kHlColor);
        RECT edge[4] = {
            {hf.left, hf.top, hf.right, hf.top + 1},
            {hf.left, hf.bottom - 1, hf.right, hf.bottom},
            {hf.left, hf.top, hf.left + 1, hf.bottom},
            {hf.right - 1, hf.top, hf.right, hf.bottom},
        };
        for (const auto& r : edge) FillRectAlpha(b, bufW_, bufH_, r, kHlEdgeHover, kHlColor);
    }

    // 选中高亮（填充 + 1px 边框）
    if (e->selected) {
        // 聚焦（唯一选中）时标签不限行、会往下长好几行 —— 高亮必须跟着一起长，
        // 否则名字从框里冒出来，看着像画错了。IconExtent 给的就是「格子 ∪ 标签」。
        int hb = y + cellH_ - 2;
        if (!dragging) {
            const RECT ext = IconExtent(e);
            if (ext.bottom - 2 > hb) hb = ext.bottom - 2;
        }
        RECT fill{x + 2, y + 2, x + cellW_ - 2, hb};
        FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, fill, kHlFill, kHlColor);
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 1},
            {fill.left, fill.bottom - 1, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 1, fill.bottom},
            {fill.right - 1, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge)
            FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, r, kHlEdgeSelect,
                          kHlColor);
    }

    // 搜索「定位」的闪烁：亮相比选中态更亮更粗，保证一眼能找到
    if (flash_ == e && (flashStep_ & 1) == 0) {
        RECT fill{x + 2, y + 2, x + cellW_ - 2, y + cellH_ - 2};
        auto* b = reinterpret_cast<BYTE*>(bits_);
        FillRectAlpha(b, bufW_, bufH_, fill, 90, RGB(0, 120, 215));
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 2},
            {fill.left, fill.bottom - 2, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 2, fill.bottom},
            {fill.right - 2, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge) FillRectAlpha(b, bufW_, bufH_, r, 235, RGB(0, 120, 215));
    }

    if (!impl_) return;

    int iw = e->bmp.valid() ? e->bmp.width : 0;
    int ih = e->bmp.valid() ? e->bmp.height : 0;

    if (iw > 0) {
        // 直接用 IconBitmap 的像素（已经是预乘 BGRA），不再多占一份 IconDib 副本
        int ix = x + (cellW_ - iw) / 2;
        int iy = y + kIconTopPad;
        BlendIntoDim(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_,
                     reinterpret_cast<const BYTE*>(e->bmp.pixels.data()), iw, ih, ix, iy,
                     ga, gd);
    }

    // 正在原地改名：名字由那个输入框代替（见 DesktopIconView::renaming）
    if (e->name.empty() || !showLabels_ || e->renaming) return;

    // 平时两行（超出用省略号）；单击选中（聚焦，且只有它一个被选中）后行数
    // **不限**，名字整段显示出来 —— 宽度仍然是格子宽，高度由 LabelDib 自己往下撑开。
    // textH 只是给「不选中」那一档的最小盒子，展开时它会被 Build 顶大。
    const int lineH = IconLabelLineHeight();
    int textTop = y + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH > lineH * 2) textH = lineH * 2;
    if (textH < 14) textH = 14;
    const int maxLines = IsLabelExpand(e) ? 0 : 2;

    if (!impl_) return;
    LabelDib* lab = impl_->Label(e, cellW_ - kLabelInset, textH, textShadow_ ? 2 : 0, maxLines);
    if (lab && lab->valid()) {
        int contentW = lab->w - lab->pad * 2;   // DIB 已按实际文字宽度收窄，按内容居中
        int left = x + (cellW_ - contentW) / 2 - lab->pad;
        BlendIntoDim(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_,
                     reinterpret_cast<const BYTE*>(lab->bits), lab->w, lab->h,
                     left, textTop - lab->pad, ga, gd);
    }
}

void DesktopSurface::PaintLayered() {
    if (!bits_ || !memDC_) return;
    std::memset(bits_, 0, (size_t)bufW_ * (size_t)bufH_ * 4);  // 全透明底

    if (!icons_) return;
    ResolveLabelExpand();   // 谁能展开标签（唯一选中的那个）

    // 整屏重画前按当前光标位置同步一次悬停（图标可能刚搬过家）
    SyncHoverToCursor();

    // 拖动中的图标在**原位**留一份灰的当占位，跟手的那张由拖拽浮层画在最上面。
    // 以前是「拖走的直接跳过、原位什么都不画」，多选拖动时看不出原来占了哪几格。
    //
    // 分两遍：选中的最后画。选中时标签行数不限，会往下压到下一排图标的格子上，
    // 按原顺序画就会被后面的图标盖掉，名字还是看不全。
    for (int pass = 0; pass < 2; ++pass) {
        for (auto* e : *icons_) {
            if (!e || !e->OnDesktop()) continue;
            const bool onTop = e->selected && !IsDragGhost(e);
            if ((pass == 1) != onTop) continue;
            DrawOneLayered(e, false, IsDragGhost(e));
        }
    }
    // 正在拖的图标交给拖拽浮层画（桌面层在所有盒子下面，不然会被盒子挡住）

    if (previewOn_) DrawPreviewLayered();
    if (seamOn_) DrawSeamLayered();

    if (mode_ == Mode::Marquee) {
        RECT m = Normalize(marqueeA_, marqueeB_);
        FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, m, 46, RGB(0, 120, 215));
        RECT edge[4] = {
            {m.left, m.top, m.right, m.top + 1},
            {m.left, m.bottom - 1, m.right, m.bottom},
            {m.left, m.top, m.left + 1, m.bottom},
            {m.right - 1, m.top, m.right, m.bottom},
        };
        for (const auto& r : edge)
            FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, r, 160, RGB(0, 120, 215));
    }

    MakeHitTestable();
}

// 落点预览框：拖动时告诉用户「松手就放这里」。
// 直角空心框（外圈 2px 描边 + 内缩 2px 的淡底）。
// 以前用圆角，但这里是逐像素手写 alpha 填充、没有抗锯齿，圆角边缘一眼就是锯齿，
// 直角反而干净利落，也和资源管理器的选中框一致。
void DesktopSurface::DrawPreviewLayered() {
    if (!bits_) return;
    RECT r{previewCell_.x - origin_.x, previewCell_.y - origin_.y,
           previewCell_.x - origin_.x + cellW_, previewCell_.y - origin_.y + cellH_};
    InflateRect(&r, -2, -2);
    if (r.right <= 0 || r.bottom <= 0 || r.left >= bufW_ || r.top >= bufH_) return;
    auto* b = reinterpret_cast<BYTE*>(bits_);

    FillRectAlpha(b, bufW_, bufH_, r, 34, RGB(0, 120, 215));   // 淡淡的底
    // 2px 边框（四条边分开画，避免整块填色叠两次）
    RECT edge[4] = {
        {r.left, r.top, r.right, r.top + 2},
        {r.left, r.bottom - 2, r.right, r.bottom},
        {r.left, r.top, r.left + 2, r.bottom},
        {r.right - 2, r.top, r.right, r.bottom},
    };
    for (const auto& e : edge) FillRectAlpha(b, bufW_, bufH_, e, 190, RGB(0, 120, 215));
}

// 拖拽插入缝隙线：3px 竖条，落在两格之间的中线上，上下各留 6px。
// 和盒子里的缝隙线是同一套视觉（那边走 GDI+，这边是手写 alpha，尺寸/颜色对齐）。
void DesktopSurface::DrawSeamLayered() {
    if (!bits_) return;
    const int x = seamX_ - origin_.x;
    const int top = seamTop_ - origin_.y;
    RECT r{x, top + 6, x + 3, top + seamH_ - 6};
    if (r.right <= 0 || r.left >= bufW_ || r.bottom <= 0 || r.top >= bufH_) return;
    FillRectAlpha(reinterpret_cast<BYTE*>(bits_), bufW_, bufH_, r, 235, RGB(0, 120, 215));
}

// per-pixel alpha 的分层窗口有个坑：alpha == 0 的像素不接收鼠标消息（点击直接穿透到
// 真实桌面），所以桌面空白处永远收不到 WM_LBUTTONDOWN —— 框选框不起来。
// 把整层的 alpha 抬到 1（0.4% 的黑，肉眼不可见）让整片区域都能命中。
void DesktopSurface::MakeHitTestable() {
    if (!bits_ || bufW_ <= 0 || bufH_ <= 0) return;
    auto* p = reinterpret_cast<uint32_t*>(bits_);
    const size_t n = (size_t)bufW_ * (size_t)bufH_;
    for (size_t i = 0; i < n; ++i) p[i] |= 0x01000000u;   // alpha 字节 |= 1
}

// ---------------------------------------------------------------------------
// 不透明路径
// ---------------------------------------------------------------------------

bool DesktopSurface::EnsureSolidDib() {
    if (solidDC_ && solidBits_) return true;
    if (!CreateDib(1, 1, solidDC_, solidBmp_, solidBits_)) return false;
    return true;
}

// 在任意 HDC 上做一次半透明纯色填充：把 1x1 纯色 DIB 拉伸过去，
// 用 SourceConstantAlpha 控透明度（不设 AC_SRC_ALPHA，走常量 alpha 通道）。
void DesktopSurface::FillRectBlend(HDC hdc, const RECT& r, int a, COLORREF color) {
    if (!hdc || a <= 0) return;
    if (a > 255) a = 255;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (!EnsureSolidDib()) return;

    auto* p = reinterpret_cast<BYTE*>(solidBits_);
    p[0] = GetBValue(color);
    p[1] = GetGValue(color);
    p[2] = GetRValue(color);
    p[3] = 255;

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = (BYTE)a;
    bf.AlphaFormat = 0;              // 用常量 alpha
    AlphaBlend(hdc, r.left, r.top, w, h, solidDC_, 0, 0, 1, 1, bf);
}

// 不透明路径：壁纸当背景（PaintDesktop 只填当前裁剪区），图标用 AlphaBlend 贴上去。
// 不需要任何整屏缓冲 —— 这就是省下约 29MB 的关键。
void DesktopSurface::PaintOpaque(HDC hdc) {
    if (!hdc) return;

    // 背景：把壁纸按裁剪区重画一遍。窗口铺满整个虚拟屏幕且原点对齐，
    // 所以贴图位置与真实桌面完全一致。
    if (!PaintDesktop(hdc)) {
        RECT cr{};
        GetClipBox(hdc, &cr);
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &cr, br);
        DeleteObject(br);
    }

    if (icons_) {
        // 整屏重画前按当前光标位置同步一次悬停（图标可能刚搬过家）
        SyncHoverToCursor();
        ResolveLabelExpand();   // 谁能展开标签（唯一选中的那个）
        // 分两遍：选中的最后画（标签不限行，会压到下面几格的图标上，见 PaintLayered）
        for (int pass = 0; pass < 2; ++pass) {
            for (auto* e : *icons_) {
                if (!e || !e->OnDesktop()) continue;
                const bool onTop = e->selected && !IsDragGhost(e);
                if ((pass == 1) != onTop) continue;
                // 拖动中的图标：原位画灰占位（跟手的那张在拖拽浮层上）
                DrawOneGdi(hdc, e, false, IsDragGhost(e));
            }
        }
    }

    if (previewOn_) DrawPreviewGdi(hdc);
    if (seamOn_) DrawSeamGdi(hdc);

    if (mode_ == Mode::Marquee) {
        RECT m = Normalize(marqueeA_, marqueeB_);
        FillRectBlend(hdc, m, 46, RGB(0, 120, 215));
        RECT edge[4] = {
            {m.left, m.top, m.right, m.top + 1},
            {m.left, m.bottom - 1, m.right, m.bottom},
            {m.left, m.top, m.left + 1, m.bottom},
            {m.right - 1, m.top, m.right, m.bottom},
        };
        for (const auto& r : edge) FillRectBlend(hdc, r, 160, RGB(0, 120, 215));
    }
}

void DesktopSurface::DrawPreviewGdi(HDC hdc) {
    if (!hdc) return;
    RECT r{previewCell_.x - origin_.x, previewCell_.y - origin_.y,
           previewCell_.x - origin_.x + cellW_, previewCell_.y - origin_.y + cellH_};
    InflateRect(&r, -2, -2);
    RECT edge[4] = {
        {r.left, r.top, r.right, r.top + 2},
        {r.left, r.bottom - 2, r.right, r.bottom},
        {r.left, r.top, r.left + 2, r.bottom},
        {r.right - 2, r.top, r.right, r.bottom},
    };
    RECT in = r;
    InflateRect(&in, -2, -2);
    FillRectBlend(hdc, in, 32, RGB(0, 120, 215));
    for (const auto& e : edge) FillRectBlend(hdc, e, 165, RGB(0, 120, 215));
}

void DesktopSurface::DrawSeamGdi(HDC hdc) {
    if (!hdc) return;
    const int x = seamX_ - origin_.x;
    const int top = seamTop_ - origin_.y;
    RECT r{x, top + 6, x + 3, top + seamH_ - 6};
    FillRectBlend(hdc, r, 235, RGB(0, 120, 215));
}

void DesktopSurface::DrawOneGdi(HDC hdc, DesktopIconView* e, bool dragging, bool ghost) {
    int x = (dragging ? e->dragPos.x : e->pos.x) - origin_.x;
    int y = (dragging ? e->dragPos.y : e->pos.y) - origin_.y;

    // 悬停底：这里用直角 + 描边，和分层路径保持一致
    //（两套渲染路径的观感必须一样，否则换 MEOW_SURFACE 就成了两个程序）
    if (e->hover && !e->selected && !dragging) {
        // 和分层路径同一套：悬停底 = 一格，不撑高（只有 active 才撑）
        const int hb = y + cellH_ - 2;
        RECT hf{x + 2, y + 2, x + cellW_ - 2, hb};
        FillRectBlend(hdc, hf, kHlFill, kHlColor);
        RECT hedge[4] = {
            {hf.left, hf.top, hf.right, hf.top + 1},
            {hf.left, hf.bottom - 1, hf.right, hf.bottom},
            {hf.left, hf.top, hf.left + 1, hf.bottom},
            {hf.right - 1, hf.top, hf.right, hf.bottom},
        };
        for (const auto& r : hedge) FillRectBlend(hdc, r, kHlEdgeHover, kHlColor);
    }

    if (e->selected) {
        // 和分层路径同一套：聚焦时高亮跟着展开的标签一起长
        int hb = y + cellH_ - 2;
        if (!dragging) {
            const RECT ext = IconExtent(e);
            if (ext.bottom - 2 > hb) hb = ext.bottom - 2;
        }
        RECT fill{x + 2, y + 2, x + cellW_ - 2, hb};
        FillRectBlend(hdc, fill, kHlFill, kHlColor);
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 1},
            {fill.left, fill.bottom - 1, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 1, fill.bottom},
            {fill.right - 1, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge) FillRectBlend(hdc, r, kHlEdgeSelect, kHlColor);
    }

    // 搜索「定位」的闪烁（和分层路径同一套观感）
    if (flash_ == e && (flashStep_ & 1) == 0) {
        RECT fill{x + 2, y + 2, x + cellW_ - 2, y + cellH_ - 2};
        FillRectBlend(hdc, fill, 90, RGB(0, 120, 215));
        RECT edge[4] = {
            {fill.left, fill.top, fill.right, fill.top + 2},
            {fill.left, fill.bottom - 2, fill.right, fill.bottom},
            {fill.left, fill.top, fill.left + 2, fill.bottom},
            {fill.right - 2, fill.top, fill.right, fill.bottom},
        };
        for (const auto& r : edge) FillRectBlend(hdc, r, 235, RGB(0, 120, 215));
    }

    if (!impl_) return;

    int ih = e->bmp.valid() ? e->bmp.height : 0;
    int iw = e->bmp.valid() ? e->bmp.width : 0;

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    // 灰占位：AlphaBlend 只有「整体不透明度」这一根旋钮，没有逐像素压暗，
    // 所以这条路径只做变淡（分层路径会连变暗一起做）。这条路要用 MEOW_SURFACE
    // 显式打开才会走，日常观感以分层路径为准。
    bf.SourceConstantAlpha = ghost ? 110 : 255;
    bf.AlphaFormat = AC_SRC_ALPHA;   // 源是预乘 alpha → 边缘不会泛白

    if (iw > 0) {
        if (IconDib* d = impl_->Icon(e)) {
            if (d->valid()) {
                // 整数坐标 + 1:1 位块传输 → 不做任何重采样
                int ix = x + (cellW_ - d->w) / 2;
                int iy = y + kIconTopPad;
                AlphaBlend(hdc, ix, iy, d->w, d->h, d->dc, 0, 0, d->w, d->h, bf);
            }
        }
    }

    // 正在原地改名：名字由那个输入框代替（见 DesktopIconView::renaming）
    if (e->name.empty() || !showLabels_ || e->renaming) return;

    // 同上：平时两行省略，聚焦（唯一选中）后不限行（和分层路径完全一致）
    const int lineH = IconLabelLineHeight();
    int textTop = y + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH > lineH * 2) textH = lineH * 2;
    if (textH < 14) textH = 14;
    const int maxLines = IsLabelExpand(e) ? 0 : 2;

    if (LabelDib* lab = impl_->Label(e, cellW_ - kLabelInset, textH, textShadow_ ? 2 : 0,
                                     maxLines)) {
        if (lab->valid()) {
            int contentW = lab->w - lab->pad * 2;
            int left = x + (cellW_ - contentW) / 2 - lab->pad;
            AlphaBlend(hdc, left, textTop - lab->pad, lab->w, lab->h,
                       lab->dc, 0, 0, lab->w, lab->h, bf);
        }
    }
}

// ---------------------------------------------------------------------------
// 脏矩形
// ---------------------------------------------------------------------------

void DesktopSurface::Dirty(const RECT& r) {
    if (!hwnd_) return;
    RECT rr = r;
    if (dirtyValid_) {
        dirty_.left = std::min(dirty_.left, rr.left);
        dirty_.top = std::min(dirty_.top, rr.top);
        dirty_.right = std::max(dirty_.right, rr.right);
        dirty_.bottom = std::max(dirty_.bottom, rr.bottom);
    } else {
        dirty_ = rr;
        dirtyValid_ = true;
    }
    InvalidateRect(hwnd_, &rr, FALSE);
}

void DesktopSurface::FlushDirty() {
    if (!hwnd_) return;
    dirtyValid_ = false;
    dirty_ = RECT{0, 0, 0, 0};
    UpdateWindow(hwnd_);   // 立刻同步重绘，拖动才跟手
}

void DesktopSurface::InvalidateCell(const DesktopIconView* e, POINT pos) {
    if (!e) return;
    // 文字可能带光晕外扩，多留一点余量
    const int pad = 12;
    RECT r{pos.x - origin_.x - pad, pos.y - origin_.y - pad,
           pos.x - origin_.x + cellW_ + pad, pos.y - origin_.y + cellH_ + pad};
    Dirty(r);
}

// 搜索窗口的「定位」：让这个图标闪两下。
// 桌面层的重绘是整屏的（分层窗口只能整张上传），所以只翻 4 相、每相 170ms：
// 总时长 680ms，刚好够注意到，也不会长时间占着 CPU。
void DesktopSurface::FlashIcon(DesktopIconView* e) {
    if (!e || !hwnd_ || !e->OnDesktop()) return;
    flash_ = e;
    flashStep_ = 0;
    SetTimer(hwnd_, 2, 170, nullptr);
    RECT r{e->pos.x - origin_.x - 4, e->pos.y - origin_.y - 4,
           e->pos.x - origin_.x + cellW_ + 4, e->pos.y - origin_.y + cellH_ + 4};
    RepaintCells(r);
}


bool DesktopSurface::Create() {
    impl_ = new Impl();

    // 默认走分层透明（唯一能正确透出「真实壁纸」的方式）。
    // MEOW_SURFACE=opaque 只适合壁纸是静态图片的机器：那条路用 PaintDesktop 现画壁纸，
    // 遇到动态壁纸（Spotlight / 壁纸软件）会画错，所以不设为默认。
    {
        wchar_t buf[32]{};
        DWORD n = GetEnvironmentVariableW(L"MEOW_SURFACE", buf, 32);
        if (n > 0 && _wcsicmp(buf, L"opaque") == 0) opaque_ = true;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = nullptr;   // 不透明路径自己画壁纸；分层路径整窗 alpha
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        vx = wa.left; vy = wa.top;
        vw = wa.right - wa.left; vh = wa.bottom - wa.top;
    }
    origin_ = POINT{vx, vy};
    width_ = vw;
    height_ = vh;

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!opaque_) exStyle |= WS_EX_LAYERED;
    hwnd_ = CreateWindowExW(
        exStyle, kClassName, L"", WS_POPUP,
        origin_.x, origin_.y, width_, height_,
        nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    if (!opaque_) {
        if (!EnsureBackbuffer(width_, height_)) {
            LogSurface("surface backbuffer FAILED err=%lu", GetLastError());
            return false;
        }
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Redraw();
    LogSurface("surface mode=%s %dx%d", opaque_ ? "opaque" : "layered", width_, height_);

    // 心跳：处理 Win+D 之类把桌面提到前面的情况，把层级摆回来
    SetTimer(hwnd_, 1, 500, nullptr);
    return true;
}

void DesktopSurface::UploadLayered() {
    if (!hwnd_ || !memDC_ || !bits_ || opaque_) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    POINT dst{origin_.x, origin_.y};
    SIZE size{width_, height_};
    POINT src{0, 0};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void DesktopSurface::Redraw() {
    if (!hwnd_) return;
    if (!opaque_) {
        if (!memDC_ || !bits_) return;
        PaintLayered();
        UploadLayered();
        return;
    }
    // 不透明路径：全屏失效，WM_PAINT 里现画壁纸 + 图标
    dirtyValid_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

// 只重画一块区域：悬停变化只影响一两个格子，没必要把 200 个图标全合成一遍。
// 分层窗口每次仍要整体上传一次（UpdateLayeredWindow 没有局部接口），
// 但省掉了「清整屏 + 重画所有图标 + 整屏 alpha|=1」这三趟全屏遍历。
void DesktopSurface::RepaintCells(const RECT& rect) {
    if (!icons_) return;
    // 注意：不透明路径没有 bits_（那是分层窗口的整屏缓冲），这条判断必须在下面
    // 那个 opaque_ 分支之后 —— 否则不透明模式下悬停高亮永远不会重画。
    if (opaque_) {   // 不透明路径有自己的脏矩形机制
        // 和分层路径同一个理由：展开的标签会超出格子，裁剪区不够就会把它擦掉一截
        ResolveLabelExpand();
        RECT c2 = rect;
        if (labelExpand_) {
            const RECT er = IconExtent(labelExpand_);
            if (er.right > c2.left && er.left < c2.right && er.bottom > c2.top &&
                er.top < c2.bottom) {
                c2.left = std::min(c2.left, er.left);
                c2.top = std::min(c2.top, er.top);
                c2.right = std::max(c2.right, er.right);
                c2.bottom = std::max(c2.bottom, er.bottom);
            }
        }
        Dirty(c2);
        FlushDirty();
        return;
    }
    if (!bits_) return;

    RECT c = rect;
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > bufW_) c.right = bufW_;
    if (c.bottom > bufH_) c.bottom = bufH_;
    if (c.right <= c.left || c.bottom <= c.top) return;

    // 局部重画也要知道「谁在展开」：IconExtent 靠它算多出来的那几行
    ResolveLabelExpand();

    // 关键：先把 rect 撑到「盖住所有与它相交的图标所在的整块」。
    // 图标平时只画在自己格子里，但**选中时标签不限行、会往下多长几行**，
    // 所以范围要用 IconExtent()（格子 ∪ 展开的标签），不能再用 cellH_。
    // 不这么做的话：清空区只盖住格子，多出来的那几行没被清、也没被重画，
    // 移走图标后就会在桌面上留一串残影。
    for (bool grew = true; grew;) {
        grew = false;
        for (auto* e : *icons_) {
            if (!e || !e->OnDesktop()) continue;
            const RECT er = IconExtent(e);
            if (er.right <= c.left || er.left >= c.right || er.bottom <= c.top ||
                er.top >= c.bottom)
                continue;
            const int l = std::min<int>(c.left, er.left);
            const int t = std::min<int>(c.top, er.top);
            const int r = std::max<int>(c.right, er.right);
            const int b = std::max<int>(c.bottom, er.bottom);
            if (l != c.left || t != c.top || r != c.right || b != c.bottom) {
                c = RECT{l, t, r, b};
                grew = true;
            }
        }
    }
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > bufW_) c.right = bufW_;
    if (c.bottom > bufH_) c.bottom = bufH_;
    if (c.right <= c.left || c.bottom <= c.top) return;

    // 清成「可命中的透明」：alpha = 1，否则这块会变成鼠标穿透
    auto* p = reinterpret_cast<uint32_t*>(bits_);
    for (int y = c.top; y < c.bottom; ++y) {
        uint32_t* row = p + (size_t)y * (size_t)bufW_;
        for (int x = c.left; x < c.right; ++x) row[x] = 0x01000000u;
    }

    // 重画所有和这块区域相交的图标（可能不止一个：相邻格子的光晕会溢进来）。
    // 同样分两遍、选中的最后画 —— 和整屏重画保持同一套层级。
    for (int pass = 0; pass < 2; ++pass) {
        for (auto* e : *icons_) {
            if (!e || !e->OnDesktop()) continue;
            const RECT er = IconExtent(e);
            if (er.right <= c.left || er.left >= c.right || er.bottom <= c.top ||
                er.top >= c.bottom)
                continue;
            const bool onTop = e->selected && !IsDragGhost(e);
            if ((pass == 1) != onTop) continue;
            DrawOneLayered(e, false, IsDragGhost(e));
        }
    }
    UploadLayered();
}

DesktopIconView* DesktopSurface::ItemAtScreen(POINT screenPt) const {
    if (!icons_ || !hwnd_) return nullptr;
    return HitTest(POINT{screenPt.x - origin_.x, screenPt.y - origin_.y});
}

// 图标标题区的屏幕矩形（原位改名用）。
// 几何和 DrawOneLayered / DrawOneGdi 里画标签那一段完全一致：
//   textTop = 格子顶 + kIconTopPad + 图标高 + kTextGap
//   宽度   = 格子宽 - kLabelInset（标签左右各缩进一半，和资源管理器折行宽度对齐）
bool DesktopSurface::IconLabelScreenRect(const DesktopIconView* e, RECT& out) const {
    if (!e || !e->OnDesktop()) return false;
    const int ih = e->bmp.valid() ? e->bmp.height : iconPx_;
    const int lineH = IconLabelLineHeight();
    const int textTop = e->pos.y + kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap;
    int textH = cellH_ - (kIconTopPad + (ih > 0 ? ih : iconPx_) + kTextGap);
    if (textH > lineH * 2) textH = lineH * 2;
    if (textH < 14) textH = 14;
    const int left = e->pos.x + kLabelInset / 2;
    const int w = cellW_ - kLabelInset;
    out = RECT{left, textTop, left + w, textTop + textH};
    return true;
}

DesktopIconView* DesktopSurface::HitTestScreen(POINT screenPt) const {
    if (!icons_ || !hwnd_) return nullptr;
    if (mode_ != Mode::None) return nullptr;   // 拖拽/框选过程中不显示悬停
    // 盒子和图标层都是我们的窗口，且盒子盖在图标层上面。光标落在盒子上时
    // 图标层收不到鼠标消息，但整屏重画时按光标位置同步悬停就会「穿透」到盒
    // 子底下的图标上 —— 用 WindowFromPoint 确认最上层确实是图标层。
    if (WindowFromPoint(screenPt) != hwnd_) return nullptr;
    return HitTest(POINT{screenPt.x - origin_.x, screenPt.y - origin_.y});
}

void DesktopSurface::SyncHoverToCursor() {
    POINT p{};
    if (!GetCursorPos(&p)) return;
    DesktopIconView* hit = HitTestScreen(p);
    if (hit == hover_) return;
    DesktopIconView* old = hover_;
    hover_ = hit;
    if (old) old->hover = false;
    if (hit) hit->hover = true;

    // 不透明路径是「按裁剪区现画」的：这次同步如果发生在一次局部绘制中，
    // 旧/新格子可能都不在裁剪区内，得补一次失效，否则旧底色会留到下次全屏重画。
    // 补完下一次 WM_PAINT 就会把两格都画对，不会反复触发（同步只在换图标时发生）。
    if (opaque_ && hwnd_) {
        // 悬停框现在只到格子高度，但「选中」那个会撑到完整标签，所以还是用
        // IconExtent() 量（它按「是不是聚焦的那个」自己判断要不要撑高）。
        RECT ro = IconExtent(old);
        InflateRect(&ro, 2, 2);
        RECT rh = IconExtent(hit);
        InflateRect(&rh, 2, 2);
        if (old) InvalidateRect(hwnd_, &ro, FALSE);
        if (hit) InvalidateRect(hwnd_, &rh, FALSE);
    }
}

// 光标压在哪个桌面图标上 → 给它画半透明底。
// 只在「换了图标」时才重画，鼠标在同一格里移动不产生任何绘制。
void DesktopSurface::UpdateHover(POINT screenPt) {
    if (!icons_) return;
    DesktopIconView* hit = HitTestScreen(screenPt);
    if (hit == hover_) return;

    // 旧格子 + 新格子一起重画（多留 2px 给圆角和光晕）
    RECT u{};
    bool has = false;
    auto add = [&](DesktopIconView* e) {
        if (!e) return;
        // 悬停框只有格子高，但选中框会撑到完整标签；IconExtent() 两种都算得对。
        RECT r = IconExtent(e);
        InflateRect(&r, 2, 2);
        if (!has) {
            u = r;
            has = true;
        } else {
            u.left = std::min(u.left, r.left);
            u.top = std::min(u.top, r.top);
            u.right = std::max(u.right, r.right);
            u.bottom = std::max(u.bottom, r.bottom);
        }
    };
    add(hover_);
    add(hit);

    if (hover_) hover_->hover = false;
    hover_ = hit;
    if (hover_) hover_->hover = true;
    if (has) RepaintCells(u);
}

void DesktopSurface::ClearHover() {
    if (!hover_) return;
    DesktopIconView* old = hover_;
    hover_ = nullptr;
    old->hover = false;
    // 悬停框 = 一格；但被清掉的这个万一还是选中态，擦到完整标签为止
    RECT r = IconExtent(old);
    InflateRect(&r, 2, 2);
    RepaintCells(r);
}

void DesktopSurface::Destroy() {
    // 悬停指针指向调用方持有的图标，Destroy 之后可能就被销毁了，这里必须清掉
    hover_ = nullptr;
    mouseTracking_ = false;
    if (hwnd_) {
        FileDrop::Detach(hwnd_);   // OLE 那边还挂着这个 HWND，先摘掉再销毁
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    bits_ = nullptr;
    bufW_ = bufH_ = 0;
    if (solidDC_) { DeleteDC(solidDC_); solidDC_ = nullptr; }
    if (solidBmp_) { DeleteObject(solidBmp_); solidBmp_ = nullptr; }
    solidBits_ = nullptr;
    delete impl_;
    impl_ = nullptr;
}
