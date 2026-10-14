#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "../src/common.h"
#include "../src/sce.h"
#include <time.h>
int sceKernelUsleep(unsigned int u){return usleep(u);}
uint64_t sceKernelGetProcessTime(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000ULL+t.tv_nsec/1000;}
int sceKernelSendNotificationRequest(int d, NotifyReq*r, size_t s, int b){(void)d;(void)s;(void)b;printf("[NOTIFY] %s\n",r->message);return 0;}
int install_start(job_t*j){ printf("[INSTALL] would install %s\n", j->path); j->state=J_INSTALLED; queue_save(); return 0;}
