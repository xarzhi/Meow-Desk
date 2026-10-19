#include "icon_font.h"

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <vector>

#include "app_icon.h"
#include "app_res.h"
#include "icon_render.h"

using namespace Gdiplus;

namespace IconFont {
namespace {

// 字体数据必须在 PrivateFontCollection 活着的时候一直有效 ——
// AddMemoryFont 不拷贝，只记住指针。所以这里把资源里的字节搬一份到静态 vector。
std::vector<BYTE> g_bytes;
PrivateFontCollection* g_pfc = nullptr;
FontFamily* g_family = nullptr;
bool g_tried = false;

void Init() {
    if (g_tried) return;
    g_tried = true;

    DWORD n = 0;
    const void* p = AppResourceBytes(IDR_ICONFONT, n);
    if (!p || n < 64) return;
    g_bytes.assign(reinterpret_cast<const BYTE*>(p),
                   reinterpret_cast<const BYTE*>(p) + n);

    EnsureGdiplus();
    g_pfc = new PrivateFontCollection();
    if (g_pfc->AddMemoryFont(g_bytes.data(), (INT)g_bytes.size()) != Ok) {
        delete g_pfc;
        g_pfc = nullptr;
        return;
    }
    // 注意 GetFamilyCount 是「返回个数」而不是 out 参数
    if (g_pfc->GetFamilyCount() <= 0) return;
    auto* fam = new FontFamily();
    INT found = 0;
    g_pfc->GetFamilies(1, fam, &found);
    if (found <= 0) {
        delete fam;
        return;
    }
    g_family = fam;
}

}  // namespace

bool Ready() {
    Init();
    return g_family != nullptr;
}

void Draw(Graphics& g, wchar_t code, float cx, float cy, float px, unsigned int argb) {
    Init();
    if (!g_family || px <= 0.0f) return;

    // 走 GraphicsPath 而不是 DrawString：DrawString 的垂直居中是按「行高（含行距）」
    // 算的，图标字体的行距各不相同，不同字形会各自偏一点；
    // 这里取**墨迹包围盒**再把盒子中心对到 (cx, cy)，无论字体怎么设计都精确居中。
    GraphicsPath path;
    if (path.AddString(&code, 1, g_family, FontStyleRegular, px, PointF(0.0f, 0.0f),
                       nullptr) != Ok)
        return;

    RectF b;
    if (path.GetBounds(&b) != Ok || b.Width <= 0.0f || b.Height <= 0.0f) return;

    Matrix m;
    m.Translate(cx - (b.X + b.Width * 0.5f), cy - (b.Y + b.Height * 0.5f));
    path.Transform(&m);

    // 花括号而不是圆括号：`SolidBrush br(Color(x))` 会被当成函数声明（most vexing parse）
    SolidBrush br{Color(argb)};
    g.FillPath(&br, &path);
}

}  // namespace IconFont
