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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "backdrop.h"
#include "config.h"
#include "desktop_surface.h"
#include "icon_render.h"

class FenceWindow {
public:
    static constexpr int kCellHCompact = 62;   // 不显示标题时的格子高度
    bool Create(int index, int x, int y, int w, int h);
    void Destroy();

    // 创建时是否允许「一出生就是收起的」。
    // 启动 / 重建盒子时传 true：伸缩模式（expandMode==0）的盒子本来就该收着，
    // 否则要等鼠标滑过去再离开才收，看起来像忘了收。
    // 刚新建的盒子传 false —— 用户刚建完就看见一条 4px 的线会以为没建成。
    void SetStartCollapsed(bool on) { startCollapsed_ = on; }

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

    // 展开时的高度（就是配置里存的那个 h）。收起状态下窗口只有 4px，
    // 拿 rect() 存配置 / 算避让会把盒子「存成一条线」，所以要用这个。
    int expandedHeight() const { return height_; }

    // 配置变化（颜色/圆角/透明度/亚克力/标题）→ 重绘。
    // recapture = true 时才重新抓取亚克力底图（很贵，拖动滑块时不要带）
    void ApplyConfig(bool recapture = false);

    // 重新抓一次磨砂底图并重绘。换壁纸 / 改分辨率后由主程序调用 ——
    // 盒子只在创建时抓过底图，壁纸变了不重抓的话磨砂里一直是旧壁纸
    // （以前只有「拖一下盒子」触发补抓才会更新）。
    void RecaptureBackdrop();

    // 壁纸可能变了（收到 SPI_SETDESKWALLPAPER / 分辨率变化）：**延后**再重抓。
    //
    // 不能收到广播就立刻抓：那条广播是先到的，DWM 往往还没把新壁纸合成出来，
    // 当场抓到的还是旧画面，之后又没有第二个人来重抓 —— 磨砂就永远停在旧壁纸上。
    // 交给 PollBackdropChange 统一处理（等一小会儿再抓）。
    void ScheduleBackdropRefresh();

    // 心跳里轮询（主程序每 500ms 叫一个盒子，转着圈来）：
    //   · 到点的延后重抓 → 现在就抓
    //   · 否则采样盒子外面一小块屏幕的均值，和上次比变了就重抓
    // 动态壁纸 / 幻灯片轮播根本不发 SPI_SETDESKWALLPAPER，只能靠像素差分发现。
    // 采样一次是一块 16×16 的 StretchBlt，微秒级；重抓只在真的变了时发生。
    void PollBackdropChange();

    void Redraw();
    void SetCollapsed(bool collapsed);
    bool collapsed() const { return collapsed_; }

    // 展开方式：1 = 展开模式（始终展开）0 = 伸缩模式（悬停展开 / 移出收起）。
    // 由头部右侧的箭头点击切换，存在 FenceConfig::expandMode 里。
    // 关掉「显示盒子标题」时头部只剩一条 2px 感应条，没有箭头可点，
    // 也就没有任何收起入口，所以这种情况下恒为「始终展开」。
    int expandMode() const {
        // 关掉标题不再等于「永远展开」：没有箭头的盒子靠「伸缩模式」的
        // 悬停展开 / 离开收起照样能收放（头部就那 4px 把手）。
        return (!cfg_ || cfg_->expandMode != 0) ? 1 : 0;
    }
    void SetExpandMode(int m);

    // 拖动时用来避让的其他盒子矩形（屏幕坐标），由 main 提供。
    //
    // 这里用「全局一份、按指针认自己」而不是「每个盒子各存一个捕获了下标的 lambda」：
    // 盒子下标是数组下标，新建/删除都会变，捕获下标的 lambda 一旦错位，
    // 轻则把自己当成障碍（盒子被自己弹开），重则一个障碍都拿不到（可以随意重叠）。
    // 传 this 进来排除自己，永远不会错。
    static void SetObstacleProvider(std::function<std::vector<RECT>(const FenceWindow*)> fn);

    // 抓磨砂底图时，让「我们自己的窗口」暂时从抓屏里消失。
    //
    // 不这么做的话，拖着盒子经过别的盒子时，磨砂里会出现那块盒子的一团鬼影
    // （被抓屏抓进去了）；桌面图标层同理。参数 rect 是这次要抓的区域，
    // 实现方只需要处理跟它相交的窗口 —— 一次改十几个窗口的显示亲和性会让
    // DWM 重新合成，看起来像闪一下。
    static void SetCaptureExcluder(std::function<void(const RECT& rect, bool exclude)> fn);

    // 屏幕坐标是否落在盒子矩形内（含收起后的头部）
    bool HitTest(POINT screenPt) const;

    // 伸缩模式下「光标已经不在盒子里了就把盒子收起来」。
    // WM_MOUSELEAVE 靠不住的地方（拖拽期间鼠标被别的窗口 SetCapture 走）由它兜底，
    // 也挂在 kIdleTimer 上每 200ms 自查一次。正在拖拽 / 拉尺寸 / 右键菜单 / 改名时不动。
    void CollapseIfIdle();

    // 盒子内容区（去掉头部）的屏幕矩形，用来判断"松手时算不算放进了盒子"
    RECT bodyRect() const;

    // 外部弹窗（改名输入框 / 原生菜单）开着时占住盒子：
    // 那些窗口一盖上来鼠标就「离开」了盒子，悬停展开的盒子会当场缩回去。
    // 和 menuOpen_ 分开记：菜单是自己知道自己开着的，改名要主程序通知。
    void SetInteractionHold(bool on) { holdOpen_ = on; }

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
    // 放弃盒内这次拖动（图标回原位置）。拖到别的程序窗口上、要交棒给系统级拖放
    // 之前必须调：交棒之后鼠标松开那一下被 OLE 吃掉，我们收不到 WM_LBUTTONUP。
    void AbortItemsDrag();
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

    // ---- 屏幕矩形 ----
    // 盒子标题 / 分组标签的屏幕矩形。有分组时「盒子标题」就是第 0 个标签；
    // 没有分组时标题单独画在头部左侧，两者几何不同，所以统一走这个函数。
    //
    // 注意：盒子/分组改名现在用**弹窗**（rename_dialog.h），不再需要这个矩形；
    // 图标改名走的是下面的 ItemLabelScreenRect（就地输入框）。这里留着是给
    // 命中判定 / 调试用，别看到名字就以为改名还贴着它走。
    RECT RenameRectScreen(int gi) const;
    // 盒内某个图标「标题区」的屏幕矩形。不在当前分组 / 滚出视野 / 盒子收起 → false。
    // 几何和 Paint 里画标签那一段一致（图标下方那一行，或列表模式里图标右边）。
    bool ItemLabelScreenRect(const DesktopIconView* e, RECT& out) const;

    // 屏幕坐标下压着哪个图标（-1 之外的空白/头部 → nullptr）。
    // 拖放落点判定用：松手时要知道「放到哪个图标上了」。
    DesktopIconView* ItemAtScreen(POINT screenPt) const;

    // 搜索窗口的「定位」：把光标带到某个图标上 ——
    //   1) 它在别的分组 → 先切过去
    //   2) 盒子是收起的 → 先展开（等展开动画结束再算滚动量，否则按 4px 高度算出来全是 0）
    //   3) 滚到它那一行（整行对齐，和滚轮同一套规则）
    //   4) 背景闪两下
    void LocateItem(DesktopIconView* e);

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
    // 双击分组标签 → 改这个标签的名字（0 = 盒子标题本身，SetGroupName 会转到 SetTitle）
    std::function<void(int group)> onRenameGroup;
    // 「查看 / 排序 / 重命名」改完之后：外部存配置 + 存图标布局
    std::function<void()> onStateChanged;

    // ---- 拖拽落位动画（给 main 用）----
    // 用法：拖拽松手时先 CaptureSlots()，接着改 g_icons 的归属/顺序，
    // 改完再 PlayPendingFlip()。后面的图标就会从旧槽位滑到新槽位。
    void CaptureSlots();
    void PlayPendingFlip();

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void EnsureBackbuffer(int w, int h);
    void Paint();
    void Present();
    // pad > 0：多抓一圈（拖动/改尺寸期间用）。之后盒子在这个圈里挪动时，
    // 底图直接按偏移「取子窗口」就够了 —— 不用每帧重新抓屏+模糊。
    //
    // defer = true：抓下来的这张先放暂存区（bdPre_），**不换上**。
    // 「按下」那一刻抓是为了让拖动的第一帧跟手，但单击（点分组标签、点空白、
    // 点一下头部）也会走到「按下」—— 那时候换底图，用户看到的就是
    // 「点一下 tab，盒子背景忽然变了」。所以按下只抓不换，真的开始拖再换。
    void RefreshBackdrop(int pad = 0, bool defer = false);
    // 把暂存的那张换上（拖动真正开始时调用）
    void ApplyPreBackdrop();
    // 丢掉暂存，释放那几 MB
    void DropPreBackdrop();
    // 把「抓一块」丢给工作线程（pad：四周多抓一圈）。已有在途请求时直接返回。
    void RequestBackdropAsync(int pad);
    // 底图还盖得住当前矩形吗？盖不住就提交一次后台补抓
    void EnsureBackdropCover();

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
    // 把第 index 个图标滚到可见范围内（按展开高度算视口，整行对齐）
    void ScrollItemIntoView(int index);

    // ---- 搜索「定位」的闪烁提示 ----
    // 收起状态下要先展开再算滚动量，所以定位可能得「延后」到展开动画结束。
    DesktopIconView* locatePending_ = nullptr;   // 等展开动画结束再定位
    DesktopIconView* flash_ = nullptr;           // 正在闪的图标（空 = 没在闪）
    int flashStep_ = 0;                           // 已翻过的相位数，偶数=亮
    void DoLocateNow(DesktopIconView* e);
    void StartFlash(DesktopIconView* e);
    void TickFlash();
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
    // 拖动落位用：先记住「插之前」每个图标的槽位，列表改完之后再整体播一次
    // 位移动画。插入到中间时后面的图标全都要往后让一格，直接瞬移会很突兀。
    std::unordered_map<DesktopIconView*, int> pendingSlots_;
    // 当前进度 0..1（未在动画时返回 1）
    float FlipProgress() const;
    // MEOW_DEBUG_HOOKS=1 时才响应 WM_APP+7（wParam=sortKey | asc<<8），
    // 给自动化脚本一个确定性的「改排序方式」入口，用来逐帧核对 flip 动画
    bool debugHooks_ = false;

    // ---- 四条边/四角拖拽改大小 ----
    enum class Edge { None, L, R, T, B, TL, TR, BL, BR };
    Edge HitEdge(POINT clientPt) const;
    // 按下 / 光标反馈用的边缘判定：关掉「显示盒子标题」时，顶上那 4px 是
    // 拖动手把，不能被 6px 的「改高度」感应带吃掉（否则盒子没法靠头部挪动，
    // 盒内拖拽是框选、不是移动）。左右角落仍然优先改尺寸。
    Edge HitEdgeForPress(POINT clientPt) const;
    void BeginPress(HWND h, POINT pt);
    void ApplyResize(POINT screenPt);

    // 实际占用的头部高度：显示标题时 = headerH_；关掉标题时只剩一条 4px 的
    // 「把手」—— 标题、分组标签、锁、收起箭头全都不画，但这一条仍然是
    // 拖动盒子的抓手动，也仍然是悬停展开的感应区（和收起态那条 4px 一样高）。
    static constexpr int kHeadStripH = 4;
    // 收起之后头部收缩到的高度：整块盒子变成一条 4px 的圆角「把手」，
    // 头部的标签 / 标题 / 锁 / 箭头全部不画（见 Paint 里的 headFade）。
    static constexpr int kCollapsedHeadH = 4;
    int HeadH() const { return showTitle_ ? headerH_ : kHeadStripH; }

    int minW() const;
    int minH() const;

    // 拖动位置夹取：只跟「屏幕」硬夹（屏幕边必须挡住），别的盒子不再是墙 ——
    // 靠得够近就把边缘吸过去（磁吸），再用力就能从它上面拖过去。
    //
    // 以前这里是「扫掠 + 硬阻挡」：盒子永远无法越过另一个盒子，想把两个盒子
    // 换个左右位置得先绕一大圈。现在改成吸附，越过别的盒子变成一次普通拖动。
    void ResolveDragMove(RECT from, int wantX, int wantY, int& outX, int& outY) const;

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
    bool startCollapsed_ = true;   // 见 SetStartCollapsed：创建时是否允许一出生就收起
    bool autoExpanded_ = false;   // 这次展开是「悬停自动展开」造成的
    // 刚用箭头手动收起时，光标还停在头部上；抑制一次自动展开，
    // 否则会「点了收起又立刻展开」，用户永远收不起来
    bool suppressAutoExpand_ = false;
    int headerH_ = 44;
    bool showLabels_ = true;
    bool showTitle_ = true;    // g_config.showFenceTitle 的镜像

    std::vector<DesktopIconView*>* all_ = nullptr;
    static std::function<std::vector<RECT>(const FenceWindow*)> s_obstacleProvider;
    static std::function<void(const RECT&, bool)> s_captureExcluder;
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
    // 底图按 1/kBackdropScale 分辨率存。
    //
    // 磨砂底图先被模糊过（半径 20），本身几乎没有高频细节，降到 1/2 边
    // （内存 1/4）再放大回来肉眼分辨不出，但七个盒子合计能省 8MB 上下。
    // 放大是 Paint 里手写的最近邻循环：实测 0.74ms，和原来 1:1 贴图的 0.69ms
    // 基本一样 —— **千万别改成 GDI+ 的 DrawImage 缩放**，同一张图它要 26ms
    // （GDI+ 的插值缩放走的是慢路径），拖动会当场卡成 PPT。
    //
    // backdropW_/H_ 仍然是**屏幕坐标下的覆盖范围**，除以 kBackdropScale 才是
    // 像素尺寸，所以取样、覆盖判断这些几何代码一行都不用改。
    static constexpr int kBackdropScale = 2;
    int BdPixW() const { return (backdropW_ + kBackdropScale - 1) / kBackdropScale; }
    int BdPixH() const { return (backdropH_ + kBackdropScale - 1) / kBackdropScale; }
    int backdropX_ = 0, backdropY_ = 0;
    // 上一次提交后台补抓的时刻（限频用）
    DWORD backdropCoverT_ = 0;
    // 壁纸指纹：定期采样盒子外面一小块屏幕的均值，变了就说明壁纸动了。
    // bdRefreshPending_ = 「有一单延后的重抓排着」（收到壁纸广播后等 DWM 合成完再抓）
    bool bdRefreshPending_ = false;
    DWORD bdDue_ = 0;
    bool probeValid_ = false;
    int probeR_ = 0, probeG_ = 0, probeB_ = 0;
    // 上一次采样用的屏幕坐标（采样块左上角）。
    // 展开/收起/拖动/改尺寸都会让采样点换地方，不同位置的均值没有可比性 ——
    // 换了位置就重新立基线，不能当成「壁纸变了」。
    int probeX_ = 0, probeY_ = 0;
    // 连续几次采样都和基线不同。要求连续两次才动手：别的窗口扫过采样点
    // （或者那附近有个图标被 hover 高亮）也会让均值变一点，一次就信会误抓。
    int probeHits_ = 0;
    // 已经确认过「这块地方的壁纸就是一片纯色」：之后快路径抓到纯色也直接用，
    // 不再走慢路径反复确认（纯色壁纸 + 反复确认 = 拖动时一直闪）。
    bool bdPlainArea_ = false;
    // 后台正在抓的那一单（抓屏 + 模糊在工作线程做，UI 线程每帧问一次好了没）
    std::shared_ptr<BackdropCaptureJob> bdJob_;
    // 「按下就抓好、真拖起来才换上」的那一张（见 RefreshBackdrop 的 defer 参数）。
    // 只在一次按下的生命周期里存在，松开就丢。
    std::vector<uint8_t> bdPre_;
    int bdPreW_ = 0, bdPreH_ = 0;
    int bdPreX_ = 0, bdPreY_ = 0;
    bool bdPreValid_ = false;

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
    // 按下的那一刻已经抓过带余量的底图了（按在头部/箭头上才抓）。
    // 放到「按下」而不是「第一次移动」抓，是因为拖动起点那一下的十几毫秒停顿
    // 就是「拖起来有点黏」的来源；按下时盒子还没动，这点时间完全看不出来。
    bool pressCaptured_ = false;
    bool resizing_ = false;
    // 这次拉伸已经抓过带余量的底图了（第一次真正拉动时才抓，光按下边缘不抓）
    bool resizeCaptured_ = false;
    Edge resizeEdge_ = Edge::None;
    RECT resizeStartRect_{};
    int resizeStartW_ = 0;
    int resizeStartH_ = 0;
    bool trackingLeave_ = false;
    POINT pressPt_{0, 0};
    POINT pressScreenPt_{0, 0};
    int dragLastX_ = 0;
    int dragLastY_ = 0;
    // 拖动时「未吸附」的原始位置：鼠标位移直接累加到它上面，吸附只决定窗口摆到
    // 哪儿。用吸附后的位置去累加会让盒子吸住之后再也走不动（每一帧都被吸回原处）。
    int dragRawX_ = 0;
    int dragRawY_ = 0;
    // 上一帧的鼠标屏幕坐标（拖动按每帧位移累计，被挡住的距离不留账）
    POINT dragLastCursor_{0, 0};
    // 系统原生右键菜单正开着：这时鼠标「离开」盒子是正常的，不能自动收起
    bool menuOpen_ = false;
    // 改名输入框正贴着这个盒子：同理不能自动收起（由主程序置位/清位）
    bool holdOpen_ = false;
    RECT dragStartRect_{};
};
