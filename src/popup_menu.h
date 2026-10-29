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
    // 贴着某个矩形弹：卡片左上角对在 rect 的**右下角**，右边放不下就翻到
    // rect 左边，下面放不下就往上贴 rect 底边。
    // 桌面框选之后那个「创建盒子」菜单用它 —— 菜单跟着那个蓝框走，
    // 而不是钉在松手时的鼠标位置（手一抖菜单就飘到框外去了）。
    void ShowAnchoredToRect(const RECT& rect, std::vector<PopupMenuItem> items);
    // notify=false：只把窗口藏掉，**不**触发 onClose（Invoke 用：命令跑完再通知）
    void Hide(bool notify = true);
    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

    void SetOwner(HWND h) { owner_ = h; }

    // 点中某个叶子项（id > 0）时回调
    std::function<void(int id)> onCommand;
    // 菜单**真正关掉**时回调一次（点了某一项、点外面、Esc 都算）。
    // 用来在菜单开着期间占住宿主窗口（比如伸缩模式的盒子不能被自动收起）。
    // 回调只触发一次，触发后就自动清空，不会串到下一个菜单。
    std::function<void()> onClose;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void Layout();
    void Paint();
    void Present();
    // 建好 items / 量完尺寸 / 画完之后，把窗口摆到 (x,y) 并显示出来。
    // Show / ShowAnchoredToRect 只负责算这个坐标。
    void PresentAt(int x, int y);
    void EnsureBuffer(int w, int h);
    // 摘下来再调：回调里即使重设 onClose 也不会在执行中把 std::function 自己析构掉
    void NotifyClosed();
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
