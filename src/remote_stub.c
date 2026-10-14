// daemon has no UI: satisfy remote.c symbols referenced by shared code
#include "common.h"
int remote_log_lines(char out[][160], int max) { (void)out; (void)max; return 0; }
