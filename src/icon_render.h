// 图标 + 桌面标签的「像素级」绘制
//
// 为什么不用 GDI+ DrawImage：
//   1. GDI+ 会把源位图当成 PixelFormat32bppPARGB（预乘）来解码。而 Shell 给回来的
//      32bpp 图标位图**可能是直通 alpha（straight alpha）**，一旦被当预乘用，
//      半透明的抗锯齿边缘就会被放大成白色/浅色描边 —— 这就是"图标有白边"。
//   2. DrawImage 即使目标矩形是 1:1，也会走一遍采样管线；配合 PixelOffsetMode 的
//      半像素偏移，结果必然是"发虚 + 1px 错位"。
// 所以这里一律**逐像素手写预乘 alpha 合成**：源与目标尺寸一致时就是纯拷贝，
// 零重采样、零 GDI 版本差异，边缘和资源管理器渲染出来的完全一致。
//
// 文字同理：GDI+ 的 DrawString 走自己的栅格器，12px 下笔画会被拉细发灰。
// 这里用 GDI 在 32bpp DIB 上写白字（ANTIALIASED_QUALITY 灰度抗锯齿），
// 把亮度转成 alpha 得到字形遮罩，再叠黑色柔和光晕 —— 与 Explorer 桌面标签一致。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "shell_icons.h"

// 进程内启动一次 GDI+（各模块共用，不重复 Startup）
void EnsureGdiplus();

// 把图标像素归一化成「预乘 alpha」（straight → premultiplied），消除白边
void NormalizePremultiplied(IconBitmap& bmp);

// 创建 32bpp 自上而下 DIB 段
bool CreateDib(int w, int h, HDC& dc, HBITMAP& bmp, void*& bits);

// 预乘 alpha 的 source-over 合成（src 左上角落在 dst 的 (x, y)）
void BlendInto(BYTE* dst, int dstW, int dstH, const BYTE* src, int srcW, int srcH,
               int x, int y);

// 用恒定颜色 + alpha 填充矩形（预乘合成）
void FillRectAlpha(BYTE* dst, int dstW, int dstH, const RECT& r, int a, COLORREF color);

// 系统桌面图标标题字体（SPI_GETICONTITLELOGFONT，灰度抗锯齿）
HFONT IconLabelFont();

// ---------------------------------------------------------------------------
// 一张图标位图对应的 DIB（缓存起来，每帧只做一次拷贝合成）
// ---------------------------------------------------------------------------
struct IconDib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    int w = 0;
    int h = 0;

    bool Build(const IconBitmap& src);
    void Free();
    bool valid() const { return dc && bits && w > 0 && h > 0; }

    IconDib() = default;
    IconDib(const IconDib&) = delete;
    IconDib& operator=(const IconDib&) = delete;
    IconDib(IconDib&& o) noexcept { Move(o); }
    IconDib& operator=(IconDib&& o) noexcept {
        if (this != &o) { Free(); Move(o); }
        return *this;
    }
    ~IconDib() { Free(); }

private:
    void Move(IconDib& o) {
        dc = o.dc; bmp = o.bmp; bits = o.bits; w = o.w; h = o.h;
        o.dc = nullptr; o.bmp = nullptr; o.bits = nullptr; o.w = o.h = 0;
    }
};

// ---------------------------------------------------------------------------
// 桌面图标标签：白字 + 黑色柔和光晕，一次性合成成单张预乘 DIB
//   boxW/boxH 是文字的排版盒子；pad 是光晕外扩，贴图时左上角要偏移 -pad
// ---------------------------------------------------------------------------
struct LabelDib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    int w = 0;      // = boxW + 2*pad
    int h = 0;      // = boxH + 2*pad
    int pad = 0;

    // glow = 0 表示不要阴影
    bool Build(const std::wstring& text, HFONT font, int boxW, int boxH, int glow);
    void Free();
    bool valid() const { return dc && bits && w > 0 && h > 0; }

    LabelDib() = default;
    LabelDib(const LabelDib&) = delete;
    LabelDib& operator=(const LabelDib&) = delete;
    LabelDib(LabelDib&& o) noexcept { Move(o); }
    LabelDib& operator=(LabelDib&& o) noexcept {
        if (this != &o) { Free(); Move(o); }
        return *this;
    }
    ~LabelDib() { Free(); }

private:
    void Move(LabelDib& o) {
        dc = o.dc; bmp = o.bmp; bits = o.bits; w = o.w; h = o.h; pad = o.pad;
        o.dc = nullptr; o.bmp = nullptr; o.bits = nullptr; o.w = o.h = 0; o.pad = 0;
    }
};
