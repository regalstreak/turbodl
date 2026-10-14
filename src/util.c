#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "common.h"
#include "sce.h"

int thread_start(pthread_t *t, void *(*fn)(void *), void *arg, int detached) {
    pthread_t tmp;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024);
    if (detached) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(t ? t : &tmp, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}

#define LOG_RING 12
static char s_log[LOG_RING][160];
static int s_log_n = 0;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_msg(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&s_log_lock);
    snprintf(s_log[s_log_n % LOG_RING], 160, "%s", buf);
    s_log_n++;
    FILE *f = fopen(APP_DIR "/log.txt", "a");
    if (f) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        if (tm) fprintf(f, "%02d:%02d:%02d ", tm->tm_hour, tm->tm_min, tm->tm_sec);
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    pthread_mutex_unlock(&s_log_lock);
    printf("[pkgdl] %s\n", buf);
}

// newest last
int log_lines(char out[][160], int max) {
    pthread_mutex_lock(&s_log_lock);
    int n = s_log_n < LOG_RING ? s_log_n : LOG_RING;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        int idx = (s_log_n - n + i) % LOG_RING;
        memcpy(out[i], s_log[idx], 160);
    }
    pthread_mutex_unlock(&s_log_lock);
    return n;
}

static void notify_send(const char *msg) {
    NotifyReq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    req.type = 0;
    req.targetId = -1;
    req.useIconImageUri = 1;
    strcpy(req.iconUri, "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void notify_quiet(const char *fmt, ...) {
    char m[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    notify_send(m);
}

void notify(const char *fmt, ...) {
    NotifyReq req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    req.type = 0;
    req.targetId = -1;
    req.useIconImageUri = 1;
    strcpy(req.iconUri, "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    log_msg("%s", req.message);
}

const char *state_str(int s) {
    switch (s) {
    case J_QUEUED: return "Queued";
    case J_DOWNLOADING: return "Downloading";
    case J_PAUSED: return "Paused";
    case J_DONE: return "Downloaded";
    case J_INSTALLING: return "Installing";
    case J_INSTALLED: return "Installed";
    case J_ERROR: return "Error";
    }
    return "?";
}

void fmt_bytes(char *out, size_t n, uint64_t b) {
    double v = (double)b;
    if (v >= 1e9) snprintf(out, n, "%.2f GB", v / 1e9);
    else if (v >= 1e6) snprintf(out, n, "%.1f MB", v / 1e6);
    else if (v >= 1e3) snprintf(out, n, "%.0f KB", v / 1e3);
    else snprintf(out, n, "%llu B", (unsigned long long)b);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
            *o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// keep filenames boring: brackets/& etc. break bgft and some servers
void sanitize_name(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == '(' || c == ')' || c == ' ')
            continue;
        *p = '_';
    }
    // trim leading dots/spaces
    while (*s == '.' || *s == ' ') memmove(s, s + 1, strlen(s));
    if (!*s) strcpy(s, "download.pkg");
}

void name_from_url(const char *url, char *out, size_t n) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", url);
    char *q = strpbrk(tmp, "?#");
    if (q) *q = 0;
    char *slash = strrchr(tmp, '/');
    const char *base = slash ? slash + 1 : tmp;
    if (!*base) base = "download.pkg";
    snprintf(out, n, "%s", base);
    url_decode(out);
    sanitize_name(out);
}

void hostname_of(const char *url, char *out, size_t n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && i < n - 1) {
        out[i] = (char)tolower((unsigned char)p[i]);
        i++;
    }
    out[i] = 0;
}

uint64_t now_ms(void) {
    return sceKernelGetProcessTime() / 1000ULL; // libc clock_gettime runs slow on PS4
}

int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

#ifdef __PS4__
// FreeBSD 9 statfs (syscall 396); musl's statvfs struct doesn't match the PS4 kernel
struct bsd_statfs {
    uint32_t f_version, f_type;
    uint64_t f_flags, f_bsize, f_iosize, f_blocks, f_bfree;
    int64_t f_bavail;
    uint64_t f_files;
    int64_t f_ffree;
    uint64_t rest[64];
};
uint64_t disk_free(const char *path) {
    struct bsd_statfs st;
    memset(&st, 0, sizeof(st));
    if (orbis_syscall(396, path, &st) != 0) return 0;
    if (st.f_bavail < 0 || st.f_bsize == 0 || st.f_bsize > (1 << 20)) return 0;
    return (uint64_t)st.f_bavail * st.f_bsize;
}
#else
uint64_t disk_free(const char *path) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}
#endif
