/*
SCOPO: segvault-server — il motore SegVault come servizio HTTP/JSON, senza GUI.
       Uso: segvault-server [opzioni] [cartella-dati]      (vedi --help e docs/API.md)
*/
#include "server.h"
#include "fastcgi.h"
#include "mail.h"
#include "tls.h"
#include "service.h"
#include "../bridge/db_api.h"
#include "../../include/common.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
#else
  #include <dirent.h>
  #include <pthread.h>
  #include <unistd.h>
#endif

static SvServer* g_server;

// ── backup pianificati: <dir>/segvault-AAAAMMGG-HHMMSS ogni N minuti, si tengono gli ultimi K ──
static const char* g_bk_dir;
static int g_bk_every, g_bk_keep = 7;
static volatile int g_bk_stop;

static int cmp_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static bool is_backup_name(const char* n) {
    if (strncmp(n, "segvault-", 9) || strlen(n) != 24 || n[17] != '-') return false;
    for (int i = 9; i < 24; i++) if (i != 17 && (n[i] < '0' || n[i] > '9')) return false;
    return true;
}

static void remove_backup(const char* path) {
    static const char* const F[] = { "segvault.db", "segvault.cat", "segvault.free", "users.dat", "segvault.wal" };
    char f[800];
    for (int i = 0; i < 5; i++) { snprintf(f, sizeof(f), "%.700s/%s", path, F[i]); remove(f); }
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

// tiene solo gli ultimi g_bk_keep backup (i nomi in ordine alfabetico sono in ordine di tempo)
static void prune_backups(void) {
    char* names[1024];
    int n = 0;
#ifdef _WIN32
    char pat[700];
    snprintf(pat, sizeof(pat), "%.600s\\segvault-*", g_bk_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < 1024 && is_backup_name(fd.cFileName)) names[n++] = strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(g_bk_dir);
    struct dirent* e;
    while (d && (e = readdir(d))) if (n < 1024 && is_backup_name(e->d_name)) names[n++] = strdup(e->d_name);
    if (d) closedir(d);
#endif
    qsort(names, (size_t)n, sizeof(char*), cmp_str);
    for (int i = 0; i < n; i++) {
        if (names[i] && i < n - g_bk_keep) {
            char p[800];
            snprintf(p, sizeof(p), "%.600s/%s", g_bk_dir, names[i]);
            remove_backup(p);
        }
        free(names[i]);
    }
}

static bool backup_now(void) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char path[800], err[600];
    snprintf(path, sizeof(path), "%.600s/segvault-%04d%02d%02d-%02d%02d%02d", g_bk_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    if (!db_backup(path, err, sizeof(err))) { fprintf(stderr, "backup failed (retrying in a minute): %s\n", err); return false; }
    printf("backup written: %s\n", path);
    fflush(stdout);
    prune_backups();
    return true;
}

#ifdef _WIN32
static DWORD WINAPI backup_thread(LPVOID arg) {
#else
static void* backup_thread(void* arg) {
#endif
    (void)arg;
    int wait = g_bk_every * 60;
    while (!g_bk_stop) {
        for (int i = 0; i < wait && !g_bk_stop; i++) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
        }
        if (g_bk_stop) break;
        wait = backup_now() ? g_bk_every * 60 : 60;     // una transazione aperta: si riprova presto
    }
    return 0;
}

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
           "      --static DIR      also serve the files in DIR (a web site, like XAMPP's htdocs) outside /api\n"
           "      --php PATH|auto   run the .php files of --static DIR with php-cgi (auto: php/ next to\n"
           "                        segvault-server, the PATH, the usual folders)\n"
           "      --php-fcgi ADDR   run them with a PHP FastCGI server already started (php-fpm):\n"
           "                        host:port or unix:/run/php/php-fpm.sock\n"
           "      --php-workers N   php-cgi processes, i.e. PHP requests at the same time (default 4)\n"
           "      --api-prefix P    where the HTTP API lives (default /api/v1; none = HTTP API off).\n"
           "                        With PHP, paths under it that are not API paths go to the site\n"
           "      --max-body MB     largest request body (default 16)\n"
           "      --timeout S       stop a request that runs longer than S seconds (default 30, 0 = never)\n"
           "      --no-auth         allow listening on a non-local address without a token\n"
           "      --mysql-port N    also speak the MySQL protocol on port N (3306 is MySQL's): PHP mysqli / PDO,\n"
           "                        JDBC, Python and the mysql client connect unchanged (users: CREATE USER)\n"
           "      --audit FILE      append every statement that changes data or schema to FILE\n"
           "                        (time, client, user, database, result, SQL)\n"
           "      --backup-dir DIR  write a backup of all data to DIR/segvault-DATE-TIME (see --backup-every)\n"
           "      --backup-every M  minutes between backups (default 60 when --backup-dir is given)\n"
           "      --backup-keep N   keep only the last N backups (default 7)\n"
           "      --site NAME=TARGET  a site at http://NAME.localhost:PORT (and NAME.test): TARGET is a folder\n"
           "                        or a dev server to proxy to (http://127.0.0.1:5173, WebSockets included).\n"
           "                        Every folder inside --static is a site too (www/blog -> blog.localhost,\n"
           "                        www/blog/public as the root when the folder has no index: Laravel, Symfony)\n"
           "      --no-auto-sites   do not turn the folders of --static into sites\n"
           "      --mail-port P     catch outgoing email: an SMTP server on port P (e.g. 1025) keeps every message\n"
           "                        instead of delivering it; read them at http://localhost:PORT/.segvault/mail/\n"
           "                        (PHP mail() is routed there automatically)\n"
           "      --mail-dir DIR    where caught messages are kept (default DATA/mail)\n"
           "      --https-port P    HTTPS too (e.g. 8443), with certificates from a local authority (DATA/certs/ca.crt)\n"
           "                        made for each site name: https://NAME.localhost:P\n"
           "      --trust-ca        add the local authority to the trusted ones (browsers, and the system if root), then exit\n"
           "      --cert-dir DIR    where the local authority is kept (default DATA/certs)\n"
           "      --no-http2        HTTPS with HTTP/1.1 only\n"
           "      --install-service start this server (with the same options) when you log in: systemd user\n"
           "                        service on Linux, LaunchAgent on macOS, Run key on Windows; no admin rights\n"
           "      --uninstall-service  remove it\n"
           "      --no-dir-listing  404 instead of a file list for folders without an index\n"
           "      --no-htaccess     ignore .htaccess files (by default their mod_rewrite rules, Require all denied\n"
           "                        and Options -Indexes apply, as in Apache)\n"
           "  -q, --quiet           do not log requests\n"
           "  -h, --help            this help\n\n"
           "API: GET /api/v1/health, GET /api/v1/status, POST /api/v1/query,\n"
           "     GET /api/v1/databases[/DB/tables[/TABLE]]   (see docs/API.md)\n",
           SV_VERSION_STR, argv0);
}

int main(int argc, char** argv) {
    // "segvault-server --sendmail PORTA [-f mittente] [-t] [-i]": il sendmail_path di PHP (vedi mail.h)
    if (argc >= 3 && !strcmp(argv[1], "--sendmail")) return mail_sendmail_main(atoi(argv[2]), argc - 3, argv + 3);
    SvServerConfig cfg = { .bind_addr = "127.0.0.1", .port = 8484, .timeout_ms = 30000, .dir_listing = true, .http2 = true };
    static SvSite sites[64];
    static char site_buf[64][1200];
    char auto_token[65];
    const char* data_dir = "./data";
    bool no_auth = false;
    const char* php_arg = NULL;
    bool trust_ca = false, background = false;
    // servizio dell'utente: stessa riga di comando, senza questa opzione
    for (int i = 1; i < argc; i++) {
        char out[8000];
        if (!strcmp(argv[i], "--install-service")) {
            int r = sv_service_install(argc - 1, argv + 1, out, sizeof(out));
            fputs(out, r ? stderr : stdout);
            return r;
        }
        if (!strcmp(argv[i], "--uninstall-service")) {
            int r = sv_service_uninstall(out, sizeof(out));
            fputs(out, stdout);
            return r;
        }
    }
    char php_path[1024];
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
        else if (ARG("--php", "--php")) php_arg = argv[++i];
        else if (ARG("--php-fcgi", "--php-fcgi")) cfg.php_fcgi = argv[++i];
        else if (ARG("--php-workers", "--php-workers")) cfg.php_workers = atoi(argv[++i]);
        else if (ARG("--api-prefix", "--api-prefix")) cfg.api_prefix = argv[++i];
        else if (ARG("--max-body", "--max-body")) cfg.max_body = (size_t)atoi(argv[++i]) << 20;
        else if (ARG("--timeout", "--timeout")) cfg.timeout_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(a, "--no-auth")) no_auth = true;
        else if (!strcmp(a, "--users")) cfg.require_login = true;
        else if (ARG("--backup-dir", "--backup-dir")) g_bk_dir = argv[++i];
        else if (ARG("--audit", "--audit")) cfg.audit_file = argv[++i];
        else if (ARG("--mysql-port", "--mysql-port")) { cfg.mysql = true; cfg.mysql_port = atoi(argv[++i]); }
        else if (ARG("--backup-every", "--backup-every")) g_bk_every = atoi(argv[++i]);
        else if (ARG("--backup-keep", "--backup-keep")) g_bk_keep = atoi(argv[++i]);
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) cfg.quiet = true;
        else if (ARG("--site", "--site")) {
            const char* v = argv[++i];
            const char* eq = strchr(v, '=');
            if (!eq || eq == v || !eq[1] || cfg.n_sites >= 64 || strlen(v) >= sizeof(site_buf[0])) {
                fprintf(stderr, "--site NAME=FOLDER or --site NAME=http://127.0.0.1:PORT\n");
                return 2;
            }
            snprintf(site_buf[cfg.n_sites], sizeof(site_buf[0]), "%s", v);
            site_buf[cfg.n_sites][eq - v] = '\0';
            sites[cfg.n_sites].name = site_buf[cfg.n_sites];
            sites[cfg.n_sites].target = site_buf[cfg.n_sites] + (eq - v) + 1;
            cfg.n_sites++;
            cfg.sites = sites;
        }
        else if (!strcmp(a, "--no-auto-sites")) cfg.no_auto_sites = true;
        else if (ARG("--mail-port", "--mail-port")) { cfg.mail = true; cfg.mail_port = atoi(argv[++i]); }
        else if (ARG("--mail-dir", "--mail-dir")) cfg.mail_dir = argv[++i];
        else if (ARG("--https-port", "--https-port")) { cfg.https = true; cfg.https_port = atoi(argv[++i]); }
        else if (ARG("--cert-dir", "--cert-dir")) cfg.cert_dir = argv[++i];
        else if (!strcmp(a, "--no-http2")) cfg.http2 = false;
        else if (!strcmp(a, "--trust-ca")) trust_ca = true;
        else if (!strcmp(a, "--background")) background = true;
        else if (!strcmp(a, "--no-dir-listing")) cfg.dir_listing = false;
        else if (!strcmp(a, "--no-htaccess")) cfg.no_htaccess = true;
        else if (a[0] == '-') { fprintf(stderr, "unknown or incomplete option: %s (see --help)\n", a); return 2; }
        else data_dir = a;
        #undef ARG
    }
    if (cfg.port < 0 || cfg.port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }
    if (php_arg && cfg.php_fcgi) { fprintf(stderr, "use either --php or --php-fcgi\n"); return 2; }
    if ((php_arg || cfg.php_fcgi) && !cfg.static_dir && !cfg.n_sites) { fprintf(stderr, "PHP runs the files of the web folder: add --static DIR\n"); return 2; }
    if (php_arg) {
        if (!strcmp(php_arg, "auto")) {
            if (!fcgi_find_php_cgi(php_path, sizeof(php_path))) {
                fprintf(stderr, "php-cgi not found: put PHP in a php/ folder next to segvault-server, add it to the PATH,\n"
                                "or give its path with --php /path/to/php-cgi\n");
                return 2;
            }
            cfg.php_cgi = php_path;
        } else cfg.php_cgi = php_arg;
    }
    if (cfg.timeout_ms < 0) cfg.timeout_ms = 0;
    if (g_bk_dir && g_bk_every <= 0) g_bk_every = 60;
    if (g_bk_keep < 1) g_bk_keep = 1;
    if (g_bk_every > 0 && !g_bk_dir) { fprintf(stderr, "--backup-every needs --backup-dir\n"); return 2; }
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

    {                                       // anche le cartelle intermedie (come mkdir -p)
        char part[1024];
        snprintf(part, sizeof(part), "%s", data_dir);
        for (char* q = part + 1; ; q++) {
            if (*q == '/' || *q == '\\' || !*q) {
                char c = *q;
                *q = '\0';
#ifdef _WIN32
                _mkdir(part);
#else
                mkdir(part, 0755);
#endif
                *q = c;
                if (!c) break;
            }
        }
    }
    static char cert_dir_buf[1200];
    if (!cfg.cert_dir) {
        snprintf(cert_dir_buf, sizeof(cert_dir_buf), "%s/certs", data_dir);
        cfg.cert_dir = cert_dir_buf;
    }
    if (trust_ca) {                     // installa la CA locale tra quelle fidate ed esce
        char out[4000];
        int r = tls_trust_ca(cfg.cert_dir, out, sizeof(out));
        fputs(out, r ? stderr : stdout);
        return r;
    }
    if (background) {                   // avviato all'accesso (servizio su Windows): niente console, registro in un file
        static char logp[1200];
        snprintf(logp, sizeof(logp), "%s/segvault-server.log", data_dir);
        sv_service_background(logp);
    }
    static char mail_dir_buf[1200];
    if (cfg.mail && !cfg.mail_dir) {
        snprintf(mail_dir_buf, sizeof(mail_dir_buf), "%s/mail", data_dir);
        cfg.mail_dir = mail_dir_buf;
    }
    if (!db_init(data_dir)) {
        fprintf(stderr, "cannot open the data directory '%s': %s\n", data_dir, db_init_error());
        return 1;
    }

    // --users senza utenti né token: nessuno potrebbe entrare per creare il primo utente
    if (cfg.require_login && !(cfg.token && cfg.token[0]) && !db_has_users()) {
        if (!sv_random_token(auto_token, 24)) { fprintf(stderr, "cannot generate a random token\n"); db_shutdown(); return 1; }
        cfg.token = auto_token;
        printf("no users yet: create the first one with this token (Authorization: Bearer %s),\n"
               "  e.g. CREATE USER admin IDENTIFIED BY '...'; GRANT ALL ON *.* TO admin WITH GRANT OPTION\n", auto_token);
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
    if (cfg.static_dir && !cfg.no_auto_sites)
        printf("  sites:    http://FOLDER.localhost:%d/  (every folder of the web site; list: http://localhost:%d/.segvault/)\n", port, port);
    for (int i = 0; i < cfg.n_sites; i++)
        printf("  site:     http://%s%s:%d/  -> %s\n", cfg.sites[i].name, strchr(cfg.sites[i].name, '.') ? "" : ".localhost", port, cfg.sites[i].target);
    if (sv_server_php(g_server)[0]) {
        printf("  PHP:      %s\n", sv_server_php(g_server));
        if (!cfg.mysql) printf("            (scripts reach the database with mysqli / PDO if you add --mysql-port 3306)\n");
    }
    if (sv_server_https_port(g_server))
        printf("  https:    https://localhost:%d/  (%s; trust it once with --trust-ca)\n", sv_server_https_port(g_server), sv_server_tls_info(g_server));
    if (sv_server_mail_port(g_server))
        printf("  mail:     smtp://127.0.0.1:%d  -> http://localhost:%d/.segvault/mail/  (%d messages)\n",
               sv_server_mail_port(g_server), port, sv_server_mail_count(g_server));
    if (sv_server_api_prefix(g_server)[0]) printf("  API:      http://%s:%d%s\n", host, port, sv_server_api_prefix(g_server));
    else printf("  API:      off (--api-prefix none)\n");
    if (cfg.mysql) printf("  MySQL:    %s:%d   (e.g. mysql -h %s -P %d -u root)\n", host, sv_server_mysql_port(g_server), host, sv_server_mysql_port(g_server));
    if (g_bk_dir) {
#ifdef _WIN32
        _mkdir(g_bk_dir);
        HANDLE bt = CreateThread(NULL, 0, backup_thread, NULL, 0, NULL);
        if (bt) CloseHandle(bt);
#else
        mkdir(g_bk_dir, 0755);
        pthread_t bt;
        if (pthread_create(&bt, NULL, backup_thread, NULL) == 0) pthread_detach(bt);
#endif
        printf("  backups:  every %d min in %s (last %d kept)\n", g_bk_every, g_bk_dir, g_bk_keep);
    }
    fflush(stdout);

    sv_server_run(g_server);

    printf("shutting down...\n");
    g_bk_stop = 1;
    sv_server_free(g_server);
    db_shutdown();                      // WAL e catalogo scritti su disco
    return 0;
}
