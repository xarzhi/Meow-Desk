// 图标布局持久化
//
// 为什么需要：`owner`（图标属于哪个盒子）以前只存在内存里，程序一退出就全丢，
// 盒子里放好的图标重启后全回到桌面。这里把「归属 + 盒内顺序 + 桌面坐标」
// 落到 SQLite 的 icon_layout 表，启动时按 path 还原。
//
// 用 path（解析名）当主键：显示名可能重名，path 唯一且稳定。
//
// 历史：早期版本用 %APPDATA%\meow-desk\layout.ini（UTF-8，TAB 分隔）。
// 现在只在两种情况下还会读它 —— 首次迁移到 SQLite、以及数据库打不开时的回退。
#include "layout_store.h"

#include "storage.h"

#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

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

std::wstring LegacyIniPath() { return AppDataDir() + L"\\layout.ini"; }

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void BackupLegacy(const std::wstring& p) {
    const std::wstring bak = p + L".bak";
    DeleteFileW(bak.c_str());
    MoveFileW(p.c_str(), bak.c_str());
}

void LogLine(const char* s) {
    FILE* f = nullptr;
    fopen_s(&f, "meow-desk.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
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

// ---- 老 layout.ini 解析（迁移 / 回退专用）----
bool LoadLegacyIni(std::vector<LayoutEntry>& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, LegacyIniPath().c_str(), L"rb") != 0 || !f) return false;

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

bool LayoutFromDb(std::vector<LayoutEntry>& out) {
    out.clear();
    storage::Stmt st("SELECT path,owner,ord,x,y FROM icon_layout");
    if (!st.ok()) return false;
    while (st.step()) {
        LayoutEntry e;
        e.path = st.colText(0);
        e.owner = st.colInt(1);
        e.order = st.colInt(2);
        e.pos.x = st.colInt(3);
        e.pos.y = st.colInt(4);
        if (e.path.empty()) continue;
        if (e.owner < 0) e.owner = -1;
        out.push_back(std::move(e));
    }
    return !out.empty();
}

}  // namespace

bool LoadLayout(std::vector<LayoutEntry>& out) {
    out.clear();
    if (storage::Open()) {
        if (LayoutFromDb(out)) return true;
        // 空表：如果有老的 layout.ini，导进来再备份掉
        const std::wstring legacy = LegacyIniPath();
        if (FileExists(legacy) && LoadLegacyIni(out)) {
            SaveLayout(out);
            BackupLegacy(legacy);
            LogLine("layout: migrated layout.ini -> meow-desk.db");
            return !out.empty();
        }
        return false;
    }
    LogLine(("layout: sqlite unavailable, fallback to ini. " + storage::LastError()).c_str());
    return LoadLegacyIni(out);
}

bool SaveLayout(const std::vector<LayoutEntry>& in) {
    if (!storage::Open()) return false;
    if (!storage::Begin()) return false;

    bool ok = storage::Exec("DELETE FROM icon_layout;");
    if (ok) {
        storage::Stmt st(
            "INSERT OR REPLACE INTO icon_layout(path,owner,ord,x,y)"
            " VALUES(?1,?2,?3,?4,?5)");
        for (const auto& e : in) {
            if (!st.ok()) {
                ok = false;
                break;
            }
            st.bindText(1, e.path);
            st.bindInt(2, e.owner);
            st.bindInt(3, e.order);
            st.bindInt(4, e.pos.x);
            st.bindInt(5, e.pos.y);
            ok = st.done() && ok;
            st.reset();
        }
    }

    if (ok) {
        storage::Commit();
        return true;
    }
    storage::Rollback();
    LogLine(("layout: save failed. " + storage::LastError()).c_str());
    return false;
}
