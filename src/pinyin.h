// 汉字 → 拼音（全拼 + 首字母）
//
// 数据来源：mozillazg/pinyin-data（MIT），由 _tmp/gen_pinyin.py 生成
// src/pinyin_table.inc。查表是 O(1) 直接下标（U+4E00..U+9FA5 连续区间），
// 多音字最多保留 2 个读音（第二读音走稀疏表 + 二分）。
//
// 匹配策略见 PinyinMatchPos：既支持全拼（wangyi），也支持首字母（wyy），
// 还支持两者混着打（wyuny），按音节贪心 + 记忆化搜索。
#pragma once

#include <string>
#include <vector>

// 一个汉字的读音（0 个 / 1 个 / 2 个）
const char* PinyinSyllableOf(wchar_t ch, int which);

// 名字 → 音节序列。汉字拆成读音，ASCII 逐字符（小写），其它字符跳过。
// 每个元素是「这个位置上可能的所有读音」，用于多音字。
std::vector<std::vector<std::string>> NameSyllables(const std::wstring& name);

// 拼音匹配：query 必须是小写 ASCII。
// 返回匹配起点下标（0 = 从头匹配），-1 表示不匹配。
int PinyinMatchPos(const std::vector<std::vector<std::string>>& syls,
                   const std::wstring& query);

// 便捷版：直接把名字和查询词丢进来
int PinyinMatchPos(const std::wstring& name, const std::wstring& query);
