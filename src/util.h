/* 底层 I/O 与通用工具：把容易写错的系统调用细节集中在这里。 */
#ifndef TL_UTIL_H
#define TL_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#if defined(_WIN32)
#include "compat_win32.h"
#endif

/*
 * 带偏移量的读写。
 * 与裸 pread/pwrite 相比，这两个封装额外处理：
 *   - 被信号打断（EINTR）时自动重试；
 *   - 一次系统调用只读写了一部分时继续补齐。
 * 返回值是实际读写的字节数；出错返回 -1（errno 有效）。
 */
ssize_t io_pread(int fd, void *buf, size_t n, int64_t off);
ssize_t io_pwrite(int fd, const void *buf, size_t n, int64_t off);

/* 把文件内容刷到磁盘；fsync 的跨平台封装。 */
int io_sync(int fd);

/* 把文件截断/扩展到 len 字节。 */
int io_truncate(int fd, int64_t len);

/* 文件当前长度（字节）；失败返回 -1。 */
int64_t io_file_size(int fd);

/*
 * 原子地改名：dst 已存在时也应被覆盖。
 * POSIX 的 rename 天然如此；Windows 的 MSVCRT rename 在目标存在时会失败，
 * 所以单独封装（Windows 下用 MoveFileEx + REPLACE_EXISTING）。
 */
int io_rename_replace(const char *src, const char *dst);

/*
 * 对“文件所在目录”执行 fsync。
 * 作用：rename(tmp -> final) 之后，目录项本身也要落盘，
 * 否则掉电后可能看到旧文件或临时文件。Windows 上为空操作。
 */
int io_sync_dir(const char *file_path);

/* 单调时钟，返回秒（用于计时）。 */
double tl_now_sec(void);

/* 主机名（作业报告截图要求能看出机器名）。 */
const char *tl_hostname(void);

/* splitmix64 风格的 64 位混合函数，用于生成确定性的测试数据。 */
uint64_t tl_hash64(uint64_t x);

/* 生成一个尽量不重复的 64 位标识，用于校验索引文件是否属于当前表。 */
uint64_t tl_new_uuid(void);

#endif /* TL_UTIL_H */
