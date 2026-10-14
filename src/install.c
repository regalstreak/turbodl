// Install a downloaded pkg through the system background installer (BGFT),
// same flow as LightningMods PS4-Store.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common.h"
#include "sce.h"

static pthread_mutex_t s_inst_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_inited = 0;

static int bgft_ready(void) {
    if (s_inited) return 1;
    int ret = sceAppInstUtilInitialize();
    if (ret) log_msg("sceAppInstUtilInitialize 0x%08X", ret);
    static struct bgft_init_params p;
    p.heapSize = 1024 * 1024;
    p.heap = calloc(1, p.heapSize);
    ret = sceBgftServiceInit(&p);
    if (ret) { log_msg("sceBgftServiceInit 0x%08X", ret); return 0; }
    s_inited = 1;
    return 1;
}

static int register_task(const char *url, const char *name, int *task) {
    struct bgft_download_param_ex dp;
    memset(&dp, 0, sizeof(dp));
    dp.param.entitlement_type = 5;
    dp.param.id = "";
    dp.param.content_url = url;
    dp.param.content_name = name;
    dp.param.icon_path = "";
    dp.param.playgo_scenario_id = "0";
    dp.param.option = BGFT_OPT_INVISIBLE;
    dp.slot = 0;
    return sceBgftServiceIntDownloadRegisterTaskByStorageEx(&dp, task);
}

static void *install_thread(void *arg) {
    job_t *j = arg;
    char title_id[16] = {0};
    int is_app = 0, task = -1, ret;

    pthread_mutex_lock(&s_inst_lock);
    j->state = J_INSTALLING;
    j->install_pct = 0;
    j->err[0] = 0;
    queue_save();

    if (access(j->path, F_OK) != 0) { snprintf(j->err, sizeof(j->err), "file missing"); goto fail; }
    if (!bgft_ready()) { snprintf(j->err, sizeof(j->err), "BGFT init failed (needs GoldHEN/jailbreak)"); goto fail; }

    ret = sceAppInstUtilGetTitleIdFromPkg(j->path, title_id, &is_app);
    if (ret) { snprintf(j->err, sizeof(j->err), "not a valid pkg (0x%08X)", ret); goto fail; }
    log_msg("Installing %s (%s)", title_id, j->name);

    for (int attempt = 0; attempt < 2; attempt++) {
        ret = register_task(j->path, j->name, &task);
        if ((ret == (int)0x80990088 || ret == (int)0x80990015) && g_replace && attempt == 0) {
            log_msg("%s already installed - uninstalling first", title_id);
            int u = sceAppInstUtilAppUnInstall(title_id);
            if (u) { snprintf(j->err, sizeof(j->err), "uninstall failed 0x%08X", u); goto fail; }
            continue;
        }
        break;
    }
    if (ret == (int)0x80990088 || ret == (int)0x80990015) {
        snprintf(j->err, sizeof(j->err), "%s already installed (enable replace in settings)", title_id);
        goto fail;
    }
    if (ret) {
        // fallback: let BGFT pull it from our own web server over loopback
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/files/%d", WEB_PORT, j->id);
        log_msg("Local register failed 0x%08X, trying %s", ret, url);
        ret = register_task(url, j->name, &task);
        if (ret) { snprintf(j->err, sizeof(j->err), "install register failed 0x%08X", ret); goto fail; }
    }

    ret = sceBgftServiceDownloadStartTask(task);
    if (ret) { snprintf(j->err, sizeof(j->err), "install start failed 0x%08X", ret); goto fail; }
    notify("Installing %s...", j->name);

    int last_pct = 0, idle = 0;
    for (;;) {
        sceKernelUsleep(500000);
        struct bgft_task_progress pr;
        memset(&pr, 0, sizeof(pr));
        ret = sceBgftServiceDownloadGetProgress(task, &pr);
        if (ret) {
            if (last_pct >= 90) break; // task finished and got cleaned up
            snprintf(j->err, sizeof(j->err), "install progress error 0x%08X", ret);
            goto fail;
        }
        if (pr.error_result) {
            snprintf(j->err, sizeof(j->err), "install failed 0x%08X", pr.error_result);
            goto fail;
        }
        if (pr.length > 0) {
            int pct = (int)((double)pr.transferred * 100.0 / (double)pr.length);
            if (pct > 100) pct = 100;
            j->install_pct = pct;
            if (pct != last_pct) idle = 0;
            last_pct = pct;
            if (pr.transferred >= pr.length) break;
        } else if (++idle > 600) { // 5 min with no info
            break;
        }
    }

    j->install_pct = 100;
    j->state = J_INSTALLED;
    notify("Installed: %s", j->name);
    if (g_delete_after) {
        unlink(j->path);
        log_msg("Deleted pkg after install");
    }
    queue_save();
    pthread_mutex_unlock(&s_inst_lock);
    return NULL;

fail:
    j->state = J_ERROR;
    notify("Install failed: %s - %s", j->name, j->err);
    queue_save();
    pthread_mutex_unlock(&s_inst_lock);
    return NULL;
}

int install_start(job_t *j) {
    if (j->state == J_INSTALLING) return -1;
    return thread_start(NULL, install_thread, j, 1);
}
