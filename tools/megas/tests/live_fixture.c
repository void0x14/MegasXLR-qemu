/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "live.h"
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
static volatile sig_atomic_t stopping;
static void stop(int signal_number) { (void)signal_number; stopping=1; }
int main(int argc,char **argv)
{
    MgLive *live;
    if(argc!=5||mgp_init())return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);
    live=mg_live_new((unsigned)atoi(argv[1]),(unsigned)atoi(argv[2]),(unsigned)atoi(argv[3]),argv[4]);
    if(!live)return 1;
    while(!stopping){mg_live_tick(live);mgp_sleep(2);}
    printf("{\"bytes\":%llu,\"slow_clients\":%llu}\n",(unsigned long long)mg_live_bytes(live),(unsigned long long)mg_live_slow_clients(live));
    mg_live_free(live);return 0;
}
