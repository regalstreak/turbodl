// PKDL00001: the visible app. Installs/updates/starts the background service (TRBD00002)
// and shows its state on the TV. Falls back to running the engine itself if the daemon can't start.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include "common.h"
#include "sce.h"

int g_remote = 0;

// ---- /system remount (FreeBSD nmount = syscall 378), same approach as Itemzflow's daemon installer ----
static void iov_add(struct iovec *iov, int *n, const char *k, const char *v) {
    iov[*n].iov_base = (void *)k;
    iov[*n].iov_len = strlen(k) + 1;
    (*n)++;
    iov[*n].iov_base = (void *)v;
    iov[*n].iov_len = v ? strlen(v) + 1 : 0;
    (*n)++;
}

static int remount_system(int rw) {
    // 9 key/value pairs = 18 entries. This used to be iov[16]: the overflow overwrote the return
    // address with 4 (strlen("511")+1) and crashed the app right after "remount /system rw".
    struct iovec iov[24];
    int n = 0;
    iov_add(iov, &n, "fstype", "exfatfs");
    iov_add(iov, &n, "fspath", "/system");
    iov_add(iov, &n, "from", "/dev/da0x4.crypt");
    iov_add(iov, &n, "large", "yes");
    iov_add(iov, &n, "timezone", "static");
    iov_add(iov, &n, "async", "");
    iov_add(iov, &n, "ignoreacl", "");
    iov_add(iov, &n, "dirmask", "511");
    iov_add(iov, &n, "mask", "511");
    uint64_t flags = 0x10000ULL /* MNT_UPDATE */ | (rw ? 0 : 1ULL /* MNT_RDONLY */);
    int r = orbis_syscall(378, iov, n, flags);
    if (r == 0) log_msg("remount /system %s: ok", rw ? "rw" : "ro");
    else log_msg("remount /system %s: FAILED 0x%08X (errno %d)", rw ? "rw" : "ro", r, errno);
    return r;
}

static long file_size(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    long s = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return s;
}

static int files_equal(const char *a, const char *b) {
    long sa = file_size(a), sb = file_size(b);
    if (sa < 0 || sa != sb) return 0;
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char *x = malloc(65536), *y = malloc(65536);
    int eq = fa && fb && x && y;
    while (eq) {
        size_t na = fread(x, 1, 65536, fa), nb = fread(y, 1, 65536, fb);
        if (na != nb || memcmp(x, y, na)) eq = 0;
        if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(x);
    free(y);
    return eq;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { log_msg("copy: can't open %s (errno %d)", src, errno); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { log_msg("copy: can't create %s (errno %d)", dst, errno); fclose(in); return -1; }
    char *buf = malloc(1 << 20);
    size_t n;
    int ok = buf ? 0 : -1;
    while (buf && (n = fread(buf, 1, 1 << 20, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = -1; break; }
    fclose(in);
    fclose(out);
    free(buf);
    return ok;
}

// unlink first: the old binary may still be executing (ETXTBSY), and unlink is allowed anyway
static int put_file(const char *src, const char *dst) {
    char tmp[420];
    snprintf(tmp, sizeof(tmp), "%s.new", dst);
    if (copy_file(src, tmp) != 0) return -1;
    if (unlink(dst) != 0 && errno != ENOENT) log_msg("put_file: unlink %s: errno %d", dst, errno);
    if (rename(tmp, dst) == 0) return 0;
    log_msg("put_file: rename failed (errno %d), copying in place", errno);
    int r = copy_file(src, dst);
    unlink(tmp);
    return r;
}

static const char *find_daemon_src(void) {
    static const char *cands[] = {
        "/mnt/sandbox/pfsmnt/PKDL00001-app0/assets/misc/daemon.bin",
        "/mnt/sandbox/pfsmnt/PKDL00001-app0-patch0-union/assets/misc/daemon.bin",
        "/app0/assets/misc/daemon.bin",
        NULL,
    };
    for (int i = 0; cands[i]; i++) if (file_size(cands[i]) > 0) return cands[i];
    return NULL;
}

// The service used to live in PKDL00002. Delete only the files we know about (and two 1-byte
// test files left by an FTP check), no directory walking.
static int old_service_present(void) {
    return file_size(OLD_DAEMON_DIR "/eboot.bin") >= 0 || file_size(OLD_DAEMON_DIR "/sce_sys/param.sfo") >= 0 ||
           file_size(OLD_DAEMON_DIR "/t.txt") >= 0 || file_size("/system/t.txt") >= 0;
}
static void remove_old_service(void) {
    static const char *files[] = {OLD_DAEMON_DIR "/eboot.bin", OLD_DAEMON_DIR "/eboot.bin.new",
                                  OLD_DAEMON_DIR "/t.txt", OLD_DAEMON_DIR "/sce_sys/param.sfo",
                                  "/system/t.txt", NULL};
    for (int i = 0; files[i]; i++)
        if (unlink(files[i]) != 0 && errno != ENOENT) log_msg("unlink %s: errno %d", files[i], errno);
    if (rmdir(OLD_DAEMON_DIR "/sce_sys") != 0) log_msg("rmdir sce_sys: errno %d", errno);
    if (rmdir(OLD_DAEMON_DIR) != 0) log_msg("rmdir: errno %d", errno);
    log_msg(file_size(OLD_DAEMON_DIR "/eboot.bin") < 0 ? "Old service folder removed" : "Old service folder still there");
}

static int daemon_build_matches(void) {
    char buf[1024];
    if (remote_get("/api/info", buf, sizeof(buf)) != 200) return -1; // not running
    if (!strstr(buf, "\"role\":\"daemon\"")) return -2;               // something else on the port
    char want[128];
    snprintf(want, sizeof(want), "\"build\":\"%s\"", BUILD_ID);
    return strstr(buf, want) ? 1 : 0;
}

static int wait_daemon(int want_up, int ms) {
    for (int t = 0; t < ms; t += 250) {
        int r = daemon_build_matches();
        if (want_up ? r == 1 : r == -1) return 1;
        sceKernelUsleep(250000);
    }
    return 0;
}

static int daemon_ensure(void) {
    int st = daemon_build_matches();
    if (st == 1) { log_msg("Background service already running"); return 1; }
    ui_status(1, "Preparing background service...");
    if (st == 0) {
        log_msg("Updating background service (asked it to quit)");
        remote_post("/api/quit", "", NULL, 0);
        wait_daemon(0, 8000);
    }
    // An old process keeps its eboot.bin busy (ETXTBSY) and may be wedged. Try to stop it, but
    // never wait long: we can still replace the file, and the launch step reports the rest.
    // NOTE: deliberately no LncUtil kill here. sceLncUtilKillApp hangs on a wedged process, and
    // that also blocks sceLncUtilGetAppId, which would wedge the whole app. The file swap works
    // anyway (see put_file); if a wedged old instance blocks the launch, the log says so.
    if (!g_jb_ok) { log_msg("no jailbreak -> can't install background service"); return 0; }

    const char *src = find_daemon_src();
    if (!src) { log_msg("assets/misc/daemon.bin not found in app"); return 0; }
    if (old_service_present()) {
        log_msg("Removing the old service folder " OLD_DAEMON_DIR);
        if (remount_system(1) == 0) { remove_old_service(); remount_system(0); }
    }
    if (!files_equal(src, DAEMON_DIR "/eboot.bin") || file_size(DAEMON_DIR "/sce_sys/param.sfo") <= 0) {
        log_msg("Installing background service to " DAEMON_DIR);
        if (remount_system(1) != 0) { log_msg("remount failed"); return 0; }
        mkdir(DAEMON_DIR, 0777);
        mkdir(DAEMON_DIR "/sce_sys", 0777);
        int ok = put_file(src, DAEMON_DIR "/eboot.bin") == 0;
        if (file_size(DAEMON_DIR "/sce_sys/param.sfo") <= 0)
            ok = ok && copy_file("/system/vsh/app/NPXS21007/sce_sys/param.sfo", DAEMON_DIR "/sce_sys/param.sfo") == 0;
        remount_system(0);
        if (!ok) { log_msg("copying daemon files failed"); return 0; }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        ui_status(1, "Starting background service%s...", attempt ? " (retry)" : "");
        int r = lnc_launch(DAEMON_TID);
        if (r < 0 && (unsigned)r != 0x8094000Cu /* already running */) log_msg("daemon launch failed 0x%08X", r);
        if (wait_daemon(1, 20000)) { log_msg("Background service started"); return 1; }
        log_msg("Background service didn't come up (attempt %d)", attempt + 1);
    }
    log_msg("Background service unavailable. If a previous version is still running, restart the");
    log_msg("console once so it is cleared, then open TurboDL again.");
    return 0;
}

static void *startup_thread(void *arg) {
    (void)arg;
    if (daemon_ensure()) {
        g_remote = 1;
        remote_start_sync();
        ui_status(0, "Running in the background");
    } else {
        ui_status(1, "Starting download engine...");
        log_msg("Running downloads inside the app (keep it open)");
        core_start_engine(15000, 0);
        ui_status(0, "Ready");
    }
    return NULL;
}

int main(void) {
    core_early_init("app", 1);
    config_load();
    core_get_ip();
    if (ui_init() != 0) log_msg("UI init failed");
    else log_msg("init: ui ok");
    pthread_t t;
    thread_start(&t, startup_thread, NULL, 1);
    ui_run();
    for (;;) sceKernelUsleep(1000000);
    return 0;
}
