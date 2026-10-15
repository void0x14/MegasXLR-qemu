/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static atomic_int release_threads;
static void *waiter(void *unused)
{
    (void)unused;
    while(!atomic_load(&release_threads))mgp_sleep(1);
    return NULL;
}
static int worker(const char *kind)
{
    if(!strcmp(kind,"tasks")){
        pthread_t threads[32];
        unsigned n=0,i;
        while(n<32&&!pthread_create(&threads[n],NULL,waiter,NULL))n++;
        atomic_store(&release_threads,1);
        for(i=0;i<n;i++)pthread_join(threads[i],NULL);
        printf("created_threads=%u\n",n);
        return n==7?0:1; /* pids.max=8 includes the main thread. */
    }
    if(!strcmp(kind,"memory")){
        volatile unsigned char *p=malloc(96*1024*1024);
        size_t i;
        if(!p)return 3;
        for(i=0;i<96*1024*1024;i+=4096)p[i]=1;
        return 1; /* Reaching here means the 32 MiB cgroup cap failed. */
    }
    if(!strcmp(kind,"cpu")){
        uint64_t until=mgp_now()+1100;
        volatile unsigned long n=0;
        while(mgp_now()<until)n++;
        return n?0:1;
    }
    return 2;
}
int main(int argc,char **argv)
{
    const char *parent=getenv("MEGAS_TEST_CGROUP_PARENT");
    const char *kinds[]={"tasks","memory","cpu"};
    char runtime[]="/tmp/megas-isolation-XXXXXX",log[1024],error[512],path[1200],line[256];
    unsigned i;
    if(argc==3&&!strcmp(argv[1],"--worker"))return worker(argv[2]);
    if(!parent||!parent[0]){puts("SKIP: real cgroup enforcement requires MEGAS_TEST_CGROUP_PARENT; no enforcement success claimed.");return 0;}
    assert(mkdtemp(runtime));assert(!mgp_init());
    for(i=0;i<3;i++){
        MgpProcess process={0};
        MgpVmLimits limits={parent,32ULL*1024*1024,25,8,1};
        char *child_argv[]={argv[0],"--worker",(char *)kinds[i],NULL};
        uint64_t deadline;
        FILE *file;
        unsigned long long metric=0;
        snprintf(log,sizeof(log),"%s/%s.log",runtime,kinds[i]);
        assert(!mgp_spawn_vm(&process,child_argv,log,&limits,error,sizeof(error)));
        deadline=mgp_now()+10000;
        while(mgp_alive(&process)&&mgp_now()<deadline)mgp_sleep(5);
        if(mgp_alive(&process)){mgp_force_kill(&process);assert(!"bounded worker exceeded deadline");}
        assert(process.exit_code==(!strcmp(kinds[i],"memory")?137:0));
        if(strcmp(kinds[i],"tasks")){
            snprintf(path,sizeof(path),"%s/%s",process.cgroup,!strcmp(kinds[i],"cpu")?"cpu.stat":"memory.events");
            file=mgp_fopen(path,"r");assert(file);
            while(fgets(line,sizeof(line),file)){
                if(sscanf(line,!strcmp(kinds[i],"cpu")?"nr_throttled %llu":"oom_kill %llu",&metric)==1)break;
            }
            fclose(file);assert(metric>0);
        }
        snprintf(path,sizeof(path),"%s",process.cgroup);
        mgp_reap(&process);
        assert(!process.cgroup[0]&&access(path,F_OK));
        printf("PASS: kernel-enforced %s cap and cgroup cleanup\n",kinds[i]);
        assert(!remove(log));
    }
    assert(!rmdir(runtime));return 0;
}
