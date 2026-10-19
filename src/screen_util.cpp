#include "screen_util.h"

#include <algorithm>
#include <vector>

namespace ScreenUtil {
namespace {

struct MonitorCollect {
    std::vector<RECT> work;
};

BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* c = reinterpret_cast<MonitorCollect*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) c->work.push_back(mi.rcWork);
    return TRUE;
}

std::vector<RECT> AllWorkAreas() {
    MonitorCollect c;
    EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.work;
}

// 两个矩形的重叠面积（不相交 = 0）。用 long long 装，4K 多屏相乘也不会溢出 int。
long long OverlapArea(const RECT& a, const RECT& b) {
    const int l = std::max(a.left, b.left);
    const int r = std::min(a.right, b.right);
    const int t = std::max(a.top, b.top);
    const int bt = std::min(a.bottom, b.bottom);
    if (r <= l || bt <= t) return 0;
    return (long long)(r - l) * (long long)(bt - t);
}

RECT PrimaryWorkArea() {
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) return wa;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

}  // namespace

RECT VirtualScreen() {
    return RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

RECT WorkAreaForPoint(POINT pt) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    return PrimaryWorkArea();
}

RECT WorkAreaForRect(const RECT& r) {
    const std::vector<RECT> all = AllWorkAreas();
    if (all.empty()) return PrimaryWorkArea();

    // 1) 重叠面积最大的那块 → 主选
    long long best = -1;
    int bestIdx = -1;
    for (size_t i = 0; i < all.size(); ++i) {
        const long long a = OverlapArea(r, all[i]);
        if (a > best) {
            best = a;
            bestIdx = (int)i;
        }
    }
    if (bestIdx < 0 || best <= 0) {
        // 完全没压在任何屏上（拖到负坐标之类的极端情况）→ 交给系统找最近的
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT rr = r;
        HMONITOR mon = MonitorFromRect(&rr, MONITOR_DEFAULTTONEAREST);
        if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
        return PrimaryWorkArea();
    }

    // 2) 跨屏：把相交的几块并成包围盒，否则交界处会被轮流夹住
    RECT uni = all[(size_t)bestIdx];
    int hit = 1;
    for (size_t i = 0; i < all.size(); ++i) {
        if ((int)i == bestIdx) continue;
        if (OverlapArea(r, all[i]) <= 0) continue;
        uni.left = std::min(uni.left, all[i].left);
        uni.top = std::min(uni.top, all[i].top);
        uni.right = std::max(uni.right, all[i].right);
        uni.bottom = std::max(uni.bottom, all[i].bottom);
        ++hit;
    }
    (void)hit;
    return uni;
}

RECT ClampRect(const RECT& r, const RECT& wa) {
    RECT out = r;
    const int w = (int)(r.right - r.left);
    const int h = (int)(r.bottom - r.top);
    const int maxX = (int)wa.right - w;
    const int maxY = (int)wa.bottom - h;
    out.left = (int)wa.left;
    out.top = (int)wa.top;
    if (maxX > (int)wa.left) out.left = std::min(std::max((int)r.left, (int)wa.left), maxX);
    if (maxY > (int)wa.top) out.top = std::min(std::max((int)r.top, (int)wa.top), maxY);
    out.right = out.left + w;
    out.bottom = out.top + h;
    return out;
}

RECT WorkAreaForCursor() {
    POINT cur{};
    if (GetCursorPos(&cur)) return WorkAreaForPoint(cur);
    return PrimaryWorkArea();
}

}  // namespace ScreenUtil
