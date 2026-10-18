// Shell 图标加载 + 系统桌面图标隐藏/恢复
#include "shell_icons.h"

#include <shobjidl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commoncontrols.h>
#include <gdiplus.h>

#include <cstring>
#include <memory>

#include "icon_render.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

static HWND FindDesktopDefView();
static HWND FindDesktopListView();

// ---------------------------------------------------------------------------
// GDI+ 只在这里用于「HICON → 像素」和「小图标缩放」，进程内启动一次即可
// ---------------------------------------------------------------------------


// HICON → 预乘 BGRA 像素（可选缩放到 target 边长）
static bool HIconToPixels(HICON ico, int target, IconBitmap& out) {
    out = IconBitmap{};
    if (!ico) return false;
    EnsureGdiplus();

    std::unique_ptr<Gdiplus::Bitmap> src(Gdiplus::Bitmap::FromHICON(ico));
    if (!src || src->GetLastStatus() != Gdiplus::Ok) return false;

    const int sw = (int)src->GetWidth();
    const int sh = (int)src->GetHeight();
    if (sw <= 0 || sh <= 0) return false;
    const int tw = target > 0 ? target : sw;
    const int th = target > 0 ? target : sh;

    std::unique_ptr<Gdiplus::Bitmap> dst(new Gdiplus::Bitmap(
        tw, th, PixelFormat32bppPARGB));
    {
        Gdiplus::Graphics g(dst.get());
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.DrawImage(src.get(), Gdiplus::Rect(0, 0, tw, th));
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    Gdiplus::Rect lockRect(0, 0, tw, th);
    Gdiplus::BitmapData bd{};
    if (dst->LockBits(&lockRect, Gdiplus::ImageLockModeRead,
                      PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return false;

    out.width = tw;
    out.height = th;
    out.pixels.assign((size_t)tw * th * 4, 0);
    for (int y = 0; y < th; ++y) {
        const BYTE* row = reinterpret_cast<const BYTE*>(bd.Scan0) + (size_t)y * bd.Stride;
        std::memcpy(out.pixels.data() + (size_t)y * tw * 4, row, (size_t)tw * 4);
    }
    dst->UnlockBits(&bd);
    return true;
}

// ---------------------------------------------------------------------------
// 图标加载
// ---------------------------------------------------------------------------
static bool BitmapToPixels(HBITMAP hbmp, IconBitmap& out) {
    if (!hbmp) return false;
    BITMAP bm{};
    if (!GetObjectW(hbmp, sizeof(bm), &bm)) return false;
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;

    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.pixels.assign((size_t)w * h * 4, 0);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return false;
    int got = GetDIBits(hdc, hbmp, 0, (UINT)h, out.pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (got == 0) return false;

    out.width = w;
    out.height = h;
    return true;
}

// ---------------------------------------------------------------------------
// 快捷方式箭头：自绘（系统 overlay 取不到干净的 HICON，自绘更可控也更清晰）
// ---------------------------------------------------------------------------
static bool MakeLinkArrowBitmap(int size, IconBitmap& out) {
    out = IconBitmap{};
    if (size < 6) return false;
    EnsureGdiplus();

    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const float s = (float)size;
        Gdiplus::GraphicsPath box;
        {
            const float r = s * 0.22f;
            const float d = r * 2.0f;
            Gdiplus::RectF rf(s * 0.03f, s * 0.03f, s * 0.94f, s * 0.94f);
            box.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
            box.AddArc(rf.X + rf.Width - d, rf.Y, d, d, 270.0f, 90.0f);
            box.AddArc(rf.X + rf.Width - d, rf.Y + rf.Height - d, d, d, 0.0f, 90.0f);
            box.AddArc(rf.X, rf.Y + rf.Height - d, d, d, 90.0f, 90.0f);
            box.CloseFigure();
        }
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        g.FillPath(&white, &box);
        Gdiplus::Pen edge(Gdiplus::Color(46, 0, 0, 0), 1.0f);
        g.DrawPath(&edge, &box);

        const Gdiplus::Color accent(255, 16, 110, 190);
        Gdiplus::Pen shaft(accent, s * 0.105f);
        shaft.SetStartCap(Gdiplus::LineCapRound);
        shaft.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&shaft, s * 0.33f, s * 0.67f, s * 0.56f, s * 0.44f);

        Gdiplus::SolidBrush head(accent);
        Gdiplus::PointF tri[3] = {
            Gdiplus::PointF(s * 0.74f, s * 0.26f),
            Gdiplus::PointF(s * 0.74f, s * 0.53f),
            Gdiplus::PointF(s * 0.47f, s * 0.26f),
        };
        g.FillPolygon(&head, tri, 3);
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    Gdiplus::Rect lockRect(0, 0, size, size);
    Gdiplus::BitmapData bd{};
    if (bmp.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) !=
        Gdiplus::Ok)
        return false;

    out.width = size;
    out.height = size;
    out.pixels.assign((size_t)size * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        const BYTE* row = reinterpret_cast<const BYTE*>(bd.Scan0) + (size_t)y * bd.Stride;
        std::memcpy(out.pixels.data() + (size_t)y * size * 4, row, (size_t)size * 4);
    }
    bmp.UnlockBits(&bd);
    NormalizePremultiplied(out);
    return true;
}

bool IsShortcutPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".lnk" || ext == L".url";
}

// 把快捷方式箭头叠到图标左下角（比例与资源管理器一致）
static void ApplyLinkOverlay(IconBitmap& icon) {
    const int ovPx = (int)(icon.width * 0.44f + 0.5f);
    IconBitmap ovBmp;
    if (!MakeLinkArrowBitmap(ovPx, ovBmp) || !ovBmp.valid()) return;

    IconDib base, over;
    if (!base.Build(icon) || !over.Build(ovBmp)) return;

    // 箭头压住图标左下角，略微外溢一点
    const int ox = -(int)(icon.width * 0.03f);
    const int oy = icon.height - ovPx + (int)(icon.width * 0.03f);
    BlendInto(reinterpret_cast<BYTE*>(base.bits), base.w, base.h,
              reinterpret_cast<const BYTE*>(over.bits), over.w, over.h, ox, oy);

    std::memcpy(icon.pixels.data(), base.bits, icon.pixels.size());
}

bool LoadShellIcon(const std::wstring& path, int size, IconBitmap& out, bool linkOverlay) {
    out = IconBitmap{};

    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr,
            IID_IShellItemImageFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        SHFILEINFOW sfi{};
        if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                SHGFI_ICON | (size > 32 ? SHGFI_LARGEICON : SHGFI_SMALLICON)))
            return false;
        bool ok = false;
        if (sfi.hIcon) {
            ok = HIconToPixels(sfi.hIcon, size, out);
            DestroyIcon(sfi.hIcon);
        }
        if (ok) {
            NormalizePremultiplied(out);
            if (linkOverlay && IsShortcutPath(path)) ApplyLinkOverlay(out);
        }
        return ok;
    }

    SIZE sz{size, size};
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    factory->Release();
    if (FAILED(hr) || !hbmp) return false;
    bool ok = BitmapToPixels(hbmp, out);
    DeleteObject(hbmp);
    if (!ok) return false;

    NormalizePremultiplied(out);
    if (linkOverlay && IsShortcutPath(path)) ApplyLinkOverlay(out);
    return true;
}

// ---------------------------------------------------------------------------
// 系统桌面图标视图：Progman (或承载 DefView 的 WorkerW) -> SHELLDLL_DefView -> SysListView32
// ---------------------------------------------------------------------------
static HWND FindDesktopDefView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) return defView;
    }
    struct Ctx { HWND found; };
    Ctx ctx{nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) {
            reinterpret_cast<Ctx*>(lp)->found = defView;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

HWND FindDesktopHostWindow() {
    HWND defView = FindDesktopDefView();
    if (defView) return GetAncestor(defView, GA_PARENT);
    return FindWindowW(L"Progman", nullptr);
}

static HWND FindDesktopListView() {
    HWND defView = FindDesktopDefView();
    if (!defView) return nullptr;
    return FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
}

bool SetSystemDesktopIconsVisible(bool visible) {
    HWND listView = FindDesktopListView();
    if (!listView) return false;
    return ShowWindowAsync(listView, visible ? SW_SHOWNA : SW_HIDE) != FALSE;
}

bool IsSystemDesktopIconsVisible() {
    HWND listView = FindDesktopListView();
    return listView && IsWindowVisible(listView);
}

// ---------------------------------------------------------------------------
// 桌面图标标签字体
// ---------------------------------------------------------------------------
HFONT DesktopIconFont() {
    static HFONT cached = nullptr;
    if (cached) return cached;

    LOGFONTW lf{};
    bool have = false;

    // 向资源管理器自己的列表视图要字体 —— 这才是桌面图标标签真正在用的那个
    HWND lv = FindDesktopListView();
    if (lv) {
        auto hf = (HFONT)SendMessageW(lv, WM_GETFONT, 0, 0);
        if (hf && GetObjectW(hf, sizeof(lf), &lf) == sizeof(lf) && lf.lfHeight != 0)
            have = true;
    }

    if (!have) {
        LOGFONTW spi{};
        if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(spi), &spi, 0) &&
            spi.lfHeight != 0) {
            lf = spi;
            have = true;
        }
    }

    if (!have) {
        lf.lfHeight = -12;
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    if (lf.lfWeight <= 0) lf.lfWeight = FW_NORMAL;
    // 灰度抗锯齿：ClearType 需要不透明底色，画在透明层上会出彩色边
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfItalic = 0;
    lf.lfUnderline = 0;
    lf.lfStrikeOut = 0;
    cached = CreateFontIndirectW(&lf);
    return cached;
}
