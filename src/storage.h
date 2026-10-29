// 本地存储：SQLite（%APPDATA%\meow-desk\meow-desk.db）
//
// 为什么换掉 *.ini：
//   INI 每次都是「整份重写」，写到一半断电/崩溃就得到一个残缺文件，配置全丢；
//   而且每加一类数据都要重新设计一种文本格式和一套解析。SQLite 用事务保证原子性，
//   加字段只要 PRAGMA user_version 升一级 + ALTER TABLE。
//
// 内存代价实测为零：SQLite 的代码页在 exe 映像里（file-backed，可换出），
// 不占 private；210 行数据的 page cache 只有几页，close 后完全释放。
// 所以定位很明确 —— **它只是一个磁盘格式，不参与运行时**：
//   · 启动时一次性读进内存模型（AppConfig / vector<LayoutEntry>）
//   · 运行期只读内存，变更时才走一次事务写入
//   · 绝不每帧查询、不常驻大 cache
//
// schema 版本走 PRAGMA user_version，以后加字段在这里升。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace storage {

// 数据库文件路径（同时保证 %APPDATA%\meow-desk 目录存在）
const std::wstring& Dir();
const std::wstring& File();

// 日志文件（Dir() 下的 meow-desk.log）。
//
// 必须是**绝对路径**：以前是 fopen("meow-desk.log")，跟着「当前目录」走。
// 手动双击时当前目录是 exe 所在目录，看着没问题；**开机自启那一次**当前目录是
// C:\Windows\System32（Explorer 处理 Run 键时 lpCurrentDirectory 给的是 NULL），
// 普通用户又没有写权限 → fopen 直接失败（我们吞掉了错误）→「开机启动要等半分钟」
// 这种问题永远查不到现场。改成绝对路径之后，开机那一次也会老老实实落在数据目录里。
const std::wstring& LogFile();

// 打开（首次会建表 + 从旧的 *.ini 导入）。多次调用只打开一次。
// 返回 false 表示数据库不可用，调用方应回退到默认值。
bool Open();
void Close();
sqlite3* db();

// ---- 内部工具（config.cpp / layout_store.cpp 用）----

// 执行一段不需要结果的 SQL（建表、PRAGMA 等）
bool Exec(const char* sql);

// 等价于「BEGIN IMMEDIATE ... COMMIT」，异常路径会 ROLLBACK
bool Begin();
bool Commit();
void Rollback();

// 预编译语句的小包装，析构自动 finalize
class Stmt {
public:
    explicit Stmt(const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return st_ != nullptr; }
    sqlite3_stmt* raw() const { return st_; }

    void bindInt(int i, int v);
    void bindText(int i, const std::wstring& v);
    void bindInt64(int i, long long v);

    void reset();       // 复位以便重复 bind/step（批量插入时用）

    bool step();        // 返回 true = SQLITE_ROW，false = 完成或出错
    bool done();        // 返回 true = 正常执行完（SQLITE_DONE）

    int colInt(int i) const;
    long long colInt64(int i) const;
    std::wstring colText(int i) const;

private:
    sqlite3_stmt* st_ = nullptr;
};

// 最近一次错误的可读描述（写日志用）
std::string LastError();

// ---- 备份 ----
//
// 数据库里装着用户全部的「心血」：盒子位置、分组、待办、图标布局、搜索常用。
// 文件一旦损坏/被误删，等于所有整理工作归零，所以这里做一层兜底：
// 每次启动（同一天只做一次）把整份快照到 <数据目录>\backup\，只留最近 N 份。
//
// 用 sqlite3_backup API 而不是复制文件：库可能正开着（还有 -journal），
// 直接拷文件会拿到半个事务。backup API 保证是一致性快照。
const std::wstring& BackupDir();     // 顺带确保目录存在
std::wstring BackupNow();            // 立刻备份，返回完整路径（失败返回空串）
void BackupDaily();                  // 启动时调一次：今天没备过就补一份 + 清理旧备份

}  // namespace storage
