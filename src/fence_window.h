// 收纳盒：纯 Win32 per-pixel alpha 分层窗口 + GDI/GDI+ 自绘
//
// 为什么不用 core-ui：core-ui 主窗口的 swap chain 不带 alpha（实测
// SetLayeredWindowAttributes / DwmEnableBlurBehindWindow / SetWindowCompositionAttribute
// 全部无效），窗口必然是不透明矩形，半透明面板只能和黑底混合。
// 这里用 UpdateLayeredWindow 自己提供 BGRA 位图，才能真透出壁纸。
//
// 附带红利：完全透明（alpha=0）的像素天然鼠标穿透，所以圆角外面那块
// 不会挡住下面的桌面图标层，不需要额外的裁剪区域。
//
// 盒子不持有图标数据：它按 owner == index() 从全局图标表里筛，
// 这样"盒子→盒子""盒子→桌面"的搬运只是改一个 int。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "desktop_surface.h"
#include "icon_render.h"

class FenceWindow {
public:
    static constexpr int kCellHCompact = 62;   // 不显示标题时的格子高度
    bool Create(int index, int x, int y, int w, int h);
    void Destroy();

    // g_config.fences 是 vector：新建盒子时 push_back 可能把元素搬到新地址，
    // 盒子手里指向自己那条配置的指针就悬空了。往容器里加过东西之后必须重绑一次。
    void RebindConfig() {
        if (index_ >= 0 && index_ < (int)g_config.fences.size()) cfg_ = &g_config.fences[index_];
    }

    // 全局图标表（内部按 owner 过滤）
    void SetIcons(std::vector<DesktopIconView*>* all) { all_ = all; }

    int index() const { return index_; }
    HWND hwnd() const { return hwnd_; }
    RECT rect() const;

    // 配置变化（颜色/圆角/透明度/亚克力/标题）→ 重绘。
    // recapture = true 时才重新抓取亚克力底图（很贵，拖动滑块时不要带）
    void ApplyConfig(bool recapture = false);

    void Redraw();
    void SetCollapsed(bool collapsed);
    bool collapsed() const { return collapsed_; }

    // 拖动时用来避让的其他盒子矩形（屏幕坐标），由 main 提供。
    //
    // 这里用「全局一份、按指针认自己」而不是「每个盒子各存一个捕获了下标的 lambda」：
    // 盒子下标是数组下标，新建/删除都会变，捕获下标的 lambda 一旦错位，
    // 轻则把自己当成障碍（盒子被自己弹开），重则一个障碍都拿不到（可以随意重叠）。
    // 传 this 进来排除自己，永远不会错。
    static void SetObstacleProvider(std::function<std::vector<RECT>(const FenceWindow*)> fn);

    // 屏幕坐标是否落在盒子矩形内（含收起后的头部）
    bool HitTest(POINT screenPt) const;

    // 盒子内容区（去掉头部）的屏幕矩形，用来判断"松手时算不算放进了盒子"
    RECT bodyRect() const;

    // ---- 布局尺寸（右键菜单的「查看」会改这些）----
    int headerH() const { return headerH_; }
    int cellH() const;
    int cellW() const { return cellW_; }

    // 「查看」菜单：0=小 1=中 2=大 3=超大 4=列表 5=详情
    // 会顺便重算格子尺寸、清掉标签缓存并重绘
    void SetViewMode(int m);
    int viewMode() const;
    // 当前「查看」档位下图标该画的像素尺寸（32/48/64/96）
    int iconPx() const;

    // 「排序方式」：0=名称 1=类型 2=大小 3=修改日期 4=自定义
    // 会立刻重排本盒子的图标顺序（g_icons 里相对顺序），自定义 = 保持原样
    void SetSort(int key, bool asc);
    int sortKey() const;
    bool sortAsc() const;

    // 标题（右键菜单「重命名分区」）
    void SetTitle(const std::wstring& t);
    std::wstring title() const;

    // ---- 拖拽落点提示 ----
    // zone: -1 = 插到这个图标左边，+1 = 插到右边，0 = 中间（用拖拽文件打开该图标的程序）
    struct DropHint {
        int item = -1;    // 目标图标下标（-1 = 没有落在任何图标上）
        int zone = 0;     // -1/0/+1
        int insert = -1;  // 插入位置（0..count），-1 表示追加到末尾
        bool active() const { return item >= 0; }
        bool openWith() const { return item >= 0 && zone == 0; }
    };
    // 由外部（main）在拖拽过程中每帧调用：pt 为屏幕坐标
    void UpdateDropHint(POINT screenPt);
    void ClearDropHint();
    DropHint dropHint() const { return drop_; }
    // 提示区域对应的屏幕矩形（用于算"插到第几个"）
    int itemCount() const;

    // 头部右侧的锁按钮：锁上之后盒子不能拖动、也不能拖边缘改尺寸
    RECT LockRect() const;    bool locked() const { return cfg_ && cfg_->locked; }
    void SetLocked(bool on);
    void ToggleLock() { SetLocked(!locked()); }

    // ---- 分组（头部的一排标签）----
    // 没有分组时 groupCount()==1，头部照旧只显示盒子标题；一旦「创建分组」，
    // groups[0] 就变成盒子标题本身，后面追加的才是新分组。
    // 每个图标只属于一个分组（DesktopIconView::group），切分组等于换一批图标。
    int groupCount() const {
        return (cfg_ && !cfg_->groups.empty()) ? (int)cfg_->groups.size() : 1;
    }
    bool hasTabs() const { return groupCount() > 1; }
    int activeGroup() const;
    void SetActiveGroup(int g);
    // 新建一个分组并切过去，返回新分组下标
    int AddGroup(const std::wstring& name = std::wstring());
    // 删掉当前分组（第 0 个是盒子标题，删不掉）。组里的图标退回第 0 组。
    void RemoveActiveGroup();
    void SetGroupName(int i, const std::wstring& n);
    std::wstring GroupName(int i) const;
    std::wstring activeGroupName() const { return GroupName(activeGroup()); }
    // 各标签的客户区矩形（只含放得下的那些；顺序和分组下标一致）
    std::vector<RECT> TabRects() const;
    // 命中哪个标签，-1 = 没命中
    int HitTab(POINT clientPt) const;

    // 屏幕坐标下压着哪个图标（-1 之外的空白/头部 → nullptr）。
    // 拖放落点判定用：松手时要知道「放到哪个图标上了」。
    DesktopIconView* ItemAtScreen(POINT screenPt) const;

    // 调试：最近一次拖动夹取的实况（障碍数量 / 期望位置 / 实际位置）。
    // 不参与任何逻辑，只往日志里写 —— 盒子互相避让出问题时，这一行能直接区分
    // 「一个障碍都没拿到」和「拿到了但没挡住」。
    std::wstring DragDebug() const;

    // 几何变化（移动后）→ 外部保存配置
    std::function<void()> onGeometryChanged;
    // 点击盒内图标
    std::function<void(DesktopIconView*)> onOpen;
    // 盒内图标被拖出：开始 / 移动 / 松手（POINT 为屏幕坐标）
    // 传的是一批：单选拖动时 list 长度为 1，多选拖动时是整批（第一个 = 按下的那个）。
    std::function<void(const std::vector<DesktopIconView*>&, POINT)> onItemsDragStart;
    std::function<void(POINT)> onItemsDragMove;
    std::function<void(const std::vector<DesktopIconView*>&, POINT)> onItemsDrop;
    // 右键（POINT 是屏幕坐标；item 为 -1 表示点在空白/头部）
    std::function<void(int item, POINT)> onContextMenu;
    // 「查看 / 排序 / 重命名」改完之后：外部存配置 + 存图标布局
    std::function<void()> onStateChanged;

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void EnsureBackbuffer(int w, int h);
    void Paint();
    void Present();
    void RefreshBackdrop();

    void Collect(std::vector<DesktopIconView*>& out) const;
    // 实际参与布局/绘制的图标（排除正在被拖出的那个）
    void CollectVisible(std::vector<DesktopIconView*>& out) const;

    // ---- 内容纵向滚动（图标多到超出盒子高度时）----
    // 只做「整行滚动」：滚半行会让上下两排都只显示一半，那需要裁剪；
    // 整行对齐之后「整行放得下才画」，不用任何裁剪，也不会漏到头部和圆角外面。
    int scrollY_ = 0;
    int MaxScroll() const;
    void ClampScroll();
    void ScrollBy(int deltaPx);
    int HitItem(POINT clientPt) const;
    // 当前宽度下能放几列（拖拽改宽度后要跟着变）。列表/详情恒为 1 列。
    int CellCols() const;
    // 两端对齐的列中心：第 0 列的中心 = kPad+kColInset + 图标宽/2，
    // 最后一列 = 右内边距-kColInset - 图标宽/2，中间等距。
    int ColCenterX(int c, int cols) const;
    // 图标列的水平范围（已含 kColInset）
    int ColLeft() const;
    int ColRight() const;
    // 第 index 个格子的客户区矩形（命中、落点分区用；格子之间无缝）
    void ItemRect(int index, RECT& r) const;
    // 悬停/选中/落点高亮专用：以图标中心为准、左右对称、固定格子宽度
    RECT ItemHighlightRect(int index) const;
    // 第 index 个图标下方的文字盒（已夹在卡片内边距之内，相邻两列不会重叠）
    void ItemLabelBox(int index, int& left, int& w) const;
    // 第 index 个格子的客户区原点（图标左上角）；放不下返回 false
    bool ItemOrigin(int index, int& x, int& y) const;

    // 按 cfg_->sortKey 重排本盒子图标在全局表里的相对顺序（自定义 = 不动）
    void SortNow();
    bool IsListView() const { return viewMode() >= 4; }

    // ---- 详情模式：每行「图标 + 名称 + 修改日期 + 类型 + 大小」 ----
    struct DetailInfo {
        std::wstring date;   // 2024/10/18 14:30
        std::wstring type;   // 文件夹 / PNG 图片 / 文本文档 …
        std::wstring size;   // 3.2 MB
    };
    // 切到详情 / 盒内图标数变了 / 文件改过才重建，渲染时不再碰磁盘
    void BuildDetailInfo();
    void InvalidateDetail() { detailInfo_.clear(); }
    std::unordered_map<DesktopIconView*, DetailInfo> detailInfo_;

    // 详情各列的几何（右侧从右往左依次是 大小 / 类型 / 修改日期）
    struct DetailCols {
        bool showDate = false;
        bool showType = false;
        bool showSize = false;
        int nameX = 0, nameW = 0;
        int dateX = 0, dateW = 0;
        int typeX = 0, typeW = 0;
        int sizeX = 0, sizeW = 0;
        int iconX = 0;
    };
    // 盒子变窄时按「大小 → 类型 → 修改日期」的顺序依次隐藏
    DetailCols DetailLayoutFor(int cellLeft, int cellWidth) const;

    // 详情列文字缓存（同一段文字同一尺寸只栅格化一次）
    struct DetailTextEntry {
        LabelDib dib;
        std::wstring key;
    };
    std::unordered_map<std::wstring, DetailTextEntry> detailText_;
    LabelDib* DetailText(const std::wstring& text, int boxW, int boxH, int glow);
    void ClearDetailCache();

    // ---- 排序 flip 动画 ----
    struct FlipNode {
        DesktopIconView* e = nullptr;
        int from = 0;   // 排序前的槽位
        int to = 0;     // 排序后的槽位
    };
    void StartFlip(const std::unordered_map<DesktopIconView*, int>& fromSlot,
                   const std::vector<DesktopIconView*>& after);
    void StopFlip();
    bool flipActive_ = false;
    DWORD flipT0_ = 0;
    int flipDur_ = 260;
    std::unordered_map<DesktopIconView*, FlipNode> flipNodes_;
    // 当前进度 0..1（未在动画时返回 1）
    float FlipProgress() const;
    // MEOW_DEBUG_HOOKS=1 时才响应 WM_APP+7（wParam=sortKey | asc<<8），
    // 给自动化脚本一个确定性的「改排序方式」入口，用来逐帧核对 flip 动画
    bool debugHooks_ = false;

    // ---- 四条边/四角拖拽改大小 ----
    enum class Edge { None, L, R, T, B, TL, TR, BL, BR };
    Edge HitEdge(POINT clientPt) const;
    void BeginPress(HWND h, POINT pt);
    void ApplyResize(POINT screenPt);

    // 实际占用的头部高度：显示标题时 = headerH_；关掉标题时只剩一条 2px 的
    // 「悬停感应条」（看不见但能收到鼠标，用来悬停展开）。
    static constexpr int kHeadStripH = 2;
    int HeadH() const { return showTitle_ ? headerH_ : kHeadStripH; }

    int minW() const;
    int minH() const;

    // 拖动位置夹取：把盒子从 from 往 (wantX,wantY) 推，遇到屏幕边 / 别的盒子就停。
    //
    // 这里必须是「扫掠」而不是「先算出目标再推开」：
    // 一帧跳几百像素时，目标点已经在障碍另一侧，「最低穿透量的方向」恰好是穿出去，
    // 于是看起来只有屏幕边挡得住。扫掠只认「前进方向上、起点还没越过的那条边」，
    // 物理上不可能跨过去。
    void ResolveDragMove(RECT from, int wantX, int wantY, int& outX, int& outY) const;

    // 松手后收尾：只处理「起点就重叠」的老配置，推出去的距离超过
    // kSnapOutMax 就不动（宁可留着重叠，也不能让盒子在松手瞬间瞬移）。
    // 返回是否真的移动了。
    bool SnapOutOfOverlap();

    LabelDib* LabelOf(DesktopIconView* e, int boxW, int boxH, int glow, int maxLines);
    HWND hwnd_ = nullptr;
    int index_ = 0;
    FenceConfig* cfg_ = nullptr;

    // 「查看」决定格子宽度；高度看 cellH()
    int cellW_ = 74;

    int width_ = 0;
    int height_ = 0;   // 展开时的高度
    int curH_ = 0;
    int targetH_ = 0;
    bool collapsed_ = false;
    bool autoExpanded_ = false;   // 这次展开是「悬停自动展开」造成的
    // 刚用箭头手动收起时，光标还停在头部上；抑制一次自动展开，
    // 否则会「点了收起又立刻展开」，用户永远收不起来
    bool suppressAutoExpand_ = false;
    int headerH_ = 44;
    bool showLabels_ = true;
    bool showTitle_ = true;    // g_config.showFenceTitle 的镜像

    std::vector<DesktopIconView*>* all_ = nullptr;
    static std::function<std::vector<RECT>(const FenceWindow*)> s_obstacleProvider;
    // 拖动夹取的最后一次实况（只给 DragDebug 用）
    mutable int dbgObstacles_ = -1;
    mutable RECT dbgWant_{};
    mutable POINT dbgGot_{0, 0};

    // 盒内标签缓存（按 图标+尺寸+颜色 复用）
    std::unordered_map<DesktopIconView*, LabelDib> labels_;
    std::unordered_map<DesktopIconView*, std::wstring> labelKeys_;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int bufW_ = 0;
    int bufH_ = 0;

    // 亚克力底图
    std::vector<uint8_t> backdrop_;
    int backdropW_ = 0, backdropH_ = 0;   // 抓取时的尺寸（总是展开尺寸）
    int backdropX_ = 0, backdropY_ = 0;

    DropHint drop_;

    // ---- 多选 ----
    // 选中态存在 DesktopIconView::selected 上（和桌面共用同一套语义）。
    // 盒子不另存一份「当前分组里的下标」：切分组 / 拖拽 / 排序都会让下标错位，
    // 存指针或标志位才不会出现「高亮框跳到别的图标上」。
    void ClearSelection();
    std::vector<DesktopIconView*> Selection() const;   // 当前分组里选中的（按显示顺序）
    void UpdateMarqueeSelection();                     // 空白处拖框 → 实时更新选中
    void SelectRange(int a, int b, bool keepOthers);   // Shift 范围选

    DesktopIconView* dragged_ = nullptr;   // 正在拖出盒子的那个图标（多选时 = 按下那个）
    std::vector<DesktopIconView*> dragItems_;   // 这次拖动的一整批（含 dragged_）
    int selAnchor_ = -1;      // Shift 范围选的锚点（当前分组里的显示下标）
    bool pressCtrl_ = false;  // 按下时 Ctrl / Shift 的状态（松手时判多选）
    bool pressShift_ = false;
    // 空白处拖框多选
    bool marquee_ = false;
    bool marqueeAdditive_ = false;   // 按住 Ctrl 拖框 = 叠加，不清掉原有选中
    POINT marqueeA_{0, 0};
    POINT marqueeB_{0, 0};
    int hot_ = -1;
    int pressItem_ = -1;
    bool pressArrow_ = false;
    bool pressLock_ = false;
    // 点击标签：松手时（没拖动过）才切
    int pressTab_ = -1;

    // ---- 标签滑块（当前分组身后那块底色）----
    // 切分组时滑块从旧标签的位置滑到新标签，而不是「啪」一下跳过去。
    // from/to 是客户区矩形，按 easeInOutCubic 插值。
    void KickTabPill(const RECT& from);
    RECT TabPillRect() const;
    RECT tabPillFrom_{};
    RECT tabPillTo_{};
    bool tabPillAnim_ = false;
    DWORD tabPillT0_ = 0;
    int tabPillDur_ = 200;
    bool pressHeader_ = false;
    bool pressBody_ = false;
    bool dragging_ = false;
    bool itemDragging_ = false;
    bool resizing_ = false;
    Edge resizeEdge_ = Edge::None;
    RECT resizeStartRect_{};
    int resizeStartW_ = 0;
    int resizeStartH_ = 0;
    bool trackingLeave_ = false;
    POINT pressPt_{0, 0};
    POINT pressScreenPt_{0, 0};
    int dragLastX_ = 0;
    int dragLastY_ = 0;
    // 上一帧的鼠标屏幕坐标（拖动按每帧位移累计，被挡住的距离不留账）
    POINT dragLastCursor_{0, 0};
    // 系统原生右键菜单正开着：这时鼠标「离开」盒子是正常的，不能自动收起
    bool menuOpen_ = false;
    RECT dragStartRect_{};
};
