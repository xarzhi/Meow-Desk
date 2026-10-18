// 设置窗口（Fluent 风格）
//
// 不依赖 core-ui：自绘一个轻量面板就够了，且能保证和盒子/桌面层用同一套
// 视觉语言（圆角、accent、灰度层次）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <vector>

class SettingsWindow {
public:
    bool Create(HWND owner);
    void Destroy();
    void Show();
    void Hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }

    // 任何设置变化（外部据此让盒子/桌面层立刻生效）
    std::function<void()> onChanged;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureBackbuffer(int w, int h);
    void Layout();
    void Paint();
    void NotifyChanged();

    int HitTest(POINT pt) const;   // 返回控件 id，-1 无

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool dark_ = false;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;

    // 控件矩形
    RECT swatches_[8]{};
    RECT rcRadius_{};
    RECT rcAlpha_{};
    RECT rcAcrylic_{};
    RECT rcShadow_{};
    RECT rcArrow_{};

    int hot_ = -1;
    int sliderDrag_ = -1;
};
