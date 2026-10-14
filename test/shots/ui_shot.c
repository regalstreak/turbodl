// Host renderer for README screenshots: renders the real ui.c drawing code into a BMP.
// Build: see ../Makefile (make shots). Not part of the PS4 build.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// pull in the real UI implementation; we provide the platform bits below
#define ui_run ui_run_not_used
#include "../../src/ui.c"
#undef ui_run

// ---- globals normally owned by core.c / remote.c ----
job_t g_jobs[MAX_JOBS];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
char g_cookie[8192] = "archive.org";
int g_conns = 12, g_auto_install = 1, g_delete_after = 0, g_replace = 0, g_notify_step = 50;
int g_max_parallel = 4, g_jb_ok = 1, g_net_ok = 1, g_remote = 1;
char g_ip[32] = "192.168.1.8";
char g_status[160] = "Ready";
volatile int g_loading = 0;
int ui_job_action(int id, const char *op) { (void)id; (void)op; return 0; }
void ui_set_conns(int n) { g_conns = n; }
void ui_set_parallel(int n) { g_max_parallel = n; }
void ui_add_url(const char *u) { (void)u; }
int remote_log_lines(char out[][160], int max) { return log_lines(out, max); }
void queue_save(void) {}   // install_start() in the shared host stubs calls this

// ---- platform stubs ----
int sceVideoOutOpen(int u, int b, int i, const void *p) { (void)u; (void)b; (void)i; (void)p; return 1; }
void sceVideoOutSetBufferAttribute(void *a, uint32_t f, uint32_t t, uint32_t ar, uint32_t w, uint32_t h, uint32_t pi) {
    (void)a; (void)f; (void)t; (void)ar; (void)w; (void)h; (void)pi;
}
int sceVideoOutRegisterBuffers(int h, int s, void *const *a, int n, const void *at) { (void)h; (void)s; (void)a; (void)n; (void)at; return 0; }
int sceVideoOutSubmitFlip(int h, int i, uint32_t m, int64_t a) { (void)h; (void)i; (void)m; (void)a; return 0; }
int sceVideoOutSetFlipRate(int h, int r) { (void)h; (void)r; return 0; }
size_t sceKernelGetDirectMemorySize(void) { return 4608u * 1024 * 1024; }
int sceKernelAvailableDirectMemorySize(off_t a, off_t b, size_t c, off_t *s, size_t *z) { (void)a; (void)b; (void)c; if (s) *s = 0; if (z) *z = 4u << 30; return 0; }
int sceKernelAllocateMainDirectMemory(size_t l, size_t a, int t, off_t *o) { (void)l; (void)a; (void)t; if (o) *o = 0; return 0; }
int sceKernelMapFlexibleMemory(void **p, size_t l, int pr, int f) { (void)pr; (void)f; *p = calloc(1, l); return *p ? 0 : -1; }
int sceKernelMapDirectMemory(void **p, size_t l, int pr, int f, off_t o, size_t a) { (void)pr; (void)f; (void)o; (void)a; *p = calloc(1, l); return *p ? 0 : -1; }
int sceKernelAllocateDirectMemory(off_t a, off_t b, size_t l, size_t al, int t, off_t *o) { (void)a; (void)b; (void)l; (void)al; (void)t; (void)o; return -1; }
int sceSysmoduleLoadModule(uint16_t id) { (void)id; return 0; }
int sceUserServiceGetInitialUser(int *uid) { *uid = 0; return 0; }
int scePadInit(void) { return 0; }
int scePadOpen(int u, int t, int i, void *p) { (void)u; (void)t; (void)i; (void)p; return -1; }
int scePadReadState(int h, PadData *d) { (void)h; (void)d; return -1; }
int sceImeDialogInit(const OrbisImeDialogSetting *p, OrbisImeSettingsExtended *e) { (void)p; (void)e; return -1; }
OrbisDialogStatus sceImeDialogGetStatus(void) { return ORBIS_DIALOG_STATUS_NONE; }
int32_t sceImeDialogGetResult(OrbisDialogResult *r) { (void)r; return -1; }
int32_t sceImeDialogTerm(void) { return 0; }

// ---- helpers ----
static void write_bmp(const char *path) {
    int w = W, h = H;
    int row = (w * 3 + 3) & ~3;
    long datasz = (long)row * h;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    unsigned char hdr[54] = {'B', 'M'};
    long filesz = 54 + datasz;
    memcpy(hdr + 2, &filesz, 4);
    int off = 54;
    memcpy(hdr + 10, &off, 4);
    int ih = 40;
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    short planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &datasz, 4);
    fwrite(hdr, 1, 54, f);
    unsigned char *line = calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {                 // BMP is bottom-up
        uint32_t *src = s_cv.pixels + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            line[x * 3 + 0] = (unsigned char)(src[x] & 0xFF);
            line[x * 3 + 1] = (unsigned char)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (unsigned char)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    printf("wrote %s\n", path);
}

static job_t *addjob(const char *name, int state, double size, double done, double spd, int ipct, const char *err) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].used) continue;
        job_t *j = &g_jobs[i];
        memset(j, 0, sizeof(*j));
        j->used = 1;
        j->id = i + 1;
        snprintf(j->name, sizeof(j->name), "%s", name);
        j->state = state;
        j->size = (uint64_t)size;
        j->done = (uint64_t)done;
        j->speed = (uint64_t)spd;
        j->install_pct = ipct;
        if (err) snprintf(j->err, sizeof(j->err), "%s", err);
        return j;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : ".";
    char p[512];
    if (ui_init() != 0) { fprintf(stderr, "ui_init failed\n"); return 1; }
    s_font_ok = 1; // font already loaded by ui_init

    // ---- scene 1: downloads in background mode (homebrew packages only) ----
    addjob("Itemzflow_v1.8.9.pkg", J_DOWNLOADING, 22500e3, 22500e3 * 0.551, 2.6e6, 0, NULL);
    addjob("Remote_Package_Installer_v1.02.pkg", J_DOWNLOADING, 4200e3, 4200e3 * 0.075, 1.2e6, 0, NULL);
    addjob("Apollo_Save_Tool_v1.0.0.pkg", J_DONE, 18600e3, 18600e3, 0, 0, NULL);
    addjob("FPKGi_v1.10.0-release.pkg", J_QUEUED, 85458e3, 0, 0, 0, NULL);
    addjob("PS4_Xplorer_v1.0.0.pkg", J_INSTALLING, 12300e3, 12300e3, 0, 42, NULL);
    addjob("RetroArch_v1.19.0.pkg", J_ERROR, 412e6, 0, 0, 0,
           "HTTP 401 - login needed? add a cookie for example.com in the web panel");
    log_msg("Cookies loaded for: example.com");
    log_msg("Loaded 6 queued item(s)");
    log_msg("Resuming at 12.41 MB");
    log_msg("Downloading with 12 connections");
    log_msg("Resuming at 315.2 KB");
    log_msg("Downloading with 12 connections");
    log_msg("Installing CUSA00000 (PS4 Xplorer)");
    snprintf(p, sizeof(p), "%s/tv-downloads.png.bmp", outdir);
    draw();
    write_bmp(p);

    // ---- scene 2: catalog tab with a filter ----
    s_tab = 1;
    s_filter[0] = 0;
    free(s_cat);
    s_cat = malloc(64 * 1024);
    snprintf(s_cat, 64 * 1024,
        "Apollo Save Tool\tAPOL00001\t18600000\tUSA\t1.00\thttps://example.invalid/apollo.pkg\n"
        "FPKGi\tPKGI00001\t85458000\tUSA\t1.10\thttps://example.invalid/fpkgi.pkg\n"
        "GoldHEN Payload Loader\tBREW00007\t2100000\tEUR\t1.00\thttps://example.invalid/gh.pkg\n"
        "Itemzflow\tITEM00001\t22500000\tUSA\t1.08\thttps://example.invalid/itemzflow.pkg\n"
        "Media Player\tBREW00011\t9400000\tEUR\t1.02\thttps://example.invalid/media.pkg\n"
        "OpenOrbis Sample App\tBREW00001\t6100000\tUSA\t1.00\thttps://example.invalid/sample.pkg\n"
        "PS4 Xplorer\tPS4X00001\t12300000\tUSA\t1.00\thttps://example.invalid/xplorer.pkg\n"
        "Remote Package Installer\tFLTZ00003\t4200000\tUSA\t1.02\thttps://example.invalid/rpi.pkg\n"
        "RetroArch\tRETR00001\t412000000\tUSA\t1.19\thttps://example.invalid/retroarch.pkg\n"
        "Save Manager\tBREW00021\t7300000\tEUR\t1.01\thttps://example.invalid/save.pkg\n"
        "TurboDL\tPKDL00001\t6619136\tUSA\t1.00\thttps://example.invalid/turbodl.pkg\n"
        "uLaunch Tester\tBREW00031\t1200000\tUSA\t1.00\thttps://example.invalid/ulaunch.pkg\n");
    cat_index();
    snprintf(s_filter, sizeof(s_filter), "pkg");
    cat_apply_filter();
    snprintf(p, sizeof(p), "%s/tv-catalog.png.bmp", outdir);
    draw();
    write_bmp(p);
    return 0;
}
