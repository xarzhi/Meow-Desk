// 收纳盒：纯 Win32 per-pixel alpha 分层窗口 + GDI/GDI+ 自绘
//
// 为什么不用 core-ui：core-ui 主窗口的 swap chain 不带 alpha（实测
// SetLayeredWindowAttributes / DwmEnableBlurBehindWindow / SetWindowCompositionAttribute
// 全部无效），窗口必然是不透明矩形，半透明面板只能和黑底混合。
// 这里用 UpdateLayeredWindow 自己提供 BGRA 位图，才能真透出壁纸。
//
// 附带红利：完全透明（alpha=0）的像素天然鼠标穿透，所以圆角外面那块
// 不会挡住下面的桌面图标层，不需要额外的裁剪区域。
//
// 盒子不持有图标数据：它按 owner == index() 从全局图标表里筛，
// 这样"盒子→盒子""盒子→桌面"的搬运只是改一个 int。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "config.h"
#include "desktop_surface.h"

class FenceWindow {
public:
    bool Create(int index, int x, int y, int w, int h);
    void Destroy();

    // 全局图标表（内部按 owner 过滤）
    void SetIcons(std::vector<DesktopIconView*>* all) { all_ = all; }

    int index() const { return index_; }
    HWND hwnd() const { return hwnd_; }
    RECT rect() const;

    // 配置变化（颜色/圆角/透明度/亚克力/标题）→ 重绘。
    // recapture = true 时才重新抓取亚克力底图（很贵，拖动滑块时不要带）
    void ApplyConfig(bool recapture = false);

    void Redraw();
    void SetCollapsed(bool collapsed);
    bool collapsed() const { return collapsed_; }

    // 屏幕坐标是否落在盒子矩形内（含收起后的头部）
    bool HitTest(POINT screenPt) const;

    // 盒子内容区（去掉头部）的屏幕矩形，用来判断"松手时算不算放进了盒子"
    RECT bodyRect() const;

    // 几何变化（移动后）→ 外部保存配置
    std::function<void()> onGeometryChanged;
    // 点击盒内图标
    std::function<void(DesktopIconView*)> onOpen;
    // 盒内图标被拖出：开始 / 移动 / 松手（POINT 为屏幕坐标）
    std::function<void(DesktopIconView*, POINT)> onItemDragStart;
    std::function<void(DesktopIconView*, POINT)> onItemDragMove;
    std::function<void(DesktopIconView*, POINT)> onItemDrop;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void EnsureBackbuffer(int w, int h);
    void Paint();
    void Present();
    void RefreshBackdrop();

    void Collect(std::vector<DesktopIconView*>& out) const;
    int HitItem(POINT clientPt) const;
    // 第 index 个格子的客户区原点；放不下返回 false
    bool ItemOrigin(int index, int& x, int& y) const;

    HWND hwnd_ = nullptr;
    int index_ = 0;
    FenceConfig* cfg_ = nullptr;

    int width_ = 0;
    int height_ = 0;   // 展开时的高度
    int curH_ = 0;
    int targetH_ = 0;
    bool collapsed_ = false;

    std::vector<DesktopIconView*>* all_ = nullptr;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;

    // 亚克力底图
    std::vector<uint8_t> backdrop_;
    int backdropX_ = 0, backdropY_ = 0;

    DesktopIconView* dragged_ = nullptr;   // 正在拖出盒子的那个图标
    int hot_ = -1;
    int pressItem_ = -1;
    bool pressArrow_ = false;
    bool pressHeader_ = false;
    bool pressBody_ = false;
    bool dragging_ = false;
    bool itemDragging_ = false;
    bool trackingLeave_ = false;
    POINT pressPt_{0, 0};
    POINT pressScreenPt_{0, 0};
    int dragLastX_ = 0;
    int dragLastY_ = 0;
    RECT dragStartRect_{};
};
