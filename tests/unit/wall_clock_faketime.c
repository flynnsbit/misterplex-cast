/* Process-clock and /dev/rtc stand-in for the wall-clock tests.
   LD_PRELOAD this, then the test flips the globals below.
   time/gettimeofday/clock_gettime(CLOCK_REALTIME) report mpx_fake_now.
   CLOCK_MONOTONIC is the real clock, so poll and timeouts still work.
   open() of mpx_fake_rtc_path is a fake RTC when mpx_fake_rtc_mode is 1. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

volatile time_t mpx_fake_now = 0;
volatile int mpx_fake_rtc_mode = 0;
volatile int mpx_fake_rtc_valid = 1;
volatile time_t mpx_fake_rtc_unix = 0;
volatile int mpx_fake_rtc_writes = 0;
volatile time_t mpx_fake_rtc_written = 0;
char mpx_fake_rtc_path[64] = "/dev/rtc-misterplex-fake";

static int (*real_open)(const char*, int, ...);
static int (*real_open64)(const char*, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_clock_gettime)(clockid_t, struct timespec*);

static pthread_mutex_t fd_mu = PTHREAD_MUTEX_INITIALIZER;
static int fake_fds[32];
static int fake_n;

static int raw_open(const char* path, int flags, mode_t mode) {
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static void remember(int fd) {
    pthread_mutex_lock(&fd_mu);
    if (fake_n < (int)(sizeof fake_fds / sizeof fake_fds[0]))
        fake_fds[fake_n++] = fd;
    pthread_mutex_unlock(&fd_mu);
}

static int is_fake(int fd) {
    int yes = 0;
    pthread_mutex_lock(&fd_mu);
    for (int i = 0; i < fake_n; ++i)
        if (fake_fds[i] == fd)
            yes = 1;
    pthread_mutex_unlock(&fd_mu);
    return yes;
}

static void forget(int fd) {
    pthread_mutex_lock(&fd_mu);
    for (int i = 0; i < fake_n; ++i) {
        if (fake_fds[i] == fd) {
            fake_fds[i] = fake_fds[--fake_n];
            break;
        }
    }
    pthread_mutex_unlock(&fd_mu);
}

/* -2: not our path. Otherwise the fd to return (or -1). */
static int try_fake(const char* path, int flags) {
    if (!path || mpx_fake_rtc_mode == 0 || strcmp(path, mpx_fake_rtc_path) != 0)
        return -2;
    int fd = raw_open("/dev/null", (flags & O_ACCMODE) == O_RDONLY ? O_RDONLY : O_RDWR, 0);
    if (fd >= 0)
        remember(fd);
    return fd;
}

static int forward_open(int (*real)(const char*, int, ...), const char* path, int flags, mode_t mode) {
    if (!real)
        return raw_open(path, flags, mode);
    if (flags & O_CREAT)
        return real(path, flags, mode);
    return real(path, flags);
}

int open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fake = try_fake(path, flags);
    if (fake != -2)
        return fake;
    if (!real_open)
        real_open = (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, "open");
    return forward_open(real_open, path, flags, mode);
}

int open64(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fake = try_fake(path, flags);
    if (fake != -2)
        return fake;
    if (!real_open64)
        real_open64 = (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, "open64");
    return forward_open(real_open64, path, flags, mode);
}

int close(int fd) {
    if (is_fake(fd))
        forget(fd);
    if (!real_close)
        real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    if (!real_close)
        return (int)syscall(SYS_close, fd);
    return real_close(fd);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (is_fake(fd) && (request == RTC_RD_TIME || request == RTC_SET_TIME)) {
        struct rtc_time* rt = (struct rtc_time*)arg;
        if (!rt) {
            errno = EFAULT;
            return -1;
        }
        if (request == RTC_RD_TIME) {
            if (!mpx_fake_rtc_valid) {
                errno = EINVAL;
                return -1;
            }
            time_t t = mpx_fake_rtc_unix;
            struct tm tm;
            if (!gmtime_r(&t, &tm)) {
                errno = EINVAL;
                return -1;
            }
            memset(rt, 0, sizeof *rt);
            rt->tm_sec = tm.tm_sec;
            rt->tm_min = tm.tm_min;
            rt->tm_hour = tm.tm_hour;
            rt->tm_mday = tm.tm_mday;
            rt->tm_mon = tm.tm_mon;
            rt->tm_year = tm.tm_year;
            rt->tm_wday = tm.tm_wday;
            rt->tm_yday = tm.tm_yday;
            rt->tm_isdst = 0;
            return 0;
        }
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_sec = rt->tm_sec;
        tm.tm_min = rt->tm_min;
        tm.tm_hour = rt->tm_hour;
        tm.tm_mday = rt->tm_mday;
        tm.tm_mon = rt->tm_mon;
        tm.tm_year = rt->tm_year;
        tm.tm_isdst = 0;
        time_t t = timegm(&tm);
        if (t == (time_t)-1) {
            errno = EINVAL;
            return -1;
        }
        mpx_fake_rtc_written = t;
        mpx_fake_rtc_unix = t;
        mpx_fake_rtc_valid = 1;
        mpx_fake_rtc_writes++;
        return 0;
    }
    if (!real_ioctl)
        real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) {
        errno = ENOTTY;
        return -1;
    }
    return real_ioctl(fd, request, arg);
}

time_t time(time_t* tloc) {
    time_t n = mpx_fake_now;
    if (tloc)
        *tloc = n;
    return n;
}

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (tv) {
        tv->tv_sec = mpx_fake_now;
        tv->tv_usec = 0;
    }
    return 0;
}

int clock_gettime(clockid_t clk, struct timespec* tp) {
    if (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE) {
        tp->tv_sec = mpx_fake_now;
        tp->tv_nsec = 0;
        return 0;
    }
    if (!real_clock_gettime)
        real_clock_gettime = (int (*)(clockid_t, struct timespec*))dlsym(RTLD_NEXT, "clock_gettime");
    if (!real_clock_gettime) {
        errno = EINVAL;
        return -1;
    }
    return real_clock_gettime(clk, tp);
}
