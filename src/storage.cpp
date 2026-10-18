#include "storage.h"

#include <shlobj.h>

#include <string>

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

// schema 版本：以后加字段就 +1，并在 Upgrade() 里补一段迁移
//   v1 — 初版：app / fence / icon_layout
//   v2 — fence 表加 viewMode / sortKey / sortAsc（每个盒子各自的「查看」「排序」）
const int kSchemaVersion = 2;

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
        "  sortAsc  INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS icon_layout ("
        "  path  TEXT PRIMARY KEY,"     // 解析名，唯一且稳定
        "  owner INTEGER NOT NULL,"     // -1 = 桌面，>=0 = 盒子下标
        "  ord   INTEGER NOT NULL,"     // 盒内顺序（order 是 SQL 关键字，改名 ord）
        "  x INTEGER NOT NULL, y INTEGER NOT NULL"
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

}  // namespace storage
