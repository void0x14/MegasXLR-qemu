/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "megas.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
static int fake_qemu(int argc,char **argv) {
    unsigned port=0;MgpSocket listener,client;int i;char line[8192],reply[8192],job[64]="";size_t used=0;int running=1;
    for(i=1;i+1<argc;i++)if(!strcmp(argv[i],"-qmp"))sscanf(argv[i+1],"tcp:127.0.0.1:%u",&port);
    listener=mgp_listen(port);assert(listener!=MGP_BAD_SOCKET);
    while((client=accept(listener,NULL,NULL))==MGP_BAD_SOCKET)mgp_sleep(1);
    mgp_nonblock(client);assert(!mgp_send_all(client,"{\"QMP\":{\"version\":{},\"capabilities\":[]}}\r\n",strlen("{\"QMP\":{\"version\":{},\"capabilities\":[]}}\r\n"),100));
    for(;;){int n=mgp_recv(client,line+used,sizeof(line)-used-1);char *end,*id;
        if(n==0||n==-1)break;
        if(n==-2){mgp_sleep(1);continue;}used+=(size_t)n;line[used]=0;end=strchr(line,'\n');if(!end)continue;*end=0;
        id=strstr(line,"\"id\":");assert(id);{char *next=id;while((next=strstr(next+1,"\"id\":")))id=next;}unsigned serial=(unsigned)strtoul(id+5,NULL,10);
        if(strstr(line,"\"execute\":\"quit\"")){snprintf(reply,sizeof(reply),"{\"return\":{},\"id\":%u}\n",serial);mgp_send_all(client,reply,strlen(reply),100);break;}
        if(strstr(line,"query-commands"))snprintf(reply,sizeof(reply),"{\"return\":[{\"name\":\"snapshot-save\"},{\"name\":\"snapshot-load\"}],\"id\":%u}\n",serial);
        else if(strstr(line,"query-status"))snprintf(reply,sizeof(reply),"{\"return\":{\"status\":\"%s\",\"running\":%s},\"id\":%u}\n",running?"running":"paused",running?"true":"false",serial);
        else if(strstr(line,"snapshot-")&&strstr(line,"job-id")){
            char *begin=strstr(line,"\"job-id\":\"")+10,*close=strchr(begin,'"');assert(close);size_t length=(size_t)(close-begin);assert(length<sizeof(job));memcpy(job,begin,length);job[length]=0;
            if(strstr(line,"rejectlate")){mgp_sleep(650);snprintf(reply,sizeof(reply),"{\"error\":{\"class\":\"GenericError\",\"desc\":\"Delayed snapshot rejection\"},\"id\":%u}\n",serial);job[0]=0;}
            else snprintf(reply,sizeof(reply),"{\"event\":\"STOP\",\"data\":{}}\n{\"return\":{},\"id\":%u}\n{\"event\":\"JOB_STATUS_CHANGE\",\"data\":{\"id\":\"%s\",\"status\":\"concluded\"}}\n",serial,job);
        }
        else if(strstr(line,"query-jobs"))snprintf(reply,sizeof(reply),"{\"return\":[{\"id\":\"%s\",\"status\":\"concluded\",\"current-progress\":1,\"total-progress\":1}],\"id\":%u}\n",job,serial);
        else {if(strstr(line,"\"execute\":\"stop\""))running=0;if(strstr(line,"\"execute\":\"cont\""))running=1;snprintf(reply,sizeof(reply),"{\"return\":{},\"id\":%u}\n",serial);}
        assert(!mgp_send_all(client,reply,strlen(reply),100));size_t consumed=(size_t)(end-line)+1;memmove(line,line+consumed,used-consumed);used-=consumed;
    }
    mgp_close(client);mgp_close(listener);return 0;
}
int main(int argc,char **argv) {
    MgConfig cfg;MgEngine *e;char runtime[]="/tmp/megas-engine-XXXXXX",response[65536],request[2048],self[1024],cwd[1024];FILE *file;uint64_t begun;
    if(argc>1)return fake_qemu(argc,argv);
    assert(mkdtemp(runtime));assert(getcwd(cwd,sizeof(cwd)));
    if(argv[0][0]=='/')snprintf(self,sizeof(self),"%s",argv[0]);else snprintf(self,sizeof(self),"%.700s/%.300s",cwd,argv[0]);
    mg_config_defaults(&cfg);snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",self);
    cfg.control_port=52601;cfg.qmp_port=52602;cfg.vnc_port=52603;cfg.ram_mb=16;cfg.width=800;cfg.height=600;
    snprintf(cfg.disk,sizeof(cfg.disk),"%s/disk.qcow2",runtime);file=fopen(cfg.disk,"wb");assert(file);fputs("fixture",file);fclose(file);
    e=mg_engine_new(&cfg);assert(e);
    assert(!mg_engine_submit(e,"{\"action\":\"start\"}",response,sizeof(response)));assert(strstr(response,"\"connected\":true"));assert(strstr(response,"\"image_path\":\"\""));
    assert(!mg_engine_submit(e,"{\"command\":\"halt\",\"nested\":{\"auth\":\"nested-value\"}}",response,sizeof(response)));assert(strstr(response,"\"running\":false"));assert(strstr(response,"\"normalized\":1"));
    assert(mg_engine_submit(e,"{\"action\":\"invented nonsense\"}",response,sizeof(response)));assert(strstr(response,"repair_prompt"));
    assert(mg_engine_submit(e,"{\"action\":\"status\",\"bad\":\"\\u0000\"}",response,sizeof(response))==0); /* unused fields do not enter file paths */
    assert(mg_engine_submit(e,"{\"action\":\"start\",\"qemu\":\"\\ud800\"}",response,sizeof(response))==0); /* already-running is idempotent */
    assert(mg_engine_submit(e,"{\"action\":\"status\",\"bad\":01}",response,sizeof(response)));assert(strstr(response,"\"ok\":false"));
    assert(!mg_engine_submit(e,"{\"action\":\"resume\"}",response,sizeof(response)));assert(strstr(response,"\"running\":true"));
    assert(mg_engine_submit(e,"{\"action\":\"checkpoint\",\"name\":\"rejectlate\"}",response,sizeof(response)));assert(strstr(response,"\"snapshot_busy\":true"));
    begun=mgp_now();while(mgp_now()-begun<800){mg_engine_tick(e);mgp_sleep(10);}
    assert(!mg_engine_submit(e,"{\"action\":\"status\"}",response,sizeof(response)));assert(strstr(response,"\"snapshot_busy\":false"));assert(strstr(response,"\"uncertain_command\":false"));
    assert(!mg_engine_submit(e,"{\"action\":\"steps\"}",response,sizeof(response)));assert(strstr(response,"step-2-before"));assert(strstr(response,"step-2-after"));
    {MgpSocket client=mgp_connect(cfg.control_port,100);char answer[65536];assert(client!=MGP_BAD_SOCKET);const char *bad="{\"action\":\"status\",\"auth\":\"wrong\"}\n";assert(!mgp_send_all(client,bad,strlen(bad),100));mg_engine_tick(e);int n=mgp_recv(client,answer,sizeof(answer)-1);assert(n>0);answer[n]=0;assert(strstr(answer,"\"action\":\"authentication\""));mgp_close(client);}
    snprintf(request,sizeof(request),"{\"action\":\"stop\"}");assert(!mg_engine_submit(e,request,response,sizeof(response)));assert(strstr(response,"\"owned_pid\":0"));
    mg_engine_free(e);
    {char path[1024],content[65536];snprintf(path,sizeof(path),"%s/actions.jsonl",runtime);file=fopen(path,"rb");assert(file);size_t n=fread(content,1,sizeof(content)-1,file);content[n]=0;fclose(file);assert(strstr(content,"late_qmp_error"));assert(strstr(content,"qmp_event"));assert(!strstr(content,"\"auth\":\"wrong\""));}
    puts("engine: QMP event interleave, aliases, state, delayed error recovery, auth rejection, journal and lifecycle passed");return 0;
}
