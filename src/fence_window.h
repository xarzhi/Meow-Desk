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

    // 拖动时用来避让的其他盒子矩形（屏幕坐标），由 main 提供
    void SetObstacles(std::function<std::vector<RECT>()> fn) { obstacles_ = std::move(fn); }

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

    // 几何变化（移动后）→ 外部保存配置
    std::function<void()> onGeometryChanged;
    // 点击盒内图标
    std::function<void(DesktopIconView*)> onOpen;
    // 盒内图标被拖出：开始 / 移动 / 松手（POINT 为屏幕坐标）
    std::function<void(DesktopIconView*, POINT)> onItemDragStart;
    std::function<void(DesktopIconView*, POINT)> onItemDragMove;
    std::function<void(DesktopIconView*, POINT)> onItemDrop;
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
    int HitItem(POINT clientPt) const;
    // 当前宽度下能放几列（拖拽改宽度后要跟着变）。列表/详情恒为 1 列。
    int CellCols() const;
    // 两端对齐的列中心：第 0 列的中心 = kPad + 图标宽/2，最后一列 = 右内边距 - 图标宽/2，
    // 中间等距。满行正好顶满左右内边距；不满的最后一排自然左对齐，且列位置与上一排对齐。
    int ColCenterX(int c, int cols) const;
    // 第 index 个格子的客户区矩形（命中、悬停、选中、落点高亮都用它）
    void ItemRect(int index, RECT& r) const;
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

    int minW() const;
    int minH() const;

    // 拖动位置夹取：不出屏幕、不压到别的盒子
    void ResolveDragPos(RECT want, int& outX, int& outY) const;

    LabelDib* LabelOf(DesktopIconView* e, int boxW, int boxH, int glow);
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

    std::vector<DesktopIconView*>* all_ = nullptr;
    std::function<std::vector<RECT>()> obstacles_;

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

    DesktopIconView* dragged_ = nullptr;   // 正在拖出盒子的那个图标
    int hot_ = -1;
    int sel_ = -1;            // 单击选中的图标（双击才打开，所以要有个选中态）
    int pressItem_ = -1;
    bool pressArrow_ = false;
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
    RECT dragStartRect_{};
};
