// 后台缩略图队列
//
// 为什么要单独一条队列：图片 / 视频 / 音乐（带内嵌封面）这些文件在桌面上应该
// 显示**缩略图**，而不是一个「这类文件」的通用图标 —— 资源管理器就是这么做的。
// 但取缩略图要解码整张图、给视频抽一帧、读音频标签，几十到几百毫秒起步，
// 一个桌面几十个文件排队就是几秒。
//
// 所以分成两步：
//   1. UI 线程先放**类型图标**（LoadShellIcon，毫秒级，界面立刻可用）；
//      同时 RequestThumbnail 排一单；
//   2. 工作线程取缩略图，好了 PostThreadMessage 把 UI 线程叫醒，
//      UI 线程 TakeThumbnail 取回来，把位图换掉、只重画那一格。
//
// 取不到（普通 exe、没有封面的 mp3、文件夹）就什么都不换，类型图标继续用。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "shell_icons.h"

struct ThumbResult {
    std::wstring path;
    int px = 0;
    IconBitmap bmp;   // !valid() = 这个文件没有缩略图
};

// UI 线程调用一次：告诉队列「结果回来叫醒哪个线程的哪条消息」。
void InitThumbnailLoader(DWORD uiThreadId, UINT wakeMsg);

// 排一单（同一个 path+px 只会有一份在途）。px 变了会再排一单新的。
void RequestThumbnail(std::wstring path, int px, bool linkOverlay);

// UI 线程：取一个已完成的结果（没有就返回 false）。
bool TakeThumbnail(ThumbResult& out);
