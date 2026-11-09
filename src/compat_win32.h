/*
 * 仅用于在 Windows 上本地调试的兼容层。
 *
 * 作业最终运行环境是 Linux，正式代码只会走 pread/pwrite/fsync/ftruncate
 * 这条 POSIX 路径；这里的宏只是为了在 MinGW 下也能编译、跑测试。
 * 提交到 Linux 时该文件不参与编译（Makefile 按平台决定）。
 */
#ifndef TL_COMPAT_WIN32_H
#define TL_COMPAT_WIN32_H

#if defined(_WIN32)

#include <io.h>
#include <stdio.h>
#include <sys/types.h>

/* MinGW 提供 _commit/_chsize_s/_fileno，语义与 POSIX 对应函数一致 */
#ifndef fsync
#define fsync _commit
#endif
#ifndef fileno
#define fileno _fileno
#endif

ssize_t tl_win_pread(int fd, void *buf, size_t n, int64_t off);
ssize_t tl_win_pwrite(int fd, const void *buf, size_t n, int64_t off);

#define pread  tl_win_pread
#define pwrite tl_win_pwrite

#endif /* _WIN32 */
#endif /* TL_COMPAT_WIN32_H */
