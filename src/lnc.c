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

// stop it and wait until it is really gone (an executing eboot.bin cannot be overwritten)
int lnc_stop(const char *tid) {
    if (!lnc_running(tid)) return 1;
    log_msg("Stopping old %s process...", tid);
    lnc_kill(tid);
    for (int i = 0; i < 100; i++) {              // up to 10 s
        sceKernelUsleep(100000);
        if (!lnc_running(tid)) { log_msg("%s stopped", tid); return 1; }
    }
    log_msg("%s is still running after kill", tid);
    return 0;
}

void core_exit(void) {
    sceSystemServiceLoadExec("exit", NULL);
    _exit(0);
}
