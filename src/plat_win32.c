/*
 * Windows 本地调试用的兼容实现（不参与 Linux 构建）。
 *
 * pread/pwrite 在 Windows 上没有直接对应物：这里用
 * “保存当前文件偏移 -> lseek 到目标位置 -> 读写 -> 恢复偏移”
 * 模拟带偏移的 I/O，并用一把全局锁保证多个线程不会互相践踏文件偏移。
 * 该锁只在这个调试兼容层里存在，Linux 上的 pread/pwrite 天然无此问题。
 */
#if defined(_WIN32)

#include <io.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <pthread.h>

#include "util.h"

static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t tl_win_pread(int fd, void *buf, size_t n, int64_t off)
{
    int64_t old;
    int r, saved;

    pthread_mutex_lock(&g_io_lock);
    old = _lseeki64(fd, 0, SEEK_CUR);
    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0) {
        pthread_mutex_unlock(&g_io_lock);
        return -1;
    }
    r = _read(fd, buf, (unsigned)(n > 0x7FFFFFFFu ? 0x7FFFFFFFu : n));
    saved = errno;
    _lseeki64(fd, old, SEEK_SET);
    pthread_mutex_unlock(&g_io_lock);
    errno = saved;
    return (ssize_t)r;
}

ssize_t tl_win_pwrite(int fd, const void *buf, size_t n, int64_t off)
{
    int64_t old;
    int r, saved;

    pthread_mutex_lock(&g_io_lock);
    old = _lseeki64(fd, 0, SEEK_CUR);
    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0) {
        pthread_mutex_unlock(&g_io_lock);
        return -1;
    }
    r = _write(fd, buf, (unsigned)(n > 0x7FFFFFFFu ? 0x7FFFFFFFu : n));
    saved = errno;
    _lseeki64(fd, old, SEEK_SET);
    pthread_mutex_unlock(&g_io_lock);
    errno = saved;
    return (ssize_t)r;
}

const char *tl_hostname(void)
{
    static char buf[256];
    DWORD len = (DWORD)sizeof(buf);

    if (buf[0] == '\0' && !GetComputerNameA(buf, &len))
        snprintf(buf, sizeof(buf), "unknown");
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

#endif /* _WIN32 */
