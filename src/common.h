#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define APP_NAME    "TurboDL"
#define APP_VERSION "1.00"
#ifndef APP_DIR
#define APP_DIR     "/data/turbodl"   // = /user/data/turbodl; /data also resolves inside a sandbox
#endif
#ifndef FPKGI_DIR
#define FPKGI_DIR   "/data/FPKGi"   // optional: read FPKGi-format list files the user already has
#endif
#ifndef WEB_PORT
#define WEB_PORT    8080
#endif
#define MAX_JOBS    64
#define CHUNK_SIZE  (16ULL * 1024 * 1024)
#define MAX_CONNS   32
#define MAX_PARALLEL 8
#ifndef BUILD_ID
#define BUILD_ID    __DATE__ " " __TIME__
#endif
#define DAEMON_TID  "TRBD00002"                     // TurboDL background service
#define DAEMON_DIR  "/system/vsh/app/" DAEMON_TID
#define OLD_DAEMON_DIR "/system/vsh/app/PKDL00002"   // where older builds installed it

enum {
    J_QUEUED = 0,
    J_DOWNLOADING,
    J_PAUSED,
    J_DONE,
    J_INSTALLING,
    J_INSTALLED,
    J_ERROR,
};

typedef struct {
    int used;
    int id;
    char url[2048];
    char name[200];
    char path[320];
    volatile int state;
    char err[200];
    volatile uint64_t size;
    volatile uint64_t done;
    volatile uint64_t speed;      // bytes/sec (smoothed)
    volatile int install_pct;
    int auto_install;
    volatile int stop;            // ask active download to stop
    volatile int del_req;         // delete after stop
    volatile int running;         // a download thread owns this job
} job_t;

// globals (main.c)
extern job_t g_jobs[MAX_JOBS];
extern pthread_mutex_t g_lock;
extern char g_cookie[8192];
extern int g_conns;
extern int g_auto_install;
extern int g_delete_after;
extern int g_replace;
extern int g_notify_step;
extern int g_max_parallel;  // downloads running at the same time   // progress popup every N % (0 = off)
extern int g_jb_ok;
extern int g_net_ok;
extern char g_ip[32];
extern char g_status[160];      // one-line status shown by the TV UI (loader text)
extern volatile int g_loading;  // TV UI shows a spinner while set
void ui_status(int loading, const char *fmt, ...);
extern volatile int g_quitting;
extern const char *g_role;
extern int g_remote;           // app: UI mirrors the daemon over 127.0.0.1

// core.c
int  core_jailbreak(void);
void boot_mark(const char *stage);      // breadcrumb for the background service (raw file write)
void core_load_modules(int with_video);
void core_get_ip(void);
void core_early_init(const char *role, int with_video);
int  core_start_engine(int wait_ms, int defer_if_busy);

// lnc.c (PS4 app launching / process control)
int  lnc_launch(const char *tid);
int  lnc_kill(const char *tid);
void core_exit(void);
job_t *job_add_local(const char *path, int auto_install);

// remote.c (app side client of the daemon API)
int  remote_get(const char *path, char *out, size_t n);
int  remote_post(const char *path, const char *body, char *out, size_t n);
void remote_start_sync(void);
int  remote_log_lines(char out[][160], int max);
int  job_action(int id, const char *op);
int  ui_job_action(int id, const char *op);
void ui_set_conns(int n);
void ui_add_url(const char *url);
void ui_set_parallel(int n);

// util.c
// PS4 default pthread stacks are tiny: always create threads through this (1 MB stack)
int  thread_start(pthread_t *t, void *(*fn)(void *), void *arg, int detached);
void log_msg(const char *fmt, ...);
int  log_lines(char out[][160], int max);
void notify(const char *fmt, ...);
void notify_quiet(const char *fmt, ...); // popup without logging
const char *state_str(int s);
void fmt_bytes(char *out, size_t n, uint64_t b);
void url_decode(char *s);
void sanitize_name(char *s);
void name_from_url(const char *url, char *out, size_t n);
uint64_t now_ms(void);
int mkdir_p(const char *path);
int app_dir_writable(void);
void klog_printf(const char *s);
int orbis_syscall(int num, ...);
uint64_t disk_free(const char *path);

// config / queue (config.c)
void config_load(void);
void config_save(void);
void cookie_load(void);
int  cookie_set(const char *domain, const char *value); // empty value removes
int  cookie_lookup(const char *host, char *out, size_t n);
void hostname_of(const char *url, char *out, size_t n);
void queue_load(void);
void queue_save(void);
job_t *job_add(const char *url, const char *name, int auto_install);
job_t *job_get(int id);
void job_delete(job_t *j);

// http.c
typedef struct { int tmpl, conn, req; char host[300]; int last_err; } hctx_t;
int  http_global_init(void);
int  http_ctx_init(hctx_t *h);
void http_ctx_free(hctx_t *h);
int  http_open(hctx_t *h, const char *url, int64_t from, int64_t to,
               int *status, uint64_t *clen, uint64_t *total, char *final_url, size_t flen);
int  http_read(hctx_t *h, void *buf, uint32_t n);
void http_close(hctx_t *h, int keep_conn);

// dl.c
void dl_start(void);
int  dl_running(void);        // number of downloads in progress
void dl_peek_progress(job_t *j);

// install.c
int  install_start(job_t *j);

// web.c
int  web_start(int wait_ms);   // returns 1 if it bound the port
int  web_ready(void);
int  catalog_tsv(const char *json, char *out, size_t outn);  // catalog.c

// ui.c
int  ui_init(void);
char *ui_ask_text(const char *title, const char *initial); // on-screen keyboard (NULL if cancelled)
void ui_run(void);
