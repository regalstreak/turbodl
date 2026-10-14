// App-side client for the background daemon's HTTP API (127.0.0.1:WEB_PORT).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"
#include "sce.h"

static int http_local(const char *method, const char *path, const char *body, char *out, size_t n) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[1024];
    size_t bl = body ? strlen(body) : 0;
    int rl = snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n",
                      method, path, bl);
    if (send(s, req, (size_t)rl, 0) != rl || (bl && send(s, body, bl, 0) != (ssize_t)bl)) { close(s); return -1; }
    const size_t cap = 64 * 1024 + 1;
    char *buf = malloc(cap);
    if (!buf) { close(s); return -1; }
    size_t got = 0;
    for (;;) {
        ssize_t r = recv(s, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= cap - 1) break;
    }
    close(s);
    buf[got] = 0;
    int status = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) { free(buf); return -1; }
    char *b = strstr(buf, "\r\n\r\n");
    if (out && n) snprintf(out, n, "%s", b ? b + 4 : "");
    free(buf);
    return status;
}

int remote_get(const char *path, char *out, size_t n) { return http_local("GET", path, NULL, out, n); }
int remote_post(const char *path, const char *body, char *out, size_t n) { return http_local("POST", path, body, out, n); }

// ---- mirror daemon state into g_jobs for the TV UI ----
static char s_rlog[8][160];
static int s_rlog_n = 0;
static pthread_mutex_t s_rl = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_last_ok = 0;

int remote_log_lines(char out[][160], int max) {
    pthread_mutex_lock(&s_rl);
    int n = s_rlog_n < max ? s_rlog_n : max;
    int start = s_rlog_n - n;
    for (int i = 0; i < n; i++) memcpy(out[i], s_rlog[start + i], 160);
    pthread_mutex_unlock(&s_rl);
    if (s_last_ok && now_ms() - s_last_ok > 6000 && n > 0) {
        snprintf(out[n - 1], 160, "!! background service not responding");
    }
    return n;
}

static int json_bool(const char *j, const char *key) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\":true", key);
    return strstr(j, k) != NULL;
}

static long long json_num(const char *j, const char *key) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(j, k);
    return p ? atoll(p + strlen(k)) : 0;
}

static void json_str(const char *j, const char *key, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\":\"", key);
    const char *p = strstr(j, k);
    out[0] = 0;
    if (!p) return;
    p += strlen(k);
    size_t i = 0;
    while (p[i] && p[i] != '"' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

static void parse_jobs(char *tsv) {
    pthread_mutex_lock(&g_lock);
    int k = 0;
    for (char *line = strtok(tsv, "\n"); line && k < MAX_JOBS; line = strtok(NULL, "\n")) {
        char *f[8];
        int c = 0;
        char *p = line;
        while (c < 8) {
            f[c++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (c < 7) continue;
        job_t *j = &g_jobs[k++];
        j->used = 1;
        j->id = atoi(f[0]);
        j->state = atoi(f[1]);
        j->size = strtoull(f[2], NULL, 10);
        j->done = strtoull(f[3], NULL, 10);
        j->speed = strtoull(f[4], NULL, 10);
        j->install_pct = atoi(f[5]);
        snprintf(j->name, sizeof(j->name), "%s", f[6]);
        snprintf(j->err, sizeof(j->err), "%s", c > 7 ? f[7] : "");
    }
    for (; k < MAX_JOBS; k++) g_jobs[k].used = 0;
    pthread_mutex_unlock(&g_lock);
}

static void *sync_thread(void *arg) {
    (void)arg;
    static char buf[64 * 1024];
    int tick = 0;
    for (;; tick++) {
        if (remote_get("/api/jobs.tsv", buf, sizeof(buf)) == 200) {
            s_last_ok = now_ms();
            parse_jobs(buf);
        }
        if (tick % 2 == 0 && remote_get("/api/logtail", buf, sizeof(buf)) == 200) {
            pthread_mutex_lock(&s_rl);
            s_rlog_n = 0;
            for (char *l = strtok(buf, "\n"); l && s_rlog_n < 8; l = strtok(NULL, "\n"))
                snprintf(s_rlog[s_rlog_n++], 160, "%s", l);
            pthread_mutex_unlock(&s_rl);
        }
        if (tick % 4 == 0 && remote_get("/api/info", buf, sizeof(buf)) == 200) {
            g_conns = (int)json_num(buf, "conns");
            g_jb_ok = json_bool(buf, "jb");
            g_net_ok = json_bool(buf, "net");
            if (json_bool(buf, "cookie")) { if (!g_cookie[0]) strcpy(g_cookie, "set"); }
            else g_cookie[0] = 0;
            char ip[32];
            json_str(buf, "ip", ip, sizeof(ip));
            if (ip[0]) snprintf(g_ip, sizeof(g_ip), "%s", ip);
        }
        sceKernelUsleep(500000);
    }
    return NULL;
}

void remote_start_sync(void) {
    pthread_t t;
    thread_start(&t, sync_thread, NULL, 1);
}

int ui_job_action(int id, const char *op) {
    if (!g_remote) return job_action(id, op);
    char body[64];
    snprintf(body, sizeof(body), "id=%d&op=%s", id, op);
    return remote_post("/api/job", body, NULL, 0) == 200 ? 0 : -1;
}

void ui_set_conns(int n) {
    if (n < 1) n = 1;
    if (n > MAX_CONNS) n = MAX_CONNS;
    g_conns = n;
    if (!g_remote) { config_save(); return; }
    char body[32];
    snprintf(body, sizeof(body), "conns=%d", n);
    remote_post("/api/settings", body, NULL, 0);
}
