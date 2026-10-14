// Segmented multi-connection downloader with resume.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "common.h"
#include "sce.h"

#define BUF_SIZE (512 * 1024)
#define STATE_MAGIC 0x32444B50u /* "PKD2": per-chunk byte progress */

enum { C_TODO = 0, C_BUSY = 1, C_DONE = 2 };

typedef struct {
    job_t *j;
    int fd;
    int ranged;
    uint64_t chunk;
    uint32_t nchunks;
    uint8_t *cmap;            // C_TODO / C_BUSY / C_DONE
    uint32_t *prog;           // bytes completed from chunk start (contiguous)
    char rurl[2048];          // resolved (post-redirect) url
    pthread_mutex_t lock;
    volatile int fatal;
    char fatal_msg[200];
    volatile int active_workers;
} dlctx_t;

static job_t *volatile s_active = NULL;

job_t *dl_active(void) { return s_active; }

// fill j->done from the on-disk resume state (for display after restart)
void dl_peek_progress(job_t *j) {
    char p[400];
    snprintf(p, sizeof(p), "%s.dlstate", j->path);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    uint32_t magic = 0, n = 0;
    uint64_t size = 0, chunk = 0, done = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == STATE_MAGIC && fread(&size, 8, 1, f) == 1 &&
        fread(&chunk, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1) {
        uint32_t v;
        for (uint32_t i = 0; i < n && fread(&v, 4, 1, f) == 1; i++) done += v;
        if (size && done <= size) { j->size = size; j->done = done; }
    }
    fclose(f);
}

static void set_fatal(dlctx_t *d, const char *msg) {
    pthread_mutex_lock(&d->lock);
    if (!d->fatal) {
        d->fatal = 1;
        snprintf(d->fatal_msg, sizeof(d->fatal_msg), "%s", msg);
    }
    pthread_mutex_unlock(&d->lock);
}

static uint64_t chunk_len(dlctx_t *d, uint32_t c) {
    uint64_t s = (uint64_t)c * d->chunk, e = s + d->chunk;
    if (e > d->j->size) e = d->j->size;
    return e - s;
}

static int next_chunk(dlctx_t *d) {
    int r = -1;
    pthread_mutex_lock(&d->lock);
    for (uint32_t i = 0; i < d->nchunks; i++)
        if (d->cmap[i] == C_TODO) { d->cmap[i] = C_BUSY; r = (int)i; break; }
    pthread_mutex_unlock(&d->lock);
    return r;
}

static void state_path(job_t *j, char *out, size_t n) { snprintf(out, n, "%s.dlstate", j->path); }

static void state_save(dlctx_t *d) {
    if (!d->ranged) return;
    char p[400], tp[410];
    state_path(d->j, p, sizeof(p));
    snprintf(tp, sizeof(tp), "%s.tmp", p);
    FILE *f = fopen(tp, "wb");
    if (!f) return;
    uint32_t magic = STATE_MAGIC;
    uint64_t size = d->j->size;
    fwrite(&magic, 4, 1, f);
    fwrite(&size, 8, 1, f);
    fwrite(&d->chunk, 8, 1, f);
    fwrite(&d->nchunks, 4, 1, f);
    uint32_t *copy = malloc((size_t)d->nchunks * 4);
    for (uint32_t i = 0; i < d->nchunks; i++) copy[i] = __atomic_load_n(&d->prog[i], __ATOMIC_SEQ_CST);
    fwrite(copy, 4, d->nchunks, f);
    free(copy);
    fclose(f);
    rename(tp, p);
}

static int state_load(dlctx_t *d) {
    char p[400];
    state_path(d->j, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint32_t magic = 0, n = 0;
    uint64_t size = 0, chunk = 0;
    int ok = fread(&magic, 4, 1, f) == 1 && fread(&size, 8, 1, f) == 1 &&
             fread(&chunk, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1;
    ok = ok && magic == STATE_MAGIC && size == d->j->size && chunk == d->chunk && n == d->nchunks;
    if (ok) ok = fread(d->prog, 4, n, f) == n;
    fclose(f);
    if (!ok) { memset(d->cmap, 0, d->nchunks); memset(d->prog, 0, (size_t)d->nchunks * 4); return 0; }
    uint64_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t L = chunk_len(d, i);
        if (d->prog[i] > L) d->prog[i] = 0;
        d->cmap[i] = d->prog[i] == L ? C_DONE : C_TODO;
        done += d->prog[i];
    }
    d->j->done = done;
    return 1;
}

static void *worker(void *arg) {
    dlctx_t *d = arg;
    job_t *j = d->j;
    hctx_t h;
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf || http_ctx_init(&h) != 0) {
        set_fatal(d, "out of memory / http template failed");
        free(buf);
        __atomic_sub_fetch(&d->active_workers, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }

    for (;;) {
        if (j->stop || d->fatal) break;
        int c = next_chunk(d);
        if (c < 0) break;
        uint64_t start = (uint64_t)c * d->chunk;
        uint64_t end = start + d->chunk;
        if (end > j->size) end = j->size;
        end -= 1; // inclusive
        uint64_t off = start + d->prog[c];
        int fails = 0;

        while (off <= end && !j->stop && !d->fatal) {
            char url[2048];
            pthread_mutex_lock(&d->lock);
            snprintf(url, sizeof(url), "%s", d->rurl);
            pthread_mutex_unlock(&d->lock);

            int st = 0;
            uint64_t clen = 0;
            int r = http_open(&h, url, (int64_t)off, (int64_t)end, &st, &clen, NULL, NULL, 0);
            if (r != 0 || st != 206) {
                http_close(&h, 0);
                fails++;
                if (st == 401 || st == 403) {
                    if (fails >= 4) { set_fatal(d, st == 401 ? "HTTP 401 - login required/expired" : "HTTP 403 - forbidden (cookie/link expired?)"); break; }
                } else if (st == 404 || st == 410) {
                    // data-node url went stale -> re-resolve from the original link
                    pthread_mutex_lock(&d->lock);
                    snprintf(d->rurl, sizeof(d->rurl), "%s", j->url);
                    pthread_mutex_unlock(&d->lock);
                    if (fails >= 6) { set_fatal(d, "HTTP 404 - file not found"); break; }
                } else if (st == 200) {
                    // server stopped honouring Range mid-way
                    if (fails >= 6) { set_fatal(d, "server ignores Range requests"); break; }
                }
                if (fails >= 40) {
                    char m[120];
                    snprintf(m, sizeof(m), "network failed (HTTP %d / 0x%08X)", st, h.last_err);
                    set_fatal(d, m);
                    break;
                }
                int wait = fails < 10 ? fails : 10;
                for (int i = 0; i < wait * 10 && !j->stop && !d->fatal; i++) sceKernelUsleep(100000);
                continue;
            }

            int n = 0;
            while (off <= end) {
                uint64_t want = end - off + 1;
                n = http_read(&h, buf, want < BUF_SIZE ? (uint32_t)want : BUF_SIZE);
                if (n <= 0) break;
                if (pwrite(d->fd, buf, (size_t)n, (off_t)off) != n) {
                    set_fatal(d, "disk write failed (disk full?)");
                    n = -1;
                    break;
                }
                off += (uint64_t)n;
                __atomic_store_n(&d->prog[c], (uint32_t)(off - start), __ATOMIC_SEQ_CST);
                __atomic_add_fetch(&j->done, (uint64_t)n, __ATOMIC_SEQ_CST);
                fails = 0;
                if (j->stop || d->fatal) break;
            }
            if (off > end) {
                http_close(&h, 1); // fully drained: keep-alive
            } else {
                http_close(&h, 0);
                if (!j->stop && !d->fatal) {
                    fails++;
                    sceKernelUsleep(500000);
                }
            }
        }

        pthread_mutex_lock(&d->lock);
        if (off > end) d->cmap[c] = C_DONE;
        else d->cmap[c] = C_TODO;
        pthread_mutex_unlock(&d->lock);
    }

    http_ctx_free(&h);
    free(buf);
    __atomic_sub_fetch(&d->active_workers, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

// No Range support: one sequential stream, restart from 0 on failure.
static void single_stream(dlctx_t *d) {
    job_t *j = d->j;
    hctx_t h;
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf || http_ctx_init(&h) != 0) { set_fatal(d, "http init failed"); free(buf); return; }
    int attempts = 0;
    uint64_t last_ms = now_ms(), last_done = 0;
    while (!j->stop && !d->fatal) {
        int st = 0;
        uint64_t clen = 0;
        j->done = 0;
        if (http_open(&h, j->url, -1, -1, &st, &clen, NULL, NULL, 0) != 0 || st != 200) {
            http_close(&h, 0);
            if (++attempts >= 10) { char m[80]; snprintf(m, sizeof(m), "HTTP %d", st); set_fatal(d, m); break; }
            sceKernelUsleep(3000000);
            continue;
        }
        if (clen) j->size = clen;
        uint64_t off = 0;
        int n;
        while ((n = http_read(&h, buf, BUF_SIZE)) > 0) {
            if (pwrite(d->fd, buf, (size_t)n, (off_t)off) != n) { set_fatal(d, "disk write failed"); break; }
            off += (uint64_t)n;
            j->done = off;
            uint64_t t = now_ms();
            if (t - last_ms >= 1000) {
                uint64_t sp = (j->done - last_done) * 1000 / (t - last_ms);
                j->speed = (j->speed * 2 + sp) / 3;
                last_ms = t;
                last_done = j->done;
            }
            if (j->stop || d->fatal) break;
        }
        http_close(&h, 0);
        if (n == 0 && !j->stop && !d->fatal && (!j->size || off == j->size)) {
            j->size = off;
            break; // success
        }
        if (j->stop || d->fatal) break;
        if (++attempts >= 10) { set_fatal(d, "connection kept dropping"); break; }
        sceKernelUsleep(2000000);
    }
    http_ctx_free(&h);
    free(buf);
}

static int check_pkg_magic(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char m[4] = {0};
    int r = (int)read(fd, m, 4);
    close(fd);
    return r == 4 && m[0] == 0x7F && m[1] == 'C' && m[2] == 'N' && m[3] == 'T';
}

static void run_download(job_t *j) {
    dlctx_t d;
    memset(&d, 0, sizeof(d));
    pthread_mutex_init(&d.lock, NULL);
    d.j = j;
    d.fd = -1;
    j->stop = 0;
    j->err[0] = 0;
    j->speed = 0;
    j->state = J_DOWNLOADING;
    queue_save();
    log_msg("Starting: %s", j->name);

    // ---- probe ----
    hctx_t h;
    if (http_ctx_init(&h) != 0) { snprintf(j->err, sizeof(j->err), "http init failed 0x%08X", h.last_err); j->state = J_ERROR; goto out; }
    int st = 0, tries = 0;
    uint64_t clen = 0, total = 0;
    for (;;) {
        int r = http_open(&h, j->url, 0, 0, &st, &clen, &total, d.rurl, sizeof(d.rurl));
        if (r == 0 && (st == 206 || st == 200)) break;
        http_close(&h, 0);
        if (st == 401 || st == 403 || st == 404 || st == 410 || ++tries >= 5 || j->stop) {
            if (st == 401 || st == 403) {
                char host[256], ck[16];
                hostname_of(j->url, host, sizeof(host));
                if (cookie_lookup(host, ck, sizeof(ck)))
                    snprintf(j->err, sizeof(j->err), "HTTP %d - cookie for %s rejected/expired, or link expired", st, host);
                else
                    snprintf(j->err, sizeof(j->err), "HTTP %d - login needed? add a cookie for %s in the web panel", st, host);
            }
            else if (st)
                snprintf(j->err, sizeof(j->err), "HTTP %d", st);
            else
                snprintf(j->err, sizeof(j->err), "can't connect (0x%08X%s)", h.last_err,
                         ((unsigned)h.last_err & 0xFFFFFF00u) == 0x80431000u ? " - TLS/SSL handshake?" : "");
            http_ctx_free(&h);
            j->state = j->stop ? J_PAUSED : J_ERROR;
            goto out;
        }
        sceKernelUsleep(2000000);
    }
    http_close(&h, 0);
    http_ctx_free(&h);

    if (st == 206 && total > 0) {
        d.ranged = 1;
        j->size = total;
    } else {
        d.ranged = 0;
        j->size = clen;
        log_msg("Server has no Range support - single connection, no resume");
    }

    {
        uint64_t freeb = disk_free(APP_DIR);
        uint64_t need = j->size > j->done ? j->size - j->done : 0; // rough: resumed bytes already on disk
        dl_peek_progress(j);
        need = j->size > j->done ? j->size - j->done : 0;
        if (freeb && need && freeb < need + 64ULL * 1024 * 1024) {
            char a[32], b[32];
            fmt_bytes(a, sizeof(a), need);
            fmt_bytes(b, sizeof(b), freeb);
            snprintf(j->err, sizeof(j->err), "not enough space: need %s, free %s", a, b);
            j->state = J_ERROR;
            goto out;
        }
    }

    d.fd = open(j->path, O_RDWR | O_CREAT, 0666);
    if (d.fd < 0) { snprintf(j->err, sizeof(j->err), "can't create %s", j->path); j->state = J_ERROR; goto out; }

    if (d.ranged) {
        d.chunk = CHUNK_SIZE;
        d.nchunks = (uint32_t)((j->size + d.chunk - 1) / d.chunk);
        d.cmap = calloc(d.nchunks ? d.nchunks : 1, 1);
        d.prog = calloc(d.nchunks ? d.nchunks : 1, 4);
        // NB: fstat() is unreliable on PS4 (libc struct mismatch) -> use lseek for the size
        uint64_t cur_size = (uint64_t)lseek(d.fd, 0, SEEK_END);
        int resumed = state_load(&d);
        if (!resumed || cur_size != j->size) {
            if (resumed) log_msg("Resume state ignored: file size %llu != %llu", (unsigned long long)cur_size, (unsigned long long)j->size);
            memset(d.cmap, 0, d.nchunks);
            memset(d.prog, 0, (size_t)d.nchunks * 4);
            j->done = 0;
            ftruncate(d.fd, (off_t)j->size);
        } else {
            char a[32];
            fmt_bytes(a, sizeof(a), j->done);
            log_msg("Resuming at %s", a);
        }
        queue_save();

        int n = g_conns;
        if ((uint32_t)n > d.nchunks) n = (int)d.nchunks;
        if (n < 1) n = 1;
        pthread_t th[MAX_CONNS];
        int started = 0;
        d.active_workers = n;
        for (int i = 0; i < n; i++) {
            if (thread_start(&th[i], worker, &d, 0) == 0) started++;
            else __atomic_sub_fetch(&d.active_workers, 1, __ATOMIC_SEQ_CST);
        }
        log_msg("Downloading with %d connections", started);

        uint64_t last_ms = now_ms(), last_done = j->done, last_save = now_ms();
        int step = g_notify_step, last_bucket = (step > 0 && j->size) ? (int)(j->done * 100 / j->size) / step : 0;
        if (step > 0) {
            char a[32];
            fmt_bytes(a, sizeof(a), j->size);
            notify_quiet("Downloading %s (%s)", j->name, a);
        }
        while (d.active_workers > 0) {
            sceKernelUsleep(250000);
            uint64_t t = now_ms();
            if (t - last_ms >= 1000) {
                uint64_t cur = j->done;
                uint64_t sp = cur > last_done ? (cur - last_done) * 1000 / (t - last_ms) : 0;
                j->speed = j->speed ? (j->speed * 3 + sp) / 4 : sp;
                last_ms = t;
                last_done = cur;
            }
            if (t - last_save >= 3000) { state_save(&d); last_save = t; }
            step = g_notify_step;
            if (step > 0 && j->size && !j->stop && !d.fatal) {
                int pct = (int)(j->done * 100 / j->size);
                if (pct / step > last_bucket && pct < 100) {
                    last_bucket = pct / step;
                    char a[32], b[32], c[32];
                    fmt_bytes(a, sizeof(a), j->done);
                    fmt_bytes(b, sizeof(b), j->size);
                    fmt_bytes(c, sizeof(c), j->speed);
                    uint64_t eta = j->speed ? (j->size - j->done) / j->speed : 0;
                    notify_quiet("%s\n%d%%  %s / %s  %s/s  ETA %llum", j->name, pct, a, b, c,
                                 (unsigned long long)((eta + 59) / 60));
                }
            }
        }
        for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
        state_save(&d);

        int all = 1;
        for (uint32_t i = 0; i < d.nchunks; i++) if (d.cmap[i] != C_DONE) { all = 0; break; }
        if (d.fatal) {
            snprintf(j->err, sizeof(j->err), "%s", d.fatal_msg);
            j->state = J_ERROR;
        } else if (j->stop || !all) {
            if (j->state == J_DOWNLOADING) j->state = g_quitting ? J_QUEUED : J_PAUSED;
        } else {
            j->done = j->size;
            j->state = J_DONE;
        }
    } else {
        ftruncate(d.fd, 0);
        single_stream(&d);
        if (d.fatal) { snprintf(j->err, sizeof(j->err), "%s", d.fatal_msg); j->state = J_ERROR; }
        else if (j->stop) { if (j->state == J_DOWNLOADING) j->state = g_quitting ? J_QUEUED : J_PAUSED; }
        else j->state = J_DONE;
    }

out:
    if (d.fd >= 0) { fsync(d.fd); close(d.fd); }
    free(d.cmap);
    free(d.prog);
    j->speed = 0;
    if (j->state == J_DONE) {
        char p[400];
        state_path(j, p, sizeof(p));
        unlink(p);
        char sz[32];
        fmt_bytes(sz, sizeof(sz), j->size);
        int is_pkg = check_pkg_magic(j->path);
        notify("Download complete: %s (%s)", j->name, sz);
        if (!is_pkg) log_msg("Note: %s is not a PS4 pkg (no CNT magic)", j->name);
        queue_save();
        if (is_pkg && j->auto_install) install_start(j);
    } else if (j->state == J_ERROR) {
        notify("Download failed: %s - %s", j->name, j->err);
    } else {
        log_msg("Paused: %s", j->name);
    }
    queue_save();
    pthread_mutex_destroy(&d.lock);
}

static void *scheduler(void *arg) {
    (void)arg;
    for (;;) {
        job_t *pick = NULL;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_JOBS; i++) {
            job_t *j = &g_jobs[i];
            if (j->used && j->state == J_QUEUED && !j->del_req) { pick = j; break; }
        }
        if (pick) s_active = pick;
        pthread_mutex_unlock(&g_lock);

        if (!pick || g_quitting) { s_active = NULL; sceKernelUsleep(500000); continue; }
        run_download(pick);
        s_active = NULL;
        if (pick->del_req) { pick->del_req = 0; job_delete(pick); }
    }
    return NULL;
}

void dl_start(void) {
    pthread_t t;
    thread_start(&t, scheduler, NULL, 1);
}
