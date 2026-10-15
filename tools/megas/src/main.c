/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "megas.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
static volatile sig_atomic_t closing;
int mg_interrupted(void){return closing != 0;}
static void stop_signal(int signal_number){(void)signal_number;closing=1;}
static void json_string(const char *s){const unsigned char *p=(const unsigned char*)s;putchar('"');while(*p){switch(*p){case '"':fputs("\\\"",stdout);break;case '\\':fputs("\\\\",stdout);break;case '\n':fputs("\\n",stdout);break;case '\r':fputs("\\r",stdout);break;case '\t':fputs("\\t",stdout);break;default:if(*p<32)printf("\\u%04x",*p);else putchar(*p);}p++;}putchar('"');}
static int value(const char *s,unsigned *out){char *end;unsigned long n;errno=0;n=strtoul(s,&end,10);if(errno||*end||n<1||n>65535)return -1;*out=(unsigned)n;return 0;}
static void usage(void){puts("Megas — native VM owner and agent control\n\nmegas [--runtime DIR] [--port PORT] [--qmp-port PORT] [--vnc-port PORT] gui|serve\nmegas [options] status|steps|repair|pause|resume|stop\nmegas [options] send '{\"action\":\"start\",\"disk\":\"work.qcow2\"}'\nmegas [options] agent [--vision|--text-only]\nmegas [options] hook SessionStart|PreToolUse|PostToolUse|UserPromptSubmit\n\nCommands return current state. Agent mode defaults to semantic state and livestream metadata; screenshots and OCR are optional.\nThe controller owns QEMU; its process and commands are audited in actions.jsonl.");}
static int mg_main(int argc,char **argv){
    MgConfig cfg;MgEngine *engine;char *response;char request[8192];int i=1,result=0;
    mg_config_defaults(&cfg);
    while(i<argc&&argv[i][0]=='-'&&strcmp(argv[i],"--help")){
        const char *option=argv[i++];if(i>=argc){usage();return 2;}
        if(!strcmp(option,"--runtime")){if(strlen(argv[i])>=sizeof(cfg.runtime_dir))return 2;strcpy(cfg.runtime_dir,argv[i]);
            mgp_setenv("MEGAS_RUNTIME_DIR",argv[i]);
        }else if(!strcmp(option,"--port")){if(value(argv[i],&cfg.control_port))return 2;}
        else if(!strcmp(option,"--qmp-port")){if(value(argv[i],&cfg.qmp_port))return 2;}
        else if(!strcmp(option,"--vnc-port")){if(value(argv[i],&cfg.vnc_port)||cfg.vnc_port<5900)return 2;}
        else {usage();return 2;}i++;
    }
    if(i<argc&&!strcmp(argv[i],"--help")){usage();return 0;}
    if(i>=argc||!strcmp(argv[i],"gui")||!strcmp(argv[i],"serve")){
        int headless=i<argc&&!strcmp(argv[i],"serve");engine=mg_engine_new(&cfg);
        if(!engine){fputs("Megas could not open its owner session. Copy this repair prompt:\nInspect the configured runtime folder and existing Megas owner. Check loopback port conflicts and folder permissions. Preserve actions.jsonl and guest disks; release only a stale owner session, then restart Megas.\n",stderr);return 1;}
        signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);
#ifndef _WIN32
        signal(SIGPIPE,SIG_IGN);
#endif
#ifndef MEGAS_HEADLESS
        if(!headless)result=mg_gui_run(engine);
        else
#else
        (void)headless;
#endif
        {printf("Megas owner ready on localhost:%u. Every process and command is visible in %s\n",cfg.control_port,mg_engine_status(engine)->journal_path);fflush(stdout);while(!closing){mg_engine_tick(engine);mgp_sleep(5);}}
        mg_engine_free(engine);return result;
    }
    if(!strcmp(argv[i],"agent")){
        int mode=0;
        if(i+1<argc){
            if(!strcmp(argv[i+1],"--vision"))mode=1;
            else if(!strcmp(argv[i+1],"--text-only"))mode=2;
            else {usage();return 2;}
        }
        return mg_agent_stdio(cfg.control_port,mode);
    }
    response=malloc(65536);if(!response)return 1;
    if(!strcmp(argv[i],"hook")){
        const char *event=i+1<argc?argv[i+1]:"PostToolUse";
        mg_client(cfg.control_port,"{\"action\":\"status\"}",response,65536);
        fputs("{\"hookSpecificOutput\":{\"hookEventName\":",stdout);json_string(event);fputs(",\"additionalContext\":",stdout);json_string(response);puts("}}");free(response);return 0;
    }
    if(!strcmp(argv[i],"send")){if(i+1>=argc||strlen(argv[i+1])>=sizeof(request)){usage();free(response);return 2;}strcpy(request,argv[i+1]);}
    else {size_t n=strlen(argv[i]),k;if(n>63){free(response);return 2;}for(k=0;k<n;k++)if(!((argv[i][k]>='a'&&argv[i][k]<='z')||argv[i][k]=='_')){free(response);return 2;}snprintf(request,sizeof(request),"{\"action\":\"%s\"}",argv[i]);}
    result=mg_client(cfg.control_port,request,response,65536);puts(response);free(response);return result?1:0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    int i, result;
    if (!argv) return 1;
    for (i = 0; i < argc; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (!bytes || !(argv[i] = malloc((size_t)bytes))) {
            while (i > 0) free(argv[--i]);
            free(argv); return 1;
        }
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], bytes, NULL, NULL);
    }
    result = mg_main(argc, argv);
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv); return result;
}
#else
int main(int argc, char **argv) { return mg_main(argc, argv); }
#endif
