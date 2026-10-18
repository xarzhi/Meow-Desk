#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// 自绘弹出菜单的一项。
// sub 非空 = 有二级菜单（右键打开，不需要点击箭头）。
struct PopupMenuItem {
    std::wstring text;
    int id = 0;
    bool separator = false;
    bool checked = false;    // 画一个对勾（单选组里表示当前项）
    bool disabled = false;
    std::vector<PopupMenuItem> sub;

    bool hasSub() const { return !sub.empty(); }

    PopupMenuItem() = default;
    explicit PopupMenuItem(std::wstring t, int i = 0)
        : text(std::move(t)), id(i) {}
    static PopupMenuItem Sep() {
        PopupMenuItem m;
        m.separator = true;
        return m;
    }
};

// 代码绘制的弹出菜单：圆角卡片 + 阴影 + 悬停高亮 + 二级菜单。
// 全程走分层窗口（UpdateLayeredWindow），不使用系统菜单。
class PopupMenu {
public:
    bool Create();
    void Release();

    // 弹出。items 会被拷贝保存，调用方不必保活。
    void Show(POINT screenPt, std::vector<PopupMenuItem> items);
    void Hide();
    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

    void SetOwner(HWND h) { owner_ = h; }

    // 点中某个叶子项（id > 0）时回调
    std::function<void(int id)> onCommand;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void Layout();
    void Paint();
    void Present();
    void EnsureBuffer(int w, int h);
    int HitTest(POINT client) const;
    void OpenSub(int index);
    void CloseSub();
    void Invoke(int index);
    bool PointInside(POINT screenPt) const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool visible_ = false;
    bool light_ = true;

    std::vector<PopupMenuItem> items_;
    std::vector<RECT> itemRects_;   // 客户区坐标
    int hover_ = -1;
    int openSub_ = -1;
    int contentW_ = 0;
    int contentH_ = 0;
    int w_ = 0;
    int h_ = 0;

    PopupMenu* sub_ = nullptr;      // 二级菜单（独立窗口实例）

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
};
