// Shared by the app (PKDL00001) and the background service (TRBD00002).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include "common.h"
#include "sce.h"

job_t g_jobs[MAX_JOBS];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
char g_cookie[8192];
int g_conns = 12;
int g_auto_install = 1;
int g_delete_after = 1;
int g_replace = 0;
int g_notify_step = 50;
int g_max_parallel = 4;
char g_status[160] = "starting...";
volatile int g_loading = 1;
int g_jb_ok = 0;
int g_net_ok = 0;
volatile int g_quitting = 0;
char g_ip[32] = "?";
const char *g_role = "app";

// raw syscall (same as GoldHEN SDK)
__asm__(
    ".intel_syntax noprefix\n"
    ".globl orbis_syscall\n"
    "orbis_syscall:\n"
    "  mov rax, 0\n"
    "  mov r10, rcx\n"
    "  syscall\n"
    "  jb 1f\n"
    "  ret\n"
    "1:\n"
    "  mov rax, -1\n"
    "  ret\n"
    ".att_syntax prefix\n");

struct jailbreak_backup {
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void *cr_prison;
    void *fd_cdir, *fd_jdir, *fd_rdir;
};

int core_jailbreak(void) {
    static struct jailbreak_backup bk;
    // GoldHEN SDK: syscall 500, cmd 2 = jailbreak
    boot_mark("jailbreak: calling");
    int r = orbis_syscall(500, 2, &bk);
    char m[64];
    snprintf(m, sizeof(m), "jailbreak: returned %d", r);
    boot_mark(m);
    return r == 0;
}

void ui_status(int loading, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_loading = loading;
}

void core_load_modules(int with_video) {
    static const uint32_t mods[] = {
        SYSMOD_SYSCORE, SYSMOD_SYSTEM_SERVICE, SYSMOD_USER_SERVICE, SYSMOD_NETCTL, SYSMOD_NET,
        SYSMOD_HTTP, SYSMOD_SSL, SYSMOD_APPINSTUTIL, SYSMOD_BGFT, SYSMOD_NP_COMMON, SYSMOD_PAD,
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        int r = sceSysmoduleLoadModuleInternal(mods[i]);
        if (r < 0) printf("[turbodl] load module 0x%08X failed 0x%08X\n", mods[i], r);
    }
    if (with_video) {
        sceSysmoduleLoadModuleInternal(SYSMOD_VIDEO_OUT);
        sceSysmoduleLoadModuleInternal(SYSMOD_AUDIOOUT);
    }
}

void core_get_ip(void) {
    union { char ip[16]; uint8_t raw[256]; } info;
    memset(&info, 0, sizeof(info));
    sceNetCtlInit();
    if (sceNetCtlGetInfo(14 /* IP_ADDRESS */, &info) == 0 && info.ip[0])
        snprintf(g_ip, sizeof(g_ip), "%s", info.ip);
}

// common early init; returns 0 on success
void core_early_init(const char *role, int with_video) {
    g_role = role;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSYS, SIG_IGN); // no GoldHEN -> syscall 500 returns ENOSYS instead of killing us
    ui_status(1, "Starting...");
    boot_mark("boot:start");
    // The service hangs forever inside GoldHEN's jailbreak syscall when LNC launches it, and it
    // does not need it: it is signed with system rights, and /data reaches the data directory
    // from inside its sandbox. Only the app (which writes to /system) jailbreaks.
    if (strcmp(role, "daemon") == 0) {
        boot_mark("boot:service skips the jailbreak");
        g_jb_ok = app_dir_writable();      // reported as "has file access"
        boot_mark(g_jb_ok ? "boot:data dir writable" : "boot:data dir NOT writable");
    }
    else g_jb_ok = core_jailbreak();
    // first: gives us the rights to load internal modules
    boot_mark(g_jb_ok ? "boot:jailbreak ok" : "boot:jailbreak FAILED");
    core_load_modules(with_video);
    boot_mark("boot:modules done");
    mkdir_p(APP_DIR "/downloads");
    mkdir_p(APP_DIR "/catalogs");
    log_msg("%s %s [%s] starting (jailbreak %s, build %s)", APP_NAME, APP_VERSION, role,
            g_jb_ok ? "ok" : "FAILED", BUILD_ID);
    sceUserServiceInitialize(NULL);
}

// load state + start downloader engine and web panel in this process
// Claim the web port first and only then start downloading: a second engine can never run.
static void *deferred_start(void *arg) {
    (void)arg;
    while (!web_ready()) sceKernelUsleep(500000);
    log_msg("Port free again - starting downloads here");
    ui_status(0, "Ready");
    dl_start();
    return NULL;
}

int core_start_engine(int wait_ms, int defer_if_busy) {
    ui_status(1, "Loading settings...");
    config_load();
    cookie_load();
    queue_load();
    ui_status(1, "Connecting...");
    // Without the jailbreak a service cannot see the data directory, so it would own the port and
    // download nothing. Step aside instead and let the app run the engine.
    if (strcmp(g_role, "daemon") == 0 && !app_dir_writable()) {
        boot_mark("data dir not writable - service exiting so the app can run");
        log_msg("Cannot write " APP_DIR " (no jailbreak?) - background service exiting");
        _exit(0);
    }
    log_msg("init: network");
    boot_mark("boot:net init");
    if (http_global_init() == 0) g_net_ok = 1;
    else {
        log_msg("Network init failed");
        ui_status(0, "Network init failed");
        return 0;
    }
    core_get_ip();
    boot_mark("boot:web start");
    if (!web_start(wait_ms)) {
        boot_mark("boot:web NOT bound");
        if (defer_if_busy) {
            log_msg("Port %d busy: waiting for it, then starting downloads", WEB_PORT);
            pthread_t t;
            thread_start(&t, deferred_start, NULL, 1);
            return 0;
        }
        log_msg("port %d busy: another instance owns it, not downloading here", WEB_PORT);
        ui_status(0, "Port %d already in use", WEB_PORT);
        return 0;
    }
    boot_mark("boot:web bound, ready");
    ui_status(0, "Ready");
    dl_start();
    return 1;
}
