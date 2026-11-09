/* 系统头文件先于 util.h 包含，避免 compat_win32.h 的宏干扰系统声明。 */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#endif

#include "util.h"

ssize_t io_pread(int fd, void *buf, size_t n, int64_t off)
{
    size_t done = 0;
    char *p = (char *)buf;

    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, (off_t)(off + (int64_t)done));
        if (r < 0) {
            if (errno == EINTR)
                continue;           /* 被信号打断：重试 */
            return -1;
        }
        if (r == 0)
            break;                  /* 提前遇到文件末尾 */
        done += (size_t)r;
    }
    return (ssize_t)done;
}

ssize_t io_pwrite(int fd, const void *buf, size_t n, int64_t off)
{
    size_t done = 0;
    const char *p = (const char *)buf;

    while (done < n) {
        ssize_t r = pwrite(fd, p + done, n - done, (off_t)(off + (int64_t)done));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

int io_sync(int fd)
{
    return fsync(fd);
}

int io_truncate(int fd, int64_t len)
{
#if defined(_WIN32)
    return _chsize_s(fd, len) == 0 ? 0 : -1;
#else
    return ftruncate(fd, (off_t)len);
#endif
}

int64_t io_file_size(int fd)
{
#if defined(_WIN32)
    return (int64_t)_filelengthi64(fd);
#else
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
#endif
}

int io_rename_replace(const char *src, const char *dst)
{
#if defined(_WIN32)
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    return -1;
#else
    return rename(src, dst);
#endif
}

int io_sync_dir(const char *file_path)
{
#if defined(_WIN32)
    (void)file_path;
    return 0;
#else
    char *tmp = strdup(file_path);
    char *slash;
    int dfd, rc = 0;

    if (!tmp)
        return -1;
    slash = strrchr(tmp, '/');
    if (slash) {
        if (slash == tmp)
            slash[1] = '\0';        /* 根目录 */
        else
            *slash = '\0';
        dfd = open(tmp, O_RDONLY);
    } else {
        dfd = open(".", O_RDONLY);  /* 当前目录 */
    }
    if (dfd >= 0) {
        if (fsync(dfd) != 0)
            rc = -1;
        close(dfd);
    }
    free(tmp);
    return rc;
#endif
}

double tl_now_sec(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

uint64_t tl_hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t tl_new_uuid(void)
{
    static uint64_t counter = 0;
#if defined(_WIN32)
    uint64_t pid = (uint64_t)_getpid();
#else
    uint64_t pid = (uint64_t)getpid();
#endif
    uint64_t seed = (uint64_t)time(NULL) ^ (pid << 32);
    return tl_hash64(seed ^ (++counter));
}

#if !defined(_WIN32)
const char *tl_hostname(void)
{
    static char buf[256];
    if (buf[0] == '\0' && gethostname(buf, sizeof(buf)) != 0)
        snprintf(buf, sizeof(buf), "unknown");
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}
#endif
