// 托盘图标 + 右键菜单
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>

class TrayIcon {
public:
    bool Create(HWND owner);
    void Destroy();
    void SetDesktopIconsVisible(bool v) { desktopIconsVisible_ = v; }

    std::function<void()> onSettings;
    std::function<void()> onReloadIcons;
    std::function<void()> onToggleDesktopIcons;
    std::function<void()> onExit;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool added_ = false;
    bool desktopIconsVisible_ = false;
};
