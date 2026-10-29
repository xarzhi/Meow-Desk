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
    // 「定位」某个图标：面板会先自己关掉，再由外部把这个图标的位置高亮出来
    // （桌面图标闪桌面层，盒内图标让盒子展开/滚动/闪两下）
    std::function<void(DesktopIconView*)> onLocate;
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
    // 第 i 行右侧「定位」按钮的矩形（点击后关面板 + 高亮那个图标的位置）
    void Locate(int i);
    int RowsVisible() const;
    RECT RowRect(int i) const;
    RECT LocateRect(int i) const;

    // ---- 常用（右上角）----
    //
    // 列表每行右侧有一个星形按钮，点一下把这个图标收进右上角的「常用」区，
    // 最多 5 个。常用项按 path（解析名）落盘，重启后还在；点击常用项直接打开它。
    //
    // 为什么只存 path：DesktopIconView* 是每次枚举重建的，存指针跨会话必悬垂；
    // name 可能重名，path 才唯一且稳定（和 icon_layout 表同一套理由）。
    RECT FavBarRect(int i) const;        // 右上角第 i 个常用项（0..kFavMax-1）
    RECT AddFavRect(int i) const;        // 第 i 行右侧的「加入常用」按钮
    bool IsFav(const DesktopIconView* e) const;
    void ToggleFav(DesktopIconView* e);  // 已在常用里 → 移除；否则（未满）追加
    void OpenFav(int i);                 // 点常用项：关面板 + 打开它
    // 把 favs_ 里的 path 反查成当前的 DesktopIconView*（找不到的留 nullptr）。
    // 面板每次弹出时算一次就够了：面板开着的时候图标列表不会变。
    void ResolveFavPtrs();
    void LoadFavs();
    void SaveFavs();

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
    int hotLoc_ = -1;   // 光标压在哪一行的「定位」按钮上（-1 = 没有）
    int hotFav_ = -1;   // 光标压在哪一行的「加入常用」按钮上（-1 = 没有）
    int hotFavBar_ = -1;  // 光标压在第几个常用项上（-1 = 没有）

    // 常用项（解析名，顺序 = 显示顺序，最多 kFavMax 个）以及它们的反查结果。
    // favPtrs_ 和 favs_ 等长、逐项对应；nullptr = 这个图标当前不在了（文件被删 /
    // 换了盘），那一格就不画。
    std::vector<std::wstring> favs_;
    std::vector<DesktopIconView*> favPtrs_;
    bool favLoaded_ = false;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;
};
