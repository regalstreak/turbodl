#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

void config_load(void) {
    FILE *f = fopen(APP_DIR "/config.ini", "r");
    if (!f) { config_save(); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        int iv = atoi(v);
        if (!strcmp(k, "connections")) g_conns = iv;
        else if (!strcmp(k, "auto_install")) g_auto_install = iv;
        else if (!strcmp(k, "delete_after_install")) g_delete_after = iv;
        else if (!strcmp(k, "replace_installed")) g_replace = iv;
        else if (!strcmp(k, "notify_progress_step")) g_notify_step = iv;
        else if (!strcmp(k, "max_parallel_downloads")) g_max_parallel = iv;
    }
    fclose(f);
    if (g_conns < 1) g_conns = 1;
    if (g_conns > MAX_CONNS) g_conns = MAX_CONNS;
    if (g_max_parallel < 1) g_max_parallel = 1;
    if (g_max_parallel > MAX_PARALLEL) g_max_parallel = MAX_PARALLEL;
}

void config_save(void) {
    FILE *f = fopen(APP_DIR "/config.ini", "w");
    if (!f) return;
    fprintf(f, "# TurboDL settings\n");
    fprintf(f, "connections=%d\n", g_conns);
    fprintf(f, "auto_install=%d\n", g_auto_install);
    fprintf(f, "delete_after_install=%d\n", g_delete_after);
    fprintf(f, "replace_installed=%d\n", g_replace);
    fprintf(f, "# popup notification every N percent while downloading (0 = off)\n");
    fprintf(f, "notify_progress_step=%d\n", g_notify_step);
    fprintf(f, "# downloads running at the same time (1-%d); connections= applies to each\n", MAX_PARALLEL);
    fprintf(f, "max_parallel_downloads=%d\n", g_max_parallel);
    fclose(f);
}

// ---- per-site cookies: APP_DIR/cookies.txt, one "domain<TAB>cookie" per line ----
// Users add their own (web panel -> Settings). Nothing is built in.
#define MAX_COOKIES 16
typedef struct { char domain[128]; char value[4096]; } cookie_t;
static cookie_t s_ck[MAX_COOKIES];
static int s_nck = 0;
static pthread_mutex_t s_ck_lock = PTHREAD_MUTEX_INITIALIZER;

// accepts raw "a=b; c=d" or a pasted "Cookie: a=b" header line
static void normalize_cookie(const char *in, char *out, size_t n) {
    char tmp[8192];
    snprintf(tmp, sizeof(tmp), "%s", in);
    char *s = trim(tmp);
    if (!strncasecmp(s, "cookie:", 7)) s = trim(s + 7);
    for (char *p = s; *p; p++) if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    snprintf(out, n, "%s", s);
}

// "https://www.Example.com/x" -> "www.example.com"; ".example.com" -> "example.com"
static void normalize_domain(const char *in, char *out, size_t n) {
    const char *p = strstr(in, "://");
    p = p ? p + 3 : in;
    while (*p == '.' || *p == ' ') p++;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != ' ' && i < n - 1) { out[i] = (char)tolower((unsigned char)p[i]); i++; }
    out[i] = 0;
}

static void cookie_summary(void) {
    size_t o = 0;
    g_cookie[0] = 0;
    for (int i = 0; i < s_nck && o + 130 < sizeof(g_cookie); i++)
        o += (size_t)snprintf(g_cookie + o, sizeof(g_cookie) - o, "%s%s", i ? ", " : "", s_ck[i].domain);
}

static void cookie_write(void) {
    FILE *f = fopen(APP_DIR "/cookies.txt", "w");
    if (!f) return;
    fprintf(f, "# domain<TAB>cookie  (sent to that domain and its subdomains)\n");
    for (int i = 0; i < s_nck; i++) fprintf(f, "%s\t%s\n", s_ck[i].domain, s_ck[i].value);
    fclose(f);
}

void cookie_load(void) {
    pthread_mutex_lock(&s_ck_lock);
    s_nck = 0;
    FILE *f = fopen(APP_DIR "/cookies.txt", "r");
    if (f) {
        static char line[8192];
        while (fgets(line, sizeof(line), f) && s_nck < MAX_COOKIES) {
            char *t = trim(line);
            if (!*t || *t == '#') continue;
            char *tab = strchr(t, '\t');
            if (!tab) { log_msg("cookies.txt: ignoring line without domain"); continue; }
            *tab = 0;
            normalize_domain(t, s_ck[s_nck].domain, sizeof(s_ck[0].domain));
            normalize_cookie(tab + 1, s_ck[s_nck].value, sizeof(s_ck[0].value));
            if (s_ck[s_nck].domain[0] && s_ck[s_nck].value[0]) s_nck++;
        }
        fclose(f);
    }
    cookie_summary();
    pthread_mutex_unlock(&s_ck_lock);
    if (s_nck) log_msg("Cookies loaded for: %s", g_cookie);
}

int cookie_set(const char *domain_in, const char *value_in) {
    char d[128], v[4096];
    normalize_domain(domain_in, d, sizeof(d));
    normalize_cookie(value_in, v, sizeof(v));
    if (!d[0]) return -1;
    pthread_mutex_lock(&s_ck_lock);
    int i;
    for (i = 0; i < s_nck; i++) if (!strcmp(s_ck[i].domain, d)) break;
    if (!v[0]) { // remove
        if (i < s_nck) { s_ck[i] = s_ck[s_nck - 1]; s_nck--; }
    } else {
        if (i == s_nck) { if (s_nck >= MAX_COOKIES) { pthread_mutex_unlock(&s_ck_lock); return -1; } s_nck++; }
        snprintf(s_ck[i].domain, sizeof(s_ck[i].domain), "%s", d);
        snprintf(s_ck[i].value, sizeof(s_ck[i].value), "%s", v);
    }
    cookie_write();
    cookie_summary();
    pthread_mutex_unlock(&s_ck_lock);
    log_msg(v[0] ? "Cookie saved for %s" : "Cookie removed for %s", d);
    return 0;
}

// host = bare hostname (lowercase). Copies the cookie for the most specific matching domain.
int cookie_lookup(const char *host, char *out, size_t n) {
    int best = -1;
    size_t bl = 0, hl = strlen(host);
    pthread_mutex_lock(&s_ck_lock);
    for (int i = 0; i < s_nck; i++) {
        size_t dl = strlen(s_ck[i].domain);
        int match = !strcmp(host, s_ck[i].domain) ||
                    (hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, s_ck[i].domain));
        if (match && dl > bl) { best = i; bl = dl; }
    }
    if (best >= 0) snprintf(out, n, "%s", s_ck[best].value);
    pthread_mutex_unlock(&s_ck_lock);
    return best >= 0;
}

// ---- queue ----
static int s_next_id = 1;

void queue_save(void) {
    pthread_mutex_lock(&g_lock);
    FILE *f = fopen(APP_DIR "/queue.tmp", "w");
    if (f) {
        for (int i = 0; i < MAX_JOBS; i++) {
            job_t *j = &g_jobs[i];
            if (!j->used) continue;
            fprintf(f, "%d\t%d\t%d\t%llu\t%s\t%s\n", j->id, j->state, j->auto_install,
                    (unsigned long long)j->size, j->name, j->url);
        }
        fclose(f);
        rename(APP_DIR "/queue.tmp", APP_DIR "/queue.txt");
    }
    pthread_mutex_unlock(&g_lock);
}

void queue_load(void) {
    FILE *f = fopen(APP_DIR "/queue.txt", "r");
    if (!f) return;
    static char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < MAX_JOBS) {
        char *p = line, *fld[6];
        int k = 0;
        for (; k < 6 && p; k++) {
            fld[k] = p;
            char *t = (k < 5) ? strchr(p, '\t') : NULL;
            if (t) { *t = 0; p = t + 1; } else p = NULL;
        }
        if (k < 6) continue;
        char *url = trim(fld[5]);
        job_t *j = &g_jobs[n++];
        memset(j, 0, sizeof(*j));
        j->used = 1;
        j->id = atoi(fld[0]);
        j->state = atoi(fld[1]);
        j->auto_install = atoi(fld[2]);
        j->size = strtoull(fld[3], NULL, 10);
        snprintf(j->name, sizeof(j->name), "%s", fld[4]);
        snprintf(j->url, sizeof(j->url), "%s", url);
        if (!strncmp(j->url, "file://", 7)) snprintf(j->path, sizeof(j->path), "%s", j->url + 7);
        else snprintf(j->path, sizeof(j->path), APP_DIR "/downloads/%s", j->name);
        if (j->state == J_DOWNLOADING) j->state = J_QUEUED;
        if (j->state == J_INSTALLING) j->state = J_DONE;
        if (j->state == J_DONE || j->state == J_INSTALLED) j->done = j->size;
        else dl_peek_progress(j);
        if (j->id >= s_next_id) s_next_id = j->id + 1;
    }
    fclose(f);
    log_msg("Loaded %d queued item(s)", n);
}

job_t *job_get(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

// percent-encode characters that are illegal in a request line (spaces, brackets, non-ASCII)
static void url_normalize(const char *in, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c <= 0x20 || c >= 0x7F || strchr("[]\"<>|^`{}\\", c)) {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        } else out[o++] = (char)c;
    }
    out[o] = 0;
}

job_t *job_add(const char *url_in, const char *name, int auto_install) {
    char url[2048];
    url_normalize(url_in, url, sizeof(url));
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return NULL;
    pthread_mutex_lock(&g_lock);
    job_t *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) if (!g_jobs[i].used) { j = &g_jobs[i]; break; }
    if (!j) { pthread_mutex_unlock(&g_lock); return NULL; }
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->id = s_next_id++;
    snprintf(j->url, sizeof(j->url), "%s", url);
    if (name && *name) { snprintf(j->name, sizeof(j->name), "%s", name); sanitize_name(j->name); }
    else name_from_url(url, j->name, sizeof(j->name));
    // avoid clobbering another job's file
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t *o = &g_jobs[i];
        if (o != j && o->used && !strcmp(o->name, j->name)) {
            char tmp[200];
            snprintf(tmp, sizeof(tmp), "%d_%s", j->id, j->name);
            snprintf(j->name, sizeof(j->name), "%s", tmp);
            break;
        }
    }
    snprintf(j->path, sizeof(j->path), APP_DIR "/downloads/%s", j->name);
    j->auto_install = auto_install;
    j->state = J_QUEUED;
    pthread_mutex_unlock(&g_lock);
    log_msg("Queued: %s", j->name);
    queue_save();
    return j;
}

// register an already-downloaded local pkg (e.g. uploaded over FTP) so it can be installed
job_t *job_add_local(const char *path, int auto_install) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    close(fd);
    pthread_mutex_lock(&g_lock);
    job_t *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) if (!g_jobs[i].used) { j = &g_jobs[i]; break; }
    if (!j) { pthread_mutex_unlock(&g_lock); return NULL; }
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->id = s_next_id++;
    snprintf(j->url, sizeof(j->url), "file://%s", path);
    const char *b = strrchr(path, '/');
    snprintf(j->name, sizeof(j->name), "%s", b ? b + 1 : path);
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->size = j->done = (uint64_t)sz;
    j->state = J_DONE;
    j->auto_install = auto_install;
    pthread_mutex_unlock(&g_lock);
    queue_save();
    return j;
}

// caller must make sure job isn't actively downloading/installing
void job_delete(job_t *j) {
    char st[400];
    unlink(j->path);
    snprintf(st, sizeof(st), "%s.dlstate", j->path);
    unlink(st);
    log_msg("Removed: %s", j->name);
    pthread_mutex_lock(&g_lock);
    j->used = 0;
    pthread_mutex_unlock(&g_lock);
    queue_save();
}
