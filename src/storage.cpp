#include "storage.h"

#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

#include "sqlite3.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace storage {
namespace {

sqlite3* g_db = nullptr;
bool g_tried = false;
std::string g_lastError;
std::wstring g_dir;
std::wstring g_file;
std::wstring g_logfile;

// schema 版本：以后加字段就 +1，并在 Upgrade() 里补一段迁移
//   v1 — 初版：app / fence / icon_layout
//   v2 — fence 表加 viewMode / sortKey / sortAsc（每个盒子各自的「查看」「排序」）
//   v3 — fence 表加 locked（盒子锁定）；新增 rule 表（整理规则）
//   v4 — fence 表加 activeGroup；新增 fence_group 表（盒子内的分组标签）；
//        icon_layout 加 grp（图标属于哪个分组）
//   v5 — fence 表加 expandMode（展开 / 伸缩模式；-1 = 老数据，启动时按全局默认补）
//   v6 — 把全局开关 defaultTelescope 落成 0：新建的盒子默认始终展开，不自己收起
//   v7 — fence 表加 kind（0 = 收纳盒 / 1 = 待办盒）；新增 todo 表（待办条目）
//   v8 — fence 表加 collapseDir（收起方向）
//   v9 — 新增 search_fav 表（搜索窗口右上角的「常用」快捷项，最多 10 条）
const int kSchemaVersion = 9;

void SetError(const char* what) {
    g_lastError = what ? what : "";
    if (g_db) {
        g_lastError += ": ";
        g_lastError += sqlite3_errmsg(g_db);
    }
}

// 只用于迁移：列已经存在时 ALTER 会报错，这里直接忽略（幂等）
void AddColumn(const char* table, const char* col, const char* decl) {
    if (!g_db) return;
    const std::string sql =
        std::string("ALTER TABLE ") + table + " ADD COLUMN " + col + " " + decl;
    sqlite3_exec(g_db, sql.c_str(), nullptr, nullptr, nullptr);
}

bool Upgrade(int from) {
    // 全新的库：建表（带最新列）
    const char* kSchema =
        "CREATE TABLE IF NOT EXISTS app ("
        "  k TEXT PRIMARY KEY,"
        "  v TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS fence ("
        "  id INTEGER PRIMARY KEY,"     // 0,1,2… 对应 fences 下标
        "  x INTEGER NOT NULL, y INTEGER NOT NULL,"
        "  w INTEGER NOT NULL, h INTEGER NOT NULL,"
        "  color INTEGER NOT NULL, alpha INTEGER NOT NULL,"
        "  radius INTEGER NOT NULL, acrylic INTEGER NOT NULL,"
        "  title TEXT NOT NULL,"
        "  viewMode INTEGER NOT NULL DEFAULT 1,"
        "  sortKey  INTEGER NOT NULL DEFAULT 4,"
        "  sortAsc  INTEGER NOT NULL DEFAULT 1,"
        "  locked   INTEGER NOT NULL DEFAULT 0,"
        "  activeGroup INTEGER NOT NULL DEFAULT 0,"
        // 展开方式：0 = 伸缩（悬停展开 / 移出收起），1 = 始终展开。
        // 默认写 -1 = 还没定过，启动时由 ConfigEnsureDefaults 按全局默认补齐
        // （和 v5 迁移给老库补的那一列保持同一套语义，免得两条路走出两种结果）。
        "  expandMode  INTEGER NOT NULL DEFAULT -1,"
        // 盒子类型：0 = 收纳盒（装图标），1 = 待办盒（To-do 清单）。
        // 老库由 Upgrade 补这一列，默认 0 = 还是收纳盒。
        "  kind INTEGER NOT NULL DEFAULT 0,"
        // 收起方向：0=上 1=下 2=左 3=右。收起后只剩 4px 的把手贴在对应的边上
        // （见 FenceWindow::collapseDir）。默认 0 = 上，和老版本行为一致。
        "  collapseDir INTEGER NOT NULL DEFAULT 0"
        ");"
        // 待办盒的条目。ord 是显示顺序（用户可能上下挪），done = 勾没勾掉。
        // 和 fence_group 一样按 (fence, ord) 存，读出来按 ord 顺序塞回配置。
        "CREATE TABLE IF NOT EXISTS todo ("
        "  fence INTEGER NOT NULL,"
        "  ord   INTEGER NOT NULL,"
        "  text  TEXT NOT NULL DEFAULT '',"
        "  done  INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(fence, ord)"
        ");"
        // 盒子内的分组（头部那一排标签）。ord=0 就是盒子自己的标题，
        // 所以一个盒子只要有分组，第一条一定是它。
        "CREATE TABLE IF NOT EXISTS fence_group ("
        "  fence INTEGER NOT NULL,"
        "  ord   INTEGER NOT NULL,"
        "  name  TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY(fence, ord)"
        ");"
        "CREATE TABLE IF NOT EXISTS rule ("
        "  id INTEGER PRIMARY KEY,"     // 0,1,2… 对应 rules 下标（顺序就是匹配优先级）
        "  name TEXT NOT NULL DEFAULT '',"
        "  exts TEXT NOT NULL DEFAULT '',"
        "  matchFolders INTEGER NOT NULL DEFAULT 0,"
        "  isFallback   INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS icon_layout ("
        "  path  TEXT PRIMARY KEY,"     // 解析名，唯一且稳定
        "  owner INTEGER NOT NULL,"     // -1 = 桌面，>=0 = 盒子下标
        "  ord   INTEGER NOT NULL,"     // 盒内顺序（order 是 SQL 关键字，改名 ord）
        "  x INTEGER NOT NULL, y INTEGER NOT NULL,"
        "  grp INTEGER NOT NULL DEFAULT 0"   // 盒内分组（标签下标）
        ");"
        // 搜索窗口右上角的「常用」快捷项。
        // 同样用解析名当主键：显示名会重名，path 唯一且稳定；ord 是显示顺序
        // （满 10 个就不再收，UI 上也只画 10 格；ord 越大越靠左）。
        "CREATE TABLE IF NOT EXISTS search_fav ("
        "  path TEXT PRIMARY KEY,"
        "  ord  INTEGER NOT NULL"
        ");";
    if (sqlite3_exec(g_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        SetError("create tables");
        return false;
    }

    if (from < 2) {
        AddColumn("fence", "viewMode", "INTEGER NOT NULL DEFAULT 1");
        AddColumn("fence", "sortKey", "INTEGER NOT NULL DEFAULT 4");
        AddColumn("fence", "sortAsc", "INTEGER NOT NULL DEFAULT 1");
    }
    if (from < 3) {
        AddColumn("fence", "locked", "INTEGER NOT NULL DEFAULT 0");
        // rule 表在上面那段 kSchema 里已经 CREATE IF NOT EXISTS 过了，
        // 老库走这里也一样会建出来（kSchema 每次都执行），不会丢数据。
    }
    if (from < 4) {
        AddColumn("fence", "activeGroup", "INTEGER NOT NULL DEFAULT 0");
        AddColumn("icon_layout", "grp", "INTEGER NOT NULL DEFAULT 0");
        // fence_group 同样由 kSchema 建好
    }
    if (from < 5) {
        // 老库补这一列时写 -1（= 没填过），启动时 ConfigEnsureDefaults 会按
        // 全局默认（老配置里的 autoExpandOnHover）决定每个盒子用哪种方式，
        // 不能直接给 1 —— 那样原来开着「悬停展开」的用户会突然发现它失效了。
        AddColumn("fence", "expandMode", "INTEGER NOT NULL DEFAULT -1");
    }
    if (from < 6) {
        // v6：新建的盒子默认不再自己收起来。
        // 「新建盒子默认伸缩模式」这个全局开关以前是开着的，于是新盒子一出生就是
        // 伸缩态、鼠标一移开就缩成一条 —— 这不是用户要的。这里把全局默认落成 0
        // （= 始终展开），开关本身留着，想要新盒子伸缩再自己打开。
        // 已经存在的盒子各自都存了 expandMode，不受这次迁移影响；
        // 盒子头部右边的箭头也照样能单独切某一个盒子。
        sqlite3_exec(g_db, "INSERT OR REPLACE INTO app(k,v) VALUES('defaultTelescope','0');",
                     nullptr, nullptr, nullptr);
    }
    if (from < 7) {
        // v7：待办盒。老库里的盒子补上 kind=0（还是收纳盒），todo 表由上面的
        // kSchema 建好（CREATE IF NOT EXISTS，每次都会执行）。
        AddColumn("fence", "kind", "INTEGER NOT NULL DEFAULT 0");
    }
    if (from < 8) {
        // v8：收起方向。老库默认 0 = 向上收（把手留在上边），
        // 和以前只会往上收的位置完全一致，用户不会看到盒子忽然换了个地方。
        AddColumn("fence", "collapseDir", "INTEGER NOT NULL DEFAULT 0");
    }

    const std::string ver = "PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";";
    if (sqlite3_exec(g_db, ver.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        SetError("set user_version");
        return false;
    }
    return true;
}

}  // namespace

// 程序自己所在的目录（便携模式用）
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) return std::wstring();
    std::wstring p(buf, n);
    const size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

// 数据目录的优先级：
//   1) 环境变量 MEOW_DATA_DIR —— 调试 / 自动化测试用，能指到临时目录
//   2) exe 同目录存在 meow-desk.portable —— 便携模式（setup 里选「与程序同目录」时写的）
//   3) 默认 %APPDATA%\meow-desk
// 安装程序只负责创建/删除那个标记文件，具体用哪个目录由这里说了算，
// 这样即使 marker 被手工删掉也能干净地回到 %APPDATA% 行为。
const std::wstring& Dir() {
    if (!g_dir.empty()) return g_dir;

    std::wstring dir;
    wchar_t env[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"MEOW_DATA_DIR", env, MAX_PATH * 2) > 0) {
        dir = env;
    } else {
        const std::wstring exe = ExeDir();
        const bool portable =
            !exe.empty() &&
            GetFileAttributesW((exe + L"\\meow-desk.portable").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (portable) {
            dir = exe;
        } else {
            wchar_t* appdata = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) &&
                appdata) {
                dir = appdata;
                CoTaskMemFree(appdata);
            }
            if (dir.empty()) dir = L".";
            dir += L"\\meow-desk";
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    g_dir = dir;
    return g_dir;
}

const std::wstring& File() {
    if (g_file.empty()) g_file = Dir() + L"\\meow-desk.db";
    return g_file;
}

const std::wstring& LogFile() {
    if (g_logfile.empty()) g_logfile = Dir() + L"\\meow-desk.log";
    return g_logfile;
}

bool Open() {
    if (g_db) return true;
    if (g_tried) return false;   // 已经失败过，不要每次调用都重试
    g_tried = true;

    // sqlite3_open16 直接吃 UTF-16，省掉路径转 UTF-8（用户名可能是中文）
    const int rc = sqlite3_open16(File().c_str(), &g_db);
    if (rc != SQLITE_OK) {
        SetError("open");
        if (g_db) {
            sqlite3_close(g_db);
            g_db = nullptr;
        }
        return false;
    }

    // 写频率很低（拖拽松手、改设置），所以：
    //   DELETE —— 默认回滚日志模式，事务提交后把 -journal 删掉，目录里不留杂物
    //   FULL   —— 配置类数据宁可慢几毫秒也不能丢
    //   cache 上限 —— 防意外，实际只用几页
    sqlite3_exec(g_db, "PRAGMA journal_mode=DELETE;"
                       "PRAGMA synchronous=FULL;"
                       "PRAGMA cache_size=-512;"
                       "PRAGMA temp_store=MEMORY;",
                 nullptr, nullptr, nullptr);

    int version = 0;
    Stmt st("PRAGMA user_version");
    if (st.ok() && st.step()) version = st.colInt(0);
    if (version < kSchemaVersion) {
        if (!Upgrade(version)) {
            sqlite3_close(g_db);
            g_db = nullptr;
            return false;
        }
    }
    return true;
}

void Close() {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
    g_tried = false;
}

sqlite3* db() { return g_db; }

bool Exec(const char* sql) {
    if (!g_db) return false;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        SetError(sql);
        return false;
    }
    return true;
}

bool Begin() { return Exec("BEGIN IMMEDIATE"); }
bool Commit() { return Exec("COMMIT"); }
void Rollback() {
    if (g_db) sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
}

std::string LastError() { return g_lastError; }

// ---------------------------------------------------------------------------
// Stmt
// ---------------------------------------------------------------------------
Stmt::Stmt(const char* sql) {
    if (!g_db) return;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st_, nullptr) != SQLITE_OK) {
        SetError(sql);
        st_ = nullptr;
    }
}

Stmt::~Stmt() {
    if (st_) sqlite3_finalize(st_);
}

void Stmt::bindInt(int i, int v) {
    if (st_) sqlite3_bind_int(st_, i, v);
}

void Stmt::bindInt64(int i, long long v) {
    if (st_) sqlite3_bind_int64(st_, i, (sqlite3_int64)v);
}

void Stmt::bindText(int i, const std::wstring& v) {
    // text16：直接把 UTF-16 交给 SQLite，不做 UTF-8 来回转换；
    // SQLITE_TRANSIENT 让 SQLite 自己复制一份，调用方字符串可以是临时的
    if (st_)
        sqlite3_bind_text16(st_, i, v.c_str(), (int)(v.size() * sizeof(wchar_t)),
                            SQLITE_TRANSIENT);
}

void Stmt::reset() {
    if (!st_) return;
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}

bool Stmt::step() {
    if (!st_) return false;
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) SetError("step");
    return false;
}

bool Stmt::done() {
    if (!st_) return false;
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_DONE) return true;
    if (rc != SQLITE_ROW) SetError("step");
    return false;
}

int Stmt::colInt(int i) const { return st_ ? sqlite3_column_int(st_, i) : 0; }
long long Stmt::colInt64(int i) const { return st_ ? sqlite3_column_int64(st_, i) : 0; }

std::wstring Stmt::colText(int i) const {
    if (!st_) return std::wstring();
    const void* p = sqlite3_column_text16(st_, i);
    if (!p) return std::wstring();
    const int bytes = sqlite3_column_bytes16(st_, i);
    return std::wstring(reinterpret_cast<const wchar_t*>(p), (size_t)bytes / sizeof(wchar_t));
}

// ---------------------------------------------------------------------------
// 备份
// ---------------------------------------------------------------------------
namespace {

// 保留多少份。14 = 两周的每日快照；每份几十~几百 KB，总共也就几 MB。
const int kKeepBackups = 14;

// 备份目录里所有 meow-desk-*.db，**升序**（文件名里带 yyyymmdd-hhmmss，
// 所以字典序就是时间序，不用去翻文件时间）。
std::vector<std::wstring> ListBackups() {
    std::vector<std::wstring> out;
    const std::wstring pat = BackupDir() + L"\\meow-desk-*.db";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

const std::wstring& BackupDir() {
    static std::wstring dir;
    if (dir.empty()) {
        dir = Dir() + L"\\backup";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring BackupNow() {
    if (!Open()) return std::wstring();

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t base[48]{};
    swprintf_s(base, L"meow-desk-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    // 同一秒里连点两次「立即备份」→ 加后缀，绝不覆盖已有备份
    std::wstring path = BackupDir() + L"\\" + base + L".db";
    for (int n = 2; n < 100 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        wchar_t nm[64]{};
        swprintf_s(nm, L"%s-%d.db", base, n);
        path = BackupDir() + L"\\" + nm;
    }

    // sqlite3_open16 直接吃 UTF-16（用户名可能是中文，走 UTF-8 反而多一层错的余地）；
    // 目标文件必须是新的，open16 会建出来。
    sqlite3* dst = nullptr;
    if (sqlite3_open16(path.c_str(), &dst) != SQLITE_OK) {
        if (dst) sqlite3_close(dst);
        SetError("backup open");
        return std::wstring();
    }

    bool ok = false;
    if (sqlite3_backup* bk = sqlite3_backup_init(dst, "main", g_db, "main")) {
        const int rc = sqlite3_backup_step(bk, -1);   // -1 = 一次搬完
        sqlite3_backup_finish(bk);
        ok = (rc == SQLITE_DONE);
    }
    sqlite3_close(dst);

    if (!ok) {
        DeleteFileW(path.c_str());   // 半截文件留着只会误导人
        SetError("backup step");
        return std::wstring();
    }
    return path;
}

void BackupDaily() {
    if (!Open()) return;

    // 今天已经备过就不再备（同一天反复启停不该刷出一堆文件）
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"meow-desk-%04d%02d%02d-", st.wYear, st.wMonth, st.wDay);
    const std::wstring pre = prefix;

    bool today = false;
    for (const auto& f : ListBackups()) {
        if (f.rfind(pre, 0) == 0) {
            today = true;
            break;
        }
    }
    if (!today) BackupNow();

    // 只留最近 kKeepBackups 份（ListBackups 是升序，砍掉最前面那批）
    const auto all = ListBackups();
    if ((int)all.size() > kKeepBackups) {
        for (int i = 0; i < (int)all.size() - kKeepBackups; ++i)
            DeleteFileW((BackupDir() + L"\\" + all[i]).c_str());
    }
}

}  // namespace storage
