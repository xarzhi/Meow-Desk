// 全局快捷键唤出的「搜索面板」：搜索桌面上所有图标的名称，回车/单击直接打开。
//
// 为什么用真 EDIT 控件而不是自绘输入框：中文图标名要靠 IME 输入，
// 自绘文本拿不到 IME 组合窗口。EDIT 放在卡片里，只把边框和底色去掉。
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

#include "desktop_surface.h"

class SearchWindow {
public:
    bool Create();
    void Destroy();

    // 图标数据由外部持有（全部桌面图标，含已经收进盒子的）
    void SetIcons(std::vector<DesktopIconView*>* all) { all_ = all; }

    void Toggle();
    void ShowPalette();
    void Hide();

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

    // 打开某个图标
    std::function<void(DesktopIconView*)> onOpen;
    // 过滤结果变化（调试/自测用：把命中的名字 dump 出来）
    std::function<void(const std::vector<DesktopIconView*>&)> onFiltered;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK LowLevelKb(int, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void Layout();
    void Repaint();
    void Filter();
    void MoveSel(int delta);
    void Activate();
    int RowsVisible() const;
    RECT RowRect(int i) const;

    // 输入法组合串（未上屏的拼音）。面板要能在组字过程中实时过滤，
    // 同时组合态下 ESC 会被 IME 吃掉，所以顺手装个低级键盘钩子兜住 Esc。
    void SyncImeComposition(HWND edit);
    void HookKeyboard();
    void UnhookKeyboard();

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    bool dark_ = false;
    bool visible_ = false;
    HHOOK kbHook_ = nullptr;

    std::wstring imeComp_;
    std::vector<DesktopIconView*>* all_ = nullptr;
    std::vector<DesktopIconView*> hits_;
    int sel_ = 0;
    int hot_ = -1;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
};
