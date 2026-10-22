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

    std::function<DWORD(const FileDrop::Paths&, POINT)> onDrop;
    std::function<DWORD(const FileDrop::Paths&, POINT, DWORD)> onHover;
    std::function<void()> onLeave;

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
    STDMETHODIMP DragEnter(IDataObject* obj, DWORD, POINTL pt, DWORD* effect) override {
        // 路径只在进入时读一次：DragOver 是每次鼠标移动都来，里面还去
        // GetData + DragQueryFile 的话，拖拽期间会一直卡在数据提取上。
        paths_ = ReadHdrop(obj);
        canDrop_ = !paths_.empty();
        if (effect) *effect = Hover(pt, *effect);
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override {
        if (effect) *effect = Hover(pt, *effect);
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        canDrop_ = false;
        paths_.clear();
        if (onLeave) onLeave();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* obj, DWORD, POINTL pt, DWORD* effect) override {
        FileDrop::Paths paths = ReadHdrop(obj);
        if (paths.empty()) paths = paths_;
        canDrop_ = false;
        paths_.clear();
        DWORD done = DROPEFFECT_NONE;
        if (!paths.empty() && onDrop) done = onDrop(paths, POINT{pt.x, pt.y});
        // 高亮/落点提示一定要在动作做完之后收掉，中途收掉的话
        // 「文件正在搬」那一瞬间会闪一下空白桌面。
        if (onLeave) onLeave();
        if (effect) *effect = (done == DROPEFFECT_MOVE || done == DROPEFFECT_COPY)
                                 ? done
                                 : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    // 问调用方「这儿收不收」，并把结果夹到拖放源允许的范围里。
    // 夹这一步不能省：源只允许 COPY 时我们回 MOVE，Explorer 会不认这次放下。
    DWORD Hover(POINTL pt, DWORD in) {
        if (!canDrop_) return DROPEFFECT_NONE;
        const DWORD allowed = Allowed(in);
        if (!onHover) return allowed;
        const DWORD want = onHover(paths_, POINT{pt.x, pt.y}, allowed);
        if (want == DROPEFFECT_NONE) return DROPEFFECT_NONE;
        const DWORD both = want & allowed;
        return both ? both : allowed;
    }

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

    static FileDrop::Paths ReadHdrop(IDataObject* obj) {
        FileDrop::Paths out;
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
    FileDrop::Paths paths_;   // DragEnter 时读一次，DragOver 复用
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

void FileDrop::Attach(HWND hwnd, std::function<DWORD(const Paths&, POINT)> onDrop,
                      std::function<DWORD(const Paths&, POINT, DWORD)> onHover,
                      std::function<void()> onLeave) {
    if (!hwnd) return;
    Detach(hwnd);

    auto* t = new DropTarget(hwnd);
    t->onDrop = std::move(onDrop);
    t->onHover = std::move(onHover);
    t->onLeave = std::move(onLeave);
    if (FAILED(RegisterDragDrop(hwnd, t))) {
        // 注册失败就是「这个窗口彻底不收拖放」，症状极易被误认成「拖过去没反应」，
        // 所以一定要留痕。
        OutputDebugStringW(L"[meow-desk] RegisterDragDrop FAILED\n");
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
