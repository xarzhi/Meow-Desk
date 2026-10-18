#include "icon_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

void EnsureGdiplus() {
    static ULONG_PTR token = 0;
    static bool once = false;
    if (!once) {
        once = true;
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
}

namespace {

inline int MulDiv255(int v, int a) { return (v * a + 127) / 255; }

// 预乘 alpha 的 source-over：d = s + d*(1 - sa)
inline void BlendPixel(BYTE* d, const BYTE* s) {
    unsigned a = s[3];
    if (a == 0) return;
    if (a == 255) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        return;
    }
    unsigned inv = 255u - a;
    d[0] = (BYTE)(s[0] + (unsigned)(d[0] * inv + 127) / 255);
    d[1] = (BYTE)(s[1] + (unsigned)(d[1] * inv + 127) / 255);
    d[2] = (BYTE)(s[2] + (unsigned)(d[2] * inv + 127) / 255);
    d[3] = (BYTE)(a + (unsigned)(d[3] * inv + 127) / 255);
}

// 一维盒式模糊（滑动窗口），用来把字形遮罩糊成柔和光晕
void BoxBlurLine(const BYTE* src, BYTE* dst, int len, int radius) {
    if (len <= 0) return;
    const int win = radius * 2 + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += src[std::min(std::max(i, 0), len - 1)];
    for (int i = 0; i < len; ++i) {
        dst[i] = (BYTE)(sum / win);
        sum -= src[std::min(std::max(i - radius, 0), len - 1)];
        sum += src[std::min(std::max(i + radius + 1, 0), len - 1)];
    }
}

void BoxBlurAlpha(const std::vector<BYTE>& in, std::vector<BYTE>& out, int w, int h, int radius, int passes) {
    if (radius <= 0) { out = in; return; }
    std::vector<BYTE> a = in, t(in.size());
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y) BoxBlurLine(&a[(size_t)y * w], &t[(size_t)y * w], w, radius);
        for (int x = 0; x < w; ++x) {
            std::vector<BYTE> col(h), ocol(h);
            for (int y = 0; y < h; ++y) col[y] = t[(size_t)y * w + x];
            BoxBlurLine(col.data(), ocol.data(), h, radius);
            for (int y = 0; y < h; ++y) a[(size_t)y * w + x] = ocol[y];
        }
    }
    out.swap(a);
}

int MeasureCx(HDC dc, const std::wstring& s) {
    if (s.empty()) return 0;
    SIZE sz{};
    if (!GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz)) return 0;
    return sz.cx;
}

bool IsWordChar(wchar_t c) {
    return c < 128 && (iswalnum((wint_t)c) || c == L'_' || c == L'.' || c == L'-' || c == L'~');
}

// 与资源管理器一致：西文按词断、长串整体省略、中日韩逐字断
std::wstring WrapLabel(HDC dc, const std::wstring& text, int maxW, int maxLines) {
    std::vector<std::wstring> chunks;
    for (size_t i = 0; i < text.size();) {
        wchar_t c = text[i];
        if (c == L'\r') { ++i; continue; }
        if (c == L' ') { chunks.emplace_back(L" "); ++i; }
        else if (c == L'\n') { chunks.emplace_back(L"\n"); ++i; }
        else if (IsWordChar(c)) {
            std::wstring w;
            while (i < text.size() && IsWordChar(text[i])) w += text[i++];
            chunks.push_back(std::move(w));
        } else {
            chunks.emplace_back(1, c);
            ++i;
        }
    }

    std::vector<std::wstring> lines(1);
    bool truncated = false;
    for (const auto& ch : chunks) {
        if (ch == L"\n") {
            if ((int)lines.size() >= maxLines) { truncated = true; break; }
            lines.emplace_back();
            continue;
        }
        if (ch == L" " && lines.back().empty()) continue;
        std::wstring trial = lines.back() + ch;
        if (MeasureCx(dc, trial) <= maxW) { lines.back() = std::move(trial); continue; }
        // 单个 token 就比整格还宽（例如 "92015e61f37b71b8a1583477627.ttf" 这种长文件名）。
        // 以前这里直接 break 会让 lines.back() 保持空串，整个标签 DIB 建不出来 →
        // 「有的图标有名字、有的没名字」。现在硬截断 + 省略号，至少能看到开头几个字。
        if (lines.back().empty()) {
            std::wstring t = ch;
            while (!t.empty() && MeasureCx(dc, t + L"...") > maxW) t.pop_back();
            if (t.empty()) {
                t = ch.substr(0, 1);   // 窄到连一个字符都放不下，也要画出来
                lines.back() = std::move(t);
            } else {
                lines.back() = std::move(t) + L"...";
            }
            truncated = false;   // 已经自带省略号，别再叠加
            break;
        }
        if ((int)lines.size() >= maxLines) { truncated = true; break; }
        if (ch == L" ") continue;
        lines.emplace_back(ch);
    }

    auto fitEllipsis = [&](std::wstring& line) {
        while (!line.empty() && MeasureCx(dc, line + L"...") > maxW) line.pop_back();
        line += L"...";
    };

    for (auto& line : lines) {
        if (line.empty() || MeasureCx(dc, line) <= maxW) continue;
        truncated = true;
        fitEllipsis(line);
    }
    if (truncated && !lines.empty() && !lines.back().empty()) {
        std::wstring& last = lines.back();
        if (last.size() < 3 || last.substr(last.size() - 3) != L"...") fitEllipsis(last);
    }

    std::wstring out;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (!out.empty()) out += L'\n';
        out += line;
    }
    return out;
}

}  // namespace

bool CreateDib(int w, int h, HDC& dc, HBITMAP& bmp, void*& bits) {
    dc = nullptr;
    bmp = nullptr;
    bits = nullptr;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP hb = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!hb || !bits) {
        if (hb) DeleteObject(hb);
        bits = nullptr;
        return false;
    }

    HDC mdc = CreateCompatibleDC(nullptr);
    if (!mdc) { DeleteObject(hb); bits = nullptr; return false; }
    SelectObject(mdc, hb);
    dc = mdc;
    bmp = hb;
    return true;
}

void NormalizePremultiplied(IconBitmap& bmp) {
    if (!bmp.valid()) return;
    auto* px = reinterpret_cast<BYTE*>(bmp.pixels.data());
    const size_t n = (size_t)bmp.width * bmp.height;

    // 判定是否直通 alpha：预乘数据永远不会出现「通道值 > alpha」。
    // 只要发现半透明像素的颜色比 alpha 还亮，就说明给的是直通 alpha。
    bool straight = false;
    for (size_t i = 0; i < n && !straight; ++i) {
        const BYTE* p = px + i * 4;
        BYTE a = p[3];
        if (a == 0 || a == 255) continue;
        if (p[0] > a + 1 || p[1] > a + 1 || p[2] > a + 1) straight = true;
    }

    if (straight) {
        for (size_t i = 0; i < n; ++i) {
            BYTE* p = px + i * 4;
            BYTE a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (BYTE)MulDiv255(p[0], a);
            p[1] = (BYTE)MulDiv255(p[1], a);
            p[2] = (BYTE)MulDiv255(p[2], a);
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            BYTE* p = px + i * 4;
            if (p[3] == 0) p[0] = p[1] = p[2] = 0;
        }
    }
}

void BlendInto(BYTE* dst, int dstW, int dstH, const BYTE* src, int srcW, int srcH,
               int x, int y) {
    if (!dst || !src || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) return;

    int sx0 = 0, sy0 = 0, sx1 = srcW, sy1 = srcH;
    int dx0 = x, dy0 = y;
    if (dx0 < 0) { sx0 = -dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 = -dy0; dy0 = 0; }
    if (dx0 + (sx1 - sx0) > dstW) sx1 = sx0 + (dstW - dx0);
    if (dy0 + (sy1 - sy0) > dstH) sy1 = sy0 + (dstH - dy0);
    if (sx1 <= sx0 || sy1 <= sy0) return;

    for (int row = sy0; row < sy1; ++row) {
        BYTE* d = dst + ((size_t)(dy0 + (row - sy0)) * dstW + dx0) * 4;
        const BYTE* s = src + ((size_t)row * srcW + sx0) * 4;
        for (int col = sx0; col < sx1; ++col, d += 4, s += 4) BlendPixel(d, s);
    }
}

void BlendIntoAlpha(BYTE* dst, int dstW, int dstH, const BYTE* src, int srcW, int srcH,
                    int x, int y, int alpha) {
    BlendIntoDim(dst, dstW, dstH, src, srcW, srcH, x, y, alpha, 255);
}

void BlendIntoDim(BYTE* dst, int dstW, int dstH, const BYTE* src, int srcW, int srcH,
                  int x, int y, int alpha, int dim) {
    if (!dst || !src || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) return;
    if (alpha <= 0 || dim <= 0) return;
    if (alpha >= 255 && dim >= 255) {
        BlendInto(dst, dstW, dstH, src, srcW, srcH, x, y);
        return;
    }
    const unsigned k = (unsigned)alpha;
    const unsigned d = (unsigned)dim;

    int sx0 = 0, sy0 = 0, sx1 = srcW, sy1 = srcH;
    int dx0 = x, dy0 = y;
    if (dx0 < 0) { sx0 = -dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 = -dy0; dy0 = 0; }
    if (dx0 + (sx1 - sx0) > dstW) sx1 = sx0 + (dstW - dx0);
    if (dy0 + (sy1 - sy0) > dstH) sy1 = sy0 + (dstH - dy0);
    if (sx1 <= sx0 || sy1 <= sy0) return;

    for (int row = sy0; row < sy1; ++row) {
        BYTE* dp = dst + ((size_t)(dy0 + (row - sy0)) * dstW + dx0) * 4;
        const BYTE* s = src + ((size_t)row * srcW + sx0) * 4;
        for (int col = sx0; col < sx1; ++col, dp += 4, s += 4) {
            // a 决定"还剩多少不透明度"，rgb 再额外乘 d 让它变暗（预乘下仍合法：rgb <= a）
            const unsigned a = (unsigned)s[3] * k / 255;
            if (a == 0) continue;
            const unsigned sr = (unsigned)s[0] * k / 255 * d / 255;
            const unsigned sg = (unsigned)s[1] * k / 255 * d / 255;
            const unsigned sb = (unsigned)s[2] * k / 255 * d / 255;
            const unsigned inv = 255u - a;
            dp[0] = (BYTE)(sb + (unsigned)(dp[0] * inv + 127) / 255);
            dp[1] = (BYTE)(sg + (unsigned)(dp[1] * inv + 127) / 255);
            dp[2] = (BYTE)(sr + (unsigned)(dp[2] * inv + 127) / 255);
            dp[3] = (BYTE)(a + (unsigned)(dp[3] * inv + 127) / 255);
        }
    }
}

void FillRectAlpha(BYTE* dst, int dstW, int dstH, const RECT& r, int a, COLORREF color) {
    if (!dst || dstW <= 0 || dstH <= 0 || a <= 0) return;
    if (a > 255) a = 255;

    int x0 = std::max<int>(0, r.left);
    int y0 = std::max<int>(0, r.top);
    int x1 = std::min<int>(dstW, r.right);
    int y1 = std::min<int>(dstH, r.bottom);
    if (x1 <= x0 || y1 <= y0) return;

    const BYTE sb0 = (BYTE)MulDiv255(GetBValue(color), a);
    const BYTE sb1 = (BYTE)MulDiv255(GetGValue(color), a);
    const BYTE sb2 = (BYTE)MulDiv255(GetRValue(color), a);
    const unsigned inv = 255u - (unsigned)a;

    for (int y = y0; y < y1; ++y) {
        BYTE* d = dst + ((size_t)y * dstW + x0) * 4;
        for (int x = x0; x < x1; ++x, d += 4) {
            d[0] = (BYTE)(sb0 + (unsigned)(d[0] * inv + 127) / 255);
            d[1] = (BYTE)(sb1 + (unsigned)(d[1] * inv + 127) / 255);
            d[2] = (BYTE)(sb2 + (unsigned)(d[2] * inv + 127) / 255);
            d[3] = (BYTE)((unsigned)a + (unsigned)(d[3] * inv + 127) / 255);
        }
    }
}

HFONT IconLabelFont() {
    static HFONT cached = nullptr;
    if (cached) return cached;

    // 优先用资源管理器桌面列表视图自己的字体（唯一权威来源）
    cached = DesktopIconFont();
    if (cached) return cached;

    LOGFONTW lf{};
    if (!SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0)) {
        lf.lfHeight = -12;
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    }
    if (lf.lfHeight == 0) lf.lfHeight = -12;
    if (lf.lfWeight <= 0) lf.lfWeight = FW_NORMAL;
    // 强制灰度抗锯齿：ClearType 需要不透明底色，画在半透明层上会出彩色边
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfItalic = 0;
    lf.lfUnderline = 0;
    lf.lfStrikeOut = 0;
    cached = CreateFontIndirectW(&lf);
    return cached;
}

// ---------------------------------------------------------------------------
// IconDib
// ---------------------------------------------------------------------------
bool IconDib::Build(const IconBitmap& src) {
    Free();
    if (!src.valid()) return false;
    if (!CreateDib(src.width, src.height, dc, bmp, bits)) return false;
    std::memcpy(bits, src.pixels.data(), src.pixels.size());
    w = src.width;
    h = src.height;
    return true;
}

void IconDib::Free() {
    if (dc) { DeleteDC(dc); dc = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    bits = nullptr;
    w = h = 0;
}

// ---------------------------------------------------------------------------
// LabelDib
// ---------------------------------------------------------------------------
bool LabelDib::Build(const std::wstring& text, HFONT font, int boxW, int boxH, int glow,
                     COLORREF color, int maxLines) {
    Free();
    if (text.empty() || boxW <= 0 || boxH <= 0 || !font) return false;
    if (maxLines < 1) maxLines = 1;

    // 1) 先按格子宽度折行，再按「实际文字宽度」收窄 DIB。
    //    绝大多数标签远窄于格子，收窄后每个文字 DIB 能小一半以上（206 个图标很可观）。
    std::wstring wrapped;
    {
        HDC measure = CreateCompatibleDC(nullptr);
        if (!measure) return false;
        HGDIOBJ oldFont = SelectObject(measure, font);
        int oldMode = SetBkMode(measure, TRANSPARENT);
        wrapped = WrapLabel(measure, text, boxW, maxLines);
        SetBkMode(measure, oldMode);
        SelectObject(measure, oldFont);
        DeleteDC(measure);
    }
    if (wrapped.empty()) return false;

    boxW = 8;
    {
        HDC measure = CreateCompatibleDC(nullptr);
        if (!measure) return false;
        HGDIOBJ oldFont = SelectObject(measure, font);
        int best = 0;
        std::wstring line;
        for (size_t i = 0; i <= wrapped.size(); ++i) {
            if (i == wrapped.size() || wrapped[i] == L'\n') {
                if (!line.empty()) {
                    int w = MeasureCx(measure, line);
                    if (w > best) best = w;
                }
                line.clear();
            } else {
                line += wrapped[i];
            }
        }
        SelectObject(measure, oldFont);
        DeleteDC(measure);
        boxW = best + 2;
    }
    if (boxW < 8) boxW = 8;

    // 2) 在 boxW x boxH 的 DIB 上用 GDI 写白字
    HDC mdc = nullptr;
    HBITMAP mbmp = nullptr;
    void* mbits = nullptr;
    if (!CreateDib(boxW, boxH, mdc, mbmp, mbits)) return false;
    std::memset(mbits, 0, (size_t)boxW * boxH * 4);

    HGDIOBJ oldFont = SelectObject(mdc, font);
    int oldMode = SetBkMode(mdc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(mdc, RGB(255, 255, 255));

    RECT rc{0, 0, boxW, boxH};
    DrawTextW(mdc, wrapped.c_str(), -1, &rc,
              DT_CENTER | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);

    SetTextColor(mdc, oldColor);
    SetBkMode(mdc, oldMode);
    SelectObject(mdc, oldFont);

    // 3) 亮度 → alpha，再按目标颜色预乘（DIB 里是 BGRA）
    auto* mp = reinterpret_cast<BYTE*>(mbits);
    const size_t n = (size_t)boxW * boxH;
    const BYTE cb = GetBValue(color), cg = GetGValue(color), cr = GetRValue(color);
    for (size_t i = 0; i < n; ++i) {
        BYTE* p = mp + i * 4;
        BYTE a = p[0];
        if (p[1] > a) a = p[1];
        if (p[2] > a) a = p[2];
        p[0] = (BYTE)MulDiv255(cb, a);
        p[1] = (BYTE)MulDiv255(cg, a);
        p[2] = (BYTE)MulDiv255(cr, a);
        p[3] = a;
    }

    // 3) 合成：外扩 pad 后把黑色光晕与白字一次画好，之后每帧只需一次合成
    const int blurR = glow > 0 ? (glow < 4 ? glow : 4) : 0;
    pad = blurR > 0 ? blurR * 2 + 2 : 0;
    const int W = boxW + pad * 2;
    const int H = boxH + pad * 2;
    HDC ddc = nullptr;
    HBITMAP dbmp = nullptr;
    void* dbits = nullptr;
    if (!CreateDib(W, H, ddc, dbmp, dbits)) {
        DeleteDC(mdc);
        DeleteObject(mbmp);
        return false;
    }
    std::memset(dbits, 0, (size_t)W * H * 4);
    auto* dp = reinterpret_cast<BYTE*>(dbits);

    if (blurR > 0) {
        // 影子 = 「1px 膨胀（贴边实心暗环）」+「高斯近似模糊（外圈柔化）」取较大值。
        // 只用模糊的话细笔画的 alpha 被摊薄，字在壁纸上就"没有影子"了。
        std::vector<BYTE> alpha(n), blurred;
        for (size_t i = 0; i < n; ++i) alpha[i] = mp[i * 4 + 3];
        BoxBlurAlpha(alpha, blurred, boxW, boxH, blurR, 2);

        std::vector<BYTE> dil(n, 0);
        for (int y = 0; y < boxH; ++y) {
            for (int x = 0; x < boxW; ++x) {
                BYTE m = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= boxH) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= boxW) continue;
                        BYTE v = alpha[(size_t)yy * boxW + xx];
                        if (v > m) m = v;
                    }
                }
                dil[(size_t)y * boxW + x] = m;
            }
        }

        std::vector<BYTE> shadow((size_t)n * 4, 0);
        for (size_t i = 0; i < n; ++i) {
            unsigned a1 = (unsigned)dil[i] * 232 / 255;
            unsigned a2 = (unsigned)blurred[i] * 200 / 255;
            unsigned v = a1 > a2 ? a1 : a2;
            shadow[i * 4 + 3] = (BYTE)(v > 255 ? 255 : v);
        }
        BlendInto(dp, W, H, shadow.data(), boxW, boxH, pad, pad);
    }

    BlendInto(dp, W, H, mp, boxW, boxH, pad, pad);

    DeleteDC(mdc);
    DeleteObject(mbmp);

    dc = ddc;
    bmp = dbmp;
    bits = dbits;
    w = W;
    h = H;
    return true;
}

void LabelDib::Free() {
    if (dc) { DeleteDC(dc); dc = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    bits = nullptr;
    w = h = 0;
    pad = 0;
}
