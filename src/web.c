// Tiny HTTP server: web panel + JSON API + file serving (with Range) on port 8080.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"
#include "sce.h"
#include "index_html.h"

int job_action(int id, const char *op);

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void respond(int fd, int code, const char *ctype, const void *body, size_t len) {
    char h[512];
    const char *msg = code == 200 ? "OK" : code == 206 ? "Partial Content" : code == 404 ? "Not Found" :
                      code == 400 ? "Bad Request" : code == 416 ? "Range Not Satisfiable" : "Error";
    int hl = snprintf(h, sizeof(h),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, msg, ctype, len);
    send_all(fd, h, (size_t)hl);
    if (len) send_all(fd, body, len);
}

static void respond_str(int fd, int code, const char *s) { respond(fd, code, "text/plain; charset=utf-8", s, strlen(s)); }

static int get_param(const char *src, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    const char *p = src;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            const char *e = strchr(p, '&');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len >= n) len = n - 1;
            memcpy(out, p, len);
            out[len] = 0;
            url_decode(out);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static void json_esc(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int file_complete(job_t *j) {
    char st[400];
    snprintf(st, sizeof(st), "%s.dlstate", j->path);
    return j->size > 0 && j->done >= j->size && access(st, F_OK) != 0 && access(j->path, F_OK) == 0;
}

int job_action(int id, const char *op) {
    job_t *j = job_get(id);
    if (!j) return -1;
    int active = j->running;
    if (!strcmp(op, "pause")) {
        if (j->state == J_DOWNLOADING || j->state == J_QUEUED) {
            j->state = J_PAUSED;
            if (active) j->stop = 1;
        }
    } else if (!strcmp(op, "resume")) {
        if (j->state == J_PAUSED) j->state = J_QUEUED;
    } else if (!strcmp(op, "retry")) {
        if (j->state == J_ERROR || j->state == J_PAUSED) {
            if (file_complete(j)) install_start(j);
            else { j->err[0] = 0; j->state = J_QUEUED; }
        }
    } else if (!strcmp(op, "install")) {
        if (j->state != J_INSTALLING && j->state != J_DOWNLOADING && file_complete(j)) install_start(j);
        else return -2;
    } else if (!strcmp(op, "toggle")) { // controller X
        if (j->state == J_DOWNLOADING || j->state == J_QUEUED) return job_action(id, "pause");
        if (j->state == J_PAUSED || j->state == J_ERROR) return job_action(id, "retry");
    } else if (!strcmp(op, "delete")) {
        if (j->state == J_INSTALLING) return -2;
        if (active) { j->del_req = 1; j->state = J_PAUSED; j->stop = 1; }
        else job_delete(j);
        return 0;
    } else return -1;
    queue_save();
    return 0;
}

static void api_jobs(int fd) {
    size_t cap = 64 * 1024, o = 0;
    char *b = malloc(cap);
    if (!b) { respond_str(fd, 500, "oom"); return; }
    o += (size_t)snprintf(b + o, cap - o, "[");
    int first = 1;
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t *j = &g_jobs[i];
        if (!j->used) continue;
        char n[400], e[400];
        json_esc(n, sizeof(n), j->name);
        json_esc(e, sizeof(e), j->err);
        if (cap - o < 1200) break;
        o += (size_t)snprintf(b + o, cap - o,
            "%s{\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"size\":%llu,\"done\":%llu,\"speed\":%llu,"
            "\"ipct\":%d,\"err\":\"%s\",\"complete\":%s}",
            first ? "" : ",", j->id, n, state_str(j->state), (unsigned long long)j->size,
            (unsigned long long)j->done, (unsigned long long)j->speed, j->install_pct, e,
            file_complete(j) ? "true" : "false");
        first = 0;
    }
    o += (size_t)snprintf(b + o, cap - o, "]");
    respond(fd, 200, "application/json", b, o);
    free(b);
}

static void api_info(int fd) {
    char b[1024 + 8192];
    int n = snprintf(b, sizeof(b),
        "{\"parallel\":%d,\"notify\":%d,\"cookies\":\"%s\",\"ip\":\"%s\",\"conns\":%d,\"cookie\":%s,\"jb\":%s,\"free\":%llu,\"auto\":%d,\"del\":%d,\"replace\":%d,\"ver\":\"%s\",\"build\":\"%s\",\"role\":\"%s\",\"net\":%s}",
        g_max_parallel, g_notify_step, g_cookie, g_ip, g_conns, g_cookie[0] ? "true" : "false", g_jb_ok ? "true" : "false",
        (unsigned long long)disk_free(APP_DIR), g_auto_install, g_delete_after, g_replace, APP_VERSION,
        BUILD_ID, g_role, g_net_ok ? "true" : "false");
    respond(fd, 200, "application/json", b, (size_t)n);
}

// compact feed for the TV UI: one job per line, tab separated
static void api_jobs_tsv(int fd) {
    size_t cap = 64 * 1024, o = 0;
    char *b = malloc(cap);
    if (!b) { respond_str(fd, 500, "oom"); return; }
    for (int i = 0; i < MAX_JOBS && cap - o > 600; i++) {
        job_t *j = &g_jobs[i];
        if (!j->used) continue;
        o += (size_t)snprintf(b + o, cap - o, "%d\t%d\t%llu\t%llu\t%llu\t%d\t%s\t%s\n", j->id, j->state,
                              (unsigned long long)j->size, (unsigned long long)j->done,
                              (unsigned long long)j->speed, j->install_pct, j->name, j->err);
    }
    respond(fd, 200, "text/plain", b, o);
    free(b);
}

static void api_logtail(int fd) {
    char lines[8][160], b[8 * 162];
    int n = log_lines(lines, 8), o = 0;
    for (int i = 0; i < n; i++) o += snprintf(b + o, sizeof(b) - (size_t)o, "%s\n", lines[i]);
    respond(fd, 200, "text/plain", b, (size_t)o);
}

static void *quit_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].used && g_jobs[i].running) g_jobs[i].stop = 1;
    for (int i = 0; i < 100 && dl_running(); i++) sceKernelUsleep(100000);
    queue_save();
    log_msg("Daemon exiting (requested)");
    sceKernelUsleep(300000);
    core_exit();
    return NULL;
}

static void send_file(int fd, const char *path, const char *ctype, const char *range) {
    int f = open(path, O_RDONLY);
    if (f < 0) { respond_str(fd, 404, "not found"); return; }
    off_t endpos = lseek(f, 0, SEEK_END); // fstat unreliable on PS4
    uint64_t size = endpos > 0 ? (uint64_t)endpos : 0, from = 0, to = size ? size - 1 : 0;
    int partial = 0;
    if (range && !strncasecmp(range, "bytes=", 6)) {
        const char *r = range + 6;
        char *dash;
        if (*r == '-') { // suffix
            uint64_t n = strtoull(r + 1, NULL, 10);
            from = n < size ? size - n : 0;
        } else {
            from = strtoull(r, &dash, 10);
            if (dash && *dash == '-' && dash[1] >= '0' && dash[1] <= '9') to = strtoull(dash + 1, NULL, 10);
        }
        if (to >= size) to = size - 1;
        if (from > to || from >= size) {
            char h[160];
            int hl = snprintf(h, sizeof(h), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", (unsigned long long)size);
            send_all(fd, h, (size_t)hl);
            close(f);
            return;
        }
        partial = 1;
    }
    uint64_t len = size ? to - from + 1 : 0;
    char h[512];
    int hl;
    if (partial)
        hl = snprintf(h, sizeof(h), "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
                      ctype, (unsigned long long)from, (unsigned long long)to, (unsigned long long)size, (unsigned long long)len);
    else
        hl = snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                      "Content-Length: %llu\r\nConnection: close\r\n\r\n", ctype, (unsigned long long)len);
    if (send_all(fd, h, (size_t)hl) == 0 && len) {
        size_t bs = 1024 * 1024;
        char *buf = malloc(bs);
        uint64_t off = from;
        while (buf && len) {
            size_t want = len < bs ? (size_t)len : bs;
            ssize_t r = pread(f, buf, want, (off_t)off);
            if (r <= 0) break;
            if (send_all(fd, buf, (size_t)r) != 0) break;
            off += (uint64_t)r;
            len -= (uint64_t)r;
        }
        free(buf);
    }
    close(f);
}

static void handle(int fd) {
    static const size_t HMAX = 16 * 1024;
    char *req = malloc(HMAX + 1);
    if (!req) return;
    size_t got = 0;
    char *hend = NULL;
    while (got < HMAX) {
        ssize_t r = recv(fd, req + got, HMAX - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = 0;
        if ((hend = strstr(req, "\r\n\r\n"))) break;
    }
    if (!hend) { free(req); return; }
    *hend = 0;
    char *body_start = hend + 4;
    size_t body_have = got - (size_t)(body_start - req);

    char method[8] = {0}, target[2048] = {0};
    sscanf(req, "%7s %2047s", method, target);
    size_t clen = 0;
    char range[128] = {0};
    for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *ln = l + 2;
        if (!strncasecmp(ln, "Content-Length:", 15)) clen = strtoul(ln + 15, NULL, 10);
        else if (!strncasecmp(ln, "Range:", 6)) {
            char *v = ln + 6;
            while (*v == ' ') v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && k < sizeof(range) - 1) { range[k] = v[k]; k++; }
            range[k] = 0;
        }
    }

    char *body = NULL;
    if (clen > 0 && clen < 512 * 1024) {
        body = malloc(clen + 1);
        if (body) {
            size_t take = body_have < clen ? body_have : clen;
            memcpy(body, body_start, take);
            size_t b = take;
            while (b < clen) {
                ssize_t r = recv(fd, body + b, clen - b, 0);
                if (r <= 0) break;
                b += (size_t)r;
            }
            body[b] = 0;
        }
    }

    char path[2048];
    snprintf(path, sizeof(path), "%s", target);
    char *query = strchr(path, '?');
    if (query) *query++ = 0;
    else query = "";

    char v[8192], v2[256];
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", web_index_html, web_index_html_len);
    } else if (!strcmp(path, "/api/jobs")) {
        api_jobs(fd);
    } else if (!strcmp(path, "/api/info")) {
        api_info(fd);
    } else if (!strcmp(path, "/api/jobs.tsv")) {
        api_jobs_tsv(fd);
    } else if (!strcmp(path, "/api/logtail")) {
        api_logtail(fd);
    } else if (!strcmp(path, "/api/quit") && !strcmp(method, "POST")) {
        g_quitting = 1;
        respond_str(fd, 200, "bye");
        pthread_t t;
        thread_start(&t, quit_thread, NULL, 1);
    } else if (!strcmp(path, "/api/launch") && body) {
        get_param(body, "tid", v2, sizeof(v2));
        char r[64];
        snprintf(r, sizeof(r), "0x%08X", (unsigned)lnc_launch(v2));
        respond_str(fd, 200, r);
    } else if (!strcmp(path, "/api/kill") && body) {
        get_param(body, "tid", v2, sizeof(v2));
        char r[64];
        snprintf(r, sizeof(r), "0x%08X", (unsigned)lnc_kill(v2));
        respond_str(fd, 200, r);
    } else if (!strcmp(path, "/api/install_path") && body) {
        get_param(body, "path", v, sizeof(v));
        job_t *j = job_add_local(v, 1);
        if (j) { install_start(j); respond_str(fd, 200, "ok"); }
        else respond_str(fd, 400, "no such file / queue full");
    } else if (!strcmp(path, "/api/add") && body) {
        char *urls = malloc(clen + 1);
        get_param(body, "urls", urls, clen + 1);
        get_param(body, "name", v2, sizeof(v2));
        char a[8];
        get_param(body, "auto", a, sizeof(a));
        int ai = a[0] ? atoi(a) : g_auto_install;
        int count = 0, nlines = 0;
        for (char *s = strtok(urls, "\r\n"); s; s = strtok(NULL, "\r\n")) {
            while (*s == ' ' || *s == '\t') s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (!*s) continue;
            nlines++;
            if (job_add(s, nlines == 1 ? v2 : NULL, ai)) count++;
        }
        free(urls);
        if (count) respond_str(fd, 200, "ok");
        else respond_str(fd, 400, "No valid http(s) URL (or queue full)");
    } else if (!strcmp(path, "/api/job") && body) {
        get_param(body, "id", v2, sizeof(v2));
        int id = atoi(v2);
        get_param(body, "op", v2, sizeof(v2));
        int r = job_action(id, v2);
        respond_str(fd, r == 0 ? 200 : 400, r == 0 ? "ok" : "not possible");
    } else if (!strcmp(path, "/api/cookie") && body) {
        char dom[256];
        get_param(body, "domain", dom, sizeof(dom));
        get_param(body, "cookie", v, sizeof(v));
        int r = cookie_set(dom, v);
        respond_str(fd, r == 0 ? 200 : 400, r == 0 ? "ok" : "need a domain (or too many sites)");
    } else if (!strcmp(path, "/api/settings") && body) {
        if (get_param(body, "conns", v2, sizeof(v2)) && atoi(v2) > 0) {
            g_conns = atoi(v2);
            if (g_conns > MAX_CONNS) g_conns = MAX_CONNS;
        }
        if (get_param(body, "auto", v2, sizeof(v2))) g_auto_install = atoi(v2);
        if (get_param(body, "del", v2, sizeof(v2))) g_delete_after = atoi(v2);
        if (get_param(body, "replace", v2, sizeof(v2))) g_replace = atoi(v2);
        if (get_param(body, "notify", v2, sizeof(v2))) g_notify_step = atoi(v2) < 0 ? 0 : atoi(v2);
        if (get_param(body, "parallel", v2, sizeof(v2)) && atoi(v2) > 0)
            g_max_parallel = atoi(v2) > MAX_PARALLEL ? MAX_PARALLEL : atoi(v2);
        config_save();
        respond_str(fd, 200, "ok");
    } else if (!strcmp(path, "/api/catalog.tsv")) {
        static const char *ok2[] = {"GAMES", "UPDATES", "DLC", "DEMOS", "APPS", "HOMEBREW", "PS1", "PS2", "PSP", "PS5", NULL};
        get_param(query, "t", v2, sizeof(v2));
        int valid = 0;
        for (int i = 0; ok2[i]; i++) if (!strcmp(v2, ok2[i])) valid = 1;
        if (!valid) respond_str(fd, 400, "bad type");
        else {
            char p[300];
            snprintf(p, sizeof(p), APP_DIR "/catalogs/%s.json", v2);
            if (access(p, R_OK) != 0) snprintf(p, sizeof(p), FPKGI_DIR "/ContentJSONs/%s.json", v2);
            int f = open(p, O_RDONLY);
            if (f < 0) respond_str(fd, 404, "no list file on the console");
            else {
                off_t sz = lseek(f, 0, SEEK_END);
                lseek(f, 0, SEEK_SET);
                char *buf = (sz > 0 && sz < 32 * 1024 * 1024) ? malloc((size_t)sz + 1) : NULL;
                if (!buf) respond_str(fd, 500, "list unreadable");
                else {
                    ssize_t got = read(f, buf, (size_t)sz);
                    buf[got > 0 ? got : 0] = 0;
                    size_t outn = 512 * 1024;
                    char *out = malloc(outn);
                    if (!out) respond_str(fd, 500, "oom");
                    else {
                        int n = catalog_tsv(buf, out, outn);
                        respond(fd, 200, "text/plain; charset=utf-8", out, n > 0 ? (size_t)n : 0);
                        free(out);
                    }
                    free(buf);
                }
                close(f);
            }
        }
    } else if (!strcmp(path, "/api/catalog")) {
        static const char *ok[] = {"GAMES", "UPDATES", "DLC", "DEMOS", "APPS", "HOMEBREW", "PS1", "PS2", "PSP", "PS5", NULL};
        get_param(query, "t", v2, sizeof(v2));
        int valid = 0;
        for (int i = 0; ok[i]; i++) if (!strcmp(v2, ok[i])) valid = 1;
        if (!valid) { respond_str(fd, 400, "bad type"); }
        else {
            // user-provided lists only; TurboDL ships none
            char p[300];
            snprintf(p, sizeof(p), APP_DIR "/catalogs/%s.json", v2);
            if (access(p, R_OK) != 0) snprintf(p, sizeof(p), FPKGI_DIR "/ContentJSONs/%s.json", v2);
            send_file(fd, p, "application/json", NULL);
        }
    } else if (!strncmp(path, "/files/", 7)) {
        job_t *j = job_get(atoi(path + 7));
        if (!j) respond_str(fd, 404, "no such job");
        else send_file(fd, j->path, "application/octet-stream", range[0] ? range : NULL);
    } else if (!strcmp(path, "/log")) {
        send_file(fd, APP_DIR "/log.txt", "text/plain; charset=utf-8", NULL);
    } else {
        respond_str(fd, 404, "not found");
    }
    free(body);
    free(req);
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle(fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

static volatile int s_bound = 0;

static void *listener(void *arg) {
    (void)arg;
    int s = -1;
    int tries = 0;
    for (;;) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) { sceKernelUsleep(2000000); continue; }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(WEB_PORT);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 16) == 0) { s_bound = 1; break; }
        if (tries++ < 3) {
            log_msg("Web panel: bind :%d failed (%d)", WEB_PORT, errno);
            klog_printf("[turbodl] web bind failed");
        }
        close(s);
        sceKernelUsleep(3000000);
    }
    log_msg("Web panel on http://%s:%d", g_ip, WEB_PORT);
    for (;;) {
        struct sockaddr_in c;
        socklen_t cl = sizeof(c);
        int fd = accept(s, (struct sockaddr *)&c, &cl);
        if (fd < 0) { sceKernelUsleep(100000); continue; }
        if (thread_start(NULL, conn_thread, (void *)(intptr_t)fd, 1) != 0) close(fd);
    }
    return NULL;
}

int web_start(int wait_ms) {
    pthread_t t;
    thread_start(&t, listener, NULL, 1);
    for (int w = 0; w < wait_ms; w += 100) {
        if (s_bound) return 1;
        sceKernelUsleep(100000);
    }
    return s_bound;
}

int web_ready(void) { return s_bound; }
