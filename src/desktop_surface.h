// 桌面层：隐藏资源管理器的真实图标，自己把图标画在壁纸上。
//
// 两条渲染路径：
//  1) 默认（不透明窗口，省内存）：窗口不透明，WM_PAINT 里用 PaintDesktop
//     把壁纸按裁剪区重画一遍当背景，再用 AlphaBlend 把图标/文字贴上去。
//     与资源管理器画桌面的方式一致，没有任何整屏缓冲。
//  2) MEOW_SURFACE=layered（兜底，旧实现）：WS_EX_LAYERED + UpdateLayeredWindow，
//     整屏 32bpp DIB 走 per-pixel alpha。视觉一样，但要多花约 29MB。
//
// 为什么不用 core-ui：core-ui 走 DXGI/DComp 直接合成，WS_EX_LAYERED 的色键和
// alpha 全部无效，窗口必然是黑底。
//
// 图标/文字都在 icon_render 里预生成为预乘 alpha 的小 DIB，
// 用 AlphaBlend 1:1 贴上去 → 零重采样，不发虚、无白边。
// 详见 icon_render.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "shell_icons.h"

// 桌面上的一个图标（视图数据）
struct DesktopIconView {
    std::wstring path;       // 解析名（文件路径或 ::{CLSID}）
    std::wstring name;       // 显示名
    POINT pos{0, 0};         // 屏幕物理像素（格子左上角），已落位的位置
    POINT dragPos{0, 0};     // 拖动中的临时位置
    IconBitmap bmp;          // 图标位图（预乘 alpha）
    int owner = -1;          // -1 = 桌面；>=0 = 第几个盒子
    bool selected = false;   // 选中
    void* pidl = nullptr;    // 资源管理器的 PIDL（写回位置用），由调用方持有

    bool OnDesktop() const { return owner < 0; }
};

class DesktopSurface {
public:
    static constexpr int kIconTopPad = 4;   // 图标相对格子顶部的偏移
    static constexpr int kTextGap = 2;      // 图标和文字之间的空隙

    // 图标数据由调用方持有，这里只存指针（vector 地址必须稳定）
    void SetIcons(std::vector<DesktopIconView*>* icons) { icons_ = icons; }

    // 图标格子尺寸（应与资源管理器桌面的 GetSpacing 一致）
    void SetCellSize(int w, int h) { cellW_ = w; cellH_ = h; }

    // 图标尺寸（用于算文字盒高度）
    void SetIconPx(int px) { iconPx_ = px; }

    // 文字阴影开关
    void SetTextShadow(bool on);
    // 强制重建全部缓存（字体/DPI 变化时）
    void InvalidateCache();

    bool Create();
    void Redraw();
    void Destroy();

    HWND hwnd() const { return hwnd_; }
    bool dragging() const { return mode_ != Mode::None; }

    // 拖到外部（盒子）时询问是否接收；返回 true 表示这批图标被拿走
    std::function<bool(const std::vector<DesktopIconView*>&, POINT screenPt)> onDrop;
    // 拖动中判断光标是否落在外部目标（盒子）上：是则图标跟手走，不再吸附格子
    std::function<bool(POINT screenPt)> overTarget;
    // 拖动结束、位置变了 → 外部负责写回资源管理器
    std::function<void(const std::vector<DesktopIconView*>&)> onMoved;
    // 拖动中：外部用拖拽浮层把图标画到盒子上面（否则会被盒子盖住）
    std::function<void(const std::vector<DesktopIconView*>&)> onDragUpdate;
    std::function<void()> onDragEnd;
    // 双击打开
    std::function<void(DesktopIconView*)> onOpen;
    // 周期性心跳（约 500ms）
    std::function<void()> onTick;

private:
    enum class Mode { None, Move, Marquee };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    DesktopIconView* HitTest(POINT clientPt) const;
    RECT CellRect(const DesktopIconView* e) const;

    void ClearSelection();
    void SelectOnly(DesktopIconView* e);
    void ToggleSelection(DesktopIconView* e);
    void UpdateMarqueeSelection();

    bool EnsureBackbuffer(int w, int h);
    void PaintLayered();                                  // 旧路径：整屏 32bpp DIB + UpdateLayeredWindow
    void PaintOpaque(HDC hdc);                            // 新路径：PaintDesktop 壁纸兜底 + AlphaBlend 图标
    void DrawOneLayered(DesktopIconView* e, bool dragging);
    void DrawOneGdi(HDC hdc, DesktopIconView* e, bool dragging);
    void FillRectBlend(HDC hdc, const RECT& r, int a, COLORREF color);
    void InvalidateCell(const DesktopIconView* e, POINT pos);
    void Dirty(const RECT& r);
    void FlushDirty();
    bool EnsureSolidDib();

    std::vector<DesktopIconView*>* icons_ = nullptr;

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    POINT origin_{0, 0};
    int cellW_ = 75;
    int cellH_ = 98;
    int iconPx_ = 48;
    bool textShadow_ = true;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;

    // 不透明路径（MEOW_SURFACE=opaque 时才用）：窗口不透明，背景用 PaintDesktop 现画壁纸，
    // 省掉整屏 DIB（2560x1440x4 ≈ 14.7MB）。只适合静态壁纸，动态壁纸会画错，故非默认。
    bool opaque_ = false;
    HDC solidDC_ = nullptr;      // 1x1 纯色 DIB，用于半透明填充
    HBITMAP solidBmp_ = nullptr;
    void* solidBits_ = nullptr;
    RECT dirty_{};
    bool dirtyValid_ = false;

    Mode mode_ = Mode::None;
    bool moved_ = false;
    bool marqueeAdditive_ = false;
    std::vector<DesktopIconView*> dragSet_;
    std::vector<POINT> dragStartPos_;
    POINT pressScreen_{0, 0};
    POINT curScreen_{0, 0};
    POINT marqueeA_{0, 0};
    POINT marqueeB_{0, 0};

    struct Impl;
    Impl* impl_ = nullptr;
};
