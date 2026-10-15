/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host protocol tests with a synthetic provider; no Android compatibility claim. */
#include "../src/engine.c"
#include <assert.h>
#include <arpa/inet.h>

int mg_interrupted(void) { return 0; }
static unsigned socket_port(MgpSocket s)
{
    struct sockaddr_in address;
    socklen_t n=sizeof(address);
    assert(!getsockname(s,(struct sockaddr *)&address,&n));
    return ntohs(address.sin_port);
}
static unsigned unused_port(void)
{
    MgpSocket s=mgp_listen(0);
    unsigned port=socket_port(s);
    mgp_close(s);return port;
}
static void feed(MgEngine *e,MgpSocket peer,const char *line)
{
    uint64_t deadline=mgp_now()+1000;
    size_t length=strlen(line)-1;
    assert(!mgp_send_all(peer,line,strlen(line),100));
    do {
        android_tick(e);
        if(strlen(mg_live_line(e->live))==length&&!memcmp(mg_live_line(e->live),line,length))return;
        mgp_sleep(1);
    }while(mgp_now()<deadline);
    assert(!"provider message was not consumed");
}
int main(void)
{
    MgConfig cfg;
    MgEngine *e;
    char runtime[]="/tmp/megas-android-XXXXXX",response[65536],wire[8192],content[65536];
    MgpSocket control_server,video_server,peer,video;
    unsigned char ts[188];
    FILE *file;
    size_t n;
    MgpVmLimits limits={"/tmp",32*1024*1024,50,16,1};
    MgpProcess child={0};
    char log[1024],error[512],*argv[]={"/bin/true",NULL};
    assert(mkdtemp(runtime));assert(!mgp_init());
    mg_config_defaults(&cfg);
    cfg.control_port=unused_port();
    snprintf(cfg.runtime_dir,sizeof(cfg.runtime_dir),"%s",runtime);
    e=mg_engine_new(&cfg);assert(e);
    assert(mg_engine_submit(e,"{\"action\":\"start\",\"android\":true,\"accel\":\"tcg\"}",response,sizeof(response)));
    assert(strstr(response,"requires an explicitly available hardware accelerator"));
    assert(!e->status.owned_pid&&!e->cfg.android);
    assert(mg_engine_submit(e,"{\"action\":\"start\",\"cpus\":343}",response,sizeof(response)));
    assert(!e->status.owned_pid&&e->cfg.cpus==1);
    snprintf(log,sizeof(log),"%s/limits.log",runtime);
    assert(mgp_spawn_vm(&child,argv,log,&limits,error,sizeof(error)));
    assert(!mgp_pid(&child)&&strstr(error,"No unbounded VM"));
    puts("android: no TCG/native confusion, resource preflight rejects unbounded launch");

    control_server=mgp_listen(0);video_server=mgp_listen(0);
    assert(control_server!=MGP_BAD_SOCKET&&video_server!=MGP_BAD_SOCKET);
    e->cfg.android=1;e->status.running=1;
    e->cfg.stream_port=unused_port();
    e->live=mg_live_new(socket_port(control_server),socket_port(video_server),e->cfg.stream_port,e->token);
    assert(e->live);android_tick(e);
    peer=accept(control_server,NULL,NULL);video=accept(video_server,NULL,NULL);
    assert(peer!=MGP_BAD_SOCKET&&video!=MGP_BAD_SOCKET);
    memset(ts,0xff,sizeof(ts));ts[0]=0x47;ts[1]=0x1f;ts[2]=0xff;ts[3]=0x10;
    assert(!mgp_send_all(video,ts,sizeof(ts),100));android_tick(e);
    assert(mg_live_video_up(e->live));
    feed(e,peer,"{\"type\":\"state\",\"epoch\":\"boot-a\",\"revision\":1,\"guest_monotonic_ms\":100,\"cause_action_id\":0,\"targets\":[{\"id\":\"n1\",\"operations\":[\"click\",\"set_text\"]},{\"id\":\"n2\",\"operations\":[\"click\"]},{\"id\":\"private_target_marker\",\"operations\":[\"set_text\"]}],\"payload\":{\"private_test_marker\":true}}\n");
    assert(e->android_revision==1);
    assert(!mg_engine_submit(e,"{\"action\":\"android_observe\"}",response,sizeof(response)));
    assert(strstr(response,"private_test_marker")&&strstr(response,"\"image_path\":\"\""));
    {MgpSocket unauthorized=mgp_connect(cfg.control_port,100);
     const char *request="{\"action\":\"status\",\"auth\":\"wrong\"}\n";
     int received;
     assert(unauthorized!=MGP_BAD_SOCKET&&!mgp_send_all(unauthorized,request,strlen(request),100));
     mg_engine_tick(e);received=mgp_recv(unauthorized,response,sizeof(response)-1);assert(received>0);response[received]=0;
     assert(strstr(response,"authentication_required")&&!strstr(response,"private_test_marker")&&!strstr(response,"observed"));
     mgp_close(unauthorized);}
    assert(mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":2,\"target\":\"n1\",\"operation\":\"click\"}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"n1\",\"operation\":\"click\",\"x\":50}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"click\",\"x\":50,\"y\":50}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"type\",\"text\":\"raw input\"}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"guessed\",\"operation\":\"click\"}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"n2\",\"operation\":\"set_text\"}",response,sizeof(response)));
    assert(!mg_engine_submit(e,"{\"action\":\"ANDROID_ACT\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"private_target_marker\",\"operation\":\"set_text\",\"text\":\"private_text_marker\"}",response,sizeof(response)));
    assert(e->android_pending==1&&strstr(response,"\"outcome\":\"queued\""));
    android_tick(e);
    n=recv(peer,wire,sizeof(wire)-1,0);assert(n>0);wire[n]=0;
    assert(strstr(wire,"\"expected_revision\":1")&&strstr(wire,"private_target_marker"));
    assert(!strstr(wire,e->token));
    feed(e,peer,"{\"type\":\"result\",\"id\":1,\"outcome\":\"applied\"}\n");
    assert(!e->android_pending&&!e->android_uncertain&&!strcmp(e->android_outcome,"applied"));
    puts("android: observed state, stale/coordinate rejection, queued versus confirmed result");

    feed(e,peer,"{\"type\":\"heartbeat\",\"epoch\":\"boot-a\",\"revision\":1}\n");
    assert(!mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"n2\",\"operation\":\"click\"}",response,sizeof(response)));
    android_tick(e);n=recv(peer,wire,sizeof(wire),0);assert(n>0);
    e->android_queued=mgp_now()-2001;android_tick(e);
    assert(e->android_pending==2&&e->android_uncertain);
    assert(mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-a\",\"expected_revision\":1,\"target\":\"n2\",\"operation\":\"click\"}",response,sizeof(response)));
    assert(mg_engine_submit(e,"{\"action\":\"android_ack_uncertain\",\"id\":1}",response,sizeof(response)));
    assert(!mg_engine_submit(e,"{\"action\":\"android_ack_uncertain\",\"id\":2}",response,sizeof(response)));
    assert(!e->android_revision&&!e->android_pending);
    mgp_close(peer);
    {uint64_t deadline=mgp_now()+1000;
     do{android_tick(e);mgp_sleep(1);}while(!mg_live_control_up(e->live)&&mgp_now()<deadline);
     assert(mg_live_control_up(e->live));
     peer=accept(control_server,NULL,NULL);assert(peer!=MGP_BAD_SOCKET);}
    feed(e,peer,"{\"type\":\"state\",\"epoch\":\"boot-a\",\"revision\":3,\"guest_monotonic_ms\":150,\"cause_action_id\":2,\"targets\":[{\"id\":\"n1\",\"operations\":[\"click\"]}],\"payload\":{}}\n");
    assert(e->android_revision==3);
    feed(e,peer,"{\"type\":\"state\",\"epoch\":\"boot-a\",\"revision\":2,\"guest_monotonic_ms\":140,\"cause_action_id\":0,\"targets\":[{\"id\":\"n1\",\"operations\":[\"click\",\"set_text\"]},{\"id\":\"n2\",\"operations\":[\"click\"]}],\"payload\":{}}\n");
    assert(!e->android_revision);
    puts("android: heartbeat, timeout, no blind replay, explicit uncertainty acknowledgement, stale events invalidate handles");
    feed(e,peer,"{\"type\":\"state\",\"epoch\":\"boot-b\",\"revision\":1,\"guest_monotonic_ms\":10,\"cause_action_id\":0,\"targets\":[{\"id\":\"n3\",\"operations\":[\"click\"]}],\"payload\":{}}\n");
    assert(!mg_engine_submit(e,"{\"action\":\"android_act\",\"epoch\":\"boot-b\",\"expected_revision\":1,\"target\":\"n3\",\"operation\":\"click\"}",response,sizeof(response)));
    android_tick(e);n=recv(peer,wire,sizeof(wire),0);assert(n>0);
    mgp_close(peer);android_tick(e);
    assert(!e->android_revision&&e->android_uncertain);
    snprintf(log,sizeof(log),"%s/actions.jsonl",runtime);
    fflush(e->journal);file=fopen(log,"rb");assert(file);
    n=fread(content,1,sizeof(content)-1,file);content[n]=0;fclose(file);
    assert(!strstr(content,"private_test_marker")&&!strstr(content,"private_target_marker")&&!strstr(content,"private_text_marker"));
    puts("android: disconnect invalidates state, uncertain action held, private payload/text never persisted");
    e->status.running=0;mg_engine_free(e);
    mgp_close(video);mgp_close(control_server);mgp_close(video_server);
    return 0;
}
