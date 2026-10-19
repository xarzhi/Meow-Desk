#include "app_icon.h"

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include "app_res.h"
#include "icon_render.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

const void* AppResourceBytes(int id, DWORD& size) {
    size = 0;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(inst, r);
    if (!g) return nullptr;
    size = SizeofResource(inst, r);
    return LockResource(g);
}

HICON LoadAppIcon(int size) {
    if (size <= 0) size = 32;
    // 不传 LR_SHARED：托盘 / 窗口各自持有、各自 DestroyIcon，共享句柄反而会被
    // 某个调用方提前销毁掉，另一个就变成野句柄（托盘图标空白）。
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                             size, size, 0);
}

Gdiplus::Bitmap* AppLogoBitmap() {
    static Gdiplus::Bitmap* cached = nullptr;
    static bool tried = false;
    if (tried) return cached;
    tried = true;

    DWORD n = 0;
    const void* p = AppResourceBytes(IDR_LOGO_PNG, n);
    if (!p || n == 0) return nullptr;

    EnsureGdiplus();
    // GDI+ 的 Bitmap 从 IStream 构造是「延迟解码」：流一释放，后面访问像素就失败。
    // 所以先构造、立刻问一次尺寸（触发解码），再释放流。
    IStream* s = SHCreateMemStream(reinterpret_cast<const BYTE*>(p), n);
    if (!s) return nullptr;
    auto* bmp = new Gdiplus::Bitmap(s, FALSE);
    if (bmp->GetLastStatus() != Gdiplus::Ok || bmp->GetWidth() == 0) {
        delete bmp;
        bmp = nullptr;
    }
    s->Release();
    cached = bmp;
    return cached;
}
