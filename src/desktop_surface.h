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
    int group = 0;           // 所属分组（盒子内标签页下标，owner >= 0 时有效）
    bool selected = false;   // 选中
    bool hover = false;      // 鼠标悬停（画一层半透明底）
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
    // 是否显示图标标题
    void SetShowLabels(bool on);
    // 强制重建全部缓存（字体/DPI 变化时）
    void InvalidateCache();

    bool Create();
    void Redraw();
    void Destroy();

    // 图标列表要变动（对象析构 / 从 vector 里被移走）之前必须调一下：
    // hover_ 只是个裸指针，图标一没它就成了悬垂指针，下次重画会踩野内存。
    // 这里只丢指针不重画（调用方紧接着都会自己刷新），所以开销可以忽略。
    void ForgetHover() {
        if (hover_) {
            hover_->hover = false;
            hover_ = nullptr;
        }
    }

    HWND hwnd() const { return hwnd_; }
    bool dragging() const { return mode_ != Mode::None; }

    // 找一个离 wantCell 最近的空闲格子（屏幕坐标，格子左上角）。
    // exclude 里的图标不算占位。放不下即返回原值。
    POINT SnapToFreeCell(POINT wantCell, const std::vector<DesktopIconView*>& exclude) const;

    // 同上，但显式指定网格锚点（anchor 必须是某个已知落在网格上的格子左上角）。
    // 真实桌面的网格不从屏幕 (0,0) 起算，用绝对坐标取整会让图标落下时整体错位，
    // 所以吸附一律相对锚点取整。
    POINT SnapToFreeCellAt(POINT wantCell, POINT anchor,
                           const std::vector<DesktopIconView*>& exclude) const;

    // 离 nearPt 最近的桌面图标位置（没有桌面图标时退回 origin_）
    POINT AutoGridAnchor(POINT nearPt) const;

    // 把「想要的左上角」吸附到网格格子（不做占位判断）
    POINT SnapCell(POINT want) const;

    // 光标在 (x,y) 松手时图标会落到哪个格子（用于落点预览和盒子 → 桌面的落位）
    POINT CellForCursor(POINT cursor, const IconBitmap* icon) const;

    // 屏幕点上有没有图标（**不带**拖拽/层级那两条约束）。
    // 拖放的落点判定要用这个：松手那一刻还在 drag 模式里，
    // HitTestScreen 会因为 mode_ != None 一律返回 nullptr。
    DesktopIconView* ItemAtScreen(POINT screenPt) const;

    // 桌面左上角起、按列（先从上到下扫完一列，再往右）的第一个空闲格子。
    // 删除盒子 / 删除分组把图标倒回桌面时用：图标会从左上角开始一个挨一个往下排，
    // 一列排满再换右边一列，而不是各自「就近抢一个」格子撒得满屏都是。
    POINT FirstFreeCell(const std::vector<DesktopIconView*>& exclude) const;

    // 桌面内拖放的落位：目标格空就直接用；被别的图标占了就和它交换
    // （资源管理器也是这个行为）。否则桌面排满时环形搜索会把图标甩到很远，
    // 观感就是「松手后图标跑掉了」。被挪走的图标通过 swapped 返回，调用方要写回位置。
    POINT ResolveDropCell(POINT wantCell, DesktopIconView* mover,
                          const std::vector<DesktopIconView*>& exclude,
                          std::vector<DesktopIconView*>* swapped) const;

    // 落点预览：在「松手会落到的格子」画一个半透明直角框。on=false 隐藏。
    void SetDropPreview(bool on, POINT cellTopLeft = POINT{0, 0});
    // 拖拽插入缝隙线（屏幕坐标）：拖到别的桌面图标左右时，告诉用户「松手会插到这儿」。
    // 数学和盒子里的缝隙线一致：线落在两格之间的中线上。
    void SetDropSeam(bool on, int screenX, int top, int h);

    // ---- 交棒给系统级拖放（拖到别的程序窗口上时）----
    // 当前正在拖的那一批（外部要拿它们拼 CF_HDROP）
    const std::vector<DesktopIconView*>& draggingItems() const { return dragSet_; }
    // 是否已经真的进入拖动（超过 3px 阈值）
    bool dragActive() const { return mode_ == Mode::Move && moved_; }
    // 放弃这次拖动、图标回原位。
    // 必须在 DoDragDrop **之前**调：交棒之后鼠标松开那一下会被 OLE 的拖放循环
    // 吃掉，我们收不到 WM_LBUTTONUP，状态只能自己清。
    void AbortDrag();

    // 拖到外部（盒子）时询问是否接收；返回 true 表示这批图标被拿走
    std::function<bool(const std::vector<DesktopIconView*>&, POINT screenPt)> onDrop;
    // 拖动中判断光标是否落在外部目标（盒子）上：是则图标跟手走，不再吸附格子
    std::function<bool(POINT screenPt)> overTarget;
    // 拖动中每帧回调（屏幕坐标）：外部据此更新盒子的「插入缝隙线」提示
    std::function<void(POINT screenPt)> onDragHover;
    // 拖动结束、位置变了 → 外部负责写回资源管理器
    std::function<void(const std::vector<DesktopIconView*>&)> onMoved;
    // 拖动中：外部用拖拽浮层把图标画到盒子上面（否则会被盒子盖住）
    std::function<void(const std::vector<DesktopIconView*>&)> onDragUpdate;
    std::function<void()> onDragEnd;
    // 双击打开
    std::function<void(DesktopIconView*)> onOpen;
    // 右键菜单执行完（可能改名/删除了文件）→ 外部重新枚举
    std::function<void()> onContextInvoked;
    // 周期性心跳（约 500ms）
    std::function<void()> onTick;

private:
    // 这个格子（左上角 x,y）上有没有桌面图标挡着。exclude 里的不算。
    // SnapToFreeCellAt 和 FirstFreeCell 共用同一套判定，免得两处口径走偏。
    bool CellIsFree(int x, int y, int cw, int ch,
                    const std::vector<DesktopIconView*>& exclude) const;
    enum class Mode { None, Move, Marquee };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    DesktopIconView* HitTest(POINT clientPt) const;
    RECT CellRect(const DesktopIconView* e) const;

    // 悬停：光标压到哪个图标上 → 给它画一层半透明底（和选中态区分开）
    void UpdateHover(POINT screenPt);
    void ClearHover();
    // 「这个屏幕点上应该悬停哪个图标」。比 HitTest 多了两条约束：
    //  1) 桌面层必须真的是这个点最上层的窗口 —— 否则光标压在盒子上时，
    //     盒子底下那个图标也会被点亮（盒子和图标层都是我们的窗口）；
    //  2) 拖拽/框选过程中一律不悬停。
    DesktopIconView* HitTestScreen(POINT screenPt) const;
    // 按「当前光标位置」重新算一次悬停标志，不触发绘制。
    // 悬停本来只跟 WM_MOUSEMOVE 走，但图标格子会因刷新图标/恢复布局/盒子开关
    // 整体搬家，那时光标没动，缓存下来的悬停格子就是旧的。整屏重画前同步一次最省心。
    void SyncHoverToCursor();
    // 只重画若干格子（分层窗口每次仍要整体上传，但不重画 200 个图标）
    void RepaintCells(const RECT& r);
    void UploadLayered();

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
    // 桌面空白处的点击也要能收到：per-pixel alpha 分层窗口里 alpha==0 的像素
    // 会让鼠标消息穿透，所以把整层的 alpha 抬到 1（视觉上不可见）
    void MakeHitTestable();
    // 落点预览框（两条渲染路径各一份）
    void DrawPreviewLayered();
    void DrawPreviewGdi(HDC hdc);
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
    bool showLabels_ = true;

    DesktopIconView* hover_ = nullptr;   // 当前悬停的桌面图标
    bool mouseTracking_ = false;          // 已经注册了 TME_LEAVE

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
    bool dragLayerShown_ = false;   // 拖拽浮层已经开始接管这批图标
    bool marqueeAdditive_ = false;
    bool previewOn_ = false;        // 落点预览框是否显示
    POINT previewCell_{0, 0};       // 预览格子左上角（屏幕坐标）
    // 拖拽插入缝隙线（屏幕坐标；只在「拖到别的图标左右」时出现）
    bool seamOn_ = false;
    int seamX_ = 0;
    int seamTop_ = 0;
    int seamH_ = 0;
    // 缝隙线对应的落点格子（屏幕坐标）：松手时优先用它，而不是光标下的格子 ——
    // 缝隙线画在两格之间的缝里，图标就该落在缝旁边那一格，不然线和结果对不上。
    bool seamCellValid_ = false;
    POINT seamCell_{0, 0};
    void DrawSeamLayered();
    void DrawSeamGdi(HDC hdc);
    std::vector<DesktopIconView*> dragSet_;
    std::vector<POINT> dragStartPos_;
    POINT pressScreen_{0, 0};
    POINT curScreen_{0, 0};
    POINT marqueeA_{0, 0};
    POINT marqueeB_{0, 0};

    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------------------
// 系统原生右键菜单（资源管理器那套 IContextMenu）
//
// 桌面层的右键菜单本来就走的这条路，盒子里右键图标也复用它 —— 用户在盒子里
// 右键一个文件，看到的应该和资源管理器里右键同一个文件完全一致
// （打开 / 打开方式 / 发送到 / 剪切 / 复制 / 删除 / 属性…），而不是我们自绘的菜单。
//
// owner 必须是「菜单的宿主窗口」。弹出期间带图标的项是 shell 自绘的，
// 宿主窗口要把 WM_INITMENUPOPUP / WM_MEASUREITEM / WM_DRAWITEM 转给
// ForwardShellMenuMessage，否则那几项会塌成 0 高度看不见。
void ShowShellItemMenu(HWND owner, const std::wstring& path, POINT screenPt);

// 返回 true = 这条消息已经被菜单处理掉了，宿主窗口直接 return TRUE / 0
bool ForwardShellMenuMessage(HWND owner, UINT msg, WPARAM wp, LPARAM lp);

// 菜单里的「重命名」由谁执行。
//
// 关键：QueryContextMenu 必须带 CMF_CANRENAME 才会出现「重命名」项 ——
// 而这个标志等于向 shell 承诺「宿主能就地改名」。shell 的 rename 动词背后
// 要求宿主是 IShellView（资源管理器靠它开原地编辑框），我们不是，
// 所以点下去什么都不发生。于是：菜单项交给 shell，真正改名交给这个回调。
//
// 回调返回 true = 已经接管（不再把命令转发给 InvokeCommand）。
void SetShellMenuRenameHandler(std::function<bool(HWND owner, const std::wstring& path)> fn);
