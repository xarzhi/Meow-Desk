#include "file_drop.h"

#include <ole2.h>
#include <shellapi.h>

#include <unordered_map>

namespace {

// 一个窗口一个。只认 CF_HDROP —— 文本/图片流这种格式一律拒绝，
// 免得把窗口自己的拖拽行为（拖图标、拖框选）搅进去。
class DropTarget : public IDropTarget {
public:
    explicit DropTarget(HWND h) : hwnd_(h) {}

    std::function<DWORD(const std::vector<std::wstring>&, POINT)> onDrop;

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    // ---- IDropTarget ----
    STDMETHODIMP DragEnter(IDataObject* obj, DWORD, POINTL, DWORD* effect) override {
        canDrop_ = HasHdrop(obj);
        if (effect) *effect = canDrop_ ? Allowed(*effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = canDrop_ ? Allowed(*effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        canDrop_ = false;
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* obj, DWORD, POINTL pt, DWORD* effect) override {
        std::vector<std::wstring> paths = ReadHdrop(obj);
        canDrop_ = false;
        if (paths.empty()) {
            if (effect) *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        if (onDrop) {
            // 落点自己决定「实际做了什么」，原样回报给拖放源。
            // 报错后果很严重：明明只复制了却报 MOVE，源程序会把原文件删掉。
            const DWORD done = onDrop(paths, POINT{pt.x, pt.y});
            if (effect) *effect = (done == DROPEFFECT_MOVE || done == DROPEFFECT_COPY)
                                     ? done
                                     : DROPEFFECT_NONE;
        } else if (effect) {
            *effect = DROPEFFECT_NONE;
        }
        return S_OK;
    }

private:
    // 源允许什么我们就报什么；一个都不允许就报「复制或移动」，让光标至少能正常显示
    static DWORD Allowed(DWORD in) {
        const DWORD ok = in & (DROPEFFECT_COPY | DROPEFFECT_MOVE);
        return ok ? ok : (DROPEFFECT_COPY | DROPEFFECT_MOVE);
    }

    static FORMATETC HdropFormat() {
        FORMATETC f{};
        f.cfFormat = (CLIPFORMAT)CF_HDROP;
        f.ptd = nullptr;
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        return f;
    }

    static bool HasHdrop(IDataObject* obj) {
        if (!obj) return false;
        FORMATETC f = HdropFormat();
        return obj->QueryGetData(&f) == S_OK;
    }

    static std::vector<std::wstring> ReadHdrop(IDataObject* obj) {
        std::vector<std::wstring> out;
        if (!obj) return out;

        FORMATETC f = HdropFormat();
        STGMEDIUM sm{};
        if (FAILED(obj->GetData(&f, &sm))) return out;

        if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal) {
            auto* h = (HDROP)GlobalLock(sm.hGlobal);
            if (h) {
                const UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    const UINT len = DragQueryFileW(h, i, nullptr, 0);
                    if (!len) continue;
                    std::wstring s(len, L'\0');
                    const UINT got = DragQueryFileW(h, i, s.data(), len + 1);
                    if (!got) continue;
                    s.resize(got);
                    out.push_back(std::move(s));
                }
                GlobalUnlock(sm.hGlobal);
            }
        }
        ReleaseStgMedium(&sm);
        return out;
    }

    LONG ref_ = 1;   // 表里持有一份引用
    HWND hwnd_ = nullptr;
    bool canDrop_ = false;
};

// hwnd → target。窗口销毁时必须 Detach，否则 OLE 还挂着死窗口。
std::unordered_map<HWND, DropTarget*> g_targets;

}  // namespace

void FileDrop::Init() {
    // WinMain 里已经 CoInitializeEx(STA) 过，这里补齐 OLE 那部分。
    // 返回值不用较真：S_FALSE = 已初始化，RPC_E_CHANGED_MODE = 线程被人占了 MTA，
    // 这两种情况下 RegisterDragDrop 照样能用（拖放不依赖 OLE 的其它子系统）。
    OleInitialize(nullptr);
}

void FileDrop::Attach(HWND hwnd,
                      std::function<DWORD(const std::vector<std::wstring>&, POINT)> onDrop) {
    if (!hwnd) return;
    Detach(hwnd);

    auto* t = new DropTarget(hwnd);
    t->onDrop = std::move(onDrop);
    if (FAILED(RegisterDragDrop(hwnd, t))) {
        t->Release();
        return;
    }
    g_targets[hwnd] = t;
}

void FileDrop::Detach(HWND hwnd) {
    auto it = g_targets.find(hwnd);
    if (it == g_targets.end()) return;
    RevokeDragDrop(hwnd);
    it->second->Release();
    g_targets.erase(it);
}
