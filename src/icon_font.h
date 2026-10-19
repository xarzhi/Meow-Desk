// 图标字体（src/iconfont/iconfont.ttf，由 RC 编进 exe）
//
// 设置页左侧导航那 5 个图标不再用「GDI+ 手搓几何图形」拼，改成真正的图标字体：
// 几何自绘的线条宽度要跟着导航项尺寸手调，加一个图标就得再写一段画法；
// 图标字体一个码位就是一个图形，换图标只换码位。
//
// 为什么用 Gdiplus::PrivateFontCollection 而不是 AddFontMemResourceEx：
//   AddFontMemResourceEx 是把字体注册进 **GDI** 的字体表，GDI+ 的 DrawString
//   走的是自己那套字体集合（InstalledFontCollection），认不到私有字体，
//   Font(L"iconfont") 会静默退回默认字体 —— 画出来是一堆方块。
//   PrivateFontCollection::AddMemoryFont 才是 GDI+ 自己的私有字体入口。
//   代价：字体数据必须在 PrivateFontCollection 存活期间一直有效，所以这里把
//   字节拷进一个静态 vector，而不是指向资源内存。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace Gdiplus {
class Graphics;
}
struct GdiplusColor;

// 五个导航图标的码位（见 src/iconfont/iconfont.json）
namespace IconCode {
constexpr wchar_t kGeneral = L'\ue632';   // 设置-常规设置
constexpr wchar_t kAppearance = L'\ue6b3';   // theme（外观）
constexpr wchar_t kInteract = L'\ue629';     // 交互设计
constexpr wchar_t kRules = L'\ue637';        // 整理箱（整理规则）
constexpr wchar_t kAbout = L'\ue60b';        // 关于
}  // namespace IconCode

namespace IconFont {

// 字体加载成功没有（失败时调用方退回原来的几何自绘）
bool Ready();

// 把一个字形画在 (cx, cy) 为中心、约 px 见方的框里。color 是 ARGB。
void Draw(Gdiplus::Graphics& g, wchar_t code, float cx, float cy, float px,
          unsigned int argb);

}  // namespace IconFont
