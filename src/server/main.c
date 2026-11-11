/*
SCOPO: segvault-server — il motore SegVault come servizio HTTP/JSON, senza GUI.
       Uso: segvault-server [opzioni] [cartella-dati]      (vedi --help e docs/API.md)
*/
#include "server.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

static SvServer* g_server;

static void on_signal(int sig) {
    (void)sig;
    if (g_server) sv_server_stop(g_server);   // solo un flag: sicuro in un gestore di segnale
}

static void usage(const char* argv0) {
    printf("SegVault %s - HTTP/JSON server\n\n"
           "Usage: %s [options] [data-directory]   (default data directory: ./data)\n\n"
           "  -p, --port N          TCP port (default 8484; 0 = any free port)\n"
           "  -b, --bind ADDR       IPv4 address to listen on (default 127.0.0.1 = this machine only)\n"
           "  -t, --token TOKEN     require \"Authorization: Bearer TOKEN\" on /api (or env SEGVAULT_TOKEN);\n"
           "                        \"auto\" generates a random token and prints it\n"
           "  -d, --database NAME   database used when a request does not name one\n"
           "      --cors ORIGIN     allow browser calls from ORIGIN (e.g. http://localhost:5173, or *)\n"
           "      --users           require a user and password (Authorization: Basic); each query\n"
           "                        runs with that user's privileges (CREATE USER / GRANT in SQL)\n"
           "      --static DIR      also serve the files in DIR (a web app) outside /api\n"
           "      --max-body MB     largest request body (default 16)\n"
           "      --timeout S       stop a request that runs longer than S seconds (default 30, 0 = never)\n"
           "      --no-auth         allow listening on a non-local address without a token\n"
           "  -q, --quiet           do not log requests\n"
           "  -h, --help            this help\n\n"
           "API: GET /api/v1/health, GET /api/v1/status, POST /api/v1/query,\n"
           "     GET /api/v1/databases[/DB/tables[/TABLE]]   (see docs/API.md)\n",
           SV_VERSION_STR, argv0);
}

int main(int argc, char** argv) {
    SvServerConfig cfg = { .bind_addr = "127.0.0.1", .port = 8484, .timeout_ms = 30000 };
    char auto_token[65];
    const char* data_dir = "./data";
    bool no_auth = false;
    cfg.token = getenv("SEGVAULT_TOKEN");

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool has_next = i + 1 < argc;
        #define ARG(s, l) ((!strcmp(a, s) || !strcmp(a, l)) && has_next)
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (ARG("-p", "--port"))     cfg.port = atoi(argv[++i]);
        else if (ARG("-b", "--bind"))     cfg.bind_addr = argv[++i];
        else if (ARG("-t", "--token"))    cfg.token = argv[++i];
        else if (ARG("-d", "--database")) cfg.default_db = argv[++i];
        else if (ARG("--cors", "--cors")) cfg.cors_origin = argv[++i];
        else if (ARG("--static", "--static")) cfg.static_dir = argv[++i];
        else if (ARG("--max-body", "--max-body")) cfg.max_body = (size_t)atoi(argv[++i]) << 20;
        else if (ARG("--timeout", "--timeout")) cfg.timeout_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(a, "--no-auth")) no_auth = true;
        else if (!strcmp(a, "--users")) cfg.require_login = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) cfg.quiet = true;
        else if (a[0] == '-') { fprintf(stderr, "unknown or incomplete option: %s (see --help)\n", a); return 2; }
        else data_dir = a;
        #undef ARG
    }
    if (cfg.port < 0 || cfg.port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }
    if (cfg.timeout_ms < 0) cfg.timeout_ms = 0;
    if (cfg.token && !strcmp(cfg.token, "auto")) {
        if (!sv_random_token(auto_token, 24)) { fprintf(stderr, "cannot generate a random token\n"); return 1; }
        cfg.token = auto_token;
        printf("token: %s\n", auto_token);
    }

    // una porta aperta sulla rete senza token = chiunque esegue SQL arbitrario
    bool local = !strncmp(cfg.bind_addr, "127.", 4);
    if (!local && !(cfg.token && cfg.token[0]) && !cfg.require_login && !no_auth) {
        fprintf(stderr, "refusing to listen on %s without a token: anyone on the network could run any SQL.\n"
                        "Use --token TOKEN (or SEGVAULT_TOKEN), --users (logins), or --no-auth if you really mean it.\n", cfg.bind_addr);
        return 2;
    }

#ifdef _WIN32
    _mkdir(data_dir);
#else
    mkdir(data_dir, 0755);
#endif
    if (!db_init(data_dir)) {
        fprintf(stderr, "cannot open the data directory '%s': %s\n", data_dir, db_init_error());
        return 1;
    }

    char err[256];
    g_server = sv_server_start(&cfg, err, sizeof(err));
    if (!g_server) {
        fprintf(stderr, "%s\n", err);
        db_shutdown();
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    // 0.0.0.0 non si apre nel browser: si mostra l'indirizzo locale
    const char* host = strcmp(cfg.bind_addr, "0.0.0.0") ? cfg.bind_addr : "127.0.0.1";
    int port = sv_server_port(g_server);
    printf("SegVault %s server (data: %s%s)\n", SV_VERSION_STR, data_dir,
           cfg.require_login ? ", login required" : cfg.token && cfg.token[0] ? ", token required" : "");
    if (cfg.static_dir) printf("  web site: http://%s:%d/          (files of %s)\n", host, port, cfg.static_dir);
    printf("  API:      http://%s:%d/api/v1\n", host, port);
    fflush(stdout);

    sv_server_run(g_server);

    printf("shutting down...\n");
    sv_server_free(g_server);
    db_shutdown();                      // WAL e catalogo scritti su disco
    return 0;
}
