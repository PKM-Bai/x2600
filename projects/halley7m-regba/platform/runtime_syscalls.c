#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <prom_print.h>
#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* The storage task overrides these. A negative result is -errno. */
#define WEAK __attribute__((weak))
WEAK int regba_fs_open(const char *path, int flags, int mode) { return -ENOSYS; }
WEAK int regba_fs_close(int fd) { return -ENOSYS; }
WEAK int regba_fs_read(int fd, void *buffer, size_t length) { return -ENOSYS; }
WEAK int regba_fs_write(int fd, const void *buffer, size_t length) { return -ENOSYS; }
WEAK long regba_fs_lseek(int fd, long offset, int whence) { return -ENOSYS; }
WEAK int regba_fs_fstat(int fd, struct stat *st) { return -ENOSYS; }
WEAK int regba_fs_stat(const char *path, struct stat *st) { return -ENOSYS; }
WEAK int regba_fs_unlink(const char *path) { return -ENOSYS; }
WEAK int regba_fs_rename(const char *oldpath, const char *newpath) { return -ENOSYS; }
WEAK int regba_fs_sync(int fd) { return -ENOSYS; }

static int fs_result(struct _reent *r, int result)
{
    if (result < 0) { r->_errno = -result; return -1; }
    return result;
}

int _open_r(struct _reent *r, const char *path, int flags, int mode)
{
    return fs_result(r, regba_fs_open(path, flags, mode));
}
int _close_r(struct _reent *r, int fd)
{
    if (fd >= 0 && fd <= 2) return 0;
    return fs_result(r, regba_fs_close(fd));
}
_ssize_t _write_r(struct _reent *r, int fd, const void *buffer, size_t length)
{
    if (fd == 1 || fd == 2) {
        const char *p = buffer;
        for (size_t i = 0; i < length; ++i) {
            if (p[i] == '\n') prom_putchar('\r');
            prom_putchar(p[i]);
        }
        return length;
    }
    return fs_result(r, fd < 3 ? -EBADF : regba_fs_write(fd, buffer, length));
}
_ssize_t _read_r(struct _reent *r, int fd, void *buffer, size_t length)
{
    if (fd == 0) {
        if (!length) return 0;
        int c;
        do {
            c = platform_uart_getchar();
            if (c < 0 && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
                vTaskDelay(1);
        } while (c < 0);
        ((unsigned char *)buffer)[0] = c;
        return 1;
    }
    return fs_result(r, fd < 3 ? -EBADF : regba_fs_read(fd, buffer, length));
}
_off_t _lseek_r(struct _reent *r, int fd, _off_t offset, int whence)
{
    long result = fd < 3 ? -ESPIPE : regba_fs_lseek(fd, offset, whence);
    if (result < 0) { r->_errno = -result; return -1; }
    return result;
}
int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
    if (fd >= 0 && fd <= 2) {
        *st = (struct stat){ .st_mode = S_IFCHR, .st_nlink = 1, .st_blksize = 1 };
        return 0;
    }
    return fs_result(r, regba_fs_fstat(fd, st));
}
int _stat_r(struct _reent *r, const char *path, struct stat *st)
{
    return fs_result(r, regba_fs_stat(path, st));
}
/* This bare-metal newlib omits the public POSIX stat wrapper. */
int stat(const char *path, struct stat *st)
{
    return _stat_r(_impure_ptr, path, st);
}
int _unlink_r(struct _reent *r, const char *path)
{
    return fs_result(r, regba_fs_unlink(path));
}
int _rename_r(struct _reent *r, const char *oldpath, const char *newpath)
{
    return fs_result(r, regba_fs_rename(oldpath, newpath));
}

int _isatty_r(struct _reent *r, int fd)
{
    if (fd >= 0 && fd <= 2) return 1;
    r->_errno = ENOTTY;
    return 0;
}
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *timezone)
{
    uint64_t us = platform_time_us();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;
    return 0;
}
clock_t _times_r(struct _reent *r, struct tms *times)
{
    clock_t ticks = platform_time_us() * CLOCKS_PER_SEC / 1000000;
    if (times) *times = (struct tms){ .tms_utime = ticks };
    return ticks;
}
int _getpid_r(struct _reent *r) { return 1; }
int _kill_r(struct _reent *r, int pid, int signal) { r->_errno = ENOSYS; return -1; }
int fsync(int fd) { return fs_result(_impure_ptr, regba_fs_sync(fd)); }

/* Non-reentrant entry points used by libraries in this toolchain. */
int _open(const char *p, int f, int m) { return _open_r(_impure_ptr, p, f, m); }
int _close(int fd) { return _close_r(_impure_ptr, fd); }
_ssize_t _read(int fd, void *b, size_t n) { return _read_r(_impure_ptr, fd, b, n); }
_ssize_t _write(int fd, const void *b, size_t n) { return _write_r(_impure_ptr, fd, b, n); }
_off_t _lseek(int fd, _off_t o, int w) { return _lseek_r(_impure_ptr, fd, o, w); }
int _fstat(int fd, struct stat *s) { return _fstat_r(_impure_ptr, fd, s); }
int _stat(const char *p, struct stat *s) { return _stat_r(_impure_ptr, p, s); }
int _unlink(const char *p) { return _unlink_r(_impure_ptr, p); }
int _rename(const char *a, const char *b) { return _rename_r(_impure_ptr, a, b); }
int _isatty(int fd) { return _isatty_r(_impure_ptr, fd); }
int _getpid(void) { return 1; }
int _kill(int pid, int signal) { return _kill_r(_impure_ptr, pid, signal); }
int _link(const char *a, const char *b) { errno = ENOSYS; return -1; }
void _exit(int status) { platform_panic("process exit"); }
