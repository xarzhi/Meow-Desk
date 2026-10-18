// 拖拽浮层：盒子之间 / 盒子与桌面之间拖动图标时显示的那个"跟着鼠标走的图标"
//
// 为什么要单独一个窗口：
//   桌面层在盒子下面，盒子在桌面层上面。如果浮层画在桌面层里，拖动到另一个
//   盒子范围内时会被那个盒子盖住。所以浮层需要比所有盒子更靠上（但仍然在
//   所有应用窗口之下，和盒子同一条层级带）。
//
// 窗口尺寸只覆盖当前浮层图标的包围盒（不是整屏），所以内存开销可以忽略。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "icon_render.h"
#include "shell_icons.h"

struct DragGhost {
    const IconBitmap* bmp = nullptr;  // 由调用方持有
    int x = 0;                        // 图标左上角（屏幕坐标）
    int y = 0;
    const std::wstring* name = nullptr;  // 可选：图标名称，画在图标正下方
};

class DragLayer {
public:
    bool Create();
    void Destroy();
    HWND hwnd() const { return hwnd_; }

    // 名称标签的排版盒子（由调用方按格子尺寸定，和桌面标签保持一致）
    void SetLabelLayout(int boxW, int boxH, bool shadow, bool show) {
        if (boxW == labelBoxW_ && boxH == labelBoxH_ && shadow == labelShadow_ &&
            show == showLabel_)
            return;   // 没变就别把标签缓存丢掉（拖动中重建会闪）
        labelBoxW_ = boxW;
        labelBoxH_ = boxH;
        labelShadow_ = shadow;
        showLabel_ = show;
        labelOwner_.clear();
        label_.reset();
    }

    // 显示/更新浮层（屏幕坐标）；ghosts 为空则隐藏
    void Show(const std::vector<DragGhost>& ghosts);
    void Hide();

private:
    bool EnsureBuffer(int w, int h);
    void Present(int x, int y, int w, int h);

    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
    bool visible_ = false;

    // 名称标签（只有一张，按名字缓存，拖动过程中不用每帧重建）
    int labelBoxW_ = 67;
    int labelBoxH_ = 34;
    bool labelShadow_ = true;
    bool showLabel_ = true;
    std::wstring labelOwner_;
    std::unique_ptr<LabelDib> label_;
    LabelDib* LabelFor(const std::wstring& name);
};
