// 诊断：AlphaBlend 各种重载/参数组合下是否报 ERROR_INVALID_PARAMETER
#include <windows.h>
#include <cstdio>
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static HDC g_screen = nullptr;

static bool MakeDib(int w, int h, bool topdown, bool useScreenDc, HDC& dc, HBITMAP& bmp, void*& bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = topdown ? -h : h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(useScreenDc ? g_screen : nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { printf("  CreateDIBSection failed %lu\n", GetLastError()); return false; }
    dc = useScreenDc ? CreateCompatibleDC(g_screen) : CreateCompatibleDC(nullptr);
    SelectObject(dc, bmp);
    memset(bits, 0x40, (size_t)w * h * 4);
    return true;
}

static void Try(const char* name, bool srcTop, bool dstTop, bool srcScreen, bool dstScreen) {
    HDC sdc = nullptr, ddc = nullptr;
    HBITMAP sb = nullptr, db = nullptr;
    void* sbits = nullptr;
    void* dbits = nullptr;
    if (!MakeDib(48, 48, srcTop, srcScreen, sdc, sb, sbits)) return;
    if (!MakeDib(200, 200, dstTop, dstScreen, ddc, db, dbits)) return;

    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetLastError(0);
    BOOL ok = AlphaBlend(ddc, 10, 10, 48, 48, sdc, 0, 0, 48, 48, bf);
    DWORD e1 = GetLastError();

    auto* p = reinterpret_cast<unsigned char*>(dbits);
    const unsigned char* px = p + ((size_t)20 * 200 + 20) * 4;
    int alphaSum = 0;
    for (int i = 0; i < 200 * 200; ++i) alphaSum += p[i * 4 + 3];

    printf("%-46s AB=%d err=%lu  dstBGRA@20,20=(%02X %02X %02X %02X) alphaSum=%d\n",
           name, ok, e1, px[0], px[1], px[2], px[3], alphaSum);

    DeleteDC(sdc); DeleteObject(sb);
    DeleteDC(ddc); DeleteObject(db);
}

int main() {
    g_screen = GetDC(nullptr);
    printf("screen bpp=%d\n", GetDeviceCaps(g_screen, BITSPIXEL));
    Try("srcTop dstTop  srcCompatNULL dstCompatNULL", true, true, false, false);
    Try("srcTop dstTop  srcCompatSCR  dstCompatSCR ", true, true, true, true);
    Try("srcBot dstBot  srcCompatNULL dstCompatNULL", false, false, false, false);
    Try("srcBot dstTop  srcCompatNULL dstCompatNULL", false, true, false, false);
    Try("srcTop dstBot  srcCompatNULL dstCompatNULL", true, false, false, false);
    ReleaseDC(nullptr, g_screen);
    return 0;
}
