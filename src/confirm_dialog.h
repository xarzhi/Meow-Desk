// 自绘确认框
//
// 为什么不用 MessageBoxW：整个程序是自绘的浅/深色卡片风格，系统消息框一出来
// 就是一块白底 + 系统按钮，和右键菜单、设置面板完全不是一个世界。
// 这里照 PopupMenu 那套做法画（分层窗口 UpdateLayeredWindow + 圆角卡片 + 多层淡阴影），
// 危险动作的按钮用系统的「删除红」。
//
// 语义：只有点「确定」才回调；取消 / Esc / 点到框外 / 窗口失去焦点都只是关掉。
// 同一时刻只存在一个实例（删除盒子是低频操作）。
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

class ConfirmBox {
public:
    // owner：确认框摆在它中间（owner 为空则摆屏幕中间）。
    // text 支持用 \n 手动换行，最多 3 行（再多会挤到按钮上）。
    void Show(HWND owner, const std::wstring& title, const std::wstring& text,
              const std::wstring& okText, std::function<void()> onOk);
    void Hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureWindow();
    void Relayout();                 // 按正文行数算尺寸 + 按钮矩形
    void EnsureBuffer(int w, int h);
    void Paint();
    void Present();
    int HitTest(POINT client) const;   // 1 = 确定，0 = 取消，-1 = 空白
    void Accept();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    std::wstring title_;
    std::wstring text_;
    std::wstring okText_;
    std::function<void()> onOk_;

    // 只有真正被激活过一次之后，才把「失活」当成取消 ——
    // 热键唤出的窗口可能一显示就被系统拒绝前台，那样会立刻自己关掉。
    bool activated_ = false;
    int hover_ = -1;
    int press_ = -1;

    RECT rcOk_{};
    RECT rcCancel_{};

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
    int cardW_ = 0;
    int cardH_ = 0;
};
