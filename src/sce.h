// Hand-written prototypes for PS4 system libs (OpenOrbis headers declare many of these as void f()).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- sysmodule ----
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleLoadModule(uint16_t id);
#define SYSMOD_SYSCORE        0x80000004
#define SYSMOD_NETCTL         0x80000009
#define SYSMOD_HTTP           0x8000000A
#define SYSMOD_SSL            0x8000000B
#define SYSMOD_NP_COMMON      0x8000000C
#define SYSMOD_SYSTEM_SERVICE 0x80000010
#define SYSMOD_USER_SERVICE   0x80000011
#define SYSMOD_APPINSTUTIL    0x80000014
#define SYSMOD_NET            0x8000001C
#define SYSMOD_PAD            0x80000024
#define SYSMOD_AUDIOOUT       0x80000001
#define SYSMOD_VIDEO_OUT      0x80000022
#define SYSMOD_BGFT           0x8000002A
#define SYSMOD_FREETYPE_OL    0x009A

// ---- net / ssl / http ----
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, void *info);
int sceSslInit(size_t pool);
int sceHttpInit(int memId, int sslId, size_t pool);
int sceHttpCreateTemplate(int ctx, const char *ua, int ver, int autoProxy);
int sceHttpDeleteTemplate(int id);
int sceHttpCreateConnectionWithURL(int tmpl, const char *url, bool keepalive);
int sceHttpDeleteConnection(int id);
int sceHttpCreateRequestWithURL(int conn, int method, const char *url, uint64_t len);
int sceHttpDeleteRequest(int id);
int sceHttpAddRequestHeader(int id, const char *name, const char *value, int mode);
int sceHttpSendRequest(int req, const void *post, size_t size);
int sceHttpGetStatusCode(int req, int *code);
int sceHttpGetResponseContentLength(int req, int *type, uint64_t *len);
int sceHttpReadData(int req, void *buf, uint32_t size);
int sceHttpGetAllResponseHeaders(int req, char **hdr, size_t *size);
int sceHttpSetAutoRedirect(int id, int enable);
int sceHttpSetConnectTimeOut(int id, uint32_t usec);
int sceHttpSetRecvTimeOut(int id, uint32_t usec);
int sceHttpSetSendTimeOut(int id, uint32_t usec);
int sceHttpSetResolveTimeOut(int id, uint32_t usec);
typedef int (*SceHttpsCb)(int, unsigned int, void *const[], int, void *);
int sceHttpsSetSslCallback(int id, SceHttpsCb cb, void *arg);
int sceHttpsDisableOption(int id, uint32_t flags);
#define HTTP_GET 0
#define HTTP_VER_1_1 2
#define HDR_OVERWRITE 0

// ---- user service ----
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *uid);
int sceUserServiceGetInitialUser(int *uid);

// ---- bgft / appinstutil (structs match PS4-Store / flatz RPI) ----
struct bgft_init_params { void *heap; size_t heapSize; };
struct bgft_download_param {
    int user_id;
    int entitlement_type;
    const char *id;
    const char *content_url;
    const char *content_ex_url;
    const char *content_name;
    const char *icon_path;
    const char *sku_id;
    int option;
    const char *playgo_scenario_id;
    const char *release_date;
    const char *package_type;
    const char *package_sub_type;
    unsigned long package_size;
};
struct bgft_download_param_ex { struct bgft_download_param param; unsigned int slot; };
struct bgft_task_progress {
    unsigned int bits;
    int error_result;
    unsigned long length;
    unsigned long transferred;
    unsigned long length_total;
    unsigned long transferred_total;
    unsigned int num_index;
    unsigned int num_total;
    unsigned int rest_sec;
    unsigned int rest_sec_total;
    int preparing_percent;
    int local_copy_percent;
};
#define BGFT_OPT_INVISIBLE 0x2
int sceBgftServiceInit(struct bgft_init_params *p);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(struct bgft_download_param_ex *p, int *task);
int sceBgftServiceDownloadStartTask(int task);
int sceBgftServiceDownloadGetProgress(int task, struct bgft_task_progress *p);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilGetTitleIdFromPkg(const char *path, char *title_id, int *is_app);
int sceAppInstUtilAppUnInstall(const char *title_id);

// ---- kernel ----
typedef struct {
    int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} NotifyReq;
int sceKernelSendNotificationRequest(int dev, NotifyReq *req, size_t size, int blocking);
int sceKernelUsleep(unsigned int usec);
uint64_t sceKernelGetProcessTime(void); // microseconds

// ---- pad ----
typedef struct { uint8_t x, y; } PadStick;
typedef struct {
    uint32_t buttons;
    PadStick leftStick, rightStick;
    uint8_t analogL2, analogR2;
    uint8_t padding[2];
    uint8_t rest[0x80]; // remainder of OrbisPadData (quat, accel, touch, ...) - oversized on purpose
} PadData;
int scePadInit(void);
int scePadOpen(int uid, int type, int idx, void *param);
int scePadReadState(int handle, PadData *data);
#define PAD_L3       0x0002
#define PAD_R3       0x0004
#define PAD_OPTIONS  0x0008
#define PAD_UP       0x0010
#define PAD_RIGHT    0x0020
#define PAD_DOWN     0x0040
#define PAD_LEFT     0x0080
#define PAD_L2       0x0100
#define PAD_R2       0x0200
#define PAD_L1       0x0400
#define PAD_R1       0x0800
#define PAD_TRIANGLE 0x1000
#define PAD_CIRCLE   0x2000
#define PAD_CROSS    0x4000
#define PAD_SQUARE   0x8000

// ---- app launching / daemon ----
typedef struct {
    uint32_t sz;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    int check_flag; // 2 = SkipSystemUpdateCheck
} LncAppParam;
uint32_t sceLncUtilLaunchApp(const char *tid, const char *argv[], LncAppParam *param);
int sceLncUtilGetAppId(const char *tid);
int sceLncUtilKillApp(uint32_t app_id);
int sceLncUtilInitialize(void);
int sceSystemServiceRegisterDaemon(void);
int sceSystemServiceLoadExec(const char *path, const char *argv[]);

// ---- GoldHEN ----
int orbis_syscall(int num, ...);
