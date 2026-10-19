#include "thumb_loader.h"

#include <objbase.h>   // CoInitializeEx / COINIT_APARTMENTTHREADED（WIN32_LEAN_AND_MEAN 不带给）

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "icon_render.h"   // EnsureGdiplus

namespace {

struct Request {
    std::wstring path;
    int px = 0;
    bool linkOverlay = false;
};

// 故意做成「永不析构」的单例：工作线程要活到进程退出，
// 静态析构阶段还在跑 Shell 缩略图提取会把 COM 拆掉。
struct Queue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Request> reqs;
    std::deque<ThumbResult> done;
    // 在途的「path|px」。同一个文件在同一个尺寸上只排一单 ——
    // 重载桌面 / 切查看档位会重复请求同一个文件，不去重就是白解码好几遍。
    std::unordered_set<std::wstring> inflight;
    std::thread th;
    bool started = false;
    DWORD uiTid = 0;
    UINT wakeMsg = 0;
};

Queue& Q() {
    static Queue* q = new Queue();
    return *q;
}

std::wstring Key(const std::wstring& path, int px) {
    return path + L'\x1' + std::to_wstring(px);
}

void Loop() {
    // 缩略图提取走的是 Shell 的缩略图提供程序（COM）。线程自己的公寓必须先初始化，
    // 否则 GetImage 直接 E_NOINTERFACE / 崩溃。
    // **一定要 MTA**：这条线程没有消息泵，STA 下只要提取器回调进来就会死锁
    // （看起来就是「缩略图永远不出现」）。MTA 没有这个问题。
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lk(Q().mu);
            Q().cv.wait(lk, [] { return !Q().reqs.empty(); });
            r = std::move(Q().reqs.front());
            Q().reqs.pop_front();
        }

        ThumbResult res;
        res.path = r.path;
        res.px = r.px;
        // 文件夹不取缩略图：资源管理器会做「文件夹内容预览」，那个走的是另一套
        // 慢路径（还要遍历目录），桌面上一堆文件夹时代价太高，保持文件夹图标。
        const DWORD attr = GetFileAttributesW(r.path.c_str());
        const bool isDir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
        if (!isDir) LoadShellThumbnail(r.path, r.px, res.bmp, r.linkOverlay);

        DWORD tid = 0;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lk(Q().mu);
            Q().inflight.erase(Key(r.path, r.px));
            Q().done.push_back(std::move(res));
            tid = Q().uiTid;
            msg = Q().wakeMsg;
        }
        // 叫醒 UI 线程：鼠标可能一直不动，靠消息而不是靠轮询
        if (tid && msg) PostThreadMessageW(tid, msg, 0, 0);
    }
}

}  // namespace

void InitThumbnailLoader(DWORD uiThreadId, UINT wakeMsg) {
    // GDI+ 在这条队列里也会用到（没有链接箭头的那部分不会，但有快捷方式时会），
    // 先在这里起好 —— EnsureGdiplus 不是线程安全的，绝不能等两个线程同时去起。
    EnsureGdiplus();
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.mu);
    q.uiTid = uiThreadId;
    q.wakeMsg = wakeMsg;
}

void RequestThumbnail(std::wstring path, int px, bool linkOverlay) {
    if (path.empty() || px <= 0) return;
    // 「::{CLSID}」这种不是文件系统路径（此电脑 / 回收站），没有缩略图可提
    if (path.size() >= 2 && path[0] == L':' && path[1] == L':') return;

    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.mu);
    const std::wstring key = Key(path, px);
    if (!q.inflight.insert(key).second) return;           // 已经在排了
    if (q.reqs.size() > 512) {                           // 极端情况：别把内存吃爆
        q.inflight.erase(key);
        return;
    }
    q.reqs.push_back(Request{std::move(path), px, linkOverlay});
    if (!q.started) {
        q.started = true;
        q.th = std::thread(Loop);
        q.th.detach();
    }
    q.cv.notify_one();
}

bool TakeThumbnail(ThumbResult& out) {
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.mu);
    if (q.done.empty()) return false;
    out = std::move(q.done.front());
    q.done.pop_front();
    return true;
}
