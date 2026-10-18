#include "config.h"

#include "storage.h"

#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

AppConfig g_config;

namespace {

// ---------------------------------------------------------------------------
// 老的 config.ini（只用于两种情况：首次迁移到 SQLite、数据库打不开时回退读取）
// ---------------------------------------------------------------------------
std::wstring LegacyIniPath() { return AppConfig::Dir() + L"\\config.ini"; }

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// config.ini → config.ini.bak，避免迁移完又导一次
void BackupLegacy(const std::wstring& p) {
    const std::wstring bak = p + L".bak";
    DeleteFileW(bak.c_str());
    MoveFileW(p.c_str(), bak.c_str());
}

// 和 main.cpp 的 LogStep 写同一个文件。这里不引 common 日志头是为了不让
// config 依赖 main，重复这 4 行比多一层依赖划算。
void LogLine(const char* s) {
    FILE* f = nullptr;
    fopen_s(&f, "meow-desk.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}

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

// 读老 ini 到 g_config（迁移和回退都用）
void LoadLegacyIni(AppConfig& c) {
    c.fences.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, LegacyIniPath().c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    FenceConfig cur;
    bool inFence = false;
    bool hasFence = false;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring s = Trim(line);
        if (s.empty() || s[0] == L'#' || s[0] == L';') continue;
        if (s[0] == L'[') {
            if (inFence && hasFence) c.fences.push_back(cur);
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
            if (k == L"textShadow") c.textShadow = ToInt(v, 1);
            else if (k == L"shortcutArrow") c.shortcutArrow = ToInt(v, 1);
            else if (k == L"showLabels") c.showLabels = ToInt(v, 1);
            else if (k == L"headerHeight") c.headerHeight = ToInt(v, 44);
            else if (k == L"autoExpandOnHover") c.autoExpandOnHover = ToInt(v, 0);
            else if (k == L"sortAnim") c.sortAnim = ToInt(v, 1);
            else if (k == L"showFenceTitle") c.showFenceTitle = ToInt(v, 1);
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
        else if (k == L"locked") cur.locked = ToInt(v, 0);
        else if (k == L"title") cur.title = v;
        else if (k == L"viewMode") cur.viewMode = ToInt(v, 1);
        else if (k == L"sortKey") cur.sortKey = ToInt(v, 4);
        else if (k == L"sortAsc") cur.sortAsc = ToInt(v, 1);
    }
    if (inFence && hasFence) c.fences.push_back(cur);
    fclose(f);
}

// 从 SQLite 读。返回 false = 库里还没初始化过（不是错误）
bool ConfigFromDb(AppConfig& c) {
    c.fences.clear();
    {
        storage::Stmt st("SELECT k, v FROM app");
        if (!st.ok()) return false;
        while (st.step()) {
            const std::wstring k = st.colText(0);
            const int n = ToInt(st.colText(1), 0);
            if (k == L"textShadow") c.textShadow = n;
            else if (k == L"shortcutArrow") c.shortcutArrow = n;
            else if (k == L"showLabels") c.showLabels = n;
            else if (k == L"headerHeight") c.headerHeight = n;
            else if (k == L"autoExpandOnHover") c.autoExpandOnHover = n;
            else if (k == L"sortAnim") c.sortAnim = n;
            else if (k == L"showFenceTitle") c.showFenceTitle = n;
            else if (k == L"autoOrganize") c.autoOrganize = n;
            else if (k == L"switchTabOnHover") c.switchTabOnHover = n;
        }
    }
    {
        storage::Stmt st(
            "SELECT x,y,w,h,color,alpha,radius,acrylic,title,viewMode,sortKey,sortAsc,locked,"
            "activeGroup FROM fence ORDER BY id");
        if (!st.ok()) return false;
        while (st.step()) {
            FenceConfig f;
            f.x = st.colInt(0);
            f.y = st.colInt(1);
            f.w = st.colInt(2);
            f.h = st.colInt(3);
            f.color = st.colInt(4);
            f.alpha = st.colInt(5);
            f.radius = st.colInt(6);
            f.acrylic = st.colInt(7);
            f.title = st.colText(8);
            f.viewMode = st.colInt(9);
            f.sortKey = st.colInt(10);
            f.sortAsc = st.colInt(11);
            f.locked = st.colInt(12);
            f.activeGroup = st.colInt(13);
            c.fences.push_back(std::move(f));
        }
    }
    {
        // 分组：一个盒子可能有 0..N 条，按 ord 排好塞回各自的 FenceConfig
        storage::Stmt st("SELECT fence,ord,name FROM fence_group ORDER BY fence,ord");
        if (st.ok()) {
            while (st.step()) {
                const int fi = st.colInt(0);
                if (fi < 0 || fi >= (int)c.fences.size()) continue;
                c.fences[fi].groups.push_back(st.colText(2));
            }
        }
    }
    {
        storage::Stmt st("SELECT name,exts,matchFolders,isFallback FROM rule ORDER BY id");
        if (st.ok()) {
            while (st.step()) {
                OrganizeRule r;
                r.name = st.colText(0);
                r.exts = st.colText(1);
                r.matchFolders = st.colInt(2);
                r.isFallback = st.colInt(3);
                c.rules.push_back(std::move(r));
            }
        }
    }
    // 没有任何盒子 = 还没初始化过（ConfigEnsureDefaults 保证至少 2 个）
    return !c.fences.empty();
}

}  // namespace

std::wstring AppConfig::Dir() { return storage::Dir(); }
std::wstring AppConfig::File() { return storage::File(); }

void ConfigEnsureDefaults() {
    bool dirty = false;
    if (g_config.rules.empty()) {
        g_config.rules = OrganizeRulesDefault();
        dirty = true;
    }
    if (g_config.fences.empty()) {
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
        dirty = true;
    }
    if (dirty) g_config.Save();
}

// ---------------------------------------------------------------------------
// 整理规则
// ---------------------------------------------------------------------------
std::vector<OrganizeRule> OrganizeRulesDefault() {
    // 顺序就是优先级，兜底规则永远在最后（Match 里也是这么处理的）
    return {
        {L"\u5feb\u6377\u65b9\u5f0f",                      // 快捷方式
         L".lnk; .url; .appref-ms; .website", 0, 0},
        {L"\u6587\u4ef6\u5939", L"", 1, 0},                // 文件夹
        {L"\u6587\u6863",                                   // 文档
         L".doc; .docx; .xls; .xlsx; .xlsm; .ppt; .pptx; .pdf; .txt; .md; .rtf; .wps; "
         L".et; .dps; .csv; .odt; .ods; .odp; .ofd; .xmind; .vsdx; .epub",
         0, 0},
        {L"\u56fe\u7247",                                   // 图片
         L".jpg; .jpeg; .png; .gif; .bmp; .webp; .svg; .ico; .tif; .tiff; .heic; "
         L".heif; .psd; .ai; .raw",
         0, 0},
        {L"\u5f71\u97f3",                                   // 影音
         L".mp4; .mkv; .avi; .mov; .wmv; .flv; .webm; .m4v; .rmvb; .mp3; .wav; .flac; "
         L".aac; .m4a; .ogg; .wma; .ape",
         0, 0},
        {L"\u538b\u7f29\u5305",                             // 压缩包
         L".zip; .rar; .7z; .tar; .gz; .tgz; .bz2; .xz; .iso; .cab", 0, 0},
        {L"\u7a0b\u5e8f",                                   // 程序
         L".exe; .msi; .bat; .cmd; .ps1; .vbs; .jar; .appx; .msix", 0, 0},
        {L"\u5176\u4ed6", L"", 0, 1},                       // 其他（兜底）
    };
}

namespace {
bool IsSep(wchar_t ch) {
    return ch == L';' || ch == L',' || ch == L' ' || ch == L'\t' || ch == L'\uFF1B' ||
           ch == L'\uFF0C';
}
}  // namespace

bool OrganizeRuleMatches(const OrganizeRule& r, const std::wstring& extLower, bool isDir) {
    if (r.isFallback) return false;   // 兜底规则由调用方在最后处理
    if (isDir) return r.matchFolders != 0;
    if (extLower.empty()) return false;
    const std::wstring& s = r.exts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSep(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !IsSep(s[j])) ++j;
        if (j > i) {
            std::wstring tok = s.substr(i, j - i);
            if (tok[0] != L'.') tok.insert(tok.begin(), L'.');
            for (auto& ch : tok)
                if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
            if (tok == extLower) return true;
        }
        i = j;
    }
    return false;
}

void AppConfig::Load() {
    fences.clear();
    rules.clear();
    if (storage::Open()) {
        if (ConfigFromDb(*this)) return;
        // 空库：如果有老的 config.ini，导进来再备份掉
        const std::wstring legacy = LegacyIniPath();
        if (FileExists(legacy)) {
            LoadLegacyIni(*this);
            if (!fences.empty()) {
                Save();
                BackupLegacy(legacy);
                LogLine("config: migrated config.ini -> meow-desk.db");
            }
        }
        return;
    }
    // 数据库不可用 → 退回读老文件，至少配置还能生效
    LogLine(("config: sqlite unavailable, fallback to ini. " + storage::LastError()).c_str());
    LoadLegacyIni(*this);
}

void AppConfig::Save() const {
    if (!storage::Open()) return;
    if (!storage::Begin()) return;

    bool ok = storage::Exec("DELETE FROM app;") && storage::Exec("DELETE FROM fence;") &&
              storage::Exec("DELETE FROM rule;") && storage::Exec("DELETE FROM fence_group;");
    if (ok) {
        storage::Stmt st("INSERT OR REPLACE INTO app(k,v) VALUES(?1,?2)");
        auto put = [&st](const wchar_t* k, int v) {
            if (!st.ok()) return;
            st.bindText(1, k);
            st.bindText(2, std::to_wstring(v));
            st.done();
            st.reset();
        };
        put(L"textShadow", textShadow);
        put(L"shortcutArrow", shortcutArrow);
        put(L"showLabels", showLabels);
        put(L"headerHeight", headerHeight);
        put(L"autoExpandOnHover", autoExpandOnHover);
        put(L"sortAnim", sortAnim);
        put(L"showFenceTitle", showFenceTitle);
        put(L"autoOrganize", autoOrganize);
        put(L"switchTabOnHover", switchTabOnHover);

        storage::Stmt fs(
            "INSERT INTO fence(id,x,y,w,h,color,alpha,radius,acrylic,title,"
            "viewMode,sortKey,sortAsc,locked,activeGroup)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");
        for (size_t i = 0; i < fences.size() && fs.ok(); ++i) {
            const FenceConfig& c = fences[i];
            fs.bindInt(1, (int)i);
            fs.bindInt(2, c.x);
            fs.bindInt(3, c.y);
            fs.bindInt(4, c.w);
            fs.bindInt(5, c.h);
            fs.bindInt(6, c.color);
            fs.bindInt(7, c.alpha);
            fs.bindInt(8, c.radius);
            fs.bindInt(9, c.acrylic);
            fs.bindText(10, c.title);
            fs.bindInt(11, c.viewMode);
            fs.bindInt(12, c.sortKey);
            fs.bindInt(13, c.sortAsc);
            fs.bindInt(14, c.locked);
            fs.bindInt(15, c.activeGroup);
            ok = fs.done() && ok;
            fs.reset();
        }

        if (ok) {
            storage::Stmt gs("INSERT INTO fence_group(fence,ord,name) VALUES(?1,?2,?3)");
            for (size_t i = 0; i < fences.size() && gs.ok(); ++i) {
                const FenceConfig& c = fences[i];
                for (size_t k = 0; k < c.groups.size() && gs.ok(); ++k) {
                    gs.bindInt(1, (int)i);
                    gs.bindInt(2, (int)k);
                    gs.bindText(3, c.groups[k]);
                    ok = gs.done() && ok;
                    gs.reset();
                }
            }
        }

        if (ok) {
            storage::Stmt rs(
                "INSERT INTO rule(id,name,exts,matchFolders,isFallback)"
                " VALUES(?1,?2,?3,?4,?5)");
            for (size_t i = 0; i < rules.size() && rs.ok(); ++i) {
                const OrganizeRule& r = rules[i];
                rs.bindInt(1, (int)i);
                rs.bindText(2, r.name);
                rs.bindText(3, r.exts);
                rs.bindInt(4, r.matchFolders);
                rs.bindInt(5, r.isFallback);
                ok = rs.done() && ok;
                rs.reset();
            }
        }
    }

    if (ok) storage::Commit();
    else {
        storage::Rollback();
        LogLine(("config: save failed. " + storage::LastError()).c_str());
    }
}
