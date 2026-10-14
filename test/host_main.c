#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include "../src/common.h"
job_t g_jobs[MAX_JOBS]; pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER; char g_cookie[8192];
int g_notify_step=10; int g_max_parallel=3; int g_conns=8,g_auto_install=1,g_delete_after=0,g_replace=0,g_jb_ok=1,g_net_ok=1; char g_ip[32]="127.0.0.1";
volatile int g_quitting=0; const char *g_role="daemon"; int g_remote=0;
int lnc_launch(const char*t){printf("[LAUNCH] %s\n",t);return 0;} int lnc_kill(const char*t){printf("[KILL] %s\n",t);return 0;}
void core_exit(void){ printf("[EXIT]\n"); exit(0);} 
int main(void){ setvbuf(stdout,NULL,_IONBF,0); signal(SIGPIPE,SIG_IGN); mkdir_p(APP_DIR "/downloads"); config_load(); cookie_load(); queue_load(); dl_start(); web_start(); for(;;)sleep(1);}
