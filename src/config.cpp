#include "config.h"

#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

AppConfig g_config;

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

int ToInt(const std::wstring& s, int def) {
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    return end && end != s.c_str() ? (int)v : def;
}

}  // namespace

std::wstring AppConfig::Dir() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = appdata;
        CoTaskMemFree(appdata);
    } else {
        dir = L".";
    }
    dir += L"\\meow-desk";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring AppConfig::File() { return Dir() + L"\\config.ini"; }

void ConfigEnsureDefaults() {
    if (!g_config.fences.empty()) return;
    FenceConfig a;
    a.x = 1100; a.y = 400;
    a.title = L"\u6211\u7684\u76d2\u5b50";
    g_config.fences.push_back(a);

    FenceConfig b;
    b.x = 1100; b.y = 100;
    b.h = 260;
    b.color = RGB(246, 248, 255);
    b.title = L"\u5de5\u4f5c";
    g_config.fences.push_back(b);
    g_config.Save();
}

void AppConfig::Load() {
    fences.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, File().c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    FenceConfig cur;
    bool inFence = false;
    bool hasFence = false;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring s = Trim(line);
        if (s.empty() || s[0] == L'#' || s[0] == L';') continue;
        if (s[0] == L'[') {
            if (inFence && hasFence) fences.push_back(cur);
            if (s.compare(0, 6, L"[fence") == 0) {
                cur = FenceConfig();
                inFence = true;
                hasFence = true;
            } else {
                inFence = false;
                hasFence = false;
            }
            continue;
        }
        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(s.substr(0, eq));
        std::wstring v = Trim(s.substr(eq + 1));

        if (!inFence) {
            if (k == L"textShadow") textShadow = ToInt(v, 1);
            else if (k == L"shortcutArrow") shortcutArrow = ToInt(v, 1);
            continue;
        }
        if (k == L"x") cur.x = ToInt(v, 100);
        else if (k == L"y") cur.y = ToInt(v, 100);
        else if (k == L"w") cur.w = ToInt(v, 340);
        else if (k == L"h") cur.h = ToInt(v, 470);
        else if (k == L"color") cur.color = ToInt(v, RGB(250, 251, 254));
        else if (k == L"alpha") cur.alpha = ToInt(v, 236);
        else if (k == L"radius") cur.radius = ToInt(v, 12);
        else if (k == L"acrylic") cur.acrylic = ToInt(v, 0);
        else if (k == L"title") cur.title = v;
    }
    if (inFence && hasFence) fences.push_back(cur);
    fclose(f);
}

void AppConfig::Save() const {
    FILE* f = nullptr;
    if (_wfopen_s(&f, File().c_str(), L"w, ccs=UTF-8") != 0 || !f) return;

    fwprintf(f, L"# meow-desk config\n");
    fwprintf(f, L"textShadow=%d\n", textShadow);
    fwprintf(f, L"shortcutArrow=%d\n", shortcutArrow);
    for (size_t i = 0; i < fences.size(); ++i) {
        const FenceConfig& c = fences[i];
        fwprintf(f, L"\n[fence%u]\n", (unsigned)i);
        fwprintf(f, L"x=%d\ny=%d\nw=%d\nh=%d\n", c.x, c.y, c.w, c.h);
        fwprintf(f, L"color=%d\nalpha=%d\nradius=%d\nacrylic=%d\n",
                 c.color, c.alpha, c.radius, c.acrylic);
        fwprintf(f, L"title=%ls\n", c.title.c_str());
    }
    fclose(f);
}
