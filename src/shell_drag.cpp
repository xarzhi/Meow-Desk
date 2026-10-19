#include "shell_drag.h"

#include <objidl.h>
#include <shlobj.h>   // SHCreateStdEnumFmtEtc / DROPFILES
#include <ole2.h>

#include <cstring>
#include <new>
#include <utility>

namespace ShellDrag {
namespace {

// ---------------------------------------------------------------------------
// 把路径列表打包成 CF_HDROP 要的 HGLOBAL：
//   DROPFILES 头 + 一串以 \0 分隔的宽字符路径 + 结尾再来一个 \0（双 \0 收尾）
// ---------------------------------------------------------------------------
HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;
    size_t chars = 1;   // 结尾额外的 \0
    for (const auto& p : paths) chars += p.size() + 1;

    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;

    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    if (!df) {
        GlobalFree(h);
        return nullptr;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;   // 关键：宽字符版本，否则中文路径会乱码

    wchar_t* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& s : paths) {
        std::memcpy(p, s.c_str(), s.size() * sizeof(wchar_t));
        p += s.size();
        *p++ = L'\0';
    }
    *p = L'\0';
    GlobalUnlock(h);
    return h;
}

// CF_UNICODETEXT：有些编辑器（Typora / 记事本）只认文本，拖进去就是粘一段路径
HGLOBAL MakeUnicodeText(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;
    std::wstring s;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) s += L"\r\n";
        s += paths[i];
    }
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    void* dst = GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        return nullptr;
    }
    std::memcpy(dst, s.c_str(), bytes);
    GlobalUnlock(h);
    return h;
}

// 支持的数据格式（EnumFormatEtc 用）
FORMATETC g_Formats[2] = {
    {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
    {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
};

// ---------------------------------------------------------------------------
// IDataObject：只提供 CF_HDROP 和 CF_UNICODETEXT
// ---------------------------------------------------------------------------
class FileData final : public IDataObject {
public:
    explicit FileData(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    // ---- IDataObject ----
    STDMETHODIMP GetData(LPFORMATETC fe, LPSTGMEDIUM med) override {
        if (!fe || !med) return E_INVALIDARG;
        if (!(fe->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        // lindex 是「同一格式的第几份」，我们只有一份
        if (fe->lindex != -1) return DV_E_LINDEX;
        if (fe->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

        HGLOBAL h = nullptr;
        if (fe->cfFormat == CF_HDROP) h = MakeHDrop(paths_);
        else if (fe->cfFormat == CF_UNICODETEXT) h = MakeUnicodeText(paths_);
        else return DV_E_FORMATETC;
        if (!h) return STG_E_MEDIUMFULL;

        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = h;
        med->pUnkForRelease = nullptr;
        return S_OK;
    }

    STDMETHODIMP GetDataHere(LPFORMATETC, LPSTGMEDIUM) override { return DATA_E_FORMATETC; }

    STDMETHODIMP QueryGetData(LPFORMATETC fe) override {
        if (!fe) return E_INVALIDARG;
        if (!(fe->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (fe->lindex != -1) return DV_E_LINDEX;
        if (fe->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (fe->cfFormat == CF_HDROP || fe->cfFormat == CF_UNICODETEXT) return S_OK;
        return DV_E_FORMATETC;
    }

    STDMETHODIMP GetCanonicalFormatEtc(LPFORMATETC, LPFORMATETC out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    // 目标会用它回告「我打算/实际做了什么」（CFSTR_PREFERREDDROPEFFECT /
    // CFSTR_PERFORMEDDROPEFFECT）。我们只回 S_OK —— 不回的话有些目标会认为
    // 源不支持协商而放弃操作。我们**永不自己删源文件**：MOVE 由目标完成。
    STDMETHODIMP SetData(LPFORMATETC, LPSTGMEDIUM med, BOOL release) override {
        if (release && med) ReleaseStgMedium(med);
        return S_OK;
    }

    STDMETHODIMP EnumFormatEtc(DWORD dir, LPENUMFORMATETC* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (dir != DATADIR_GET) return E_NOTIMPL;   // 我们不接受 SetData 塞数据
        return SHCreateStdEnumFmtEtc(2, g_Formats, out);
    }

    // 数据变更通知：拖放的源不需要（返回「不支持」是标准做法）
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~FileData() = default;   // 只走 Release
    LONG ref_ = 1;
    std::vector<std::wstring> paths_;
};

// ---------------------------------------------------------------------------
// IDropSource：告诉 OLE 什么时候算「放下」、什么时候算「取消」
// ---------------------------------------------------------------------------
class FileDropSrc final : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropSource)) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        // 左键松开 = 放下。用 OLE 给的按键状态，不要自己 GetAsyncKeyState
        // （那条路上的状态和 OLE 内部不一定同步，会出现「松手了还在拖」）。
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }

    // 交给系统画光标（复制/移动/禁止），别自己画
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ~FileDropSrc() = default;
    LONG ref_ = 1;
};

}  // namespace

DWORD DragFiles(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return DROPEFFECT_NONE;

    auto* data = new (std::nothrow) FileData(paths);
    if (!data) return DROPEFFECT_NONE;
    auto* src = new (std::nothrow) FileDropSrc();
    if (!src) {
        data->Release();
        return DROPEFFECT_NONE;
    }

    // 交棒之前必须放掉鼠标捕获：OLE 的 StartDrag 要自己抓，
    // 抓不到就变成「拖半天目标一点反应都没有」。
    if (GetCapture()) ReleaseCapture();

    DWORD eff = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, src,
                                  DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    src->Release();
    data->Release();

    return SUCCEEDED(hr) ? eff : DROPEFFECT_NONE;
}

}  // namespace ShellDrag
