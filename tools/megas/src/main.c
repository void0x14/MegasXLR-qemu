/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "megas.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#endif
static volatile sig_atomic_t closing;
int mg_interrupted(void){return closing != 0;}
static void stop_signal(int signal_number){(void)signal_number;closing=1;}
static void json_string(const char *s){const unsigned char *p=(const unsigned char*)s;putchar('"');while(*p){switch(*p){case '"':fputs("\\\"",stdout);break;case '\\':fputs("\\\\",stdout);break;case '\n':fputs("\\n",stdout);break;case '\r':fputs("\\r",stdout);break;case '\t':fputs("\\t",stdout);break;default:if(*p<32)printf("\\u%04x",*p);else putchar(*p);}p++;}putchar('"');}
static int value(const char *s,unsigned *out){char *end;unsigned long n;errno=0;n=strtoul(s,&end,10);if(errno||*end||n<1||n>65535)return -1;*out=(unsigned)n;return 0;}
static void usage(void){puts("Megas — native VM owner and agent control\n\nmegas [--runtime DIR] [--port PORT] [--qmp-port PORT] [--vnc-port PORT] [--qga-port PORT] gui|serve\nmegas [options] status|steps|repair|pause|resume|stop\nmegas [options] send '{\"action\":\"start\",\"disk\":\"work.qcow2\"}'\nmegas [options] agent [--text-only]\nmegas [options] hook SessionStart|PreToolUse|PostToolUse|UserPromptSubmit\n\nCommands always return current VM state and latest live image/fallback.\nThe controller owns QEMU; its process and commands are audited in actions.jsonl.");}
static int is_native_megas_command(const char *cmd) {
    if (!cmd) return 0;
    return (!strcmp(cmd, "gui") ||
            !strcmp(cmd, "serve") ||
            !strcmp(cmd, "agent") ||
            !strcmp(cmd, "hook") ||
            !strcmp(cmd, "send") ||
            !strcmp(cmd, "status") ||
            !strcmp(cmd, "steps") ||
            !strcmp(cmd, "repair") ||
            !strcmp(cmd, "pause") ||
            !strcmp(cmd, "resume") ||
            !strcmp(cmd, "stop"));
}
static int mg_main(int argc,char **argv){
    MgConfig cfg;MgEngine *engine;char *response;char request[8192];int i=1,result=0;
    mg_config_defaults(&cfg);

    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-version")) {
            const char *prog = argv[0];
            const char *slash = strrchr(prog, '/');
            if (slash) prog = slash + 1;
            if (strstr(prog, "qemu")) {
                char real_qemu[1024];
                mgp_real_qemu(argv[0], real_qemu, sizeof(real_qemu));
                argv[0] = real_qemu;
                execv(real_qemu, argv);
            }
            puts("QEMU emulator version 9.1.0 (MegasXLR integrated)");
            return 0;
        }
        if (!strcmp(argv[i], "--runtime")) {
            i++; if (i >= argc) { usage(); return 2; }
            if (strlen(argv[i]) >= sizeof(cfg.runtime_dir)) return 2;
            strcpy(cfg.runtime_dir, argv[i]);
            mgp_setenv("MEGAS_RUNTIME_DIR", argv[i]);
            i++;
        } else if (!strcmp(argv[i], "--port")) {
            i++; if (i >= argc) { usage(); return 2; }
            if (value(argv[i], &cfg.control_port)) return 2;
            i++;
        } else if (!strcmp(argv[i], "--qmp-port")) {
            i++; if (i >= argc) { usage(); return 2; }
            if (value(argv[i], &cfg.qmp_port)) return 2;
            i++;
        } else if (!strcmp(argv[i], "--vnc-port")) {
            i++; if (i >= argc) { usage(); return 2; }
            if (value(argv[i], &cfg.vnc_port) || cfg.vnc_port < 5900) return 2;
            i++;
        } else if (!strcmp(argv[i], "--qga-port")) {
            i++; if (i >= argc) { usage(); return 2; }
            if (value(argv[i], &cfg.qga_port)) return 2;
            i++;
        } else {
            break;
        }
    }

    if (i >= argc) {
        const char *prog = argv[0];
        const char *slash = strrchr(prog, '/');
        if (slash) prog = slash + 1;
        if (strstr(prog, "qemu")) {
            char *resp = malloc(65536);
            if (resp && mg_client(cfg.control_port, "{\"action\":\"status\"}", resp, 65536) == 0) {
                printf("MegasXLR: VM Komuta Merkezi is active on localhost:%u.\n", cfg.control_port);
                puts(resp);
                free(resp);
                return 0;
            }
            free(resp);
        }
        goto run_owner_gui;
    }

    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-help") || !strcmp(argv[i], "-h")) {
        usage();
        return 0;
    }
    if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-version")) {
        const char *prog = argv[0];
        const char *slash = strrchr(prog, '/');
        if (slash) prog = slash + 1;
        if (strstr(prog, "qemu")) {
            char real_qemu[1024];
            mgp_real_qemu(argv[0], real_qemu, sizeof(real_qemu));
            argv[0] = real_qemu;
            execv(real_qemu, argv);
        }
        puts("QEMU emulator version 9.1.0 (MegasXLR integrated)");
        return 0;
    }

    if (!is_native_megas_command(argv[i]) &&
        !(i + 1 >= argc && !strcmp(argv[i], "start")) &&
        !(i + 1 < argc && !strcmp(argv[i], "start") && argv[i+1][0] != '-')) {
        char *raw_req = malloc(65536);
        char *resp = malloc(65536);
        if (!raw_req || !resp) { free(raw_req); free(resp); return 1; }
        strcpy(raw_req, "{\"action\":\"start\",\"raw_argv\":[");
        size_t req_len = strlen(raw_req);
        int start_idx = (argv[i][0] == '-') ? 0 : i;
        for (int arg_idx = start_idx; arg_idx < argc; arg_idx++) {
            if (arg_idx > start_idx && arg_idx < i) continue;
            if (req_len > strlen("{\"action\":\"start\",\"raw_argv\":[")) {
                raw_req[req_len++] = ','; raw_req[req_len] = 0;
            }
            raw_req[req_len++] = '"';
            const char *p = argv[arg_idx];
            while (*p && req_len + 10 < 65000) {
                if (*p == '"') { raw_req[req_len++] = '\\'; raw_req[req_len++] = '"'; }
                else if (*p == '\\') { raw_req[req_len++] = '\\'; raw_req[req_len++] = '\\'; }
                else if (*p == '\n') { raw_req[req_len++] = '\\'; raw_req[req_len++] = 'n'; }
                else if (*p == '\r') { raw_req[req_len++] = '\\'; raw_req[req_len++] = 'r'; }
                else if (*p == '\t') { raw_req[req_len++] = '\\'; raw_req[req_len++] = 't'; }
                else raw_req[req_len++] = *p;
                p++;
            }
            raw_req[req_len++] = '"';
            raw_req[req_len] = 0;
        }
        strcpy(raw_req + req_len, "]}");

        int owner_running = (mg_client(cfg.control_port, "{\"action\":\"status\"}", resp, 65536) == 0);
        if (!owner_running) {
#ifndef _WIN32
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                if (!getenv("DISPLAY")) {
                    setenv("DISPLAY", ":0", 1);
                }
                execlp("megas", "megas", "gui", NULL);
                _exit(1);
            }
            uint64_t deadline = mgp_now() + 5000;
            while (mgp_now() < deadline) {
                mgp_sleep(100);
                if (mg_client(cfg.control_port, "{\"action\":\"status\"}", resp, 65536) == 0) {
                    owner_running = 1;
                    break;
                }
            }
#endif
        }
        result = mg_client(cfg.control_port, raw_req, resp, 65536);
        free(raw_req);
        if (result != 0 || strstr(resp, "\"ok\":false")) {
            fprintf(stderr, "MegasXLR: VM start failed:\n%s\n", resp);
            free(resp);
            return 1;
        }
        printf("MegasXLR: QEMU command intercepted -> displaying in Megas Komuta Merkezi.\n");
        fflush(stdout);

        signal(SIGINT, stop_signal);
        signal(SIGTERM, stop_signal);
        while (!closing) {
            mgp_sleep(500);
            if (mg_client(cfg.control_port, "{\"action\":\"status\"}", resp, 65536) != 0) break;
            if (strstr(resp, "\"phase\":\"stopped\"") || strstr(resp, "\"phase\":\"exited\"")) break;
        }
        if (closing) {
            mg_client(cfg.control_port, "{\"action\":\"stop\"}", resp, 65536);
        }
        free(resp);
        return 0;
    }

run_owner_gui:
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
    if(!strcmp(argv[i],"agent"))return mg_agent_stdio(cfg.control_port,!(i+1<argc&&!strcmp(argv[i+1],"--text-only")));
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
