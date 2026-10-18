#include "pinyin.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include "pinyin_table.inc"

namespace {

struct SylSet {
    const char* s[2] = {nullptr, nullptr};
    int n = 0;
};

SylSet Lookup(wchar_t ch) {
    SylSet out;
    if (ch < PY_CP_LO || ch > PY_CP_HI) return out;
    const unsigned short idx = kPyFirst[ch - PY_CP_LO];
    // 注意：不能用 0 判断「无读音」，0 是音节池的合法下标（yi）。哨兵是 PY_SYL_NONE。
    if (idx >= PY_SYL_COUNT) return out;
    out.s[0] = kPySyl + kPySylOff[idx];
    out.n = 1;

    // 第二读音：稀疏表二分
    int lo = 0, hi = (int)(sizeof(kPyAltCp) / sizeof(kPyAltCp[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const unsigned short cp = kPyAltCp[mid];
        if (cp == (unsigned short)ch) {
            const unsigned short a = kPyAltIdx[mid];
            if (a < PY_SYL_COUNT) {
                out.s[1] = kPySyl + kPySylOff[a];
                out.n = 2;
            }
            break;
        }
        if (cp < (unsigned short)ch) lo = mid + 1;
        else hi = mid - 1;
    }
    return out;
}

}  // namespace

const char* PinyinSyllableOf(wchar_t ch, int which) {
    const SylSet s = Lookup(ch);
    if (which < 0 || which >= s.n) return nullptr;
    return s.s[which];
}

std::vector<std::vector<std::string>> NameSyllables(const std::wstring& name) {
    std::vector<std::vector<std::string>> out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        std::vector<std::string> alts;
        if (c < 0x80) {
            if (iswalnum((wint_t)c))
                alts.emplace_back(1, (char)towlower((wint_t)c));
        } else {
            const SylSet s = Lookup(c);
            for (int i = 0; i < s.n; ++i) alts.emplace_back(s.s[i]);
        }
        out.push_back(std::move(alts));
    }
    return out;
}

namespace {

struct Matcher {
    const std::vector<std::vector<std::string>>& syls;
    const std::wstring& q;
    // memo: (si, qi) 三态：0 未知 / 1 可以 / 2 不行
    std::vector<unsigned char> memo;
    int qn;

    bool Run(int si, int qi) {
        if (qi == qn) return true;
        if (si >= (int)syls.size()) return false;
        const size_t key = (size_t)si * (size_t)(qn + 1) + (size_t)qi;
        if (memo[key]) return memo[key] == 1;
        bool ok = false;
        for (const auto& s : syls[si]) {
            if (s.empty()) continue;
            // 整段吃掉（全拼）
            const int remain = (int)s.size();
            if (qi + remain <= qn) {
                bool eq = true;
                for (int k = 0; k < remain; ++k) {
                    if (q[qi + k] != (wchar_t)s[k]) { eq = false; break; }
                }
                if (eq && Run(si + 1, qi + remain)) { ok = true; break; }
            }
            // 只吃首字母（首字母缩写）
            if (q[qi] == (wchar_t)s[0] && Run(si + 1, qi + 1)) { ok = true; break; }
        }
        memo[key] = ok ? 1 : 2;
        return ok;
    }
};

// query 里允许小写字母和数字：名字里常有 "winui3"、"360"、"7z" 这类，数字要能参与匹配
bool IsQueryChar(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); }

}  // namespace

int PinyinMatchPos(const std::vector<std::vector<std::string>>& syls,
                   const std::wstring& query) {
    if (query.empty()) return 0;
    for (wchar_t c : query)
        if (!IsQueryChar(c)) return -1;
    const int qn = (int)query.size();
    const int sn = (int)syls.size();
    if (sn == 0) return -1;
    // 每个起点单独一套 memo（si ≤ sn 保证不越界）
    std::vector<unsigned char> memo((size_t)(sn + 1) * (size_t)(qn + 1), 0);
    for (int start = 0; start < sn; ++start) {
        std::fill(memo.begin(), memo.end(), 0);
        Matcher m{syls, query, memo, qn};
        if (m.Run(start, 0)) return start;
    }
    return -1;
}

int PinyinMatchPos(const std::wstring& name, const std::wstring& query) {
    return PinyinMatchPos(NameSyllables(name), query);
}
