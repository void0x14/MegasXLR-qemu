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
#include <stdint.h>
#include <time.h>

#define TEST_DISK_BASE 1024ULL
#define TEST_SNAPSHOT_BUDGET (2ULL * 1024 * 1024 * 1024)

typedef struct {
    MgEngine *engine;
    char runtime[1024], control[1024], trace[1024], catalog[1024];
} BudgetFixture;

static char test_program[1024];
static unsigned test_port = 52701;

static void fake_trace(const char *path,const char *command,const char *tag) {
    FILE *file;
    if(!path||!*path)return;
    file=fopen(path,"ab");
    if(!file)return;
    fprintf(file,"%s%s%s\n",command,tag&&*tag?" ":"",tag&&*tag?tag:"");
    fclose(file);
}
static void fake_control_stage(const char *path,char *stage,size_t cap) {
    FILE *file;
    size_t n;
    if(!cap)return;
    snprintf(stage,cap,"normal");
    if(!path||!*path||(file=fopen(path,"rb"))==NULL)return;
    n=fread(stage,1,cap-1,file);stage[n]=0;fclose(file);
    while(n&&(stage[n-1]=='\n'||stage[n-1]=='\r'))stage[--n]=0;
}
static void fake_field(const char *line,const char *key,char *out,size_t cap) {
    char needle[80];const char *begin,*end;size_t n;
    if(!cap)return;
    out[0]=0;
    if(snprintf(needle,sizeof(needle),"\"%s\":\"",key)>=(int)sizeof(needle))return;
    begin=strstr(line,needle);if(!begin)return;begin+=strlen(needle);end=strchr(begin,'"');
    if(!end)return;
    n=(size_t)(end-begin);if(n>=cap)n=cap-1;memcpy(out,begin,n);out[n]=0;
}
static unsigned fake_catalog_read(const char *path,char tags[][128],uint64_t created[],uint64_t states[],unsigned maximum) {
    FILE *file;char line[256],tag[128];unsigned count=0;unsigned long long when,state;
    if(!path||!*path||(file=fopen(path,"rb"))==NULL)return 0;
    while(count<maximum&&fgets(line,sizeof(line),file)){
        if(sscanf(line,"%127[^\t]\t%llu\t%llu",tag,&when,&state)!=3)continue;
        snprintf(tags[count],128,"%s",tag);created[count]=(uint64_t)when;states[count]=(uint64_t)state;count++;
    }
    fclose(file);return count;
}
static void fake_catalog_write(const char *path,char tags[][128],uint64_t created[],uint64_t states[],unsigned count) {
    FILE *file;unsigned i;
    if(!path||!*path)return;
    file=fopen(path,"wb");assert(file);
    for(i=0;i<count;i++)fprintf(file,"%s\t%llu\t%llu\n",tags[i],(unsigned long long)created[i],(unsigned long long)states[i]);
    fclose(file);
}
static void fake_catalog_seed(const char *path,const char *tag,uint64_t created,uint64_t state) {
    char tags[64][128];uint64_t dates[64],states[64];unsigned count=fake_catalog_read(path,tags,dates,states,64),i;
    for(i=0;i<count;i++)if(!strcmp(tags[i],tag)){dates[i]=created;states[i]=state;fake_catalog_write(path,tags,dates,states,count);return;}
    if(count==64)return;
    snprintf(tags[count],sizeof(tags[count]),"%s",tag);dates[count]=created;states[count]=state;
    fake_catalog_write(path,tags,dates,states,count+1);
}
static void fake_catalog_change(const char *path,const char *operation,const char *tag) {
    char tags[64][128];uint64_t dates[64],states[64];unsigned count=fake_catalog_read(path,tags,dates,states,64),i;
    if(!path||!*path||!tag||!*tag)return;
    for(i=0;i<count;i++)if(!strcmp(tags[i],tag))break;
    if(!strcmp(operation,"snapshot-save")){
        if(i<count||count==64)return;
        snprintf(tags[count],sizeof(tags[count]),"%s",tag);dates[count]=(uint64_t)time(NULL)+count;states[count]=511160;count++;
    }else if(!strcmp(operation,"snapshot-delete")&&i<count){
        memmove(tags[i],tags[i+1],(count-i-1)*sizeof(tags[0]));
        memmove(dates+i,dates+i+1,(count-i-1)*sizeof(dates[0]));
        memmove(states+i,states+i+1,(count-i-1)*sizeof(states[0]));count--;
    }
    fake_catalog_write(path,tags,dates,states,count);
}
static void fake_catalog_json(const char *path,const char *mode,char *out,size_t cap) {
    char tags[64][128];uint64_t dates[64],states[64];unsigned count=fake_catalog_read(path,tags,dates,states,64),i;size_t used=0;
    if(!cap)return;
    out[0]=0;
    if(mode&&!strcmp(mode,"malformed")){
        snprintf(out,cap,",\"snapshots\":\"invalid\"");return;
    }
    if(snprintf(out,cap,",\"snapshots\":[")>=(int)cap)return;
    used=strlen(out);
    for(i=0;i<count;i++){
        int n=snprintf(out+used,cap-used,"%s{\"name\":\"%s\",\"vm-state-size\":%llu,\"date-sec\":%llu,\"date-nsec\":0}",
            i?",":"",tags[i],(unsigned long long)states[i],(unsigned long long)dates[i]);
        if(n<0||(size_t)n>=cap-used){out[0]=0;return;}used+=(size_t)n;
    }
    if(mode&&!strcmp(mode,"overflow")){
        int n=snprintf(out+used,cap-used,"%s{\"name\":\"foreign-overflow\",\"vm-state-size\":511160,\"date-sec\":9999999999,\"date-nsec\":0}",count?",":"");
        if(n<0||(size_t)n>=cap-used){out[0]=0;return;}used+=(size_t)n;
    }
    if(used+2>cap){out[0]=0;return;}out[used++]=']';out[used]=0;
}
static void fake_block_reply(char *reply,size_t cap,unsigned serial,const char *mode,
                             const char *control,unsigned query_count,
                             unsigned pressure_deletes,int overshoot_saved,const char *catalog_json) {
    char stage[32],metric[64]="1024";const char *driver="qcow2",*format="qcow2";
    uint64_t actual=TEST_DISK_BASE,foreign_actual=TEST_DISK_BASE+TEST_SNAPSHOT_BUDGET+1;
    int omit=0;
    fake_control_stage(control,stage,sizeof(stage));
    if(!strcmp(mode,"pressure")&&!strcmp(stage,"pressure")) {
        actual=pressure_deletes==0?TEST_DISK_BASE+TEST_SNAPSHOT_BUDGET+1:
               pressure_deletes==1?TEST_DISK_BASE+TEST_SNAPSHOT_BUDGET-(8ULL*1024*1024):TEST_DISK_BASE+4096;
        snprintf(metric,sizeof(metric),"%llu",(unsigned long long)actual);
    } else if(!strcmp(mode,"stuck-reuse")&&!strcmp(stage,"pressure")) {
        actual=TEST_DISK_BASE+TEST_SNAPSHOT_BUDGET+1;
        snprintf(metric,sizeof(metric),"%llu",(unsigned long long)actual);
    } else if(!strcmp(mode,"overshoot")&&!strcmp(stage,"overshoot")&&overshoot_saved) {
        actual=TEST_DISK_BASE+TEST_SNAPSHOT_BUDGET+1;
        snprintf(metric,sizeof(metric),"%llu",(unsigned long long)actual);
    }
    if(!strcmp(mode,"startup-missing")&&query_count==1)omit=1;
    if(query_count==2) {
        if(!strcmp(mode,"metric-missing"))omit=1;
        else if(!strcmp(mode,"metric-negative"))strcpy(metric,"-1");
        else if(!strcmp(mode,"metric-malformed"))strcpy(metric,"\"not-a-number\"");
        else if(!strcmp(mode,"metric-decimal"))strcpy(metric,"1024.0");
        else if(!strcmp(mode,"metric-exponent"))strcpy(metric,"1e3");
        else if(!strcmp(mode,"metric-overflow"))strcpy(metric,"9223372036854775808");
        else if(!strcmp(mode,"metric-driver"))driver="raw";
        else if(!strcmp(mode,"metric-format"))format="raw";
    }
    if(!strcmp(mode,"node-selection")) {
        if(query_count>=2)foreign_actual=TEST_DISK_BASE+2*TEST_SNAPSHOT_BUDGET+2;
        snprintf(reply,cap,"{\"return\":[{\"device\":\"foreign\",\"inserted\":{\"node-name\":\"foreign-disk\",\"drv\":\"qcow2\",\"image\":{\"format\":\"qcow2\",\"actual-size\":%llu}}},{\"device\":\"megasdisk\",\"inserted\":{\"node-name\":\"megasdisk\",\"drv\":\"qcow2\",\"image\":{\"format\":\"qcow2\",\"actual-size\":1024%s}}}],\"id\":%u}\n",
                 (unsigned long long)foreign_actual,catalog_json?catalog_json:"",serial);
    } else if(omit) {
        snprintf(reply,cap,"{\"return\":[{\"device\":\"megasdisk\",\"inserted\":{\"node-name\":\"megasdisk\",\"drv\":\"qcow2\",\"image\":{\"format\":\"qcow2\"}}}],\"id\":%u}\n",serial);
    } else {
        snprintf(reply,cap,"{\"return\":[{\"device\":\"megasdisk\",\"inserted\":{\"node-name\":\"megasdisk\",\"drv\":\"%s\",\"image\":{\"format\":\"%s\",\"actual-size\":%s%s}}}],\"id\":%u}\n",driver,format,metric,catalog_json?catalog_json:"",serial);
    }
}

static int fake_qemu(int argc,char **argv) {
    unsigned port=0,query_blocks=0,pressure_deletes=0;MgpSocket listener,client;int i,overshoot_saved=0;char line[65536],reply[65536],job[64]="",tag[128]="",mode[64]="normal",control[1024]="",trace[1024]="",catalog[1024]="",catalog_mode[64]="",catalog_json[16384];size_t used=0;int running=1;
    if(mgp_env("MEGAS_TEST_QMP_MODE",mode,sizeof(mode)))strcpy(mode,"normal");
    if(mgp_env("MEGAS_TEST_QMP_CONTROL",control,sizeof(control)))control[0]=0;
    if(mgp_env("MEGAS_TEST_QMP_TRACE",trace,sizeof(trace)))trace[0]=0;
    if(mgp_env("MEGAS_TEST_QMP_CATALOG",catalog,sizeof(catalog)))catalog[0]=0;
    if(mgp_env("MEGAS_TEST_QMP_CATALOG_MODE",catalog_mode,sizeof(catalog_mode)))catalog_mode[0]=0;
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
        else if(strstr(line,"\"execute\":\"query-block\"")){query_blocks++;fake_trace(trace,"query-block","");fake_catalog_json(catalog,catalog_mode,catalog_json,sizeof(catalog_json));fake_block_reply(reply,sizeof(reply),serial,mode,control,query_blocks,pressure_deletes,overshoot_saved,catalog_json);}
        else if(strstr(line,"query-status"))snprintf(reply,sizeof(reply),"{\"return\":{\"status\":\"%s\",\"running\":%s},\"id\":%u}\n",running?"running":"paused",running?"true":"false",serial);
        else if(strstr(line,"snapshot-")&&strstr(line,"job-id")){
            char stage[32];
            char *begin=strstr(line,"\"job-id\":\"")+10,*close=strchr(begin,'"');assert(close);size_t length=(size_t)(close-begin);assert(length<sizeof(job));memcpy(job,begin,length);job[length]=0;
            fake_field(line,"tag",tag,sizeof(tag));
            if(strstr(line,"\"execute\":\"snapshot-save\"")){
                fake_trace(trace,"snapshot-save",tag);
                fake_catalog_change(catalog,"snapshot-save",tag);
                if(!strcmp(mode,"overshoot")&&!strcmp(tag,"megas-owned:cannot-fit"))overshoot_saved=1;
            }else if(strstr(line,"\"execute\":\"snapshot-delete\"")){
                fake_trace(trace,"snapshot-delete",tag);
                fake_catalog_change(catalog,"snapshot-delete",tag);
                fake_control_stage(control,stage,sizeof(stage));
                if(!strcmp(mode,"pressure")&&!strcmp(stage,"pressure"))pressure_deletes++;
            }else if(strstr(line,"\"execute\":\"snapshot-load\""))fake_trace(trace,"snapshot-load",tag);
            if(strstr(line,"rejectlate")){mgp_sleep(650);snprintf(reply,sizeof(reply),"{\"error\":{\"class\":\"GenericError\",\"desc\":\"Delayed snapshot rejection\"},\"id\":%u}\n",serial);job[0]=0;}
            else snprintf(reply,sizeof(reply),"{\"event\":\"STOP\",\"data\":{}}\n{\"return\":{},\"id\":%u}\n{\"event\":\"JOB_STATUS_CHANGE\",\"data\":{\"id\":\"%s\",\"status\":\"concluded\"}}\n",serial,job);
        }
        else if(strstr(line,"query-jobs"))snprintf(reply,sizeof(reply),"{\"return\":[{\"id\":\"%s\",\"status\":\"concluded\",\"current-progress\":1,\"total-progress\":1}],\"id\":%u}\n",job,serial);
        else {if(strstr(line,"\"execute\":\"stop\""))running=0;if(strstr(line,"\"execute\":\"cont\""))running=1;snprintf(reply,sizeof(reply),"{\"return\":{},\"id\":%u}\n",serial);}
        assert(!mgp_send_all(client,reply,strlen(reply),100));size_t consumed=(size_t)(end-line)+1;memmove(line,line+consumed,used-consumed);used-=consumed;
    }
    mgp_close(client);mgp_close(listener);return 0;
}

static void budget_control(BudgetFixture *fixture,const char *stage) {
    FILE *file=fopen(fixture->control,"wb");assert(file);fputs(stage,file);fputc('\n',file);fclose(file);
}
static void budget_start(BudgetFixture *fixture,const char *mode,unsigned ram_mb,char *response,size_t response_cap) {
    MgConfig cfg;char template[]="/tmp/megas-budget-XXXXXX";FILE *file;char *runtime=mkdtemp(template);
    assert(runtime);memset(fixture,0,sizeof(*fixture));snprintf(fixture->runtime,sizeof(fixture->runtime),"%s",runtime);
    assert(strlen(fixture->runtime)+sizeof("/qmp-control")<=sizeof(fixture->control));
    strcpy(fixture->control,fixture->runtime);strcat(fixture->control,"/qmp-control");
    assert(strlen(fixture->runtime)+sizeof("/qmp-trace")<=sizeof(fixture->trace));
    strcpy(fixture->trace,fixture->runtime);strcat(fixture->trace,"/qmp-trace");
    assert(strlen(fixture->runtime)+sizeof("/qmp-catalog")<=sizeof(fixture->catalog));
    strcpy(fixture->catalog,fixture->runtime);strcat(fixture->catalog,"/qmp-catalog");
    budget_control(fixture,"normal");
    mg_config_defaults(&cfg);snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",fixture->runtime);
    snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",test_program);
    assert(strlen(fixture->runtime)+sizeof("/disk.qcow2")<=sizeof(cfg.disk));
    strcpy(cfg.disk,fixture->runtime);strcat(cfg.disk,"/disk.qcow2");
    cfg.control_port=test_port;cfg.qmp_port=test_port+1;cfg.vnc_port=test_port+2;cfg.ram_mb=ram_mb;snprintf(cfg.disk_bus,sizeof(cfg.disk_bus),"virtio");cfg.width=800;cfg.height=600;test_port+=10;
    file=fopen(cfg.disk,"wb");assert(file);fputs("fake qcow2 fixture",file);fclose(file);
    file=fopen(fixture->catalog,"wb");assert(file);fclose(file);
    assert(!setenv("MEGAS_TEST_QMP_MODE",mode,1));assert(!setenv("MEGAS_TEST_QMP_CONTROL",fixture->control,1));assert(!setenv("MEGAS_TEST_QMP_TRACE",fixture->trace,1));assert(!setenv("MEGAS_TEST_QMP_CATALOG",fixture->catalog,1));unsetenv("MEGAS_TEST_QMP_CATALOG_MODE");
    fixture->engine=mg_engine_new(&cfg);assert(fixture->engine);
    assert(!mg_engine_submit(fixture->engine,"{\"action\":\"start\"}",response,response_cap));
    assert(strstr(response,"\"connected\":true"));
}
static void budget_stop(BudgetFixture *fixture,char *response,size_t response_cap) {
    assert(!mg_engine_submit(fixture->engine,"{\"action\":\"stop\"}",response,response_cap));
    mg_engine_free(fixture->engine);fixture->engine=NULL;
    unsetenv("MEGAS_TEST_QMP_MODE");unsetenv("MEGAS_TEST_QMP_CONTROL");unsetenv("MEGAS_TEST_QMP_TRACE");unsetenv("MEGAS_TEST_QMP_CATALOG");unsetenv("MEGAS_TEST_QMP_CATALOG_MODE");
}
static void budget_close_owner(BudgetFixture *fixture) {
    char response[65536];
    assert(!mg_engine_submit(fixture->engine,"{\"action\":\"stop\"}",response,sizeof(response)));
    mg_engine_free(fixture->engine);fixture->engine=NULL;
}
static void budget_reowner_start(BudgetFixture *fixture,const char *mode,unsigned ram_mb,char *response,size_t response_cap) {
    MgConfig cfg;
    memset(&cfg,0,sizeof(cfg));mg_config_defaults(&cfg);
    snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",fixture->runtime);
    snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",test_program);
    assert(strlen(fixture->runtime)+sizeof("/disk.qcow2")<=sizeof(cfg.disk));
    strcpy(cfg.disk,fixture->runtime);strcat(cfg.disk,"/disk.qcow2");
    cfg.control_port=test_port;cfg.qmp_port=test_port+1;cfg.vnc_port=test_port+2;cfg.ram_mb=ram_mb;snprintf(cfg.disk_bus,sizeof(cfg.disk_bus),"virtio");cfg.width=800;cfg.height=600;test_port+=10;
    assert(!setenv("MEGAS_TEST_QMP_MODE",mode,1));
    assert(!setenv("MEGAS_TEST_QMP_CATALOG",fixture->catalog,1));
    fixture->engine=mg_engine_new(&cfg);assert(fixture->engine);
    assert(!mg_engine_submit(fixture->engine,"{\"action\":\"start\"}",response,response_cap));
    assert(strstr(response,"\"connected\":true"));
}
static int budget_checkpoint(BudgetFixture *fixture,const char *name,char *response,size_t response_cap) {
    char request[160];snprintf(request,sizeof(request),"{\"action\":\"checkpoint\",\"name\":\"%s\"}",name);
    return mg_engine_submit(fixture->engine,request,response,response_cap);
}
static void budget_steps(BudgetFixture *fixture,char *response,size_t response_cap) {
    assert(!mg_engine_submit(fixture->engine,"{\"action\":\"steps\"}",response,response_cap));
}
static void read_text_file(const char *path,char *text,size_t cap) {
    FILE *file=fopen(path,"rb");size_t n;assert(file);n=fread(text,1,cap-1,file);text[n]=0;fclose(file);
}
static unsigned occurrences(const char *text,const char *needle) {
    unsigned count=0;size_t n=strlen(needle);const char *p=text;
    while((p=strstr(p,needle))!=NULL){count++;p+=n;}
    return count;
}
static const char *within(const char *text,const char *first,const char *second,size_t window) {
    const char *p=strstr(text,first),*q;
    if(!p)return NULL;
    q=strstr(p,second);
    return q&&(size_t)(q-p)<window?q:NULL;
}
static void test_vm_profile_reuse(void) {
    MgConfig cfg;MgEngine *engine;char runtime[]="/tmp/megas-profile-XXXXXX",response[65536],request[2048],journal[65536];
    char control[1024],trace[1024],catalog[1024];FILE *file;int n;
    assert(mkdtemp(runtime));
    snprintf(control,sizeof(control),"%s/qmp-control",runtime);
    snprintf(trace,sizeof(trace),"%s/qmp-trace",runtime);
    snprintf(catalog,sizeof(catalog),"%s/qmp-catalog",runtime);
    file=fopen(control,"wb");assert(file);fclose(file);
    file=fopen(catalog,"wb");assert(file);fclose(file);
    assert(!setenv("MEGAS_TEST_QMP_MODE","normal",1));
    assert(!setenv("MEGAS_TEST_QMP_CONTROL",control,1));
    assert(!setenv("MEGAS_TEST_QMP_TRACE",trace,1));
    assert(!setenv("MEGAS_TEST_QMP_CATALOG",catalog,1));

    mg_config_defaults(&cfg);assert(cfg.ram_mb==0);assert(cfg.disk_bus[0]==0);
    snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);
    snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",test_program);
    snprintf(cfg.disk,sizeof(cfg.disk),"%s/disk.qcow2",runtime);
    file=fopen(cfg.disk,"wb");assert(file);fputs("fake qcow2 fixture",file);fclose(file);
    cfg.control_port=test_port;cfg.qmp_port=test_port+1;cfg.vnc_port=test_port+2;cfg.width=800;cfg.height=600;test_port+=10;
    engine=mg_engine_new(&cfg);assert(engine);

    n=snprintf(request,sizeof(request),"{\"action\":\"preflight\",\"disk\":\"%s\"}",cfg.disk);
    assert(n>0&&(size_t)n<sizeof(request));
    assert(mg_engine_submit(engine,request,response,sizeof(response)));
    assert(strstr(response,"No recorded profile"));
    /* preflight asks for the values it cannot know instead of inventing them */
    assert(strstr(response,"Ask the user for ram_mb"));
    assert(strstr(response,"\"image_exists\":true"));
    assert(strstr(response,"\"profile_known\":false"));
    assert(mg_engine_submit(engine,"{\"action\":\"start\"}",response,sizeof(response)));
    assert(strstr(response,"VM memory is not established"));

    n=snprintf(request,sizeof(request),"{\"action\":\"start\",\"ram_mb\":4096,\"bus\":\"ide\"}");
    assert(n>0&&(size_t)n<sizeof(request));
    assert(!mg_engine_submit(engine,request,response,sizeof(response)));
    assert(strstr(response,"\"connected\":true"));
    assert(strstr(response,"\"memory_mb\":4096"));
    assert(strstr(response,"\"bus\":\"ide\""));
    assert(strstr(response,"\"image_exists\":true"));
    assert(!mg_engine_submit(engine,"{\"action\":\"stop\"}",response,sizeof(response)));
    /* another disk never inherits this VM's memory or controller */
    n=snprintf(request,sizeof(request),"%s/other.qcow2",runtime);
    assert(n>0&&(size_t)n<sizeof(request));
    file=fopen(request,"wb");assert(file);fputs("another fake qcow2",file);fclose(file);
    n=snprintf(request,sizeof(request),"{\"action\":\"start\",\"disk\":\"%s/other.qcow2\"}",runtime);
    assert(n>0&&(size_t)n<sizeof(request));
    assert(mg_engine_submit(engine,request,response,sizeof(response)));
    assert(strstr(response,"VM memory is not established"));
    assert(strstr(response,"never reuses another disk's memory"));
    /* restarting the same disk keeps its recorded memory */
    n=snprintf(request,sizeof(request),"{\"action\":\"start\",\"disk\":\"%s\"}",cfg.disk);
    assert(n>0&&(size_t)n<sizeof(request));
    assert(!mg_engine_submit(engine,request,response,sizeof(response)));
    assert(strstr(response,"\"memory_mb\":4096"));
    assert(strstr(response,"\"bus\":\"ide\""));
    assert(!mg_engine_submit(engine,"{\"action\":\"stop\"}",response,sizeof(response)));
    mg_engine_free(engine);

    mg_config_defaults(&cfg);assert(cfg.ram_mb==0);assert(cfg.disk_bus[0]==0);
    snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);
    snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",test_program);
    snprintf(cfg.disk,sizeof(cfg.disk),"%s/disk.qcow2",runtime);
    cfg.control_port=test_port;cfg.qmp_port=test_port+1;cfg.vnc_port=test_port+2;cfg.width=800;cfg.height=600;test_port+=10;
    engine=mg_engine_new(&cfg);assert(engine);
    assert(!mg_engine_submit(engine,"{\"action\":\"start\"}",response,sizeof(response)));
    assert(strstr(response,"\"connected\":true"));
    assert(strstr(response,"\"memory_mb\":4096"));assert(!strstr(response,"\"memory_mb\":256"));
    assert(strstr(response,"\"bus\":\"ide\""));
    assert(!mg_engine_submit(engine,"{\"action\":\"stop\"}",response,sizeof(response)));
    mg_engine_free(engine);

    snprintf(request,sizeof(request),"{\"action\":\"preflight\",\"disk\":\"%s/disk.qcow2\"}",runtime);
    mg_config_defaults(&cfg);snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);
    engine=mg_engine_new(&cfg);assert(engine);
    assert(!mg_engine_submit(engine,request,response,sizeof(response)));
    assert(strstr(response,"4096"));assert(strstr(response,"ide"));
    mg_engine_free(engine);

    snprintf(request,sizeof(request),"%s/actions.jsonl",runtime);
    read_text_file(request,journal,sizeof(journal));
    assert(strstr(journal,"ram_mb\\\":4096"));
    assert(strstr(journal,"ide-hd,drive=megasdisk"));
    /* the audit record of the start itself states the memory it applied */
    assert(within(journal,"\\\"action\\\":\\\"start\\\",\\\"ram_mb\\\":4096","\\\"effective_ram_mb\\\":4096",4096));
    assert(within(journal,"\\\"action\\\":\\\"start\\\",\\\"ram_mb\\\":4096","\\\"effective_bus\\\":\\\"ide\\\"",4096));
    /* a refused start records why preflight must answer first */
    assert(within(journal,"\\\"action\\\":\\\"start\\\"","preflight_required",8192));
    assert(strstr(journal,"\"type\":\"preflight\""));
    unsetenv("MEGAS_TEST_QMP_MODE");unsetenv("MEGAS_TEST_QMP_CONTROL");unsetenv("MEGAS_TEST_QMP_TRACE");unsetenv("MEGAS_TEST_QMP_CATALOG");
    puts("engine: unknown VM profile fails closed; explicit RAM/bus is journaled and reused instead of a 256 MB default");
}
static void test_checkpoint_node_selection(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"node-selection",16,response,sizeof(response));
    assert(strstr(response,"\"checkpoint_budget_baseline_available\":true"));
    assert(strstr(response,"\"checkpoint_budget_bytes\":2147483648"));
    assert(!budget_checkpoint(&fixture,"exact-node",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(strstr(response,"exact-node"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-save megas-owned:exact-node")==1);
    puts("engine: allocated-size budget selects the exact megasdisk qcow2 node");
}
static void test_checkpoint_bad_metric(const char *mode) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,mode,16,response,sizeof(response));
    assert(budget_checkpoint(&fixture,"metric-rejected",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(!strstr(response,"metric-rejected"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"query-block")==2);assert(!strstr(trace,"snapshot-save"));
    printf("engine: allocated-size metric '%s' fails closed before save\n",mode);
}
static void test_checkpoint_unknown_baseline(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"startup-missing",16,response,sizeof(response));
    assert(strstr(response,"\"checkpoint_budget_baseline_available\":false"));
    assert(budget_checkpoint(&fixture,"unknown-baseline",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(strstr(response,"\"retained_checkpoints\":0"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"query-block")==1);assert(!strstr(trace,"snapshot-save"));
    puts("engine: missing startup allocation baseline remains explicitly unavailable");
}
static void test_checkpoint_pressure_eviction(void) {
    BudgetFixture fixture;char response[65536],trace[65536];char *first,*second,*save;
    budget_start(&fixture,"pressure",16,response,sizeof(response));
    assert(!budget_checkpoint(&fixture,"pressure-old-1",response,sizeof(response)));
    assert(!budget_checkpoint(&fixture,"pressure-old-2",response,sizeof(response)));
    budget_control(&fixture,"pressure");
    assert(!budget_checkpoint(&fixture,"pressure-new",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));
    assert(strstr(response,"pressure-new"));assert(!strstr(response,"pressure-old-1"));assert(!strstr(response,"pressure-old-2"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    first=strstr(trace,"snapshot-delete megas-owned:pressure-old-1");second=strstr(trace,"snapshot-delete megas-owned:pressure-old-2");save=strstr(trace,"snapshot-save megas-owned:pressure-new");
    assert(first&&second&&save&&first<second&&second<save);
    assert(occurrences(trace,"snapshot-delete megas-owned:pressure-old-1")==1);assert(occurrences(trace,"snapshot-delete megas-owned:pressure-old-2")==1);assert(occurrences(trace,"query-block")==9);
    puts("engine: allocated-size pressure remeasures after bounded oldest-owned eviction");
}
static void test_checkpoint_one_reuse_is_bounded(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"stuck-reuse",16,response,sizeof(response));
    assert(!budget_checkpoint(&fixture,"reuse-old",response,sizeof(response)));
    budget_control(&fixture,"pressure");
    assert(budget_checkpoint(&fixture,"reuse-new",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(strstr(response,"\"retained_checkpoints\":0"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-save megas-owned:reuse-new")==1);
    assert(occurrences(trace,"snapshot-delete megas-owned:reuse-old")==1);
    assert(occurrences(trace,"snapshot-delete megas-owned:reuse-new")==1);
    assert(occurrences(trace,"query-block")==6);
    puts("engine: unchanged allocation gets one verified reuse attempt, then bounded rollback");
}
static void test_checkpoint_single_item_cap(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"overshoot",16,response,sizeof(response));budget_control(&fixture,"overshoot");
    assert(budget_checkpoint(&fixture,"cannot-fit",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(strstr(response,"\"retained_checkpoints\":0"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-save megas-owned:cannot-fit")==1);assert(occurrences(trace,"snapshot-delete megas-owned:cannot-fit")==1);
    assert(occurrences(trace,"query-block")<=3);
    puts("engine: a checkpoint that exceeds the hard cap is removed and reported failed");
}
static void test_checkpoint_catalog_reowner(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"normal",16,response,sizeof(response));
    assert(!budget_checkpoint(&fixture,"past",response,sizeof(response)));
    assert(!budget_checkpoint(&fixture,"future",response,sizeof(response)));
    budget_close_owner(&fixture);
    fake_catalog_seed(fixture.catalog,"legacy-foreign",10,511160);
    fake_catalog_seed(fixture.catalog,"disk-only-foreign",11,0);
    fake_catalog_seed(fixture.catalog,"megas-owned-important",12,511160);
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"checkpoint_catalog_available\":true"));
    assert(strstr(response,"\"owned_checkpoints\":2"));
    budget_steps(&fixture,response,sizeof(response));
    assert(strstr(response,"\"name\":\"past\""));assert(strstr(response,"\"name\":\"future\""));
    assert(strstr(response,"\"name\":\"legacy-foreign\",\"step\":0,\"owned\":false,\"restorable\":true"));
    assert(strstr(response,"\"name\":\"disk-only-foreign\",\"step\":0,\"owned\":false,\"restorable\":false"));
    assert(strstr(response,"\"name\":\"megas-owned-important\",\"step\":0,\"owned\":false,\"restorable\":true"));
    assert(mg_engine_submit(fixture.engine,"{\"action\":\"restore\",\"name\":\"disk-only-foreign\"}",response,sizeof(response)));
    assert(strstr(response,"\"connected\":true"));
    assert(!mg_engine_submit(fixture.engine,"{\"action\":\"restore\",\"name\":\"past\"}",response,sizeof(response)));
    assert(!mg_engine_submit(fixture.engine,"{\"action\":\"restore\",\"name\":\"legacy-foreign\"}",response,sizeof(response)));
    assert(!mg_engine_submit(fixture.engine,"{\"action\":\"restore\",\"name\":\"megas-owned-important\"}",response,sizeof(response)));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-load megas-owned:past")==1);
    assert(occurrences(trace,"snapshot-load legacy-foreign")==1);
    assert(occurrences(trace,"snapshot-load megas-owned-important")==1);
    assert(!strstr(trace,"snapshot-load disk-only-foreign"));
    puts("engine: a new owner imports managed and foreign checkpoints, maps labels, and restores only verified VM states");
}
static void test_checkpoint_catalog_automatic_name_collision(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"normal",16,response,sizeof(response));
    assert(!budget_checkpoint(&fixture,"step-2-before",response,sizeof(response)));
    budget_close_owner(&fixture);
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"checkpoint_catalog_available\":true"));
    assert(!mg_engine_submit(fixture.engine,"{\"action\":\"pause\"}",response,sizeof(response)));
    assert(!mg_engine_submit(fixture.engine,"{\"action\":\"steps\"}",response,sizeof(response)));
    assert(strstr(response,"\"name\":\"step-2-before\""));
    assert(strstr(response,"\"name\":\"step-2-before-2\""));
    assert(!strstr(response,"megas-owned:"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-save megas-owned:step-2-before\n")==1);
    assert(occurrences(trace,"snapshot-save megas-owned:step-2-before-2\n")==1);
    assert(occurrences(trace,"snapshot-save megas-owned:step-2-after\n")==1);
    puts("engine: a post-restart automatic action chooses unique managed labels without replacing existing history");
}
static void test_checkpoint_catalog_foreign_never_auto_gc(void) {
    BudgetFixture fixture;char response[65536],trace[65536],tag[128];unsigned i;
    budget_start(&fixture,"normal",16,response,sizeof(response));budget_close_owner(&fixture);
    fake_catalog_seed(fixture.catalog,"megas-owned-important",1,511160);
    for(i=0;i<16;i++){
        snprintf(tag,sizeof(tag),"megas-owned:seed-%02u",i);
        fake_catalog_seed(fixture.catalog,tag,100+i,511160);
    }
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"owned_checkpoints\":16"));assert(strstr(response,"\"retained_checkpoints\":17"));
    assert(!budget_checkpoint(&fixture,"replacement",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));
    assert(strstr(response,"\"name\":\"megas-owned-important\""));
    assert(!strstr(response,"\"name\":\"seed-00\""));
    assert(strstr(response,"\"name\":\"replacement\""));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(occurrences(trace,"snapshot-delete megas-owned:seed-00")==1);
    assert(!strstr(trace,"snapshot-delete megas-owned-important"));
    puts("engine: retention pressure evicts the oldest owned state and never auto-deletes a foreign state");
}
static void test_checkpoint_catalog_full_foreign_fails_closed(void) {
    BudgetFixture fixture;char response[65536],trace[65536],tag[128];unsigned i;
    budget_start(&fixture,"normal",16,response,sizeof(response));budget_close_owner(&fixture);
    for(i=0;i<64;i++){snprintf(tag,sizeof(tag),"foreign-%02u",i);fake_catalog_seed(fixture.catalog,tag,100+i,511160);}
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"checkpoint_catalog_available\":true"));assert(strstr(response,"\"retained_checkpoints\":64"));
    assert(budget_checkpoint(&fixture,"no-slot",response,sizeof(response)));
    budget_steps(&fixture,response,sizeof(response));assert(strstr(response,"foreign-00"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(!strstr(trace,"snapshot-save megas-owned:no-slot"));assert(!strstr(trace,"snapshot-delete"));
    puts("engine: a full catalog of foreign states is preserved and rejects new saves without eviction");
}
static void test_checkpoint_catalog_unavailable_keeps_vm(void) {
    BudgetFixture fixture;char response[65536],trace[65536];
    budget_start(&fixture,"normal",16,response,sizeof(response));budget_close_owner(&fixture);
    assert(!setenv("MEGAS_TEST_QMP_CATALOG_MODE","malformed",1));
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"connected\":true"));assert(strstr(response,"\"owned_pid\":\"" )==NULL);
    assert(strstr(response,"\"checkpoint_catalog_available\":false"));assert(strstr(response,"\"rollback_available\":false"));
    assert(mg_engine_submit(fixture.engine,"{\"action\":\"checkpoint\",\"name\":\"blocked\"}",response,sizeof(response)));
    assert(strstr(response,"\"connected\":true"));assert(strstr(response,"existing states were not assumed empty"));
    budget_stop(&fixture,response,sizeof(response));read_text_file(fixture.trace,trace,sizeof(trace));
    assert(!strstr(trace,"snapshot-save"));
    puts("engine: malformed QMP catalog disables checkpoint claims while leaving the VM available");
}
static void test_checkpoint_catalog_over_limit_keeps_vm(void) {
    BudgetFixture fixture;char response[65536],tag[128];unsigned i;
    budget_start(&fixture,"normal",16,response,sizeof(response));budget_close_owner(&fixture);
    for(i=0;i<64;i++){snprintf(tag,sizeof(tag),"foreign-%02u",i);fake_catalog_seed(fixture.catalog,tag,100+i,511160);}
    assert(!setenv("MEGAS_TEST_QMP_CATALOG_MODE","overflow",1));
    budget_reowner_start(&fixture,"normal",16,response,sizeof(response));
    assert(strstr(response,"\"connected\":true"));assert(strstr(response,"\"checkpoint_catalog_available\":false"));
    assert(strstr(response,"\"rollback_available\":false"));
    assert(mg_engine_submit(fixture.engine,"{\"action\":\"checkpoint\",\"name\":\"blocked\"}",response,sizeof(response)));
    assert(strstr(response,"\"connected\":true"));
    budget_stop(&fixture,response,sizeof(response));
    puts("engine: an over-limit QMP catalog fails closed for checkpointing without stopping the VM");
}

int main(int argc,char **argv) {
    MgConfig cfg;MgEngine *e;char runtime[]="/tmp/megas-engine-XXXXXX",response[65536],request[2048],self[1024],cwd[1024];FILE *file;uint64_t begun;
    if(argc>1)return fake_qemu(argc,argv);
    assert(mkdtemp(runtime));assert(getcwd(cwd,sizeof(cwd)));
    if(argv[0][0]=='/')snprintf(self,sizeof(self),"%s",argv[0]);else snprintf(self,sizeof(self),"%.700s/%.300s",cwd,argv[0]);
    snprintf(test_program,sizeof(test_program),"%s",self);
    mg_config_defaults(&cfg);snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);snprintf(cfg.qemu,sizeof(cfg.qemu),"%s",self);
    cfg.control_port=52601;cfg.qmp_port=52602;cfg.vnc_port=52603;cfg.ram_mb=16;snprintf(cfg.disk_bus,sizeof(cfg.disk_bus),"virtio");cfg.width=800;cfg.height=600;
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
    puts("engine: QMP event interleave, aliases, state, delayed error recovery, auth rejection, journal and lifecycle passed");
    test_checkpoint_node_selection();
    test_checkpoint_bad_metric("metric-missing");test_checkpoint_bad_metric("metric-negative");
    test_checkpoint_bad_metric("metric-malformed");test_checkpoint_bad_metric("metric-decimal");
    test_checkpoint_bad_metric("metric-exponent");test_checkpoint_bad_metric("metric-overflow");
    test_checkpoint_bad_metric("metric-driver");test_checkpoint_bad_metric("metric-format");
    test_checkpoint_unknown_baseline();
    test_checkpoint_pressure_eviction();test_checkpoint_one_reuse_is_bounded();test_checkpoint_single_item_cap();
    test_checkpoint_catalog_reowner();test_checkpoint_catalog_automatic_name_collision();
    test_checkpoint_catalog_foreign_never_auto_gc();test_checkpoint_catalog_full_foreign_fails_closed();
    test_checkpoint_catalog_unavailable_keeps_vm();test_checkpoint_catalog_over_limit_keeps_vm();
    test_vm_profile_reuse();
    return 0;
}
