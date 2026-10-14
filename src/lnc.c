// PS4 process control: launch/kill apps by title id, exit cleanly.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common.h"
#include "sce.h"

static int s_lnc_init = 0;

static void lnc_init(void) {
    if (!s_lnc_init) {
        int r = sceLncUtilInitialize();
        if (r) log_msg("sceLncUtilInitialize 0x%08X", r);
        s_lnc_init = 1;
    }
}

int lnc_launch(const char *tid) {
    lnc_init();
    LncAppParam p;
    memset(&p, 0, sizeof(p));
    p.sz = sizeof(p);
    p.user_id = (uint32_t)-1;
    p.check_flag = 2; // SkipSystemUpdateCheck
    uint32_t r = sceLncUtilLaunchApp(tid, NULL, &p);
    log_msg("launch %s -> 0x%08X", tid, r);
    return (int)r;
}

int lnc_kill(const char *tid) {
    lnc_init();
    int id = sceLncUtilGetAppId(tid);
    if (((unsigned)id & ~0xFFFFFFu) != 0x60000000u) { log_msg("kill %s: not running (0x%08X)", tid, id); return id; }
    int r = sceLncUtilKillApp((uint32_t)id);
    log_msg("kill %s (appid 0x%08X) -> 0x%08X", tid, id, r);
    return r;
}

// is a process with this title id alive right now?
int lnc_running(const char *tid) {
    lnc_init();
    int id = sceLncUtilGetAppId(tid);
    return ((unsigned)id & ~0xFFFFFFu) == 0x60000000u;
}

static void *kill_thread(void *arg) {
    const char *tid = arg;
    log_msg("kill thread: asking %s to die", tid);
    lnc_kill(tid);                 // may block on a wedged process; harmless in a detached thread
    return NULL;
}

// Ask it to die from a detached thread and wait briefly. Never blocks the caller for long,
// because sceLncUtilKillApp can hang on a process that is itself stuck.
int lnc_stop(const char *tid, int wait_ms) {
    if (!lnc_running(tid)) return 1;
    log_msg("Asking old %s process to stop", tid);
    pthread_t t;
    thread_start(&t, kill_thread, (void *)tid, 1);
    for (int i = 0; i < wait_ms / 100; i++) {
        sceKernelUsleep(100000);
        if (!lnc_running(tid)) { log_msg("%s stopped", tid); return 1; }
    }
    log_msg("%s did not stop (still running)", tid);
    return 0;
}

void core_exit(void) {
    sceSystemServiceLoadExec("exit", NULL);
    _exit(0);
}
