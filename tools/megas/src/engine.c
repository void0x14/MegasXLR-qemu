/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "megas.h"
#include "platform.h"
#include "guest.h"
#include "seed.h"
#include "live.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

#define LINE_CAP 65536
#define REQUEST_CAP 8192
#define TOKEN_CAP 4096
#define HISTORY_CAP 128
#define SNAPSHOT_CAP 16
#define SNAPSHOT_CATALOG_CAP 64
#define SNAPSHOT_TAG_CAP 128
#define SNAPSHOT_MANAGED_PREFIX "megas-owned:"
/* Retain at least two full RAM snapshots plus bounded qcow2 metadata. */
#define SNAPSHOT_METADATA_RESERVE (64ULL * 1024 * 1024)
#define JOURNAL_CAP (64ULL * 1024 * 1024)
#define SNAPSHOT_BUDGET_FLOOR (2ULL * 1024 * 1024 * 1024)
#define FREE_RESERVE (512ULL * 1024 * 1024)
#define COMMAND_MS 4500
#define OCR_MEMORY_BYTES (48ULL * 1024 * 1024)
#define OCR_WALL_MS 10000
#define OCR_TEXT_CAP 4096

enum { OCR_NEVER, OCR_RUNNING, OCR_COMPLETE, OCR_UNAVAILABLE, OCR_FAILED, OCR_TIMEOUT, OCR_STALE };

typedef struct { int start, end, parent, type;
 } JsonToken;

typedef struct { const char *s;
 JsonToken t[TOKEN_CAP];
 int n, pos, depth;
 } Json;

typedef struct { char *s;
 size_t cap, n;
 int error;
 } Buffer;

typedef struct { uint64_t step;
 char name[64], tag[SNAPSHOT_TAG_CAP];
 int owned, restorable;
 } Snapshot;

typedef struct { uint64_t step, ms;
 int ok;
 char action[32], before[64], after[64];
 } History;

/* Cached public capabilities; the private accessibility tree stays outside. */
enum { ANDROID_TARGET_MAX=256, ANDROID_HISTORY_MAX=16 };
typedef struct {
    char id[257];
    unsigned char allowed;
} MgAndroidTarget;

typedef struct {
    uint64_t revision, guest_ms, host_ms, reported_cause;
    unsigned targets;
} MgAndroidEvent;

typedef struct { MgpSocket socket;
 char request[REQUEST_CAP];
 size_t used;
 uint64_t accepted;
 char *response;
 size_t response_used, response_sent;
 } Client;

struct MgEngine {
    MgConfig cfg;

    MgStatus status;

    MgRfb *display;
    MgLive *live;
    char android_state[MG_LIVE_LINE], android_epoch[65];
    char android_outcome[32];
    MgAndroidTarget android_targets[ANDROID_TARGET_MAX];
    unsigned android_target_count;
    MgAndroidEvent android_history[ANDROID_HISTORY_MAX];
    unsigned android_history_count, android_history_next;
    uint64_t android_revision, android_received, android_generation;
    uint64_t android_action_id, android_pending, android_queued, android_completed;
    uint64_t android_events, android_rejected, resource_sample;
    uint64_t android_guest_ms, android_guest_delta_ms, android_host_delta_ms;
    uint64_t android_state_host_ms, android_cause_action_id, android_revision_gaps;
    int android_uncertain, include_image;

    MgpSocket qmp, listener;

    MgpProcess process, harnesses[4], utility, ocr_process;
    MgGuestJob guest;

    intptr_t owner_lock;

    FILE *journal;

    char token[65], token_path[1024], qemu_log[1024];

    char qmp_buffer[LINE_CAP], qmp_reply[LINE_CAP];

    size_t qmp_used;

    uint64_t next_id, pending_id, late_id, started, last_state, last_rfb, image_ms;
    uint64_t guest_step;

    int reply_ready, reply_error, qmp_greeting, snapshot_supported, auto_checkpoint;

    int job_active, job_concluded, poisoned, disposed;

    char job_id[64], job_kind[24], job_tag[SNAPSHOT_TAG_CAP], job_snapshot_name[64];
    int job_snapshot_owned;

    uint64_t job_started, disk_base, snapshot_serial, last_export_generation, last_frame, last_rfb_attempt, last_job_poll;
    int disk_base_known, snapshot_catalog_available;
    int transient_response, harness_live[4], utility_live;
    int guest_active, guest_checkpoint, guest_passwordless, guest_passwordless_before;
    int guest_password_set, guest_auto_password_set;
    int guest_auto_pending, guest_auto_running, guest_auto_attempts;
    char guest_kind[24], guest_progress[512];
    uint64_t guest_pid;
    uint64_t guest_auto_deadline, guest_auto_next, guest_auto_step;
    int guest_auto_enabled, guest_auto_checkpoint;
    char guest_auto_username[33], guest_auto_password[257], guest_auto_os[16];
    char guest_auto_marker[1024];

    int ocr_active, ocr_status;
    uint64_t ocr_generation, ocr_deadline, ocr_pid, ocr_peak_memory;
    char ocr_text[4097], ocr_output[1024], ocr_log[1024], ocr_message[256];

    Snapshot snapshots[SNAPSHOT_CATALOG_CAP];
 unsigned snapshot_count;

    History history[HISTORY_CAP];
 unsigned history_count, history_next;

    Client clients[4];

};


static void binit(Buffer *b, char *s, size_t cap) { b->s=s;
 b->cap=cap;
 b->n=0;
 b->error=0;
 if(cap)s[0]=0;
 }
static void bput(Buffer *b, const char *s) {
    size_t n=strlen(s);
 if(b->error)return;

    if(n>=b->cap-b->n){b->error=1;
return;
} memcpy(b->s+b->n,s,n+1);
b->n+=n;

}
static void bfmt(Buffer *b, const char *fmt, ...) {
    va_list ap;
 int n;
 if(b->error)return;

    va_start(ap,fmt);
n=vsnprintf(b->s+b->n,b->cap-b->n,fmt,ap);
va_end(ap);

    if(n<0 || (size_t)n>=b->cap-b->n){b->error=1;
return;
}b->n+=(size_t)n;

}
static void bstr(Buffer *b, const char *s) {
    const unsigned char *p=(const unsigned char *)s;
 bput(b,"\"");

    while(*p){switch(*p){case '"':bput(b,"\\\"");
break;
case '\\':bput(b,"\\\\");
break;

    case '\n':bput(b,"\\n");
break;
case '\r':bput(b,"\\r");
break;
case '\t':bput(b,"\\t");
break;

    default:if(*p<32)bfmt(b,"\\u%04x",*p);
else {char c[2]={(char)*p,0};
bput(b,c);
} }p++;
}bput(b,"\"");

}
static int jvalue(Json *j, int parent) {
    int token, c;
 const char *s=j->s;

    while(isspace((unsigned char)s[j->pos]))j->pos++;

    if(j->n==TOKEN_CAP || ++j->depth>32)return -1;

    token=j->n++;
j->t[token].start=j->pos;
j->t[token].parent=parent;

    c=s[j->pos++];

    if(c=='{' || c=='[') {
        int end=c=='{'?'}':']';
 j->t[token].type=c;

        while(isspace((unsigned char)s[j->pos]))j->pos++;

        if(s[j->pos]==end)j->pos++;

        else while(1) {
            if(c=='{') {
                while(isspace((unsigned char)s[j->pos]))j->pos++;

                if(s[j->pos]!='"' || jvalue(j,token)<0)return -1;

                while(isspace((unsigned char)s[j->pos]))j->pos++;

                if(s[j->pos++]!=':')return -1;

            }
            if(jvalue(j,token)<0)return -1;

            while(isspace((unsigned char)s[j->pos]))j->pos++;

            if(s[j->pos]==end){j->pos++;
break;
}
            if(s[j->pos++]!=',')return -1;

        }
    } else if(c=='"') {
        j->t[token].type='s';

        while(s[j->pos] && s[j->pos]!='"') {
            if((unsigned char)s[j->pos]<32)return -1;

            if(s[j->pos++]=='\\') {
                char escape=s[j->pos++];
int i;

                if(!escape || !strchr("\"\\/bfnrtu",escape))return -1;

                if(escape=='u')for(i=0;i<4;i++)if(!isxdigit((unsigned char)s[j->pos++]))return -1;

            }
        }
        if(s[j->pos++]!='"')return -1;

    } else {
        int begin=j->pos-1;

        j->t[token].type='p';

        if(c=='t' || c=='f' || c=='n') {
            const char *word=c=='t'?"true":c=='f'?"false":"null";
int n=(int)strlen(word);

            if(strncmp(s+begin,word,(size_t)n))return -1;
j->pos=begin+n;

        } else {
            if(c=='-')c=s[j->pos++];

            if(!isdigit(c))return -1;

            if(c=='0' && isdigit((unsigned char)s[j->pos]))return -1;

            while(isdigit((unsigned char)s[j->pos]))j->pos++;

            if(s[j->pos]=='.'){j->pos++;
if(!isdigit((unsigned char)s[j->pos]))return -1;
while(isdigit((unsigned char)s[j->pos]))j->pos++;
}
            if(s[j->pos]=='e'||s[j->pos]=='E'){j->pos++;
if(s[j->pos]=='+'||s[j->pos]=='-')j->pos++;
if(!isdigit((unsigned char)s[j->pos]))return -1;
while(isdigit((unsigned char)s[j->pos]))j->pos++;
}
        }
    }
    j->t[token].end=j->pos;
j->depth--;
return token;

}
static int jparse(Json *j, const char *s) {
    memset(j,0,sizeof(*j));
j->s=s;

    if(jvalue(j,-1)<0)return -1;
while(isspace((unsigned char)s[j->pos]))j->pos++;

    return s[j->pos]? -1:0;

}
static int jeq(Json *j,int i,const char *s) {
    int start,end;
if(i<0)return 0;
start=j->t[i].start;
end=j->t[i].end;

    if(j->t[i].type=='s'){start++;
end--;
}
    return end-start==(int)strlen(s) && !memcmp(j->s+start,s,(size_t)(end-start));

}
static int jget(Json *j,int parent,const char *key) {
    int i;
for(i=parent+1;i<j->n;i++){
        if(j->t[i].start>=j->t[parent].end)break;

        if(j->t[i].parent==parent && j->t[i].type=='s' && jeq(j,i,key))return i+1;

        if(j->t[i].parent==parent && j->t[i].type=='s') {
            i++;
while(i+1<j->n && j->t[i+1].start<j->t[i].end)i++;

        }
    }return -1;

}
static int utf8(char *out,size_t cap,size_t *n,unsigned cp) {
    unsigned char bytes[4];
size_t count;

    if(cp<0x80){bytes[0]=(unsigned char)cp;
count=1;
}
    else if(cp<0x800){bytes[0]=0xc0|(cp>>6);
bytes[1]=0x80|(cp&63);
count=2;
}
    else if(cp<0x10000){bytes[0]=0xe0|(cp>>12);
bytes[1]=0x80|((cp>>6)&63);
bytes[2]=0x80|(cp&63);
count=3;
}
    else {bytes[0]=0xf0|(cp>>18);
bytes[1]=0x80|((cp>>12)&63);
bytes[2]=0x80|((cp>>6)&63);
bytes[3]=0x80|(cp&63);
count=4;
}
    if(*n+count>=cap)return -1;
memcpy(out+*n,bytes,count);
*n+=count;
return 0;

}
static unsigned hex4(const char *s) {unsigned n=0;
int i;
for(i=0;i<4;i++){n=n*16+(isdigit((unsigned char)s[i])?s[i]-'0':tolower((unsigned char)s[i])-'a'+10);
}return n;
}
static int jstring(Json *j,int i,char *out,size_t cap) {
    int p,end;
size_t n=0;
if(i<0||j->t[i].type!='s'||!cap)return -1;

    p=j->t[i].start+1;
end=j->t[i].end-1;

    while(p<end){unsigned cp=(unsigned char)j->s[p++];

        if(cp=='\\'){cp=(unsigned char)j->s[p++];
switch(cp){case 'b':cp=8;
break;
case 'f':cp=12;
break;
case 'n':cp=10;
break;
case 'r':cp=13;
break;
case 't':cp=9;
break;

        case 'u':cp=hex4(j->s+p);
p+=4;
if(cp>=0xd800&&cp<=0xdbff){unsigned lo;
if(p+6>end||j->s[p]!='\\'||j->s[p+1]!='u')return -1;
lo=hex4(j->s+p+2);
if(lo<0xdc00||lo>0xdfff)return -1;
cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
p+=6;
}else if(cp>=0xdc00&&cp<=0xdfff)return -1;
if(!cp||utf8(out,cap,&n,cp))return -1;
continue;
default:break;
}}
        if(!cp||n+1>=cap)return -1;
out[n++]=(char)cp;

    }out[n]=0;
return 0;

}
static int jnumber(Json *j,int i,unsigned *n) {
    char copy[32],*end;
unsigned long value;
int length;

    if(i<0||j->t[i].type!='p')return -1;
length=j->t[i].end-j->t[i].start;

    if(length<1||length>=31)return -1;
memcpy(copy,j->s+j->t[i].start,(size_t)length);
copy[length]=0;

    if(copy[0]=='-'||copy[0]=='+')return -1;
errno=0;
value=strtoul(copy,&end,10);

    if(errno||*end||value>0xffffffffUL)return -1;
*n=(unsigned)value;
return 0;

}
static int juint64(Json *j,int i,uint64_t *n) {
    int p,end;
uint64_t value=0;
if(i<0||j->t[i].type!='p')return -1;
p=j->t[i].start;
end=j->t[i].end;
if(p==end)return -1;
for(;p<end;p++){
    unsigned digit;
    if(j->s[p]<'0'||j->s[p]>'9')return -1;
    digit=(unsigned)(j->s[p]-'0');
    if(value>(UINT64_MAX-digit)/10)return -1;
    value=value*10+digit;
}
if(value>INT64_MAX)return -1;
*n=value;
return 0;

}
static unsigned number_or(Json *j,const char *key,unsigned fallback){unsigned n;
return jnumber(j,jget(j,0,key),&n)?fallback:n;
}
static void feedback(MgEngine *e,const char *fmt,...) {va_list ap;
va_start(ap,fmt);
vsnprintf(e->status.feedback,sizeof(e->status.feedback),fmt,ap);
va_end(ap);
}
static void wipe_memory(void *memory, size_t length)
{
    volatile unsigned char *p = memory;
    while (length--) *p++ = 0;
}
static int journal(MgEngine *e,const char *type,const char *payload) {
    char line[REQUEST_CAP*2+2048];
Buffer b;

    if(!e->journal || mgp_file_size(e->status.journal_path)>=JOURNAL_CAP){e->poisoned=1;
return -1;
}
    binit(&b,line,sizeof(line));
bfmt(&b,"{\"monotonic_ms\":%llu,\"unix_time\":%lld,\"type\":",(unsigned long long)mgp_now(),(long long)time(NULL));
bstr(&b,type);
bput(&b,",\"data\":");
bstr(&b,payload);
bput(&b,"}\n");

    if(b.error||fwrite(line,1,b.n,e->journal)!=b.n||fflush(e->journal)){e->poisoned=1;
return -1;
}return 0;

}
static void repair(MgEngine *e,const char *action) {
    snprintf(e->status.repair_prompt,sizeof(e->status.repair_prompt),
        "Repair MegasXLR native VM controller without deleting guest disks or forensic logs.\n"
        "Requested action: %.63s\nObserved phase: %.63s\nFeedback: %.511s\n"
        "Owner PID: %d; QMP 127.0.0.1:%u; live RFB 127.0.0.1:%u.\n"
        "Checkpoint catalog verified: %s; listed entries: %u; allocated-size baseline verified: %s.\n"
        "Read action/event journal: %.1023s\nRead owned process output: %.1023s\n"
        "Read latest visible state: %.1023s\n"
        "Find the concrete cause, apply the smallest fix, verify current state through megas status, and report evidence. "
        "Do not fabricate successful input, guest login policy, display resize, or snapshot restore. Do not retry uncertain mutations blindly.",
        action,e->status.phase,e->status.feedback,e->status.owned_pid,e->cfg.qmp_port,e->cfg.vnc_port,
        e->snapshot_catalog_available?"yes":"no",e->snapshot_count,e->disk_base_known?"yes":"no",
        e->status.journal_path,e->qemu_log,e->status.image_path);

}
static void qmp_disconnect(MgEngine *e) {
    mgp_close(e->qmp);
e->qmp=MGP_BAD_SOCKET;
e->status.connected=0;
e->qmp_used=0;
e->pending_id=0;

    snprintf(e->status.phase,sizeof(e->status.phase),"disconnected");

}
static int qmp_line(MgEngine *e,const char *line) {
    Json *j=malloc(sizeof(*j));
unsigned id;
int event,error;

    if(!j)return -1;

    if(jparse(j,line)){free(j);
feedback(e,"Invalid or oversized QMP response; connection closed safely.");
qmp_disconnect(e);
return -1;
}
    event=jget(j,0,"event");
error=jget(j,0,"error");

    if(event>=0){char name[64];
jstring(j,event,name,sizeof(name));
e->status.events++;
journal(e,"qmp_event",line);

        if(!strcmp(name,"STOP")){e->status.running=0;
strcpy(e->status.phase,"paused");
}
        if(!strcmp(name,"RESUME")){e->status.running=1;
strcpy(e->status.phase,"running");
}
        if(!strcmp(name,"SHUTDOWN")){e->status.running=0;
strcpy(e->status.phase,"shutdown");
}
        if(!strcmp(name,"JOB_STATUS_CHANGE")){int data=jget(j,0,"data");
char job[64],status[32];

            if(data>=0&&!jstring(j,jget(j,data,"id"),job,sizeof(job))&&!jstring(j,jget(j,data,"status"),status,sizeof(status))&&!strcmp(job,e->job_id)&&!strcmp(status,"concluded"))e->job_concluded=1;
}
    } else if(jget(j,0,"QMP")>=0)e->qmp_greeting=1;

    else if(!jnumber(j,jget(j,0,"id"),&id)) {
        if(id==e->pending_id){snprintf(e->qmp_reply,sizeof(e->qmp_reply),"%s",line);
e->reply_ready=1;
e->reply_error=error>=0;
}
        else if(id==e->late_id){journal(e,error>=0?"late_qmp_error":"late_qmp_completion",line);
e->late_id=0;

            if(error>=0&&e->job_active){e->job_active=0;
e->job_concluded=0;
feedback(e,"Snapshot command was rejected after the wait deadline; no snapshot success was claimed.");
}}
        else journal(e,"unmatched_qmp_response",line);

    }
    free(j);
return 0;

}
static int qmp_pump(MgEngine *e) {
    int n;
size_t begin=0,i;

    if(e->qmp==MGP_BAD_SOCKET)return -1;

    n=mgp_recv(e->qmp,e->qmp_buffer+e->qmp_used,sizeof(e->qmp_buffer)-e->qmp_used-1);

    if(n==0||n==-1){journal(e,"qmp_disconnect","Persistent connection ended.");
qmp_disconnect(e);
return -1;
}
    if(n==-2)return 0;
e->qmp_used+=(size_t)n;
e->qmp_buffer[e->qmp_used]=0;

    for(i=0;i<e->qmp_used;i++)if(e->qmp_buffer[i]=='\n'){
        e->qmp_buffer[i]=0;
if(i>begin&&e->qmp_buffer[i-1]=='\r')e->qmp_buffer[i-1]=0;

        if(qmp_line(e,e->qmp_buffer+begin))return -1;
begin=i+1;

    }
    if(begin){memmove(e->qmp_buffer,e->qmp_buffer+begin,e->qmp_used-begin);
e->qmp_used-=begin;
}
    if(e->qmp_used==sizeof(e->qmp_buffer)-1){feedback(e,"QMP response exceeded 64 KiB; controller stayed available.");
qmp_disconnect(e);
return -1;
}
    return n;

}
static void display_tick(MgEngine *e) {
    char error[512];
int result=mg_rfb_tick(e->display,error,sizeof(error));

    unsigned w,h;
uint64_t generation;

    if(result<0&&result!=-2){if(e->last_rfb){journal(e,"display_disconnect",error);
e->last_rfb=0;
}}
    else if(result==-2){feedback(e,"%s",error);
journal(e,"display_notice",error);
}
    else {const uint8_t *pixels=mg_rfb_pixels(e->display,&w,&h,&generation);
if(pixels){e->status.width=w;
e->status.height=h;
if(generation!=e->status.frame_generation)e->last_frame=mgp_now();
e->status.frame_generation=generation;
e->last_rfb=mgp_now();
}}
}
static int qmp_exec(MgEngine *e,const char *execute,const char *arguments,unsigned timeout_ms) {
    char request[REQUEST_CAP];
Buffer b;
uint64_t deadline, command_id;

    if(e->qmp==MGP_BAD_SOCKET){feedback(e,"VM control is not connected. Start a VM or repair its audited process.");
return -1;
}
    if(e->late_id){feedback(e,"Previous command is still unconfirmed. Waiting for its actual reply; new commands are held safely.");
return -1;
}
    e->pending_id=++e->next_id;
command_id=e->pending_id;
e->reply_ready=0;
e->reply_error=0;

    binit(&b,request,sizeof(request));
bput(&b,"{\"execute\":");
bstr(&b,execute);

    if(arguments){bput(&b,",\"arguments\":");
bput(&b,arguments);
}bfmt(&b,",\"id\":%llu}\n",(unsigned long long)e->pending_id);

    if(b.error||journal(e,"qmp_request",request)){feedback(e,"Command was not sent because its audit record could not be saved.");
e->pending_id=0;
return -1;
}
    if(mgp_send_all(e->qmp,request,b.n,200)){feedback(e,"Control delivery is uncertain; transport closed instead of blindly retrying.");
e->late_id=e->pending_id;
qmp_disconnect(e);
return -1;
}
    deadline=mgp_now()+timeout_ms;

    while(!e->reply_ready&&e->qmp!=MGP_BAD_SOCKET&&mgp_now()<deadline){qmp_pump(e);
display_tick(e);
if(!e->reply_ready&&e->qmp!=MGP_BAD_SOCKET)mgp_ready(e->qmp,0,10);
}
    if(!e->reply_ready){e->late_id=command_id;
e->pending_id=0;
feedback(e,"Command has no confirmed result within %u ms; it is not retried or reported successful.",timeout_ms);
return -1;
}
    journal(e,"qmp_response",e->qmp_reply);
e->pending_id=0;

    if(e->reply_error){Json *j=malloc(sizeof(*j));
char desc[400]="QEMU rejected the requested operation.";

        if(j&&!jparse(j,e->qmp_reply)){int err=jget(j,0,"error");
if(err>=0)jstring(j,jget(j,err,"desc"),desc,sizeof(desc));
}free(j);
feedback(e,"%s",desc);
return -1;
}
    return 0;

}
static int connect_qmp(MgEngine *e) {
    uint64_t deadline=mgp_now()+COMMAND_MS;

    while(mgp_now()<deadline){e->qmp=mgp_connect(e->cfg.qmp_port,100);
if(e->qmp!=MGP_BAD_SOCKET)break;
if(e->status.owned_pid&&!mgp_alive(&e->process))break;
mgp_sleep(20);
}
    if(e->qmp==MGP_BAD_SOCKET){feedback(e,"VM did not open its control channel. Read its process log through the repair prompt.");
return -1;
}
    e->qmp_greeting=0;
e->qmp_used=0;
e->late_id=0;

    while(!e->qmp_greeting&&mgp_now()<deadline){qmp_pump(e);
if(e->qmp==MGP_BAD_SOCKET)break;
mgp_ready(e->qmp,0,10);
}
    if(!e->qmp_greeting||qmp_exec(e,"qmp_capabilities",NULL,500)){qmp_disconnect(e);
return -1;
}
    e->status.connected=1;
return 0;

}
static int refresh_state(MgEngine *e) {
    Json *j;
int ret;
char phase[64];

    if(e->qmp==MGP_BAD_SOCKET||e->late_id)return -1;

    if(qmp_exec(e,"query-status",NULL,300))return -1;

    j=malloc(sizeof(*j));
if(!j)return -1;

    if(jparse(j,e->qmp_reply)){free(j);
return -1;
}ret=jget(j,0,"return");

    if(ret>=0&&!jstring(j,jget(j,ret,"status"),phase,sizeof(phase)))strcpy(e->status.phase,phase);

    e->status.running=ret>=0&&jeq(j,jget(j,ret,"running"),"true");
e->last_state=mgp_now();
free(j);
return 0;

}
static int has_command(MgEngine *e,const char *name) {
    Json *j=malloc(sizeof(*j));
int ret,i,found=0;

    if(!j)return 0;
if(jparse(j,e->qmp_reply)){free(j);
return 0;
}ret=jget(j,0,"return");

    if(ret>=0)for(i=ret+1;i<j->n;i++)if(j->t[i].parent==ret&&j->t[i].type=='{'&&jeq(j,jget(j,i,"name"),name)){found=1;
break;
}
    free(j);
return found;

}
static int valid_name(const char *name);
static int snapshot_catalog_import(MgEngine *e,Json *j,int image);
static int snapshot_disk_allocated(MgEngine *e,uint64_t *allocated,int import_catalog) {
    Json *j;
int ret,i,matches=0,failed=0,image_for_catalog=-1;
int qcow2_nodes=0;
uint64_t value=0;

    if(import_catalog){e->snapshot_catalog_available=0;e->snapshot_count=0;}

    if(qmp_exec(e,"query-block",NULL,500)){
        char reason[512];snprintf(reason,sizeof(reason),"%.511s",e->status.feedback);
        if(import_catalog)feedback(e,"Checkpoint catalog unavailable because QEMU query-block did not complete. Existing checkpoints were not imported.");
        feedback(e,"Cannot verify checkpoint disk allocation because QEMU query-block did not complete: %s",reason);
        return -1;
    }
    j=malloc(sizeof(*j));
if(!j){if(import_catalog)feedback(e,"Checkpoint catalog unavailable because memory for QEMU's bounded response parser is unavailable.");feedback(e,"Cannot verify checkpoint disk allocation: memory for QEMU's bounded response parser is unavailable.");return -1;}
    if(jparse(j,e->qmp_reply)){
        free(j);if(import_catalog)feedback(e,"Checkpoint catalog unavailable because QEMU returned invalid query-block JSON.");feedback(e,"Cannot verify checkpoint disk allocation: QEMU returned invalid query-block JSON.");return -1;
    }
    ret=jget(j,0,"return");
if(ret<0||j->t[ret].type!='[')failed=1;
    if(!failed)for(i=ret+1;i<j->n;i++)if(j->t[i].parent==ret&&j->t[i].type=='{'){
        int inserted=jget(j,i,"inserted"),node,drv,image,format,actual;
        if(inserted<0||j->t[inserted].type!='{')continue;
        node=jget(j,inserted,"node-name");
        if(node<0||!jeq(j,node,"megasdisk"))continue;
        matches++;
        drv=jget(j,inserted,"drv");
        image=jget(j,inserted,"image");
        if(image>=0&&j->t[image].type=='{')image_for_catalog=image;
        format=image>=0&&j->t[image].type=='{'?jget(j,image,"format"):-1;
        actual=image>=0&&j->t[image].type=='{'?jget(j,image,"actual-size"):-1;
        if(drv>=0&&jeq(j,drv,"qcow2")&&format>=0&&jeq(j,format,"qcow2"))qcow2_nodes++;
        else failed=1;
        if(juint64(j,actual,&value))failed=1;
    }
    if(import_catalog){
        if(matches!=1||qcow2_nodes!=1||image_for_catalog<0){
            e->snapshot_count=0;
            feedback(e,"Checkpoint catalog unavailable because QEMU did not report exactly one verified qcow2 megasdisk image.");
        }else if(snapshot_catalog_import(e,j,image_for_catalog)){
            e->snapshot_count=0;
            e->snapshot_catalog_available=0;
            feedback(e,"Checkpoint catalog unavailable because QEMU's managed qcow2 snapshot list was malformed or exceeded the bounded 64-entry limit.");
        }else e->snapshot_catalog_available=1;
    }
    free(j);
    if(failed||matches!=1){
        feedback(e,"Cannot verify checkpoint disk allocation: QEMU did not report one qcow2 megasdisk node with a valid nonnegative 64-bit actual-size.");
        return -1;
    }
    *allocated=value;
return 0;

}
static int valid_name(const char *name) {size_t i,n=strlen(name);
if(!n||n>=64)return 0;
for(i=0;i<n;i++)if(!isalnum((unsigned char)name[i])&&name[i]!='_'&&name[i]!='-'&&name[i]!='.')return 0;
return 1;
}
static int snapshot_name_used(const Snapshot *snapshots,unsigned count,unsigned skip,const char *name) {
    unsigned i;
    for(i=0;i<count;i++)if(i!=skip&&snapshots[i].name[0]&&!strcmp(name,snapshots[i].name))return 1;
    return 0;
}
static int snapshot_alias_name(Snapshot *snapshots,unsigned count,unsigned index) {
    char candidate[64];
    unsigned suffix;
    for(suffix=1;suffix<1000;suffix++){
        if(suffix==1)snprintf(candidate,sizeof(candidate),"foreign-%u",index+1);
        else snprintf(candidate,sizeof(candidate),"foreign-%u-%u",index+1,suffix);
        if(!snapshot_name_used(snapshots,count,index,candidate)){
            snprintf(snapshots[index].name,sizeof(snapshots[index].name),"%s",candidate);
            return 0;
        }
    }
    return -1;
}
static uint64_t snapshot_step_from_name(const char *name) {
    const char *p;
    char *end;
    unsigned long long step;
    if(strncmp(name,"step-",5))return 0;
    p=name+5;
    if(!isdigit((unsigned char)*p))return 0;
    errno=0;step=strtoull(p,&end,10);
    return errno||*end!='-'?0:(uint64_t)step;
}
static int snapshot_catalog_import(MgEngine *e,Json *j,int image) {
    Snapshot imported[SNAPSHOT_CATALOG_CAP];
    const size_t prefix_len=strlen(SNAPSHOT_MANAGED_PREFIX);
    int list=jget(j,image,"snapshots");
    unsigned count=0,i;
    int token;

    memset(imported,0,sizeof(imported));
    /* QEMU omits the optional field when a qcow2 image has no snapshots. */
    if(list<0){e->snapshot_count=0;return 0;}
    if(j->t[list].type!='[')return -1;
    for(token=list+1;token<j->n;token++){
        Snapshot *s;
        char raw[SNAPSHOT_TAG_CAP];
        int state;
        uint64_t value;
        if(j->t[token].start>=j->t[list].end)break;
        if(j->t[token].parent!=list||j->t[token].type!='{')continue;
        if(count==SNAPSHOT_CATALOG_CAP)return -1;
        s=&imported[count];
        if(jstring(j,jget(j,token,"name"),raw,sizeof(raw))||!raw[0])return -1;
        snprintf(s->tag,sizeof(s->tag),"%s",raw);
        s->owned=!strncmp(raw,SNAPSHOT_MANAGED_PREFIX,prefix_len);
        if(s->owned){
            const char *label=raw+prefix_len;
            if(!valid_name(label))return -1;
            memcpy(s->name,label,strlen(label)+1);
        }else if(valid_name(raw))memcpy(s->name,raw,strlen(raw)+1);
        state=jget(j,token,"vm-state-size");
        s->restorable=!juint64(j,state,&value)&&value>0;
        s->step=snapshot_step_from_name(s->name);
        for(i=0;i<count;i++)if(!strcmp(imported[i].tag,s->tag))return -1;
        count++;
    }
    /* Preserve a useful name for ordinary foreign tags, while assigning a
     * selectable local alias to unusual or colliding names. */
    for(i=0;i<count;i++)if(imported[i].owned){
        unsigned j2;
        for(j2=0;j2<i;j2++)if(imported[j2].owned&&!strcmp(imported[j2].name,imported[i].name))return -1;
    }
    for(i=0;i<count;i++)if(!imported[i].owned){
        if(!valid_name(imported[i].tag)||snapshot_name_used(imported,count,i,imported[i].tag)){
            if(snapshot_alias_name(imported,count,i))return -1;
        }else snprintf(imported[i].name,sizeof(imported[i].name),"%s",imported[i].tag);
    }
    memcpy(e->snapshots,imported,count*sizeof(imported[0]));
    e->snapshot_count=count;
    return 0;
}
static int snapshot_find(MgEngine *e,const char *name) {unsigned i;
for(i=0;i<e->snapshot_count;i++)if(!strcmp(name,e->snapshots[i].name))return (int)i;
return -1;
}
static int snapshot_find_tag(MgEngine *e,const char *tag) {unsigned i;
for(i=0;i<e->snapshot_count;i++)if(!strcmp(tag,e->snapshots[i].tag))return (int)i;
return -1;
}
static unsigned snapshot_owned_count(MgEngine *e) {unsigned i,count=0;
for(i=0;i<e->snapshot_count;i++)if(e->snapshots[i].owned)count++;
return count;
}
static int snapshot_oldest_owned(MgEngine *e) {
    unsigned i;
    /* QEMU's qcow2 query-block snapshot array follows its append-only snapshot
     * table, so the first retained owned entry is the oldest. */
    for(i=0;i<e->snapshot_count;i++)if(e->snapshots[i].owned)return (int)i;
    return -1;
}
static void snapshot_done(MgEngine *e,int ok) {
    char info[192];

    if(ok&&!strcmp(e->job_kind,"snapshot-save")&&e->snapshot_count<SNAPSHOT_CATALOG_CAP){Snapshot *s=&e->snapshots[e->snapshot_count++];
memset(s,0,sizeof(*s));
snprintf(s->name,sizeof(s->name),"%s",e->job_snapshot_name);
snprintf(s->tag,sizeof(s->tag),"%s",e->job_tag);
s->step=snapshot_step_from_name(s->name);
s->owned=e->job_snapshot_owned;
s->restorable=1;
}
    if(ok&&!strcmp(e->job_kind,"snapshot-delete")){int i=snapshot_find_tag(e,e->job_tag);
if(i>=0){memmove(e->snapshots+i,e->snapshots+i+1,(e->snapshot_count-(unsigned)i-1)*sizeof(Snapshot));
e->snapshot_count--;
}}
    if(!strcmp(e->job_kind,"snapshot-load"))e->status.last_restore_ms=(double)(mgp_now()-e->job_started);

    snprintf(info,sizeof(info),"%s %s %s in %llu ms",e->job_kind,e->job_tag,ok?"confirmed":"failed",(unsigned long long)(mgp_now()-e->job_started));
journal(e,"snapshot_result",info);

    e->job_active=0;
e->job_concluded=0;
}

static int snapshot_poll(MgEngine *e) {
    Json *j;
int ret,i,result=0,found=0;
char id[64],status[32],error[400];
char args[128];
Buffer b;

    if(!e->job_active)return 1;

    if(qmp_exec(e,"query-jobs",NULL,300))return -1;

    j=malloc(sizeof(*j));
if(!j)return -1;

    if(jparse(j,e->qmp_reply)){free(j);
return -1;
}ret=jget(j,0,"return");

    if(ret>=0)for(i=ret+1;i<j->n;i++)if(j->t[i].parent==ret&&j->t[i].type=='{'){
        if(jstring(j,jget(j,i,"id"),id,sizeof(id))||strcmp(id,e->job_id))continue;
        found=1;

        if(!jstring(j,jget(j,i,"status"),status,sizeof(status))&&!strcmp(status,"concluded")){
            int err=jget(j,i,"error");
result=err<0?1:-1;

            if(err>=0&&!jstring(j,err,error,sizeof(error)))feedback(e,"Snapshot failed: %s",error);

            binit(&b,args,sizeof(args));
bput(&b,"{\"id\":");
bstr(&b,e->job_id);
bput(&b,"}");

            snapshot_done(e,result>0);
free(j);
qmp_exec(e,"job-dismiss",args,300);
return result;

        }
    }free(j);
if(!found&&!result&&!e->late_id&&mgp_now()-e->job_started>1000){
    /* QEMU creates the job before acknowledging it; disappearance is uncertain. */
    feedback(e,"Snapshot job disappeared without a confirmed result; inspect the forensic journal.");
    snapshot_done(e,0);
    return -1;
}
return result;

}
static int snapshot(MgEngine *e,const char *kind,const char *tag);
static int snapshot_disk_directory(MgEngine *e,char *directory,size_t capacity) {
    char *p;
if(snprintf(directory,capacity,"%s",e->cfg.disk)>=(int)capacity)return -1;
p=strrchr(directory,'/');
#ifdef _WIN32
    {char *q=strrchr(directory,'\\');
if(q&&(!p||q>p))p=q;
}
#endif
    if(p)*p=0;
else if(snprintf(directory,capacity,".")>=(int)capacity)return -1;
return 0;

}
static uint64_t snapshot_budget_bytes(MgEngine *e) {
    uint64_t ram_bytes=(uint64_t)e->cfg.ram_mb*1024*1024;
uint64_t scaled_budget;
if(ram_bytes>(UINT64_MAX-SNAPSHOT_METADATA_RESERVE)/2)return UINT64_MAX;
scaled_budget=2*ram_bytes+SNAPSHOT_METADATA_RESERVE;
return scaled_budget>SNAPSHOT_BUDGET_FLOOR?scaled_budget:SNAPSHOT_BUDGET_FLOOR;

}
static int snapshot_room(MgEngine *e,int reserve_next,uint64_t *allocated,uint64_t *free_bytes) {
    char directory[1024];
uint64_t ram_bytes=(uint64_t)e->cfg.ram_mb*1024*1024;
uint64_t budget_reserve=reserve_next?ram_bytes:0;
uint64_t budget=snapshot_budget_bytes(e);
uint64_t disk_growth;
int budget_ok,free_ok;

    if(!e->disk_base_known){
        feedback(e,"Checkpoint refused because QEMU did not provide a trustworthy allocated-size baseline for the managed megasdisk qcow2 node.");
        return -1;
    }
    if(budget_reserve>budget){
        feedback(e,"One checkpoint at the configured RAM size cannot fit within the allocated-size checkpoint budget; no checkpoint was saved.");
        return -1;
    }
    if(snapshot_disk_allocated(e,allocated,0))return -1;
    if(snapshot_disk_directory(e,directory,sizeof(directory))){
        feedback(e,"Cannot verify checkpoint free space because the managed disk path is too long.");
        return -1;
    }
    *free_bytes=mgp_disk_free(directory);
    disk_growth=*allocated>e->disk_base?*allocated-e->disk_base:0;
    budget_ok=disk_growth<=budget-budget_reserve;
    free_ok=*free_bytes>=FREE_RESERVE+budget_reserve;
    return budget_ok&&free_ok?1:0;

}
static int snapshot_delete_oldest(MgEngine *e,const char *reason) {
    char oldest[SNAPSHOT_TAG_CAP],label[64],audit[192];
int i=snapshot_oldest_owned(e);
if(i<0){feedback(e,"No Megas-owned checkpoint is available for automatic eviction; foreign checkpoints were preserved.");return -1;}
snprintf(oldest,sizeof(oldest),"%s",e->snapshots[i].tag);
snprintf(label,sizeof(label),"%s",e->snapshots[i].name);
snprintf(audit,sizeof(audit),"%s: evicting oldest owned checkpoint '%s'",reason,label);
if(journal(e,"checkpoint_eviction",audit)){
    feedback(e,"Checkpoint eviction was not attempted because its audit record could not be saved.");
    return -1;
}
return snapshot(e,"snapshot-delete",oldest);

}
static int snapshot_make_room(MgEngine *e,const char *protected_tag,int reserve_next,int enforce_retention) {
    uint64_t allocated=0,free_bytes=0,previous_allocated=0,ram_bytes=(uint64_t)e->cfg.ram_mb*1024*1024;
uint64_t budget=snapshot_budget_bytes(e);
unsigned evicted=0;
char detail[256];
int room,have_previous=0,retention_pending,catalog_pending;

    if(reserve_next&&ram_bytes>budget){
        feedback(e,"One checkpoint at the configured RAM size cannot fit within the allocated-size checkpoint budget; no checkpoint was saved.");
        return -1;
    }
    for(;;){
        retention_pending=enforce_retention&&snapshot_owned_count(e)>=SNAPSHOT_CAP;
        catalog_pending=enforce_retention&&e->snapshot_count>=SNAPSHOT_CATALOG_CAP;
        room=snapshot_room(e,reserve_next,&allocated,&free_bytes);
        if(room<0)return -1;
        if(room&&!retention_pending&&!catalog_pending)return 0;
        if(!room&&!retention_pending&&!catalog_pending&&have_previous&&reserve_next&&!protected_tag&&allocated==previous_allocated&&
           free_bytes>=FREE_RESERVE+ram_bytes){
            snprintf(detail,sizeof(detail),"one reuse retry after confirmed eviction; allocated=%llu baseline=%llu budget=%llu free=%llu",
                (unsigned long long)allocated,(unsigned long long)e->disk_base,
                (unsigned long long)budget,(unsigned long long)free_bytes);
            if(journal(e,"checkpoint_reuse_retry",detail)){
                feedback(e,"Checkpoint was not saved because the bounded reuse retry could not be recorded in the audit journal.");
                return -1;
            }
            return 1;
        }
        snprintf(detail,sizeof(detail),"allocated=%llu baseline=%llu budget=%llu free=%llu ram=%llu next=%s",
            (unsigned long long)allocated,(unsigned long long)e->disk_base,
            (unsigned long long)budget,(unsigned long long)free_bytes,(unsigned long long)ram_bytes,
            reserve_next?"true":"false");
        if(!room&&journal(e,"checkpoint_budget_pressure",detail)){
            feedback(e,"Checkpoint was not saved because the budget decision could not be recorded in the audit journal.");
            return -1;
        }
        if(snapshot_owned_count(e)&&evicted<SNAPSHOT_CATALOG_CAP){
            {int oldest=snapshot_oldest_owned(e);
            if(protected_tag&&oldest>=0&&!strcmp(e->snapshots[oldest].tag,protected_tag)){
                if(snapshot(e,"snapshot-delete",protected_tag))return -1;
                feedback(e,"Checkpoint '%s' exceeded the allocated-size or free-space budget and was removed; no checkpoint was reported successful.",e->job_snapshot_name);
                return -1;
            }}
            previous_allocated=allocated;
            have_previous=1;
            if(snapshot_delete_oldest(e,retention_pending?"retention cap":catalog_pending?"bounded catalog capacity":"allocated-size or free-space pressure"))return -1;
            evicted++;
            continue;
        }
        snprintf(detail,sizeof(detail),"%u owned checkpoint(s) evicted; allocated=%llu; budget=%llu; free=%llu",
            evicted,(unsigned long long)allocated,(unsigned long long)budget,(unsigned long long)free_bytes);
        if(journal(e,"checkpoint_budget_exhausted",detail)){
            feedback(e,"Checkpoint was not saved because the exhausted-budget decision could not be recorded in the audit journal.");
            return -1;
        }
        if(evicted)feedback(e,"Checkpoint budget or free-space reserve remains exhausted after evicting %u oldest owned checkpoint(s); allocation budget is %llu MiB; no new checkpoint was saved.",evicted,(unsigned long long)(budget/(1024*1024)));
        else feedback(e,"Checkpoint budget or free-space reserve is exhausted; allocation budget is %llu MiB; no new checkpoint was saved.",(unsigned long long)(budget/(1024*1024)));
        return -1;
    }

}
static int snapshot(MgEngine *e,const char *kind,const char *tag) {
    char args[2048];
Buffer b;
uint64_t deadline;
int result,reuse_attempt=0;
int index;

    if(!e->snapshot_supported){feedback(e,"This VM cannot capture reversible RAM and disk state. Use a managed qcow2 disk and QEMU snapshot support.");
return -1;
}
    if(!e->snapshot_catalog_available){feedback(e,"Checkpoint operation refused because QEMU's existing checkpoint catalog could not be verified. The VM remains available; existing states were not assumed empty.");return -1;}
    if(e->job_active){feedback(e,"Snapshot job is still running; new mutations wait for its confirmed completion.");
return -1;
}
    if(!tag||!tag[0]||strlen(tag)>=SNAPSHOT_TAG_CAP){feedback(e,"Checkpoint QMP name exceeds its bounded 127-byte limit.");
return -1;
}
    if(!strcmp(kind,"snapshot-save")){
        if(snapshot_find_tag(e,tag)>=0){feedback(e,"This checkpoint QMP name already exists; choose another name.");
return -1;
}
        if(e->snapshot_count>=SNAPSHOT_CATALOG_CAP&&snapshot_owned_count(e)==0){feedback(e,"The bounded 64-entry checkpoint catalog is full of existing states; no history was dropped and no new checkpoint was saved.");return -1;}
        result=snapshot_make_room(e,NULL,1,1);
if(result<0)return -1;
reuse_attempt=result>0;
    }else {
        index=snapshot_find_tag(e,tag);
        if(index<0){feedback(e,"Checkpoint is not in the verified QEMU catalog. The forensic journal is preserved.");
return -1;
        }
        if(!strcmp(kind,"snapshot-load")&&!e->snapshots[index].restorable){feedback(e,"This checkpoint is disk-only or has no verified VM state; Megas will not claim it can restore running RAM and devices.");return -1;}
        if(!strcmp(kind,"snapshot-delete")&&!e->snapshots[index].owned){feedback(e,"Foreign checkpoint '%s' is protected from automatic deletion.",e->snapshots[index].name);return -1;}
}
    snprintf(e->job_id,sizeof(e->job_id),"megas-job-%llu",(unsigned long long)++e->snapshot_serial);

    binit(&b,args,sizeof(args));
bput(&b,"{\"job-id\":");
bstr(&b,e->job_id);
bput(&b,",\"tag\":");
bstr(&b,tag);

    if(strcmp(kind,"snapshot-delete"))bput(&b,",\"vmstate\":\"megasdisk\"");
bput(&b,",\"devices\":[\"megasdisk\"]}");

    e->job_started=mgp_now();
strcpy(e->job_kind,kind);
strcpy(e->job_tag,tag);
e->job_active=1;
e->job_concluded=0;

    if(qmp_exec(e,kind,args,500)){if(!e->late_id)e->job_active=0;
return -1;
}
    deadline=e->job_started+COMMAND_MS;

    while(mgp_now()<deadline){result=snapshot_poll(e);
if(result){
    if(result>0&&!strcmp(kind,"snapshot-save")){
        int post_room;
        if(reuse_attempt){uint64_t allocated=0,free_bytes=0;
            post_room=snapshot_room(e,0,&allocated,&free_bytes);
            if(post_room==0)feedback(e,"One bounded checkpoint reuse attempt did not bring allocated-size or free space within budget.");
        }else post_room=snapshot_make_room(e,tag,0,0)?0:1;
        if(post_room<=0){
            char reason[512],cleanup[512];
            snprintf(reason,sizeof(reason),"%.511s",e->status.feedback);
            if(snapshot_find_tag(e,tag)>=0){
                if(!e->late_id&&!snapshot(e,"snapshot-delete",tag))
                    feedback(e,"Checkpoint '%s' was confirmed but removed because post-save budget verification failed: %.280s",e->job_snapshot_name,reason);
                else {
                    snprintf(cleanup,sizeof(cleanup),"%.511s",e->status.feedback);
                    feedback(e,"Checkpoint '%s' was confirmed, but post-save budget verification failed and its removal is unconfirmed. Inspect the retained checkpoint list and audit journal. Verification: %.150s Removal: %.150s",e->job_snapshot_name,reason,cleanup);
                }
            }
            return -1;
        }
    }
    return result>0?0:-1;
}
display_tick(e);
mgp_sleep(20);
}
    feedback(e,"Snapshot is still working after 4.5 seconds. It remains visible and new mutations are held until completion.");
return -1;

}
static int snapshot_save(MgEngine *e,const char *requested,int automatic,char *saved_name,size_t saved_capacity) {
    char name[64],tag[SNAPSHOT_TAG_CAP],base[64];
    unsigned suffix;
    size_t prefix_len=strlen(SNAPSHOT_MANAGED_PREFIX);
    if(!valid_name(requested)){feedback(e,"Checkpoint name accepts 1-63 letters, numbers, '.', '_' and '-'.");return -1;}
    snprintf(base,sizeof(base),"%s",requested);
    for(suffix=1;suffix<1000000;suffix++){
        if(suffix==1)snprintf(name,sizeof(name),"%s",base);
        else {
            char tail[16];size_t tail_len,base_len;
            snprintf(tail,sizeof(tail),"-%u",suffix);tail_len=strlen(tail);
            base_len=strlen(base);if(base_len+tail_len>=sizeof(name))base_len=sizeof(name)-tail_len-1;
            memcpy(name,base,base_len);memcpy(name+base_len,tail,tail_len+1);
        }
        if(prefix_len+strlen(name)>=sizeof(tag)){feedback(e,"Checkpoint name cannot fit the managed QMP name bound.");return -1;}
        snprintf(tag,sizeof(tag),"%s%s",SNAPSHOT_MANAGED_PREFIX,name);
        if(snapshot_find(e,name)<0&&snapshot_find_tag(e,tag)<0)break;
        if(!automatic){feedback(e,"This checkpoint name already exists; choose another name.");return -1;}
    }
    if(suffix==1000000){feedback(e,"No unique automatic checkpoint name is available in the bounded catalog.");return -1;}
    snprintf(e->job_snapshot_name,sizeof(e->job_snapshot_name),"%s",name);
    e->job_snapshot_owned=1;
    if(saved_name&&saved_capacity)snprintf(saved_name,saved_capacity,"%s",name);
    return snapshot(e,"snapshot-save",tag);

}
static int export_live(MgEngine *e) {
    char metadata[1024],error[512];
unsigned w,h;
uint64_t generation;

    if(!e->status.frame_generation||!mg_rfb_pixels(e->display,&w,&h,&generation))return -1;

    if(e->status.frame_generation==e->last_export_generation || (e->transient_response && e->last_export_generation))return 0;

    snprintf(metadata,sizeof(metadata),"Live RFB generation %llu; %ux%u. Screen text is not inferred by this controller; use the accompanying current VM state and vision model.",
        (unsigned long long)e->status.frame_generation,e->status.width,e->status.height);

    if(mg_rfb_png(e->display,e->status.image_path,metadata,error,sizeof(error))){journal(e,"image_export_failed",error);
return -1;
}
    e->last_export_generation=e->status.frame_generation;
e->image_ms=mgp_now();
return 0;

}
static int parent_path(char *path);
static void discover_package(void);
static void guest_auto_schedule(MgEngine *e, const char *username, int enabled,
                                const char *password);
void mg_config_defaults(MgConfig *cfg) {
    const char *base;
char env[1024], home[1024];
memset(cfg,0,sizeof(*cfg));
discover_package();
base=mgp_env("MEGAS_RUNTIME_DIR",env,sizeof(env))?NULL:env;

    if(base)snprintf(cfg->runtime_dir,sizeof(cfg->runtime_dir),"%s",base);

    else {
#ifdef _WIN32
        base=mgp_env("LOCALAPPDATA",home,sizeof(home))?NULL:home;
snprintf(cfg->runtime_dir,sizeof(cfg->runtime_dir),"%.900s/Megas",base?base:".");

#else
        base=mgp_env("HOME",home,sizeof(home))?NULL:home;
snprintf(cfg->runtime_dir,sizeof(cfg->runtime_dir),"%.900s/.local/state/megas",base?base:".");

#endif
    }
    base=mgp_env("MEGAS_QEMU",env,sizeof(env))?NULL:env;
snprintf(cfg->qemu,sizeof(cfg->qemu),"%s",base?base:"qemu-system-x86_64");
cfg->ram_mb=256;
cfg->width=1280;
cfg->height=720;

    cfg->control_port=49391;
cfg->qmp_port=49392;
cfg->vnc_port=49393;
cfg->qga_port=49394;
cfg->android_port=49395;
cfg->video_port=49396;
cfg->stream_port=49397;
cfg->cpus=1;
cfg->cpu_percent=100;
cfg->max_threads=48;
snprintf(cfg->accel,sizeof(cfg->accel),"auto");
snprintf(cfg->disk_bus,sizeof(cfg->disk_bus),"virtio");
if(!mgp_env("MEGAS_CGROUP_PARENT",env,sizeof(env)))snprintf(cfg->cgroup_parent,sizeof(cfg->cgroup_parent),"%s",env);
cfg->passwordless=1;

}
MgEngine *mg_engine_new(const MgConfig *cfg) {
    MgEngine *e;
unsigned char bytes[32];
unsigned i;
char path[1024];
FILE *token;

    if(mgp_init()||!cfg||strlen(cfg->runtime_dir)>900||mgp_mkdir(cfg->runtime_dir))return NULL;

    e=calloc(1,sizeof(*e));
if(!e)return NULL;
e->cfg=*cfg;
e->qmp=e->listener=MGP_BAD_SOCKET;
e->owner_lock=-1;

    for(i=0;i<4;i++)e->clients[i].socket=MGP_BAD_SOCKET;

    snprintf(path,sizeof(path),"%.900s/owner-%u.lock",cfg->runtime_dir,cfg->control_port);

    if(mgp_owner_lock(path,&e->owner_lock))goto fail;

    e->listener=mgp_listen(cfg->control_port);
if(e->listener==MGP_BAD_SOCKET)goto fail;

    if(mgp_random(bytes,sizeof(bytes)))goto fail;

    for(i=0;i<32;i++)snprintf(e->token+i*2,3,"%02x",bytes[i]);

    snprintf(e->token_path,sizeof(e->token_path),"%.900s/session-%u.token",cfg->runtime_dir,cfg->control_port);

    token=mgp_fopen(e->token_path,"wb");
if(!token)goto fail;

#ifndef _WIN32
    chmod(e->token_path,0600);

#endif
    if(fwrite(e->token,1,64,token)!=64){fclose(token);
goto fail;
}fclose(token);

    snprintf(e->status.journal_path,sizeof(e->status.journal_path),"%.900s/actions.jsonl",cfg->runtime_dir);

    snprintf(e->status.image_path,sizeof(e->status.image_path),"%.900s/latest.png",cfg->runtime_dir);

    snprintf(e->qemu_log,sizeof(e->qemu_log),"%.900s/qemu-output.log",cfg->runtime_dir);

    e->journal=mgp_fopen(e->status.journal_path,"ab");
if(!e->journal)goto fail;

#ifndef _WIN32
    chmod(e->status.journal_path,0600);

    fcntl(fileno(e->journal),F_SETFD,FD_CLOEXEC);

#endif
    e->display=mg_rfb_new();
if(!e->display)goto fail;

    e->auto_checkpoint=1;
e->started=mgp_now();
strcpy(e->status.phase,"stopped");
feedback(e,"Ready. Start a VM; its control and display stay on this computer.");

    journal(e,"owner_start","Native controller visible; loopback control authenticated; automatic checkpoint policy enabled.");
repair(e,"startup");
return e;

fail:
    mg_engine_free(e);
return NULL;

}
const MgConfig *mg_engine_config(MgEngine *e){return &e->cfg;
}
const MgStatus *mg_engine_status(MgEngine *e){return &e->status;
}
MgRfb *mg_engine_display(MgEngine *e){return e->display;
}
size_t mg_engine_checkpoints(MgEngine *e,char names[][64],size_t maximum){size_t i,n=e->snapshot_count;
if(n>maximum)n=maximum;
for(i=0;i<n;i++)strcpy(names[i],e->snapshots[i].name);
return n;
}
static int config_string(Json *j,const char *key,char *target,size_t cap){int i=jget(j,0,key);
return i<0?0:jstring(j,i,target,cap);
}
static int parent_path(char *path)
{
    char *slash = strrchr(path, '/');
#ifdef _WIN32
    char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    if (!slash) return -1;
    if (slash == path) slash[1] = 0;
#ifdef _WIN32
    else if (slash == path + 2 && path[1] == ':') slash[1] = 0;
#endif
    else *slash = 0;
    return 0;
}
static void discover_package(void)
{
    char package[1024], root[1024], candidate[1400];
    int level;
#ifdef _WIN32
    char qemu[1200];
#endif
    if (mgp_env("MEGAS_PACKAGE_DIR", package, sizeof(package))) {
        if (mgp_executable_dir(root, sizeof(root))) return;
        for (level = 0; level < 5; level++) {
            snprintf(candidate, sizeof(candidate), "%.1000s/guest/linux-policy.sh", root);
            if (mgp_file_size(candidate)) break;
            snprintf(candidate, sizeof(candidate), "%.1000s/guest/windows-policy.ps1", root);
            if (mgp_file_size(candidate)) break;
            snprintf(candidate, sizeof(candidate), "%.1000s/qemu/bin/qemu-system-x86_64%s", root,
#ifdef _WIN32
                     ".exe"
#else
                     ""
#endif
            );
            if (mgp_file_size(candidate)) break;
            if (parent_path(root)) return;
        }
        if (level >= 5) return;
        if (mgp_setenv("MEGAS_PACKAGE_DIR", root)) return;
        snprintf(package, sizeof(package), "%s", root);
    }
#ifdef _WIN32
    if (mgp_env("MEGAS_QEMU", qemu, sizeof(qemu)) &&
        snprintf(qemu, sizeof(qemu), "%.1000s/qemu/bin/qemu-system-x86_64.exe", package) < (int)sizeof(qemu) &&
        mgp_file_size(qemu)) (void)mgp_setenv("MEGAS_QEMU", qemu);
    if (mgp_env("MEGAS_QEMU_IMG", qemu, sizeof(qemu)) &&
        snprintf(qemu, sizeof(qemu), "%.1000s/qemu/bin/qemu-img.exe", package) < (int)sizeof(qemu) &&
        mgp_file_size(qemu)) (void)mgp_setenv("MEGAS_QEMU_IMG", qemu);
    if (mgp_env("MEGAS_QEMU_DATA_DIR", qemu, sizeof(qemu)) &&
        snprintf(qemu, sizeof(qemu), "%.1000s/qemu/share/qemu", package) < (int)sizeof(qemu) &&
        mgp_file_size(qemu)) (void)mgp_setenv("MEGAS_QEMU_DATA_DIR", qemu);
    if (mgp_env("QEMU_MODULE_DIR", qemu, sizeof(qemu)) &&
        snprintf(qemu, sizeof(qemu), "%.1000s/qemu/lib/qemu", package) < (int)sizeof(qemu) &&
        mgp_file_size(qemu)) (void)mgp_setenv("QEMU_MODULE_DIR", qemu);
#endif
}
static int guest_helper_dir(char *out, size_t capacity)
{
    char root[1024], package[1024], candidate[2048];
    int level;
    if (!mgp_env("MEGAS_PACKAGE_DIR", package, sizeof(package)) && package[0]) {
        snprintf(root, sizeof(root), "%s", package);
    } else if (mgp_executable_dir(root, sizeof(root))) {
        return -1;
    }
    for (level = 0; level < 5; level++) {
        snprintf(candidate, sizeof(candidate), "%.900s/guest/linux-policy.sh", root);
        if (mgp_file_size(candidate)) {
            if (snprintf(out, capacity, "%.900s/guest", root) >= (int)capacity) return -1;
            return 0;
        }
        snprintf(candidate, sizeof(candidate), "%.900s/guest/windows-policy.ps1", root);
        if (mgp_file_size(candidate)) {
            if (snprintf(out, capacity, "%.900s/guest", root) >= (int)capacity) return -1;
            return 0;
        }
        /* Linux bundles also stage helpers beside the private ELF. */
        snprintf(candidate, sizeof(candidate), "%.900s/linux-policy.sh", root);
        if (mgp_file_size(candidate)) {
            if (strlen(root) >= capacity) return -1;
            strcpy(out, root);
            return 0;
        }
        snprintf(candidate, sizeof(candidate), "%.900s/windows-policy.ps1", root);
        if (mgp_file_size(candidate)) {
            if (strlen(root) >= capacity) return -1;
            strcpy(out, root);
            return 0;
        }
        if (parent_path(root)) break;
    }
    return -1;
}
static int package_root(char *out, size_t capacity)
{
    char root[1024], package[1024];
    int level;
    if (!mgp_env("MEGAS_PACKAGE_DIR", package, sizeof(package)) && package[0]) {
        if (strlen(package) >= capacity) return -1;
        strcpy(out, package);
        return 0;
    }
    if (mgp_executable_dir(root, sizeof(root))) return -1;
    for (level = 0; level < 5; level++) {
        char candidate[1400];
        snprintf(candidate, sizeof(candidate), "%.1000s/ocr/tesseract%s", root,
#ifdef _WIN32
                 ".exe"
#else
                 ""
#endif
        );
        if (mgp_file_size(candidate)) break;
        snprintf(candidate, sizeof(candidate), "%.1000s/guest/linux-policy.sh", root);
        if (mgp_file_size(candidate)) break;
        if (parent_path(root)) return -1;
    }
    if (level >= 5 || strlen(root) >= capacity) return -1;
    strcpy(out, root);
    return 0;
}
static int tesseract_path(char *out, size_t capacity,
                          char *data_dir, size_t data_capacity)
{
    char configured[1024], root[1024], candidate[1400];
    data_dir[0] = 0;
    if (!mgp_env("MEGAS_TESSERACT", configured, sizeof(configured)) && configured[0] &&
        mgp_file_size(configured)) {
        if (strlen(configured) >= capacity) return -1;
        strcpy(out, configured);
        return 0;
    }
    if (!package_root(root, sizeof(root))) {
        snprintf(candidate, sizeof(candidate), "%.1000s/ocr/tesseract%s", root,
#ifdef _WIN32
                 ".exe"
#else
                 ""
#endif
        );
        if (mgp_file_size(candidate) && strlen(candidate) < capacity) {
            char traineddata[1400];
            snprintf(traineddata, sizeof(traineddata), "%.1000s/ocr/tessdata/eng.traineddata", root);
            {
                int data_length = snprintf(data_dir, data_capacity, "%.1000s/ocr/tessdata", root);
                if (mgp_file_size(traineddata) && data_length >= 0 &&
                    (size_t)data_length < data_capacity) {
                strcpy(out, candidate);
                /* Use bundle-relative data even when launched outside the bundle
                 * working directory. The explicit CLI option below is primary;
                 * this environment value also supports native Tesseract helpers. */
                if (mgp_env("TESSDATA_PREFIX", configured, sizeof(configured)))
                    (void)mgp_setenv("TESSDATA_PREFIX", data_dir);
                return 0;
                }
            }
        }
    }
#ifdef _WIN32
    if (strlen("tesseract.exe") >= capacity) return -1;
    strcpy(out, "tesseract.exe");
#else
    if (strlen("tesseract") >= capacity) return -1;
    strcpy(out, "tesseract");
#endif
    return 0;
}
static const char *ocr_status_name(int status)
{
    switch (status) {
    case OCR_RUNNING: return "running";
    case OCR_COMPLETE: return "complete";
    case OCR_UNAVAILABLE: return "unavailable";
    case OCR_FAILED: return "failed";
    case OCR_TIMEOUT: return "deadline_exceeded";
    case OCR_STALE: return "stale_image";
    default: return "not_run";
    }
}
static void ocr_cleanup(MgEngine *e, int remove_output)
{
    if (e->ocr_active) {
        mgp_force_kill(&e->ocr_process);
        mgp_reap(&e->ocr_process);
        e->ocr_active = 0;
    }
    if (remove_output && e->ocr_output[0]) (void)mgp_remove(e->ocr_output);
    e->ocr_pid = 0;
}
static int ocr_start(MgEngine *e, Json *j)
{
    char program[1024], base[1024], input[1024], data_dir[1200], error[512];
    char *argv[10];
    unsigned expected = 0, nonce = 0;
    if (!e->status.connected || !e->status.running) {
        e->ocr_status = OCR_FAILED;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR requires a running VM and a current display frame.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    if (export_live(e)) {
        e->ocr_status = OCR_FAILED;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "No current live image is available for OCR.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    if (jget(j, 0, "generation") >= 0 &&
        (jnumber(j, jget(j, 0, "generation"), &expected) || expected != e->last_export_generation)) {
        e->ocr_status = OCR_STALE;
        e->ocr_generation = e->last_export_generation;
        e->ocr_text[0] = 0;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "The live image changed before OCR started; no text was extracted.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    if (e->ocr_active && e->ocr_generation == e->last_export_generation) {
        snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR is still running for this live image.");
        feedback(e, "%s", e->ocr_message);
        return 0;
    }
    if (e->ocr_status == OCR_COMPLETE && e->ocr_generation == e->last_export_generation) {
        snprintf(e->ocr_message, sizeof(e->ocr_message), "Using the confirmed Tesseract result for the current live image generation.");
        feedback(e, "%s", e->ocr_message);
        return 0;
    }
    if (e->ocr_active) {
        journal(e, "ocr_cancelled", "A newer display generation replaced the OCR input; only the OCR child was stopped.");
        ocr_cleanup(e, 1);
    }
    e->ocr_text[0] = 0;
    e->ocr_generation = e->last_export_generation;
    e->ocr_status = OCR_FAILED;
    e->ocr_peak_memory = 0;
    e->ocr_message[0] = 0;
    if (tesseract_path(program, sizeof(program), data_dir, sizeof(data_dir))) {
        e->ocr_status = OCR_UNAVAILABLE;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "Tesseract OCR is unavailable; controller metadata is not screen text.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    if (mgp_random(&nonce, sizeof(nonce))) nonce = (unsigned)mgp_now();
    if (snprintf(input, sizeof(input), "%.950s", e->status.image_path) >= (int)sizeof(input) ||
        snprintf(base, sizeof(base), "%.800s/ocr-%llu-%08x", e->cfg.runtime_dir,
                 (unsigned long long)e->ocr_generation, nonce) >= (int)sizeof(base) ||
        snprintf(e->ocr_output, sizeof(e->ocr_output), "%s.txt", base) >= (int)sizeof(e->ocr_output) ||
        snprintf(e->ocr_log, sizeof(e->ocr_log), "%.900s/ocr.log", e->cfg.runtime_dir) >= (int)sizeof(e->ocr_log)) {
        snprintf(e->ocr_message, sizeof(e->ocr_message), "The bounded OCR paths exceed the runtime folder limit.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    argv[0] = program; argv[1] = input; argv[2] = base;
    argv[3] = "--psm"; argv[4] = "6";
    if (data_dir[0]) {
        argv[5] = "--tessdata-dir"; argv[6] = data_dir; argv[7] = NULL;
    } else argv[5] = NULL;
    if (journal(e, "ocr_start", "OCR child requested for the current live image generation; memory limit 48 MiB; CPU limit 8 seconds; wall deadline 10 seconds.")) {
        snprintf(e->ocr_message, sizeof(e->ocr_message), "The journal is full; OCR was not started.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    if (mgp_spawn_bounded(&e->ocr_process, argv, e->ocr_log,
                         OCR_MEMORY_BYTES, 8, error, sizeof(error))) {
        e->ocr_status = OCR_UNAVAILABLE;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "Tesseract OCR could not start: %.170s. No OCR text was inferred.", error);
        journal(e, "ocr_unavailable", "Tesseract process could not start within the native resource limits.");
        feedback(e, "%s", e->ocr_message);
        return -1;
    }
    e->ocr_active = 1;
    e->ocr_status = OCR_RUNNING;
    e->ocr_deadline = mgp_now() + OCR_WALL_MS;
    e->ocr_pid = (uint64_t)mgp_pid(&e->ocr_process);
    snprintf(e->ocr_message, sizeof(e->ocr_message), "Tesseract OCR is running asynchronously on image generation %llu.",
             (unsigned long long)e->ocr_generation);
    feedback(e, "%s", e->ocr_message);
    return 0;
}
static void ocr_tick(MgEngine *e)
{
    FILE *file;
    unsigned char raw[OCR_TEXT_CAP + 1];
    size_t n, i, used = 0;
    int code;
    if (!e->ocr_active) return;
    {
        uint64_t resident = mgp_memory_bytes(&e->ocr_process);
        if (resident > e->ocr_peak_memory) e->ocr_peak_memory = resident;
        if (resident > OCR_MEMORY_BYTES) {
            ocr_cleanup(e, 1);
            e->ocr_status = OCR_FAILED;
            snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR exceeded its observed 48 MiB process-memory budget and was stopped; the VM remains live.");
            journal(e, "ocr_memory_limit", "OCR child exceeded 48 MiB measured process memory; only the OCR child was stopped.");
            feedback(e, "%s", e->ocr_message);
            return;
        }
    }
    if (mgp_now() >= e->ocr_deadline) {
        ocr_cleanup(e, 1);
        e->ocr_status = OCR_TIMEOUT;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR exceeded its 10-second deadline and was stopped; the VM remains live.");
        journal(e, "ocr_deadline", "OCR child stopped at the 10-second wall deadline; the VM process was not touched.");
        feedback(e, "%s", e->ocr_message);
        return;
    }
    if (mgp_alive(&e->ocr_process)) return;
    code = e->ocr_process.exit_code;
    mgp_reap(&e->ocr_process);
    e->ocr_active = 0;
    e->ocr_pid = 0;
    if (code != 0 || mgp_file_size(e->ocr_output) > 65536) {
        e->ocr_status = OCR_FAILED;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR did not produce a bounded successful result (exit %d, memory cap 48 MiB); no text was inferred.", code);
        journal(e, "ocr_failed", "OCR exited unsuccessfully or exceeded its bounded text output; VM remains live.");
        (void)mgp_remove(e->ocr_output);
        feedback(e, "%s", e->ocr_message);
        return;
    }
    file = mgp_fopen(e->ocr_output, "rb");
    if (!file) {
        e->ocr_status = OCR_FAILED;
        snprintf(e->ocr_message, sizeof(e->ocr_message), "Tesseract completed without a readable text result.");
        journal(e, "ocr_failed", "Tesseract exited successfully but its bounded output file was unavailable.");
        feedback(e, "%s", e->ocr_message);
        return;
    }
    n = fread(raw, 1, OCR_TEXT_CAP, file);
    raw[n] = 0;
    if (ferror(file)) n = 0;
    fclose(file);
    (void)mgp_remove(e->ocr_output);
    for (i = 0; i < n && used < OCR_TEXT_CAP; ) {
        unsigned char c = raw[i];
        size_t count = 1, k;
        if (c < 0x80) {
            if ((c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\r' || c == '\t') e->ocr_text[used++] = (char)c;
            else e->ocr_text[used++] = ' ';
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) count = 2;
        else if (c >= 0xe0 && c <= 0xef) count = 3;
        else if (c >= 0xf0 && c <= 0xf4) count = 4;
        if (i + count > n) { e->ocr_text[used++] = '?'; i++; continue; }
        for (k = 1; k < count; k++) if ((raw[i + k] & 0xc0) != 0x80) break;
        if (k != count || (count == 3 && c == 0xe0 && raw[i + 1] < 0xa0) ||
            (count == 3 && c == 0xed && raw[i + 1] >= 0xa0) ||
            (count == 4 && c == 0xf0 && raw[i + 1] < 0x90) ||
            (count == 4 && c == 0xf4 && raw[i + 1] >= 0x90)) {
            e->ocr_text[used++] = '?'; i++; continue;
        }
        for (k = 0; k < count && used < OCR_TEXT_CAP; k++) e->ocr_text[used++] = (char)raw[i++];
    }
    e->ocr_text[used] = 0;
    e->ocr_status = OCR_COMPLETE;
    snprintf(e->ocr_message, sizeof(e->ocr_message), "OCR completed for live image generation %llu; the returned text is Tesseract output.",
             (unsigned long long)e->ocr_generation);
    { char detail[192]; snprintf(detail, sizeof(detail), "{\"generation\":%llu,\"bytes\":%llu,\"peak_memory_bytes\":%llu}",
        (unsigned long long)e->ocr_generation, (unsigned long long)used,
        (unsigned long long)e->ocr_peak_memory); journal(e, "ocr_complete", detail); }
    feedback(e, "%s", e->ocr_message);
}
static int vm_start(MgEngine *e,Json *j) {
    char qmp[128],vnc[128],qga[160],memory[32],video[128],disk[4096],cdrom[4096],seed_file[4096],seed_raw[512],seed_path[1024],username[33],password[257]="",bios[1024],error[512],data_dir[1024],helpers[1024];

    char *argv[64];
    char accel[64], smp[32], android_control[180], android_video[180];
    MgpVmLimits limits;
    MgConfig prior=e->cfg;
int n=0;
Buffer b;
    const char *extension;
char checkpoint_status[192];
int login_enabled=e->cfg.passwordless;
#define VM_START_FAIL() do { if(!e->status.owned_pid)e->cfg=prior; wipe_memory(password,sizeof(password)); return -1; } while (0)

    if(e->status.owned_pid&&mgp_alive(&e->process)){
        if(e->status.connected){feedback(e,"VM is already running; its current state is returned.");return 0;}
        journal(e,"control_reconnect","Owned VM remains alive; reconnecting control without replaying any uncertain mutation.");
        if(connect_qmp(e))VM_START_FAIL();
        refresh_state(e);
        feedback(e,"Control reconnected to the owned VM. Current state is confirmed; previous uncertain actions were not replayed.");
        return 0;
}
    if(config_string(j,"qemu",e->cfg.qemu,sizeof(e->cfg.qemu))||config_string(j,"disk",e->cfg.disk,sizeof(e->cfg.disk))||config_string(j,"iso",e->cfg.iso,sizeof(e->cfg.iso))||config_string(j,"seed_iso",e->cfg.seed_iso,sizeof(e->cfg.seed_iso))){feedback(e,"A file path is missing, too long or invalid JSON text.");
VM_START_FAIL();
}
    { unsigned value; int token;
      static const char *const keys[]={"cpus","cpu_percent","max_threads","android_port","video_port","stream_port"};
      unsigned *fields[]={&e->cfg.cpus,&e->cfg.cpu_percent,&e->cfg.max_threads,&e->cfg.android_port,&e->cfg.video_port,&e->cfg.stream_port};
      unsigned i;
      for(i=0;i<6;i++)if((token=jget(j,0,keys[i]))>=0){
          if(jnumber(j,token,&value)){feedback(e,"%s must be an integer.",keys[i]);VM_START_FAIL();}
          *fields[i]=value;
      }
      token=jget(j,0,"android");
      if(token>=0){
          if(!jeq(j,token,"true")&&!jeq(j,token,"false")){feedback(e,"android must be boolean.");VM_START_FAIL();}
          e->cfg.android=jeq(j,token,"true");
      }
      if(config_string(j,"accel",e->cfg.accel,sizeof(e->cfg.accel))||
         config_string(j,"disk_bus",e->cfg.disk_bus,sizeof(e->cfg.disk_bus))||
         config_string(j,"cgroup_parent",e->cfg.cgroup_parent,sizeof(e->cfg.cgroup_parent))){feedback(e,"Invalid acceleration or delegated resource path.");VM_START_FAIL();}
      if(!strcmp(e->cfg.accel,"auto"))snprintf(e->cfg.accel,sizeof(e->cfg.accel),"%s",mgp_default_accel());
      if(strcmp(e->cfg.accel,"kvm")&&strcmp(e->cfg.accel,"whpx")&&strcmp(e->cfg.accel,"hvf")&&strcmp(e->cfg.accel,"tcg")){
          feedback(e,"Choose auto, kvm, whpx, hvf or explicitly tcg. Accelerators never silently fall back.");VM_START_FAIL();
      }
      if(strcmp(e->cfg.disk_bus,"virtio")&&strcmp(e->cfg.disk_bus,"ide")){
          feedback(e,"Choose virtio or ide for guest disk transport.");VM_START_FAIL();
      }
      if(e->cfg.cpus<1||e->cfg.cpus>4||e->cfg.cpu_percent<1||e->cfg.cpu_percent>400||e->cfg.max_threads<8||e->cfg.max_threads>128){
          feedback(e,"Choose 1-4 vCPUs, CPU budget 1-400 percent of one host CPU and 8-128 host tasks.");VM_START_FAIL();
      }
      if(e->cfg.android&&!strcmp(e->cfg.accel,"tcg")){
          feedback(e,"Android native profile requires an explicitly available hardware accelerator; TCG is software emulation.");VM_START_FAIL();
      }
    }
    snprintf(username,sizeof(username),"megas");
    if(config_string(j,"username",username,sizeof(username))||config_string(j,"password",password,sizeof(password))){feedback(e,"Guest username or password exceeds its supported size.");wipe_memory(password,sizeof(password));return -1;}
    {int policy=jget(j,0,"passwordless");
        if(policy>=0){if(jeq(j,policy,"true"))login_enabled=1;
            else if(jeq(j,policy,"false"))login_enabled=0;
            else {feedback(e,"passwordless must be a JSON boolean.");VM_START_FAIL();}}
    }
    if(!e->cfg.android&&login_enabled&&password[0]){feedback(e,"Omit password when enabling passwordless sign-in; no guest command was sent.");wipe_memory(password,sizeof(password));return -1;}
    if(!e->cfg.android&&!login_enabled&&!password[0]){feedback(e,"A new managed guest starts with a locked account. Choose passwordless sign-in or provide a password before starting.");wipe_memory(password,sizeof(password));return -1;}
    e->cfg.passwordless=login_enabled;
    e->cfg.ram_mb=number_or(j,"ram_mb",e->cfg.ram_mb);
e->cfg.width=number_or(j,"width",e->cfg.width);
e->cfg.height=number_or(j,"height",e->cfg.height);

    if(e->cfg.ram_mb<16||e->cfg.ram_mb>32768||e->cfg.width<320||e->cfg.height<200||e->cfg.width>4096||e->cfg.height>4096||(uint64_t)e->cfg.width*e->cfg.height>9437184){feedback(e,"Choose RAM 16-32768 MB and a screen between 320x200 and 9 million pixels.");
VM_START_FAIL();
}
    if(!e->cfg.qemu[0]){feedback(e,"Select a QEMU executable first.");
wipe_memory(password,sizeof(password));return -1;
}
    if(!e->cfg.android&&guest_helper_dir(helpers,sizeof(helpers))){feedback(e,"Megas guest helpers are missing from the package; the managed guest policy cannot be verified.");wipe_memory(password,sizeof(password));return -1;}
    limits.cgroup_parent=e->cfg.cgroup_parent;
    limits.memory_bytes=((uint64_t)e->cfg.ram_mb+256)*1024*1024;
    limits.cpu_percent=e->cfg.cpu_percent;limits.tasks=e->cfg.max_threads;
    limits.required=e->cfg.android||e->cfg.cgroup_parent[0];
    {uint64_t available=mgp_available_memory();
     if(available&&available<limits.memory_bytes+256ULL*1024*1024){
         feedback(e,"Insufficient available host RAM for VM RAM, 256 MiB emulator allowance and 256 MiB host reserve. No VM started.");VM_START_FAIL();
     }}
    snprintf(qmp,sizeof(qmp),"tcp:127.0.0.1:%u,server=on,wait=off",e->cfg.qmp_port);
snprintf(vnc,sizeof(vnc),"127.0.0.1:%u",e->cfg.vnc_port-5900);
    snprintf(qga,sizeof(qga),"socket,id=megas-qga,host=127.0.0.1,port=%u,server=on,wait=off",e->cfg.qga_port);

    snprintf(memory,sizeof(memory),"%u",e->cfg.ram_mb);
snprintf(video,sizeof(video),"virtio-vga,xres=%u,yres=%u",e->cfg.width,e->cfg.height);

    argv[n++]=e->cfg.qemu;
argv[n++]="-accel";
snprintf(accel,sizeof(accel),"%s%s",e->cfg.accel,!strcmp(e->cfg.accel,"tcg")?",thread=single,tb-size=32":"");
argv[n++]=accel;
argv[n++]="-smp";snprintf(smp,sizeof(smp),"%u",e->cfg.cpus);argv[n++]=smp;
argv[n++]="-m";
argv[n++]=memory;
argv[n++]="-qmp";
argv[n++]=qmp;
argv[n++]="-vnc";
argv[n++]=vnc;

    argv[n++]="-display";
argv[n++]="none";
argv[n++]="-device";
argv[n++]=video;
argv[n++]="-fw_cfg";
argv[n++]="name=opt/megas/guest,string=1";

    argv[n++]="-smbios";
argv[n++]="type=1,serial=MEGAS-GUEST";

    argv[n++]="-chardev";
argv[n++]=qga;
argv[n++]="-device";
argv[n++]="virtio-serial-pci";
argv[n++]="-device";
argv[n++]="virtserialport,chardev=megas-qga,name=org.qemu.guest_agent.0";
    if(e->cfg.android){
        unsigned ports[]={e->cfg.control_port,e->cfg.qmp_port,e->cfg.vnc_port,e->cfg.qga_port,e->cfg.android_port,e->cfg.video_port,e->cfg.stream_port};
        unsigned a,c;
        for(a=0;a<7;a++){if(!ports[a]||ports[a]>65535){feedback(e,"Invalid Android channel port.");VM_START_FAIL();}
            for(c=0;c<a;c++)if(ports[a]==ports[c]){feedback(e,"Every VM channel needs a distinct loopback port.");VM_START_FAIL();}}
        snprintf(android_control,sizeof(android_control),"socket,id=megas-control,host=127.0.0.1,port=%u,server=on,wait=off",e->cfg.android_port);
        snprintf(android_video,sizeof(android_video),"socket,id=megas-video,host=127.0.0.1,port=%u,server=on,wait=off",e->cfg.video_port);
        argv[n++]="-chardev";argv[n++]=android_control;
        argv[n++]="-device";argv[n++]="virtserialport,chardev=megas-control,name=org.megas.control";
        argv[n++]="-chardev";argv[n++]=android_video;
        argv[n++]="-device";argv[n++]="virtserialport,chardev=megas-video,name=org.megas.video";
    }

    if(e->cfg.disk[0]){
        extension=strrchr(e->cfg.disk,'.');
e->snapshot_supported=extension&&!strcmp(extension,".qcow2");

        binit(&b,disk,sizeof(disk));
bput(&b,"{\"driver\":");
bstr(&b,e->snapshot_supported?"qcow2":"raw");
bput(&b,",\"node-name\":\"megasdisk\",\"file\":{\"driver\":\"file\",\"filename\":");
bstr(&b,e->cfg.disk);
bput(&b,"}}");

        if(b.error){feedback(e,"Disk path is too long.");
VM_START_FAIL();
}
        argv[n++]="-blockdev";
argv[n++]=disk;
argv[n++]="-device";
argv[n++]=!strcmp(e->cfg.disk_bus,"ide")?"ide-hd,drive=megasdisk":"virtio-blk-pci,drive=megasdisk";

    }else e->snapshot_supported=0;

    if(e->cfg.iso[0]){size_t i,used=0;
for(i=0;e->cfg.iso[i]&&used+2<sizeof(cdrom);i++){cdrom[used++]=e->cfg.iso[i];
if(e->cfg.iso[i]==',')cdrom[used++]=',';
}cdrom[used]=0;
argv[n++]="-cdrom";
argv[n++]=cdrom;
}
    if(!e->cfg.android){
    if(e->cfg.seed_iso[0]) {
        snprintf(seed_path,sizeof(seed_path),"%s",e->cfg.seed_iso);
        if(!mgp_file_size(seed_path)) {feedback(e,"The configured NoCloud seed ISO does not exist or is empty.");VM_START_FAIL();}
    } else {
        if(snprintf(seed_path,sizeof(seed_path),"%.900s/seed.iso",e->cfg.runtime_dir)>=(int)sizeof(seed_path)) {
            feedback(e,"The default NoCloud seed path is too long.");VM_START_FAIL();
        }
        if(mg_seed_write(seed_path,username,1,error,sizeof(error))) {
            feedback(e,"Could not create the default NoCloud seed: %s",error);wipe_memory(password,sizeof(password));return -1;
        }
    }
    binit(&b,seed_file,sizeof(seed_file));
    bput(&b,"{\"driver\":\"file\",\"node-name\":\"megasseedfile\",\"read-only\":true,\"filename\":");
    bstr(&b,seed_path);bput(&b,"}");
    if(b.error) {feedback(e,"The NoCloud seed path is too long.");VM_START_FAIL();}
    binit(&b,seed_raw,sizeof(seed_raw));
    bput(&b,"{\"driver\":\"raw\",\"node-name\":\"megasseed\",\"read-only\":true,\"file\":\"megasseedfile\"}");
    if(b.error) {feedback(e,"Could not configure the NoCloud seed device.");VM_START_FAIL();}
    argv[n++]="-blockdev";argv[n++]=seed_file;
    argv[n++]="-blockdev";argv[n++]=seed_raw;
    argv[n++]="-device";argv[n++]="ide-cd,drive=megasseed";
    }
    snprintf(bios,sizeof(bios),"%s",e->cfg.qemu);
{char *p=strrchr(bios,'/');

#ifdef _WIN32
        char *q=strrchr(bios,'\\');
if(q&&(!p||q>p))p=q;

#endif
        if(p){char firmware[1024];
*p=0;
snprintf(firmware,sizeof(firmware),"%.900s/../share/qemu",bios);
if(mgp_file_size(firmware)){strcpy(bios,firmware);
argv[n++]="-L";
argv[n++]=bios;
}}}
    if(!mgp_env("MEGAS_QEMU_DATA_DIR",data_dir,sizeof(data_dir))){argv[n++]="-L";
argv[n++]=data_dir;
}
    argv[n]=NULL;
e->disk_base=0;
e->disk_base_known=0;
e->snapshot_count=0;
e->snapshot_catalog_available=0;

    {char command[REQUEST_CAP];
Buffer audit;
binit(&audit,command,sizeof(command));
bput(&audit,"[");
for(n=0;argv[n];n++){if(n)bput(&audit,",");
bstr(&audit,argv[n]);
}bput(&audit,"]");
if(journal(e,"process_spawn",command))VM_START_FAIL();
}
    mg_live_free(e->live);e->live=NULL;
    if(e->cfg.android){
        e->live=mg_live_new(e->cfg.android_port,e->cfg.video_port,e->cfg.stream_port,e->token);
        if(!e->live){feedback(e,"Live stream port unavailable; no Android VM was started.");VM_START_FAIL();}
    }
    if(mgp_spawn_vm(&e->process,argv,e->qemu_log,&limits,error,sizeof(error))){feedback(e,"%s",error);
mg_live_free(e->live);e->live=NULL;
VM_START_FAIL();
}
    e->status.owned_pid=mgp_pid(&e->process);
strcpy(e->status.phase,"starting");

    if(connect_qmp(e))VM_START_FAIL();

    if(!qmp_exec(e,"query-commands",NULL,500)){e->snapshot_supported=e->snapshot_supported&&has_command(e,"snapshot-save")&&has_command(e,"snapshot-load");
}
    if(e->snapshot_supported){
        char baseline[128];
        if(!snapshot_disk_allocated(e,&e->disk_base,1)){
            e->disk_base_known=1;
            snprintf(baseline,sizeof(baseline),"QMP megasdisk allocated-size baseline=%llu bytes",(unsigned long long)e->disk_base);
            journal(e,"checkpoint_budget_baseline",baseline);
        }else journal(e,"checkpoint_budget_metric_unavailable","QEMU did not report one valid allocated-size for the managed megasdisk qcow2 node at startup.");
    }
    if(e->snapshot_supported){
        if(!e->snapshot_catalog_available)snprintf(checkpoint_status,sizeof(checkpoint_status),"Checkpoint catalog unavailable; existing QEMU states were not assumed empty and saves/restores are disabled.");
        else if(e->disk_base_known)snprintf(checkpoint_status,sizeof(checkpoint_status),"Checkpoint catalog verified; allocation budget: %llu MiB (minimum 2048 MiB, scales with RAM).",(unsigned long long)(snapshot_budget_bytes(e)/(1024*1024)));
        else snprintf(checkpoint_status,sizeof(checkpoint_status),"Existing checkpoints were imported; new saves are unavailable because disk allocation could not be verified.");
    }else snprintf(checkpoint_status,sizeof(checkpoint_status),"Manual checkpoints require a qcow2 disk and snapshot-capable QEMU.");
    if(mg_rfb_connect(e->display,"127.0.0.1",e->cfg.vnc_port,error,sizeof(error)))feedback(e,"VM started; live display is connecting: %.300s. %.180s",error,checkpoint_status);

    else {display_tick(e);
feedback(e,"VM started. Live display is connected. %s",checkpoint_status);
}
    refresh_state(e);
    if(e->cfg.android){
        e->android_state[0]=e->android_epoch[0]=0;e->android_revision=e->android_received=0;
        e->android_pending=0;e->android_uncertain=0;e->android_generation=0;
        e->auto_checkpoint=0;
        e->include_image=jeq(j,jget(j,0,"include_image"),"true");
        feedback(e,"Android VM started with bounded resources; waiting for its local semantic provider and encoded livestream.");
        wipe_memory(password,sizeof(password));return 0;
    }
    e->auto_checkpoint=1;
    if (e->cfg.seed_iso[0]) {
        /* A caller-supplied seed owns account creation; apply only the explicit
         * policy requested by this start after the guest agent identifies it. */
        if (jget(j,0,"passwordless") < 0 && !password[0]) {
            wipe_memory(password,sizeof(password));
            return 0;
        }
    }
    guest_auto_schedule(e, username, login_enabled, password);
    wipe_memory(password,sizeof(password));
#undef VM_START_FAIL
return 0;

}
static int create_disk(MgEngine *e, Json *j) {
    char path[1024], tool[1024], size[32], log[1024], error[512], audit[3072];
    char *argv[8]; unsigned gib = number_or(j, "size_gb", 16);
    MgpProcess *child = &e->utility; uint64_t deadline; Buffer b;
    if (jstring(j, jget(j, 0, "path"), path, sizeof(path)) || !path[0] || gib < 1 || gib > 1024) {
        feedback(e, "Choose a new disk path and size_gb between 1 and 1024."); return -1;
    }
    if(e->utility_live){feedback(e,"A disk utility is still being cleaned up; its process remains visible.");return -1;}
    if (mgp_create_exclusive(path)) { feedback(e, "This disk path already exists or cannot be reserved; existing work was preserved."); return -1; }
    if (mgp_env("MEGAS_QEMU_IMG", tool, sizeof(tool))) {
        char *slash;
        snprintf(tool, sizeof(tool), "%s", e->cfg.qemu); slash = strrchr(tool, '/');
#ifdef _WIN32
        { char *back = strrchr(tool, '\\'); if (back && (!slash || back > slash)) slash = back; }
#endif
        if (slash) {size_t prefix=(size_t)(slash-tool)+1;if(prefix+13>=sizeof(tool)){feedback(e,"Executable folder path is too long.");return -1;}strcpy(tool+prefix,"qemu-img");} else strcpy(tool, "qemu-img");
#ifdef _WIN32
        if (slash) strcat(tool, ".exe");
#endif
    }
    snprintf(size, sizeof(size), "%uG", gib);
    snprintf(log, sizeof(log), "%.900s/disk-create.log", e->cfg.runtime_dir);
    argv[0] = tool; argv[1] = "create"; argv[2] = "-f"; argv[3] = "qcow2";
    argv[4] = "--"; argv[5] = path; argv[6] = size; argv[7] = NULL;
    binit(&b, audit, sizeof(audit)); bput(&b, "{\"tool\":"); bstr(&b, tool);
    bput(&b, ",\"path\":"); bstr(&b, path); bfmt(&b, ",\"size_gb\":%u}", gib);
    if (journal(e, "disk_create_spawn", audit) || mgp_spawn(child, argv, log, error, sizeof(error))) {
        feedback(e, "Disk creation did not start. Inspect disk-create.log and the repair prompt."); return -1;
    }
    e->utility_live=1;
    snprintf(audit, sizeof(audit), "qemu-img owner child PID %d; output %.1023s", mgp_pid(child), log);
    journal(e, "owned_child", audit); deadline = mgp_now() + COMMAND_MS;
    while (mgp_alive(child) && mgp_now() < deadline) { display_tick(e); mgp_sleep(10); }
    if (mgp_alive(child)) {
        mgp_force_kill(child);
        feedback(e, "Disk creation exceeded its time budget. Its cleanup remains visible; inspect the partial file and repair prompt."); return -1;
    }
    e->utility_live=0;mgp_reap(child);
    if (child->exit_code || !mgp_file_size(path)) { feedback(e, "Disk creation was rejected. Repair prompt identifies the output log; existing work was preserved."); return -1; }
    snprintf(e->cfg.disk, sizeof(e->cfg.disk), "%s", path);
    journal(e, "disk_create_confirmed", path);
    feedback(e, "Created %u GB qcow2 disk. Select an operating system installer and start the VM.", gib); return 0;
}
static uint32_t key_name(const char *name) {
    static const struct{const char *name;
uint32_t code;
} keys[]={
        {"enter",0xff0d},{"return",0xff0d},{"escape",0xff1b},{"esc",0xff1b},{"tab",0xff09},{"backspace",0xff08},{"delete",0xffff},{"space",32},
        {"left",0xff51},{"up",0xff52},{"right",0xff53},{"down",0xff54},{"home",0xff50},{"end",0xff57},{"pageup",0xff55},{"pagedown",0xff56},
        {"ctrl",0xffe3},{"control",0xffe3},{"shift",0xffe1},{"alt",0xffe9},{"super",0xffeb},{"windows",0xffeb},{NULL,0}};

    char lower[64];
size_t i,n=strlen(name);
if(n==1)return (unsigned char)name[0];
if(n>=sizeof(lower))return 0;

    for(i=0;i<=n;i++)lower[i]=(char)tolower((unsigned char)name[i]);
for(i=0;keys[i].name;i++)if(!strcmp(lower,keys[i].name))return keys[i].code;

    if(lower[0]=='f'){char *end;
long f=strtol(lower+1,&end,10);
if(!*end&&f>=1&&f<=12)return (uint32_t)(0xffbd+f);
}return 0;

}
static int keys(MgEngine *e,Json *j) {
    char key[128];
unsigned code;
int down=jget(j,0,"down");

    if(jnumber(j,jget(j,0,"keysym"),&code)){if(jstring(j,jget(j,0,"key"),key,sizeof(key))){feedback(e,"Choose a key name or keysym.");
return -1;
}code=key_name(key);
}
    if(!code){feedback(e,"Key name is unknown. Use Enter, Escape, Tab, arrows, modifiers or a keysym.");
return -1;
}
    if(down>=0){if(!jeq(j,down,"true")&&!jeq(j,down,"false")){feedback(e,"Key down must be true or false.");
return -1;
}return mg_rfb_key(e->display,code,jeq(j,down,"true"));
}
    if(mg_rfb_key(e->display,code,1)){feedback(e,"Key press was not delivered.");
return -1;
}
    if(mg_rfb_key(e->display,code,0)){feedback(e,"Key release was not delivered; inspect live input state.");
return -1;
}return 0;

}
static int type_text(MgEngine *e,Json *j) {
    char text[REQUEST_CAP];
size_t i=0;
unsigned count=0;

    if(jstring(j,jget(j,0,"text"),text,sizeof(text))){feedback(e,"Text is missing or exceeds the bounded input size.");
return -1;
}
    while(text[i]){uint32_t cp=(unsigned char)text[i++],code;
unsigned extra=0,k;

        if(cp>=0xc2&&cp<=0xdf){cp&=31;
extra=1;
}else if(cp>=0xe0&&cp<=0xef){cp&=15;
extra=2;
}else if(cp>=0xf0&&cp<=0xf4){cp&=7;
extra=3;
}else if(cp>=0x80){feedback(e,"Text contains invalid UTF-8; no further keys sent.");
return -1;
}
        for(k=0;k<extra;k++){unsigned c=(unsigned char)text[i++];
if((c&0xc0)!=0x80){feedback(e,"Text contains invalid UTF-8; no further keys sent.");
return -1;
}cp=(cp<<6)|(c&63);
}
        if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)){feedback(e,"Text contains an invalid Unicode character.");
return -1;
}
        code=cp=='\n'||cp=='\r'?0xff0d:cp=='\t'?0xff09:cp<=255?cp:0x01000000|cp;

        if(mg_rfb_key(e->display,code,1)||mg_rfb_key(e->display,code,0)){feedback(e,"Text delivery stopped at character %u; the exact attempted action is journaled.",count);
return -1;
}
        count++;
if(!(count%32))display_tick(e);

    }return 0;

}
static const char *normalize(const char *name) {
    static const struct{const char *from,*to;
} aliases[]={
        {"boot","start"},{"launch","start"},{"poweron","start"},{"shutdown","stop"},{"poweroff","stop"},{"quit","stop"},
        {"continue","resume"},{"cont","resume"},{"unpause","resume"},{"halt","pause"},{"reboot","reset"},{"restart","reset"},
        {"screen","screenshot"},{"snapshot","checkpoint"},{"save","checkpoint"},{"load","restore"},{"history","steps"},{"state","status"},
        {"send_key","key"},{"press","key"},{"text","type"},{"mouse","click"},{"resolution","resize"},{"read_screen","ocr"},{NULL,NULL}};

    size_t i;
for(i=0;aliases[i].from;i++)if(!strcmp(name,aliases[i].from))return aliases[i].to;
return name;

}

/* The private provider owns the tree. The public host only checks immutable
 * per-revision handles and permitted operations announced in its envelope.
 * No coordinates, guessed handles, or private tree schemas are required. */
static unsigned android_op_bit(const char *operation)
{
    static const char *const names[]={
        "click","long_click","set_text","scroll_forward","scroll_backward","focus"
    };
    unsigned i;
    for(i=0;i<sizeof(names)/sizeof(names[0]);i++)
        if(!strcmp(operation,names[i]))return 1u<<i;
    return 0;
}

/* Compile observed capabilities once per revision. Action dispatch then
 * avoids reparsing the large guest event or allocating scratch storage. */
static int android_cache_targets(MgEngine *e, Json *j)
{
    int list=jget(j,0,"targets"),i;
    unsigned count=0;
    if(list<0||j->t[list].type!='[')return -1;
    for(i=list+1;i<j->n&&j->t[i].start<j->t[list].end;i++){
        MgAndroidTarget *node;
        unsigned prior;
        int operations,k;
        if(j->t[i].parent!=list)continue;
        if(j->t[i].type!='{'||count==ANDROID_TARGET_MAX)return -1;
        node=&e->android_targets[count];
        if(jstring(j,jget(j,i,"id"),node->id,sizeof(node->id))||
           !node->id[0])return -1;
        for(prior=0;prior<count;prior++)
            if(!strcmp(e->android_targets[prior].id,node->id))return -1;
        operations=jget(j,i,"operations");
        if(operations<0||j->t[operations].type!='[')return -1;
        node->allowed=0;
        for(k=operations+1;k<j->n&&j->t[k].start<j->t[operations].end;k++){
            char op[32];
            unsigned bit;
            if(j->t[k].parent!=operations)continue;
            if(jstring(j,k,op,sizeof(op))||
               !(bit=android_op_bit(op))||(node->allowed&bit))return -1;
            node->allowed|=(unsigned char)bit;
        }
        if(!node->allowed)return -1;
        count++;
    }
    e->android_target_count=count;
    return 0;
}
static int android_target_allowed(MgEngine *e,const char *id,unsigned operation)
{
    unsigned i;
    for(i=0;i<e->android_target_count;i++)
        if(!strcmp(e->android_targets[i].id,id))
            return !!(e->android_targets[i].allowed&operation);
    return 0;
}
static void android_invalidate(MgEngine *e)
{
    e->android_state[0]=e->android_epoch[0]=0;
    memset(e->android_targets,0,sizeof(e->android_targets));
    e->android_target_count=0;
    e->android_history_count=e->android_history_next=0;
    e->android_revision=e->android_received=0;
    e->android_guest_ms=e->android_guest_delta_ms=e->android_host_delta_ms=0;
    e->android_state_host_ms=e->android_cause_action_id=0;
    if(e->android_pending){e->android_uncertain=1;snprintf(e->android_outcome,sizeof(e->android_outcome),"uncertain");}
}
static int android_tick_one(MgEngine *e)
{
    const char *line;
    Json *j;
    uint64_t revision, id, guest_ms, cause;
    char epoch[65], type[32], outcome[32];
    if(!e->live)return 0;
    mg_live_tick(e->live);
    if(e->android_generation!=mg_live_generation(e->live)){
        e->android_generation=mg_live_generation(e->live);
        android_invalidate(e);
    }
    if(e->android_pending&&!e->android_uncertain&&mgp_now()-e->android_queued>=2000){
        e->android_uncertain=1;snprintf(e->android_outcome,sizeof(e->android_outcome),"uncertain");
        mg_live_abort_control(e->live);android_invalidate(e);
        journal(e,"android_action_uncertain","Guest result deadline expired; no automatic replay. Inspect state and explicitly acknowledge the uncertain action before another mutation.");
    }
    line=mg_live_line(e->live);
    if(!line[0])return 0;
    j=malloc(sizeof(*j));
    if(!j)return 0;
    if(jparse(j,line)||j->t[0].type!='{'||jstring(j,jget(j,0,"type"),type,sizeof(type)))goto invalid;
    if(!strcmp(type,"state")){
        if(jstring(j,jget(j,0,"epoch"),epoch,sizeof(epoch))||!epoch[0]||
           juint64(j,jget(j,0,"revision"),&revision)||!revision||
           juint64(j,jget(j,0,"guest_monotonic_ms"),&guest_ms)||
           juint64(j,jget(j,0,"cause_action_id"),&cause)||
           jget(j,0,"payload")<0)goto invalid;
        if(e->android_epoch[0]&&strcmp(epoch,e->android_epoch))android_invalidate(e);
        if(revision<=e->android_revision||
           (e->android_revision&&guest_ms<e->android_guest_ms)||
           android_cache_targets(e,j))goto invalid;
        if(e->android_revision){
            if(revision-e->android_revision>1)
                e->android_revision_gaps+=revision-e->android_revision-1;
            e->android_guest_delta_ms=guest_ms-e->android_guest_ms;
            e->android_host_delta_ms=mgp_now()-e->android_state_host_ms;
        }else{
            e->android_guest_delta_ms=e->android_host_delta_ms=0;
        }
        snprintf(e->android_epoch,sizeof(e->android_epoch),"%s",epoch);
        snprintf(e->android_state,sizeof(e->android_state),"%s",line);
        e->android_guest_ms=guest_ms;e->android_cause_action_id=cause;
        e->android_revision=revision;
        e->android_received=e->android_state_host_ms=mgp_now();
        e->android_events++;
        {
            MgAndroidEvent *event=&e->android_history[e->android_history_next];
            event->revision=revision;event->guest_ms=guest_ms;
            event->host_ms=e->android_state_host_ms;
            event->reported_cause=cause;event->targets=e->android_target_count;
            e->android_history_next=(e->android_history_next+1)%ANDROID_HISTORY_MAX;
            if(e->android_history_count<ANDROID_HISTORY_MAX)e->android_history_count++;
        }
        /* Private provider payloads are memory-only: never persist trees or text. */
    }else if(!strcmp(type,"heartbeat")){
        if(jstring(j,jget(j,0,"epoch"),epoch,sizeof(epoch))||strcmp(epoch,e->android_epoch)||
           juint64(j,jget(j,0,"revision"),&revision)||revision!=e->android_revision||!revision)goto invalid;
        e->android_received=mgp_now();
    }else if(!strcmp(type,"result")){
        if(juint64(j,jget(j,0,"id"),&id)||!id||id!=e->android_pending||
           jstring(j,jget(j,0,"outcome"),outcome,sizeof(outcome))||
           (strcmp(outcome,"applied")&&strcmp(outcome,"rejected")))goto invalid;
        snprintf(e->android_outcome,sizeof(e->android_outcome),"%s",outcome);
        e->android_completed=mgp_now();e->android_pending=0;e->android_uncertain=0;
        if(!strcmp(outcome,"rejected"))e->android_rejected++;
        journal(e,"android_action_result",outcome);
    }else goto invalid;
    free(j);return 1;
invalid:
    e->android_rejected++;
    /* Invalid state cannot leave previously usable handles looking fresh. */
    android_invalidate(e);
    free(j);
    return 1;
}
/* Drain bursts without forcing the agent to wait for N separate owner
 * iterations. Both work and latency are bounded; UI/video stay responsive. */
static void android_tick(MgEngine *e)
{
    unsigned processed;
    uint64_t began=mgp_now();
    for(processed=0;processed<32;processed++){
        if(!android_tick_one(e))break;
        if(mgp_now()-began>=2)break;
    }
}
static int android_act(MgEngine *e,Json *j)
{
    char epoch[65],target[257],operation[32],text[4097]="",wire[8192];
    uint64_t revision;
    Buffer b;
    if(!e->cfg.android||!e->live||!mg_live_control_up(e->live)||!mg_live_video_up(e->live)||!e->status.running){
        feedback(e,"Android semantic provider is unavailable; no coordinate or ADB fallback was used.");return -1;
    }
    if(e->android_pending){feedback(e,"Android action %llu is pending or uncertain; inspect state before another mutation.",(unsigned long long)e->android_pending);return -1;}
    if(!e->android_received||mgp_now()-e->android_received>2000||
       jstring(j,jget(j,0,"epoch"),epoch,sizeof(epoch))||strcmp(epoch,e->android_epoch)||
       juint64(j,jget(j,0,"expected_revision"),&revision)||revision!=e->android_revision){
        feedback(e,"Android state is missing, stale or has changed. Observe a fresh state and use its epoch/revision.");return -1;
    }
    if(jget(j,0,"x")>=0||jget(j,0,"y")>=0||
       jstring(j,jget(j,0,"target"),target,sizeof(target))||!target[0]||
       jstring(j,jget(j,0,"operation"),operation,sizeof(operation))||
       config_string(j,"text",text,sizeof(text))){feedback(e,"Choose a provider-issued target and semantic operation; coordinate actions are forbidden.");return -1;}
    {
        unsigned bit=android_op_bit(operation);
        if(!bit){feedback(e,"Unsupported semantic operation; no input was sent.");return -1;}
        if(!android_target_allowed(e,target,bit)){
            feedback(e,"Target or operation was not issued by the observed revision. No input was sent.");
            return -1;
        }
    }
    binit(&b,wire,sizeof(wire));
    bfmt(&b,"{\"type\":\"action\",\"id\":%llu,\"expected_revision\":%llu,\"epoch\":",
          (unsigned long long)(e->android_action_id+1),(unsigned long long)revision);
    bstr(&b,epoch);bput(&b,",\"target\":");bstr(&b,target);
    bput(&b,",\"operation\":");bstr(&b,operation);bput(&b,",\"text\":");bstr(&b,text);bput(&b,"}");
    if(b.error||mg_live_send(e->live,wire)){feedback(e,"Semantic transport busy; no action was queued.");return -1;}
    e->android_pending=++e->android_action_id;e->android_queued=mgp_now();e->android_completed=0;
    snprintf(e->android_outcome,sizeof(e->android_outcome),"queued");
    feedback(e,"Android action %llu queued; this is transport acceptance, not confirmed task success.",(unsigned long long)e->android_pending);
    return 0;
}
static void response(MgEngine *e,int ok,const char *action,char *out,size_t cap,int include_steps) {
    Buffer b;
unsigned i;
int image=e->include_image&&export_live(e)==0;
int ocr_status=e->ocr_status;
char fallback[1024];

    if(ocr_status==OCR_COMPLETE&&e->ocr_generation!=e->last_export_generation)ocr_status=OCR_STALE;

    snprintf(fallback,sizeof(fallback),"VM %s; %s; observed display %ux%u; live RFB generation %llu; %s. This text describes verified controller state, not OCR of guest screen.",
        e->status.phase,e->status.feedback,e->status.width,e->status.height,(unsigned long long)e->status.frame_generation,image?"latest live image available":"no live image yet");

    binit(&b,out,cap);
bfmt(&b,"{\"ok\":%s,\"action\":",ok?"true":"false");
bstr(&b,action);
bput(&b,",\"state\":{\"phase\":");
bstr(&b,e->status.phase);

    bfmt(&b,",\"connected\":%s,\"running\":%s,\"owned_pid\":%d,\"observed_width\":%u,\"observed_height\":%u,\"requested_width\":%u,\"requested_height\":%u,\"frame_generation\":%llu,\"fresh_state_age_ms\":%llu,\"snapshot_busy\":%s,\"uncertain_command\":%s,\"rollback_available\":%s,\"automatic_checkpoints\":%s,\"retained_checkpoints\":%u,\"owned_checkpoints\":%u,\"checkpoint_catalog_available\":%s,\"checkpoint_budget_bytes\":%llu,\"checkpoint_budget_baseline_available\":%s,\"last_restore_ms\":%.0f,\"guest_job\":{\"active\":%s,\"kind\":",
        e->status.connected?"true":"false",e->status.running?"true":"false",e->status.owned_pid,e->status.width,e->status.height,e->cfg.width,e->cfg.height,(unsigned long long)e->status.frame_generation,
        (unsigned long long)(e->last_state?mgp_now()-e->last_state:0),e->job_active?"true":"false",e->late_id?"true":"false",(e->snapshot_supported&&e->snapshot_catalog_available)?"true":"false",e->auto_checkpoint?"true":"false",e->snapshot_count,snapshot_owned_count(e),e->snapshot_catalog_available?"true":"false",(unsigned long long)(e->snapshot_supported?snapshot_budget_bytes(e):0),e->disk_base_known?"true":"false",e->status.last_restore_ms,e->guest_active?"true":"false");
    bstr(&b,e->guest_kind);
    bfmt(&b,",\"guest_pid\":%llu,\"automatic_pending\":%s,\"progress\":",(unsigned long long)e->guest_pid,e->guest_auto_pending?"true":"false");
    bstr(&b,e->guest_progress);
    bput(&b,"}},\"resources\":{\"accel\":");bstr(&b,e->cfg.accel);
    bput(&b,",\"disk_bus\":");bstr(&b,e->cfg.disk_bus);
    bfmt(&b,",\"vcpus\":%u,\"cpu_percent_of_one_core\":%u,\"max_host_tasks\":%u,\"qemu_threads\":%u,\"qemu_memory_bytes\":%llu,\"hard_task_limit\":%s},",
          e->cfg.cpus,e->cfg.cpu_percent,e->cfg.max_threads,e->status.qemu_threads,
          (unsigned long long)e->status.qemu_memory_bytes,e->process.cgroup[0]?"true":"false");
    bfmt(&b,"\"android\":{\"enabled\":%s,\"provider_connected\":%s,\"state_age_ms\":%llu,\"events\":%llu,\"rejected\":%llu,\"pending_action_id\":%llu,\"uncertain\":%s,\"last_action_id\":%llu,\"outcome\":",
          e->cfg.android?"true":"false",mg_live_control_up(e->live)?"true":"false",
          (unsigned long long)(e->android_received?mgp_now()-e->android_received:0),
          (unsigned long long)e->android_events,(unsigned long long)e->android_rejected,
          (unsigned long long)e->android_pending,e->android_uncertain?"true":"false",(unsigned long long)e->android_action_id);
    bstr(&b,e->android_outcome);
    bfmt(&b,",\"queued_host_ms\":%llu,\"completed_host_ms\":%llu,\"round_trip_ms\":%llu,\"observed\":",
          (unsigned long long)e->android_queued,(unsigned long long)e->android_completed,
          (unsigned long long)(e->android_completed?e->android_completed-e->android_queued:0));
    bput(&b,e->android_state[0]?e->android_state:"null");
    bfmt(&b,",\"state_metrics\":{\"revision\":%llu,\"guest_monotonic_ms\":%llu,\"guest_delta_ms\":%llu,\"host_received_monotonic_ms\":%llu,\"host_delta_ms\":%llu,\"reported_cause_action_id\":%llu,\"revision_gaps_total\":%llu,\"cause_trust\":\"guest_report_not_verified\"}",
          (unsigned long long)e->android_revision,
          (unsigned long long)e->android_guest_ms,
          (unsigned long long)e->android_guest_delta_ms,
          (unsigned long long)e->android_state_host_ms,
          (unsigned long long)e->android_host_delta_ms,
          (unsigned long long)e->android_cause_action_id,
          (unsigned long long)e->android_revision_gaps);
    bput(&b,",\"recent_state_events\":[");
    for(i=0;i<e->android_history_count;i++){
        const MgAndroidEvent *ev=&e->android_history[
            (e->android_history_next+ANDROID_HISTORY_MAX-e->android_history_count+i)%ANDROID_HISTORY_MAX
        ];
        if(i)bput(&b,",");
        bfmt(&b,"{\"revision\":%llu,\"guest_ms\":%llu,\"host_ms\":%llu,\"reported_cause_action_id\":%llu,\"target_count\":%u}",
             (unsigned long long)ev->revision,
             (unsigned long long)ev->guest_ms,
             (unsigned long long)ev->host_ms,
             (unsigned long long)ev->reported_cause,ev->targets);
    }
    bput(&b,"]");
    bfmt(&b,"},\"livestream\":{\"available\":%s,\"transport\":\"mpegts-http\",\"url\":\"http://127.0.0.1:%u/live.ts\",\"authorization\":\"Bearer session credential; never put it in a URL\",\"bytes_relayed\":%llu,\"source_age_ms\":%llu,\"slow_clients_disconnected\":%llu},\"ocr\":{\"status\":",
          mg_live_video_up(e->live)?"true":"false",e->cfg.stream_port,
          (unsigned long long)mg_live_bytes(e->live),(unsigned long long)mg_live_video_age(e->live),
          (unsigned long long)mg_live_slow_clients(e->live));
    bstr(&b,ocr_status_name(ocr_status));
    bfmt(&b,",\"generation\":%llu,\"pid\":%llu,\"peak_memory_bytes\":%llu,\"text\":",
         (unsigned long long)e->ocr_generation,(unsigned long long)e->ocr_pid,
         (unsigned long long)e->ocr_peak_memory);
    bstr(&b,ocr_status==OCR_COMPLETE?e->ocr_text:"");
    bput(&b,",\"message\":");bstr(&b,e->ocr_message);
    bput(&b,"},\"feedback\":");

    bstr(&b,e->status.feedback);
bput(&b,",\"latest_image\":");
if(image)bstr(&b,e->status.image_path);
else bput(&b,"null");

    bput(&b,",\"image_path\":");
bstr(&b,image?e->status.image_path:"");

    bfmt(&b,",\"frame_age_ms\":%llu,\"image_generation\":%llu",(unsigned long long)(e->last_frame?mgp_now()-e->last_frame:0),(unsigned long long)e->last_export_generation);

    bfmt(&b,",\"image_age_ms\":%llu,\"fallback_text\":",(unsigned long long)(e->image_ms?mgp_now()-e->image_ms:0));
bstr(&b,fallback);

    bfmt(&b,",\"statistics\":{\"actions\":%llu,\"failed\":%llu,\"normalized\":%llu,\"events\":%llu},\"journal\":",(unsigned long long)e->status.actions,(unsigned long long)e->status.failed,(unsigned long long)e->status.normalized,(unsigned long long)e->status.events);

    bstr(&b,e->status.journal_path);
bput(&b,",\"repair_prompt\":");
bstr(&b,e->status.repair_prompt);

    if(include_steps){bput(&b,",\"checkpoints\":[");
for(i=0;i<e->snapshot_count;i++){if(i)bput(&b,",");
bput(&b,"{\"name\":");
bstr(&b,e->snapshots[i].name);
bfmt(&b,",\"step\":%llu,\"owned\":%s,\"restorable\":%s}",(unsigned long long)e->snapshots[i].step,e->snapshots[i].owned?"true":"false",e->snapshots[i].restorable?"true":"false");
}bput(&b,"],\"recent_steps\":[");

        for(i=0;i<e->history_count;i++){unsigned index=(e->history_next+HISTORY_CAP-e->history_count+i)%HISTORY_CAP;
History *h=&e->history[index];
if(i)bput(&b,",");
bfmt(&b,"{\"step\":%llu,\"ok\":%s,\"duration_ms\":%llu,\"action\":",(unsigned long long)h->step,h->ok?"true":"false",(unsigned long long)h->ms);
bstr(&b,h->action);
bput(&b,",\"before\":");
bstr(&b,h->before);
bput(&b,",\"after\":");
bstr(&b,h->after);
bput(&b,"}");
}bput(&b,"]");
}
    bput(&b,"}");
if(b.error&&cap>512)snprintf(out,cap,"{\"ok\":false,\"feedback\":\"Response capacity is too small. Use at least 64 KiB for steps.\",\"state\":{\"phase\":\"%s\"},\"latest_image\":null,\"fallback_text\":\"Controller remains available; request status separately.\"}",e->status.phase);

}
static int vm_stop(MgEngine *e) {
    uint64_t deadline;
    mg_live_free(e->live);e->live=NULL;android_invalidate(e);

    if(!e->status.owned_pid){feedback(e,"VM is already stopped.");
return 0;
}
    if(e->qmp!=MGP_BAD_SOCKET&&!e->late_id)qmp_exec(e,"quit",NULL,500);

    deadline=mgp_now()+1000;
while(mgp_alive(&e->process)&&mgp_now()<deadline)mgp_sleep(10);

    if(mgp_alive(&e->process)){journal(e,"process_terminate","Owned QEMU did not exit after quit; terminating requested VM.");
mgp_terminate(&e->process);
}
    deadline=mgp_now()+500;
while(mgp_alive(&e->process)&&mgp_now()<deadline)mgp_sleep(10);

    if(mgp_alive(&e->process)){mgp_force_kill(&e->process);
deadline=mgp_now()+500;
while(mgp_alive(&e->process)&&mgp_now()<deadline)mgp_sleep(10);
}
    mgp_reap(&e->process);
e->status.owned_pid=0;
e->status.running=0;
e->job_active=0;
e->late_id=0;
    if(e->guest_active){journal(e,"guest_job_cancelled","VM stopped before the guest helper confirmed completion.");mg_guest_clear(&e->guest);e->guest_active=0;}
    e->guest_auto_pending=0;e->guest_auto_running=0;
    wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
    e->guest_checkpoint=0;

    qmp_disconnect(e);
mg_rfb_free(e->display);
e->display=mg_rfb_new();
e->status.frame_generation=0;
e->last_export_generation=0;

    strcpy(e->status.phase,"stopped");
feedback(e,"VM stopped; its command and event journal is preserved.");
journal(e,"process_exit","Owned QEMU stopped.");
return 0;

}
static int launch_harness(MgEngine *e,Json *j) {
    char name[32],prompt[8192],path[1024],error[512];
 char *argv[9];
unsigned index;
int grok=0;

    if(jstring(j,jget(j,0,"name"),name,sizeof(name))){feedback(e,"Choose Claude Code, OpenCode or Codex for repair.");
return -1;
}
    if(!strcmp(name,"claude")){index=0;
argv[0]="claude";
argv[1]="-p";
}
    else if(!strcmp(name,"opencode")){index=1;
argv[0]="opencode";
argv[1]="run";
}
    else if(!strcmp(name,"codex")){index=2;
argv[0]="codex";
argv[1]="exec";
}
    else if(!strcmp(name,"grok")){index=3;grok=1;
argv[0]="grok";
argv[1]="--no-auto-update";
argv[2]="--no-alt-screen";
argv[3]="-p";
}
    else {feedback(e,"This harness has no verified native launch contract. The repair prompt remains copyable; no invented command was run.");
return -1;
}
    if(mgp_alive(&e->harnesses[index])){feedback(e,"The selected repair agent is already running; see its visible log.");
return -1;
}
    if(jget(j,0,"prompt")<0)snprintf(prompt,sizeof(prompt),"%s",e->status.repair_prompt);

    else if(jstring(j,jget(j,0,"prompt"),prompt,sizeof(prompt))){feedback(e,"Repair prompt exceeds the supported length.");
return -1;
}
    if(grok){argv[4]=prompt;argv[5]="--output-format";argv[6]="plain";argv[7]=NULL;}
    else {argv[2]=prompt;argv[3]=NULL;}

    snprintf(path,sizeof(path),"%.900s/harness-%s.log",e->cfg.runtime_dir,name);

    {char record[REQUEST_CAP+2048];
Buffer b;
binit(&b,record,sizeof(record));
bput(&b,"{\"harness\":");
bstr(&b,name);
bput(&b,",\"prompt\":");
bstr(&b,prompt);
bput(&b,",\"output_log\":");
bstr(&b,path);
bput(&b,"}");
if(journal(e,"harness_spawn",record))return -1;
}
    mgp_reap(&e->harnesses[index]);

    if(mgp_spawn(&e->harnesses[index],argv,path,error,sizeof(error))){feedback(e,"%s",error);
return -1;
}
    e->harness_live[index]=1;
    feedback(e,"Repair agent %s started as PID %d. Progress output: %s",name,mgp_pid(&e->harnesses[index]),path);
return 0;

}
static void history_after_checkpoint(MgEngine *e,uint64_t step,const char *name)
{
    unsigned i;
    for(i=0;i<e->history_count;i++){
        unsigned index=(e->history_next+HISTORY_CAP-e->history_count+i)%HISTORY_CAP;
        if(e->history[index].step==step){snprintf(e->history[index].after,sizeof(e->history[index].after),"%.63s",name);return;}
    }
}
static int guest_job_start(MgEngine *e,Json *j,const char *kind,History *history)
{
    char helpers[1024],username[33]="megas",password[257]="",os[16]="",wad[1024]="",error[1024],audit[1400];
    int enabled=e->cfg.passwordless,token;
    Buffer b;
    e->guest_password_set=0;
    if(!e->status.connected||!e->status.running){feedback(e,"Resume a running VM before starting a guest policy or Doom job.");return -1;}
    if(e->guest_auto_pending&&(!e->guest_active||
       (e->guest_auto_running&&!e->guest.uncertain))){
        if(e->guest_active)mg_guest_clear(&e->guest);
        e->guest_active=0;e->guest_auto_running=0;e->guest_auto_pending=0;
        wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
        journal(e,"guest_auto_cancelled","An explicit user guest job replaced the bounded automatic QGA probe before any helper upload began.");
    }
    if(e->guest_active||e->guest_auto_pending){feedback(e,"A guest helper is already pending after its identity check; inspect its visible progress before retrying.");return -1;}
    if(guest_helper_dir(helpers,sizeof(helpers))){feedback(e,"Megas guest helper files were not found beside the installed package. No guest command was sent.");return -1;}
    if(!strcmp(kind,"password")){
        if(config_string(j,"username",username,sizeof(username))||config_string(j,"password",password,sizeof(password))||config_string(j,"os",os,sizeof(os))){feedback(e,"Guest username, password or OS hint exceeds its supported size.");goto invalid;}
        token=jget(j,0,"enabled");
        if(token>=0){if(jeq(j,token,"true"))enabled=1;else if(jeq(j,token,"false"))enabled=0;else {feedback(e,"Guest policy enabled must be a JSON boolean.");goto invalid;}}
        if(enabled&&password[0]){feedback(e,"Omit password when enabling passwordless sign-in; no password was sent to the guest.");goto invalid;}
        if(strlen(username)>32){feedback(e,"Guest username must be at most 32 characters.");goto invalid;}
        e->guest_passwordless_before=e->cfg.passwordless;
        e->guest_passwordless=enabled;
        e->guest_password_set=!enabled&&password[0];
    }else if(!strcmp(kind,"doom")){
        if(config_string(j,"wad",wad,sizeof(wad))){feedback(e,"Doom WAD path exceeds its supported size.");goto invalid;}
    }
    if(e->auto_checkpoint&&e->snapshot_supported){
        char desired[64];
        snprintf(desired,sizeof(desired),"step-%llu-before",(unsigned long long)history->step);
        if(snapshot_save(e,desired,1,history->before,sizeof(history->before)))goto invalid;
        e->guest_checkpoint=1;
    }else e->guest_checkpoint=0;
    if(!strcmp(kind,"password")){
        if(mg_guest_policy_start(&e->guest,e->cfg.qga_port,os,enabled,username,password,helpers,error,sizeof(error))){feedback(e,"Guest policy was not queued: %s",error);goto failed_start;}
    }else if(mg_guest_doom_start(&e->guest,e->cfg.qga_port,helpers,wad,error,sizeof(error))){feedback(e,"Doom was not queued: %s",error);goto failed_start;}
    e->guest_active=1;
    e->guest_step=history->step;
    snprintf(e->guest_kind,sizeof(e->guest_kind),"%s",kind);
    snprintf(e->guest_progress,sizeof(e->guest_progress),"%s",e->guest.progress[0]?e->guest.progress:"Guest helper queued; completion is not confirmed yet.");
    e->guest_pid=e->guest.pid;
    binit(&b,audit,sizeof(audit));
    bput(&b,"{\"kind\":");bstr(&b,kind);
    bfmt(&b,",\"guest_pid\":%llu",(unsigned long long)e->guest_pid);
    if(username[0]&&!strcmp(kind,"password")){bput(&b,",\"username\":");bstr(&b,username);}
    bput(&b,"}");
    if(!journal(e,"guest_job_queued",audit)){
        feedback(e,"Guest %s queued; guest PID %llu. %s",kind,(unsigned long long)e->guest_pid,e->guest_progress);
    }else{
        feedback(e,"Guest %s queued; guest PID %llu. Audit journal is full, so preserve it and do not retry blindly.",kind,(unsigned long long)e->guest_pid);
    }
    wipe_memory(password,sizeof(password));
    return 0;
failed_start:
    e->guest_checkpoint=0;
invalid:
    mg_guest_clear(&e->guest);
    wipe_memory(password,sizeof(password));
    return -1;
}
static void guest_job_tick(MgEngine *e)
{
    char error[1024]={0},audit[1200];
    Buffer b;
    uint64_t old_pid=e->guest_pid;
    int result,was_auto=e->guest_auto_running;
    if(!e->guest_active)return;
    result=mg_guest_poll(&e->guest,error,sizeof(error));
    if(e->guest.pid!=old_pid)e->guest_pid=e->guest.pid;
    if((e->guest.progress[0]&&strcmp(e->guest.progress,e->guest_progress))||e->guest.pid!=old_pid){
        if(e->guest.progress[0])snprintf(e->guest_progress,sizeof(e->guest_progress),"%s",e->guest.progress);
        binit(&b,audit,sizeof(audit));
        bput(&b,"{\"kind\":");bstr(&b,e->guest_kind);
        bfmt(&b,",\"guest_pid\":%llu,\"progress\":",(unsigned long long)e->guest_pid);
        bstr(&b,e->guest_progress);bput(&b,"}");
        if(!b.error)journal(e,"guest_job_progress",audit);
        if(e->guest_pid)feedback(e,"Guest %s: %s (guest PID %llu)",e->guest_kind,e->guest_progress,(unsigned long long)e->guest_pid);
        else feedback(e,"Guest %s: %s",e->guest_kind,e->guest_progress);
    }
    /* guest-file-open is queued by mg_guest_poll after identity verification,
     * but is not sent until its next poll. Capture the automatic before-state
     * in this gap so no guest helper bytes are uploaded before the checkpoint. */
    if(!result&&was_auto&&e->guest.uncertain&&!e->guest_checkpoint&&
       e->auto_checkpoint&&e->snapshot_supported){
        char before[64],desired[64];
        snprintf(desired,sizeof(desired),"step-%llu-before",(unsigned long long)e->guest_auto_step);
        if(snapshot_save(e,desired,1,before,sizeof(before))){
            char reason[512];snprintf(reason,sizeof(reason),"%.511s",e->status.feedback);
            journal(e,"guest_auto_checkpoint_failed","Automatic guest policy stopped before helper upload because its before checkpoint was not confirmed.");
            mg_guest_clear(&e->guest);e->guest_active=0;e->guest_auto_running=0;
            e->guest_auto_pending=0;e->guest_checkpoint=0;
            wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
            feedback(e,"Automatic guest policy was not applied because its before checkpoint failed: %s",reason);
            return;
        }
        e->guest_checkpoint=1;
        history_after_checkpoint(e,e->guest_auto_step,before);
    }
    if(!result)return;
    e->guest_active=0;
    if(result>0){
        if(!strcmp(e->guest_kind,"password")&&
           (e->guest_passwordless||e->guest_password_set))
            e->cfg.passwordless=e->guest_passwordless;
        snprintf(e->guest_progress,sizeof(e->guest_progress),"%s",e->guest.progress[0]?e->guest.progress:"Guest helper completed successfully.");
        snprintf(audit,sizeof(audit),"{\"kind\":\"%.23s\",\"guest_pid\":%llu,\"confirmed\":true}",e->guest_kind,(unsigned long long)e->guest_pid);
        journal(e,"guest_job_complete",audit);
        if(!strcmp(e->guest_kind,"password")&&!e->guest_passwordless&&!e->guest_password_set)
            feedback(e,"Guest helper completed with confirmed exit status; the saved sign-in policy was restored, while the account password state was not changed or inferred.");
        else feedback(e,"Guest %s completed successfully; guest PID %llu confirmed.",e->guest_kind,(unsigned long long)e->guest_pid);
        if(e->guest_checkpoint){
        char after[64],desired[64];
            snprintf(desired,sizeof(desired),"step-%llu-after",(unsigned long long)e->guest_step);
            if(snapshot_save(e,desired,1,after,sizeof(after))){
                char reason[512];snprintf(reason,sizeof(reason),"%.511s",e->status.feedback);
                feedback(e,"Guest %s completed, but its after-checkpoint is unconfirmed: %s",e->guest_kind,reason);
            }else history_after_checkpoint(e,e->guest_step,after);
        }
        if(was_auto){
            e->guest_auto_pending=0;e->guest_auto_running=0;
            if(e->guest_auto_marker[0]){
                FILE *marker=mgp_fopen(e->guest_auto_marker,"wb");
                if(marker){fputs("confirmed\n",marker);fclose(marker);}
                else journal(e,"guest_auto_marker_failed","Guest policy was confirmed, but its local per-disk completion marker could not be saved; next start may reapply the helper.");
            }
            wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
            snprintf(e->guest_progress,sizeof(e->guest_progress),"Managed guest login policy confirmed; guest helper exit status is zero.");
            feedback(e,"Managed guest login policy was confirmed by the guest helper; checkpoint history shows any captured before/after state.");
        }
    }else{
        if(!strcmp(e->guest_kind,"password"))e->cfg.passwordless=e->guest_passwordless_before;
        if(was_auto&&!e->guest.synced&&!e->guest.uncertain&&
           e->guest_auto_pending&&mgp_now()<e->guest_auto_deadline&&e->guest_auto_attempts<60){
            e->guest_active=0;e->guest_auto_running=0;e->guest_pid=0;
            e->guest_auto_next=mgp_now()+2000;
            if(e->guest_auto_attempts==1||e->guest_auto_attempts%10==0){
                snprintf(e->guest_progress,sizeof(e->guest_progress),"QEMU Guest Agent is not ready; bounded retry %d/60 is scheduled. No guest account command was sent.",e->guest_auto_attempts);
                feedback(e,"%s",e->guest_progress);
            }
            snprintf(audit,sizeof(audit),"{\"attempt\":%d,\"next_retry_ms\":2000,\"guest_mutation_uncertain\":false}",e->guest_auto_attempts);
            journal(e,"guest_auto_retry",audit);
            mg_guest_clear(&e->guest);
            return;
        }
        if(error[0])feedback(e,"Guest %s was not confirmed: %s",e->guest_kind,error);
        else feedback(e,"Guest %s ended without a confirmed success; do not assume the guest changed.",e->guest_kind);
        snprintf(audit,sizeof(audit),"{\"kind\":\"%.23s\",\"guest_pid\":%llu,\"confirmed\":false}",e->guest_kind,(unsigned long long)e->guest_pid);
        journal(e,"guest_job_failed",audit);
        e->guest_checkpoint=0;
        if(was_auto){e->guest_auto_pending=0;e->guest_auto_running=0;wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));}
    }
    e->guest_password_set=0;
    mg_guest_clear(&e->guest);
}
static void guest_auto_schedule(MgEngine *e,const char *username,int enabled,const char *password)
{
    uint64_t hash=1469598103934665603ULL;
    const unsigned char *p;
    char material[1100];
    if(!e||!username)return;
    e->guest_auto_pending=0;e->guest_auto_running=0;e->guest_auto_attempts=0;
    e->guest_auto_checkpoint=0;e->guest_auto_enabled=!!enabled;
    e->guest_auto_step=e->status.actions;
    e->guest_auto_password_set=password&&password[0];
    snprintf(e->guest_auto_username,sizeof(e->guest_auto_username),"%.32s",username);
    e->guest_auto_os[0]=0;
    snprintf(e->guest_auto_password,sizeof(e->guest_auto_password),"%s",password?password:"");
    e->guest_auto_marker[0]=0;
    if(e->cfg.disk[0]&&enabled&&!(password&&password[0])){
        snprintf(material,sizeof(material),"%.1020s|%.32s",e->cfg.disk,username);
        for(p=(const unsigned char *)material;*p;p++){hash^=*p;hash*=1099511628211ULL;}
        snprintf(e->guest_auto_marker,sizeof(e->guest_auto_marker),"%.900s/policy-%016llx.done",e->cfg.runtime_dir,(unsigned long long)hash);
        if(mgp_file_size(e->guest_auto_marker)){
            e->guest_auto_password[0]=0;
            snprintf(e->guest_progress,sizeof(e->guest_progress),"Previously confirmed login policy found for this disk and user; no guest helper was re-run.");
            feedback(e,"%s",e->guest_progress);
            return;
        }
    }
    if(e->cfg.disk[0]&&enabled&&password&&password[0]){
        /* A changed password must always reach the helper, so do not reuse the
         * passwordless completion marker for this explicit request. */
        e->guest_auto_marker[0]=0;
    }
    e->guest_auto_pending=1;
    e->guest_auto_deadline=mgp_now()+120000;
    e->guest_auto_next=mgp_now()+250;
    e->guest_auto_step=e->status.actions;
    e->guest_auto_password_set=password&&password[0];
    snprintf(e->guest_progress,sizeof(e->guest_progress),"Waiting for the loopback QEMU Guest Agent; normal VM input remains available until a verified helper upload begins.");
    feedback(e,"%s",e->guest_progress);
    journal(e,"guest_auto_scheduled","Bounded managed-guest policy check scheduled; QGA identity is required before helper upload.");
}
static void guest_auto_tick(MgEngine *e)
{
    char helpers[1024],error[1024],audit[256];
    if(!e->guest_auto_pending||e->guest_auto_running||e->guest_active)return;
    if(!e->status.owned_pid||!mgp_alive(&e->process)){
        e->guest_auto_pending=0;wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
        journal(e,"guest_auto_cancelled","Owned QEMU exited before automatic guest policy began.");
        return;
    }
    if(mgp_now()>=e->guest_auto_deadline||e->guest_auto_attempts>=60){
        e->guest_auto_pending=0;wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
        snprintf(e->guest_progress,sizeof(e->guest_progress),"QEMU Guest Agent did not become ready within the bounded 120-second window. The VM remains usable; no guest account change is assumed.");
        journal(e,"guest_auto_expired","Bounded QGA readiness attempts expired without an unconfirmed guest mutation.");
        feedback(e,"%s",e->guest_progress);
        return;
    }
    if(mgp_now()<e->guest_auto_next)return;
    if(guest_helper_dir(helpers,sizeof(helpers))){
        e->guest_auto_pending=0;wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
        snprintf(e->guest_progress,sizeof(e->guest_progress),"Packaged guest helpers are unavailable; no guest policy command was sent.");
        journal(e,"guest_auto_failed","Packaged guest policy helper was not found.");feedback(e,"%s",e->guest_progress);return;
    }
    e->guest_auto_attempts++;
    if(mg_guest_policy_start(&e->guest,e->cfg.qga_port,e->guest_auto_os,
            e->guest_auto_enabled,e->guest_auto_username,e->guest_auto_password,
            helpers,error,sizeof(error))){
        mg_guest_clear(&e->guest);
        e->guest_auto_next=mgp_now()+2000;
        snprintf(audit,sizeof(audit),"{\"attempt\":%d,\"queued\":false,\"guest_mutation_uncertain\":false}",e->guest_auto_attempts);
        journal(e,"guest_auto_probe",audit);
        if(e->guest_auto_attempts==1||e->guest_auto_attempts%10==0){
            snprintf(e->guest_progress,sizeof(e->guest_progress),"QEMU Guest Agent probe %d/60 did not connect; retry in 2 seconds. Normal VM input remains available.",e->guest_auto_attempts);
            feedback(e,"%s",e->guest_progress);
        }
        return;
    }
    e->guest_active=1;e->guest_auto_running=1;e->guest_checkpoint=0;
    e->guest_step=e->guest_auto_step;e->guest_passwordless_before=e->cfg.passwordless;
    e->guest_passwordless=e->guest_auto_enabled;e->guest_password_set=e->guest_auto_password_set;
    snprintf(e->guest_kind,sizeof(e->guest_kind),"password");
    snprintf(e->guest_progress,sizeof(e->guest_progress),"%s",e->guest.progress[0]?e->guest.progress:"Guest Agent identity check queued.");
    e->guest_pid=e->guest.pid;
    snprintf(audit,sizeof(audit),"{\"attempt\":%d,\"queued\":true,\"guest_pid\":%llu}",e->guest_auto_attempts,(unsigned long long)e->guest_pid);
    journal(e,"guest_auto_queued",audit);
    feedback(e,"Managed guest policy job queued. %s",e->guest_progress);
}
static int sensitive_json_key(Json *j, int key)
{
    char decoded[64];
    size_t i;
    if (jstring(j,key,decoded,sizeof(decoded))) return 1;
    for(i=0;decoded[i];i++)decoded[i]=(char)tolower((unsigned char)decoded[i]);
    return !strcmp(decoded,"auth")||!strcmp(decoded,"password")||
           !strcmp(decoded,"passphrase")||!strcmp(decoded,"secret")||
           !strcmp(decoded,"token");
}
static int json_token_after(Json *j, int token)
{
    int after=token+1;
    while(after<j->n&&j->t[after].start<j->t[token].end)after++;
    return after;
}
static void json_safe_value(Json *j, int token, Buffer *b)
{
    int i;
    if(token<0||token>=j->n){b->error=1;return;}
    if(j->t[token].type=='{'){
        int first=1;
        bput(b,"{");
        for(i=token+1;i<j->n&&j->t[i].start<j->t[token].end;){
            int key=i,value=key+1;
            if(j->t[key].parent!=token||j->t[key].type!='s'||value>=j->n||j->t[value].parent!=token){b->error=1;return;}
            if(!first)bput(b,",");
            first=0;
            bfmt(b,"%.*s:",j->t[key].end-j->t[key].start,j->s+j->t[key].start);
            if(sensitive_json_key(j,key))bput(b,"\"[redacted]\"");
            else json_safe_value(j,value,b);
            i=json_token_after(j,value);
        }
        bput(b,"}");
    }else if(j->t[token].type=='['){
        int first=1;
        bput(b,"[");
        for(i=token+1;i<j->n&&j->t[i].start<j->t[token].end;){
            if(j->t[i].parent==token){
                if(!first)bput(b,",");
                first=0;
                json_safe_value(j,i,b);
                i=json_token_after(j,i);
            }else i++;
        }
        bput(b,"]");
    }else{
        int length=j->t[token].end-j->t[token].start;
        if(length<0||(size_t)length>=b->cap-b->n){b->error=1;return;}
        memcpy(b->s+b->n,j->s+j->t[token].start,(size_t)length);
        b->n+=(size_t)length;b->s[b->n]=0;
    }
}
static int request_log(MgEngine *e,Json *j,const char *action) {
    char sanitized[REQUEST_CAP];
Buffer b;
    if(!strcmp(action,"android_act"))
        return journal(e,"action_request","Android semantic action requested; private target and text are not persisted.");

    binit(&b,sanitized,sizeof(sanitized));
json_safe_value(j,0,&b);
return b.error?-1:journal(e,"action_request",sanitized);

}
int mg_engine_submit(MgEngine *e,const char *request,char *out,size_t cap) {
    Json *j;
char *request_copy=NULL;
size_t request_length=0;
char requested[64]="invalid",action[64]="invalid",name[64],error[512];

    uint64_t begun=mgp_now();
int ok=-1,mutating=0,capture=0;
History *h=NULL;

    if(!e||!request||!out||cap<512)return -1;
    e->include_image=!e->cfg.android;

    j=malloc(sizeof(*j));
if(!j)return -1;

    request_length=strlen(request);
    e->include_image=!e->cfg.android;
    if(request_length>=REQUEST_CAP){feedback(e,"Use a JSON object with an action. Invalid input is logged and the controller stays available.");
journal(e,"invalid_request","Malformed or oversized JSON object received.");
goto done;
}
    request_copy=malloc(request_length+1);
    if(!request_copy){feedback(e,"Could not allocate the bounded request buffer.");goto done;}
    memcpy(request_copy,request,request_length+1);
    if(jparse(j,request_copy)||j->t[0].type!='{'){feedback(e,"Use a JSON object with an action. Invalid input is logged and the controller stays available.");
journal(e,"invalid_request","Malformed or oversized JSON object received.");
goto done;
}
    {int token=jget(j,0,"action");
if(token<0)token=jget(j,0,"command");
if(token<0)token=jget(j,0,"cmd");

        if(jstring(j,token,requested,sizeof(requested))){feedback(e,"Choose an action: start, status, key, click, type, resize, checkpoint, restore, steps or repair.");
goto done;
}}
    {size_t i;
for(i=0;requested[i];i++)requested[i]=(char)tolower((unsigned char)requested[i]);
}
    snprintf(action,sizeof(action),"%s",normalize(requested));
if(strcmp(action,requested))e->status.normalized++;
    {int image=jget(j,0,"include_image");
     if(image>=0&&!jeq(j,image,"true")&&!jeq(j,image,"false")){feedback(e,"include_image must be boolean.");goto done;}
     e->include_image=image>=0?jeq(j,image,"true"):!e->cfg.android;
     if(!strcmp(action,"screenshot")||!strcmp(action,"ocr"))e->include_image=1;
    }

    e->transient_response = jeq(j,jget(j,0,"checkpoint"),"false") && (!strcmp(action,"pointer") || (!strcmp(action,"key") && jget(j,0,"down")>=0));
    e->status.actions++;

    h=&e->history[e->history_next];
memset(h,0,sizeof(*h));
h->step=e->status.actions;
snprintf(h->action,sizeof(h->action),"%.31s",action);

    if(request_log(e,j,action)&&strcmp(action,"status")&&strcmp(action,"steps")&&strcmp(action,"repair")&&strcmp(action,"stop")){feedback(e,"Forensic journal cannot accept more data. No mutation ran; preserve/export the journal and repair its storage.");
goto done;
}
    mutating=strcmp(action,"status")&&strcmp(action,"android_observe")&&strcmp(action,"steps")&&strcmp(action,"repair")&&strcmp(action,"screenshot")&&strcmp(action,"stop")&&strcmp(action,"ocr");

    if(mutating&&(e->job_active||e->late_id||
       (e->guest_active&&(!e->guest_auto_running||e->guest.uncertain)))){feedback(e,"Previous operation is still unconfirmed. New mutations are held while live state remains visible.");
goto done;
}
    if(!strcmp(action,"status")||!strcmp(action,"steps")||!strcmp(action,"repair")||!strcmp(action,"android_observe")){ok=0;
goto done;
}
    if(!strcmp(action,"android_act")){ok=android_act(e,j);goto done;}
    if(!strcmp(action,"android_ack_uncertain")){
        uint64_t id;
        if(!e->android_uncertain||juint64(j,jget(j,0,"id"),&id)||id!=e->android_pending){feedback(e,"Acknowledge the exact uncertain action id after inspecting its effects.");goto done;}
        e->android_pending=0;e->android_uncertain=0;
        snprintf(e->android_outcome,sizeof(e->android_outcome),"acknowledged_uncertain");
        feedback(e,"Uncertain action acknowledged; no input was replayed. Observe fresh state before continuing.");
        android_invalidate(e);ok=0;goto done;
    }
    if(!strcmp(action,"create")){ok=create_disk(e,j); goto done;}
    if(!strcmp(action,"start")){ok=vm_start(e,j);
goto done;
}
    if(!strcmp(action,"stop")){ok=vm_stop(e);
goto done;
}
    if(!strcmp(action,"password")){ok=guest_job_start(e,j,"password",h);
goto done;
}
    if(!strcmp(action,"doom")){ok=guest_job_start(e,j,"doom",h);
goto done;
}
    if(!strcmp(action,"harness")){ok=launch_harness(e,j);
goto done;
}
    if(!strcmp(action,"checkpoint")){
        if(jget(j,0,"name")<0)snprintf(name,sizeof(name),"manual-%llu",(unsigned long long)e->status.actions);

        else if(jstring(j,jget(j,0,"name"),name,sizeof(name))){feedback(e,"Checkpoint name is missing or too long.");
goto done;
}
        ok=snapshot_save(e,name,0,NULL,0);
if(!ok)feedback(e,"Checkpoint '%s' captured RAM, device state and disk state.",name);
goto done;

    }
    if(!strcmp(action,"restore")){
        int checkpoint_index;
        if(!e->snapshot_catalog_available){feedback(e,"Restore refused because QEMU's existing checkpoint catalog could not be verified. The VM remains available; no state was assumed empty.");ok=-1;goto done;}
        if(jstring(j,jget(j,0,"name"),name,sizeof(name))){feedback(e,"Choose a retained checkpoint name from steps.");
goto done;
}
        checkpoint_index=snapshot_find(e,name);
        if(checkpoint_index<0){feedback(e,"Choose a checkpoint from the verified QEMU catalog shown in steps.");ok=-1;goto done;}
        android_invalidate(e);
        ok=snapshot(e,"snapshot-load",e->snapshots[checkpoint_index].tag);
if(!ok)feedback(e,"Checkpoint '%s' restored and confirmed in %.0f ms.",name,e->status.last_restore_ms);
goto done;

    }
    if(!strcmp(action,"screenshot")){
        char path[1024];
if(jget(j,0,"path")<0)snprintf(path,sizeof(path),"%.900s/screen-%llu.png",e->cfg.runtime_dir,(unsigned long long)e->status.actions);

        else if(jstring(j,jget(j,0,"path"),path,sizeof(path))){feedback(e,"Screenshot path is missing or too long.");
goto done;
}
        {uint64_t deadline=mgp_now()+300;
do{display_tick(e);
ok=mg_rfb_png(e->display,path,e->status.feedback,error,sizeof(error));
if(!ok)break;
mgp_sleep(5);
}while(mgp_now()<deadline);
}
        if(ok)feedback(e,"Screenshot unavailable: %s",error);
else feedback(e,"Screenshot exported: %s",path);
goto done;

    }
    if(!strcmp(action,"ocr")){ok=ocr_start(e,j);goto done;}
    if(!strcmp(action,"pause")||!strcmp(action,"resume")||!strcmp(action,"reset")||!strcmp(action,"key")||!strcmp(action,"type")||!strcmp(action,"click")||!strcmp(action,"pointer")||!strcmp(action,"resize")){
        if(e->cfg.android&&(!strcmp(action,"key")||!strcmp(action,"type")||!strcmp(action,"click")||!strcmp(action,"pointer"))){
            feedback(e,"Android agent input requires android_act with an observed target and revision. Raw input and coordinate guessing are disabled.");goto done;
        }
        if(!e->status.connected){feedback(e,"Start the VM before sending this action.");
goto done;
}
        if(!e->status.running && (!strcmp(action,"type") || !strcmp(action,"click") || !strcmp(action,"pointer") || !strcmp(action,"key")) && !jeq(j,jget(j,0,"down"),"false")) {feedback(e,"The VM is paused; resume it before input so keys are not discarded.");goto done;}
        capture=e->auto_checkpoint&&e->snapshot_supported&&!jeq(j,jget(j,0,"checkpoint"),"false");

        if(capture){char desired[64];snprintf(desired,sizeof(desired),"step-%llu-before",(unsigned long long)h->step);
if(snapshot_save(e,desired,1,h->before,sizeof(h->before)))goto done;
}
        if(!strcmp(action,"pause"))ok=qmp_exec(e,"stop",NULL,COMMAND_MS);

        else if(!strcmp(action,"resume"))ok=qmp_exec(e,"cont",NULL,COMMAND_MS);

        else if(!strcmp(action,"reset")){android_invalidate(e);ok=qmp_exec(e,"system_reset",NULL,COMMAND_MS);}

        else if(!strcmp(action,"key"))ok=keys(e,j);

        else if(!strcmp(action,"type"))ok=type_text(e,j);

        else if(!strcmp(action,"click")||!strcmp(action,"pointer")){
            unsigned x,y,buttons=0,button=number_or(j,"button",1);

            if(jnumber(j,jget(j,0,"x"),&x)||jnumber(j,jget(j,0,"y"),&y)||x>=e->status.width||y>=e->status.height){feedback(e,"Pointer coordinates must lie inside the observed guest screen.");
goto done;
}
            if(!strcmp(action,"pointer")){buttons=number_or(j,"buttons",0);
if(buttons>255){feedback(e,"Pointer button mask must be 0-255.");
goto done;
}ok=mg_rfb_pointer(e->display,x,y,buttons);
}
            else if(button<1||button>3){feedback(e,"Choose mouse button 1, 2 or 3.");
goto done;
}
            else {buttons=1u<<(button-1);
ok=mg_rfb_pointer(e->display,x,y,buttons);
if(!ok)ok=mg_rfb_pointer(e->display,x,y,0);
}
        } else if(!strcmp(action,"resize")){
            unsigned w=number_or(j,"width",0),height=number_or(j,"height",0);
uint64_t deadline;

            if(w<320||height<200||w>4096||height>4096||(uint64_t)w*height>9437184){feedback(e,"Choose screen size at least 320x200 and at most 9 million pixels.");
goto done;
}
            e->cfg.width=w;
e->cfg.height=height;
ok=mg_rfb_resize(e->display,w,height);

            if(ok){feedback(e,"Guest display has not advertised live resize support. A supported guest driver/helper is required.");
goto done;
}
            deadline=mgp_now()+1000;
ok=-1;

            while(mgp_now()<deadline){display_tick(e);
if(e->status.width==w&&e->status.height==height){ok=0;
break;
}mgp_sleep(5);
}
            if(ok){feedback(e,"Resize requested %ux%u; guest still reports %ux%u. The controller does not pretend this changed.",w,height,e->status.width,e->status.height);
goto done;
}
        }
        if(!ok){feedback(e,"Action '%s' was delivered; current VM state and live display follow.",action);

            if(capture){char desired[64];snprintf(desired,sizeof(desired),"step-%llu-after",(unsigned long long)h->step);
if(snapshot_save(e,desired,1,h->after,sizeof(h->after))){char reason[512];
strcpy(reason,e->status.feedback);
feedback(e,"Action ran, but its after-checkpoint is unconfirmed: %s",reason);
ok=-1;
}}}
        else if(!e->status.feedback[0])feedback(e,"Action was not delivered. Use the repair prompt.");
goto done;

    }
    feedback(e,"Unknown action '%s'. Supported aliases are normalized; an unknown command cannot be guessed safely. Use the copyable repair prompt.",requested);

done:
    display_tick(e);
    if(e->status.connected&&!e->late_id&&!e->transient_response&&
       strcmp(action,"android_act")&&strcmp(action,"android_observe"))refresh_state(e);

    repair(e,action);

    if(ok)e->status.failed++;

    if(h){char result[512];
h->ok=!ok;
h->ms=mgp_now()-begun;
e->history_next=(e->history_next+1)%HISTORY_CAP;
if(e->history_count<HISTORY_CAP)e->history_count++;

        snprintf(result,sizeof(result),"step=%llu requested=%s normalized=%s ok=%d duration_ms=%llu before=%s after=%s",(unsigned long long)h->step,requested,action,!ok,(unsigned long long)h->ms,h->before,h->after);
journal(e,"action_result",result);
}
    response(e,!ok,action,out,cap,!strcmp(action,"steps"));
e->transient_response=0;
if(request_copy){wipe_memory(request_copy,request_length+1);free(request_copy);}
free(j);
return ok;

}
static int authenticate(MgEngine *e,const char *request) {
    Json *j=malloc(sizeof(*j));
char token[65];
unsigned mismatch=0;
size_t i;
int ok=0;

    if(!j)return 0;

    if(!jparse(j,request)&&!jstring(j,jget(j,0,"auth"),token,sizeof(token))&&strlen(token)==64){for(i=0;i<64;i++)mismatch|=(unsigned char)token[i]^(unsigned char)e->token[i];
ok=mismatch==0;
}
    free(j);
return ok;

}
int mg_engine_tick(MgEngine *e) {
    unsigned i;
char error[512];

    if(!e)return -1;

    if(e->qmp!=MGP_BAD_SOCKET)qmp_pump(e);
display_tick(e);
    android_tick(e);
    e->status.android_ready=mg_live_control_up(e->live)&&e->android_received&&mgp_now()-e->android_received<=2000;
    e->status.video_ready=mg_live_video_up(e->live);
    if(e->status.owned_pid&&mgp_now()-e->resource_sample>=500){
        e->resource_sample=mgp_now();
        e->status.qemu_memory_bytes=mgp_memory_bytes(&e->process);
        e->status.qemu_threads=mgp_process_threads(&e->process);
    }

    if(e->job_active&&!e->late_id&&mgp_now()-e->last_job_poll>100){snapshot_poll(e);
e->last_job_poll=mgp_now();
}
    if(e->status.owned_pid&&!mgp_alive(&e->process)){journal(e,"process_exit","Owned QEMU exited; output log remains available.");
mgp_reap(&e->process);
e->status.owned_pid=0;
e->status.running=0;
qmp_disconnect(e);
strcpy(e->status.phase,"exited");
feedback(e,"VM process exited. Its log and repair prompt are available.");
repair(e,"process_exit");
        mg_live_free(e->live);e->live=NULL;android_invalidate(e);
        if(e->guest_active){journal(e,"guest_job_cancelled","Owned QEMU exited before the guest helper confirmed completion.");mg_guest_clear(&e->guest);e->guest_active=0;}
        e->guest_auto_pending=0;e->guest_auto_running=0;
        wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
}
    guest_job_tick(e);
    guest_auto_tick(e);
    ocr_tick(e);
    if(e->status.connected&&!e->last_rfb&&mgp_now()-e->last_rfb_attempt>250){if(!mg_rfb_connect(e->display,"127.0.0.1",e->cfg.vnc_port,error,sizeof(error)))display_tick(e);
e->last_rfb_attempt=mgp_now();
}
    if(e->status.connected&&!e->late_id&&!e->job_active&&mgp_now()-e->last_state>1000)refresh_state(e);

    int listener_ready = (e->listener != MGP_BAD_SOCKET) && (mgp_ready(e->listener, 0, 0) > 0);
    for(i=0;i<4;i++){
        Client *c=&e->clients[i];

        if(c->socket==MGP_BAD_SOCKET && listener_ready){
            MgpSocket incoming=accept(e->listener,NULL,NULL);
            if(incoming!=MGP_BAD_SOCKET){
                mgp_nonblock(incoming);
                c->socket=incoming;
                c->used=0;
                c->accepted=mgp_now();
                listener_ready = 0;
            }else continue;
        }
        if(c->socket!=MGP_BAD_SOCKET&&!c->response){int n=mgp_recv(c->socket,c->request+c->used,sizeof(c->request)-c->used-1);
char *newline;

            if(n>0){c->used+=(size_t)n;
c->request[c->used]=0;
newline=strchr(c->request,'\n');
if(newline){char *answer=malloc(LINE_CAP);
*newline=0;

                if(answer){if(authenticate(e,c->request))mg_engine_submit(e,c->request,answer,LINE_CAP);

                    else {journal(e,"control_auth_failed","Local control request rejected; token not logged.");
feedback(e,"Local control credential was invalid.");
repair(e,"authentication");
e->include_image=0;
/* Authentication errors must not disclose private provider state. */
snprintf(answer,LINE_CAP,"{\"ok\":false,\"action\":\"authentication\",\"state\":{\"phase\":\"authentication_required\"},\"feedback\":\"Local control credential was invalid.\",\"image_path\":\"\",\"latest_image\":null}");
}
                    c->response=answer;c->response_used=strlen(answer);
                    c->response[c->response_used++]='\n';c->response_sent=0;c->accepted=mgp_now();
}else{mgp_close(c->socket);c->socket=MGP_BAD_SOCKET;}
}
            }else if(n==0||n==-1){mgp_close(c->socket);
c->socket=MGP_BAD_SOCKET;
}
            if(c->socket!=MGP_BAD_SOCKET&&!c->response&&(c->used==sizeof(c->request)-1||mgp_now()-c->accepted>2000)){journal(e,"control_request_expired","Oversized or incomplete local request discarded.");
mgp_close(c->socket);
c->socket=MGP_BAD_SOCKET;
}
        }
        if(c->socket!=MGP_BAD_SOCKET&&c->response){
            int n=mgp_send(c->socket,c->response+c->response_sent,c->response_used-c->response_sent);
            if(n>0)c->response_sent+=(size_t)n;
            if(n==0||n==-1||c->response_sent==c->response_used||mgp_now()-c->accepted>2000){
                mgp_close(c->socket);c->socket=MGP_BAD_SOCKET;
                free(c->response);c->response=NULL;
            }
        }
    }
    return 0;

}
void mg_engine_free(MgEngine *e) {
    unsigned i;
if(!e)return;

    if(e->status.owned_pid)vm_stop(e);
    if(e->guest_active){mg_guest_clear(&e->guest);e->guest_active=0;}
    e->guest_auto_pending=0;e->guest_auto_running=0;
    wipe_memory(e->guest_auto_password,sizeof(e->guest_auto_password));
    ocr_cleanup(e,1);

    for(i=0;i<4;i++){if(mgp_alive(&e->harnesses[i])){journal(e,"harness_terminate","Owner closing; stopping its visible repair child.");
mgp_terminate(&e->harnesses[i]);
mgp_sleep(10);
if(mgp_alive(&e->harnesses[i]))mgp_force_kill(&e->harnesses[i]);
}mgp_reap(&e->harnesses[i]);
mgp_close(e->clients[i].socket);
free(e->clients[i].response);
}
    if(e->journal){journal(e,"owner_exit","Controller closed; no owned QEMU process left running.");
fclose(e->journal);
}
    mgp_close(e->qmp);
mgp_close(e->listener);
if(e->owner_lock!=-1)mgp_owner_unlock(e->owner_lock);

    if(e->token_path[0])mgp_remove(e->token_path);
mg_rfb_free(e->display);
mg_live_free(e->live);
free(e);

}
int mg_client(unsigned port,const char *request,char *out,size_t cap) {
    MgConfig cfg;
MgpSocket socket_fd;
char path[1024],token[65],wire[REQUEST_CAP];
FILE *file;
Buffer b;
size_t length,used=0;
uint64_t deadline;

    if(!out||cap<512||!request||mgp_init())return -1;
out[0]=0;
mg_config_defaults(&cfg);

    snprintf(path,sizeof(path),"%.900s/session-%u.token",cfg.runtime_dir,port);
file=mgp_fopen(path,"rb");

    if(!file){snprintf(out,cap,"{\"ok\":false,\"image_path\":\"\",\"fallback_text\":\"Controller is not running. Open Megas or run megas serve.\",\"repair_prompt\":\"Start the native Megas owner using the configured runtime directory and inspect actions.jsonl if it fails.\"}");
return -1;
}
    length=fread(token,1,64,file);
fclose(file);
token[length]=0;

    if(length!=64)return -1;
length=strlen(request);

    if(length<2||request[0]!='{'||request[length-1]!='}'||length>REQUEST_CAP-128){snprintf(out,cap,"{\"ok\":false,\"image_path\":\"\",\"fallback_text\":\"Send a bounded JSON action object.\"}");
return -1;
}
    binit(&b,wire,sizeof(wire));
bput(&b,"{\"auth\":");
bstr(&b,token);

    if(length>2)bput(&b,",");
bput(&b,request+1);
bput(&b,"\n");

    socket_fd=mgp_connect(port,500);

    if(socket_fd==MGP_BAD_SOCKET||b.error||mgp_send_all(socket_fd,wire,b.n,500)){mgp_close(socket_fd);
snprintf(out,cap,"{\"ok\":false,\"image_path\":\"\",\"fallback_text\":\"Native controller is unreachable; its runtime journal contains the owner state.\"}");
return -1;
}
    deadline=mgp_now()+20000;

    while(used+1<cap&&mgp_now()<deadline){int n=mgp_recv(socket_fd,out+used,cap-used-1);
if(n>0){used+=(size_t)n;
out[used]=0;
{char *newline=strchr(out,'\n');
if(newline){*newline=0;
break;
}}}else if(n==0||n==-1)break;
else mgp_ready(socket_fd,0,20);
}
    mgp_close(socket_fd);

    if(!used){snprintf(out,cap,"{\"ok\":false,\"image_path\":\"\",\"fallback_text\":\"No confirmed response yet. Inspect live owner state; do not blindly retry a mutation.\"}");
return -1;
}
    {Json *j=malloc(sizeof(*j));
int ok=j&&!jparse(j,out)&&jeq(j,jget(j,0,"ok"),"true");
free(j);
return ok?0:-1;
}
}
