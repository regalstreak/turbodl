// TRBD00002: the background service. Lives in /system/vsh/app/TRBD00002, keeps running while games play.
#include <stdio.h>
#include "common.h"
#include "sce.h"

int g_remote = 0;
int ui_job_action(int id, const char *op) { return job_action(id, op); }
void ui_set_conns(int n) { (void)n; }
void ui_set_parallel(int n) { (void)n; }

// runs before main(): tells us whether the process gets that far at all
__attribute__((constructor)) static void early_mark(void) {
    klog_printf("[turbodl:daemon] constructor (before main)\n");
}

int main(void) {
    g_role = "daemon";             // so the breadcrumbs land in boot-daemon.txt
    boot_mark("main entered");
    core_early_init("daemon", 0);
    int r = sceSystemServiceRegisterDaemon();
    boot_mark("boot:registered");
    log_msg("RegisterDaemon -> 0x%08X", r);
    core_start_engine(3000, 1);
    notify("TurboDL running in background: http://%s:%d", g_ip, WEB_PORT);
    for (;;) sceKernelUsleep(1000000);
    return 0;
}
