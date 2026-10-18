#include "layout_store.h"

#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const wchar_t* kHeader = L"# meow-desk layout v1";

std::wstring AppDataDir() {
    wchar_t* p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) {
        wchar_t buf[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) dir = buf;
    }
    if (dir.empty()) return L".";
    dir += L"\\meow-desk";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::vector<std::wstring> SplitTab(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L'\t') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

std::wstring LayoutFilePath() { return AppDataDir() + L"\\layout.ini"; }

bool LoadLayout(std::vector<LayoutEntry>& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, LayoutFilePath().c_str(), L"rb") != 0 || !f) return false;

    // UTF-8（兼容 BOM）
    std::string data;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);

    const int wn = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), nullptr, 0);
    std::wstring w((size_t)wn, L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &w[0], wn);

    size_t pos = 0;
    while (pos <= w.size()) {
        size_t nl = w.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = w.size();
        std::wstring line = w.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;

        auto parts = SplitTab(line);
        if (parts.size() < 5) continue;
        LayoutEntry e;
        e.owner = _wtoi(parts[0].c_str());
        e.order = _wtoi(parts[1].c_str());
        e.pos.x = _wtoi(parts[2].c_str());
        e.pos.y = _wtoi(parts[3].c_str());
        // path 可能被 TAB 切碎（理论上不该有），把剩下的拼回去
        e.path = parts[4];
        for (size_t i = 5; i < parts.size(); ++i) {
            e.path += L'\t';
            e.path += parts[i];
        }
        if (e.path.empty()) continue;
        if (e.owner < 0) e.owner = -1;
        out.push_back(std::move(e));
    }
    return !out.empty();
}

bool SaveLayout(const std::vector<LayoutEntry>& in) {
    const std::wstring path = LayoutFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;

    std::wstring text;
    text += kHeader;
    text += L"\n";
    for (const auto& e : in) {
        wchar_t buf[64];
        swprintf_s(buf, L"%d\t%d\t%d\t%d\t", e.owner, e.order, e.pos.x, e.pos.y);
        text += buf;
        text += e.path;
        text += L"\n";
    }

    const int an = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                       nullptr, nullptr);
    std::string utf8((size_t)an, '\0');
    if (an > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], an, nullptr,
                            nullptr);
    const bool ok = fwrite(utf8.data(), 1, utf8.size(), f) == utf8.size();
    fclose(f);
    return ok;
}
