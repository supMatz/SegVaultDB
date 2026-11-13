/*
SCOPO: client FastCGI e gruppo di processi php-cgi (vedi fastcgi.h).
*/
#include "fastcgi.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  typedef CRITICAL_SECTION fc_mutex;
  static void mx_init(fc_mutex* m)    { InitializeCriticalSection(m); }
  static void mx_lock(fc_mutex* m)    { EnterCriticalSection(m); }
  static void mx_unlock(fc_mutex* m)  { LeaveCriticalSection(m); }
  static void mx_destroy(fc_mutex* m) { DeleteCriticalSection(m); }
  static void sleep_ms(int ms) { Sleep((DWORD)ms); }
  static double now_ms(void) { return (double)GetTickCount64(); }
  #define PATH_SEP ';'
  #define PHP_CGI_NAME "php-cgi.exe"
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/prctl.h>
  #include <sys/socket.h>
  #include <sys/syscall.h>
  #include <sys/un.h>
  #include <sys/wait.h>
  #include <unistd.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  typedef pthread_mutex_t fc_mutex;
  static void mx_init(fc_mutex* m)    { pthread_mutex_init(m, NULL); }
  static void mx_lock(fc_mutex* m)    { pthread_mutex_lock(m); }
  static void mx_unlock(fc_mutex* m)  { pthread_mutex_unlock(m); }
  static void mx_destroy(fc_mutex* m) { pthread_mutex_destroy(m); }
  static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
  static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
  #define PATH_SEP ':'
  #define PHP_CGI_NAME "php-cgi"
  extern char** environ;
#endif

// tipi dei record FastCGI
enum { FCGI_BEGIN_REQUEST = 1, FCGI_ABORT_REQUEST = 2, FCGI_END_REQUEST = 3, FCGI_PARAMS = 4,
       FCGI_STDIN = 5, FCGI_STDOUT = 6, FCGI_STDERR = 7 };
#define FCGI_RESPONDER 1
#define FCGI_MAX_CONTENT 65535
#define START_WAIT_MS   5000     // tempo per un php-cgi appena avviato prima di accettare connessioni
#define BUSY_WAIT_MS    30000    // attesa massima di un processo libero

typedef struct {
    int    port;
    bool   busy;                 // una richiesta lo sta usando
    bool   started;              // il processo è stato avviato almeno una volta
#ifdef _WIN32
    HANDLE proc;
#else
    pid_t  pid;
#endif
} FcgiWorker;

struct FcgiPool {
    // backend esterno
    bool        external;
    bool        unix_sock;
    char        host[256];
    int         port;
    // processi avviati da qui
    char*       exe;
    char**      args;            // argv dei processi: exe, -d k=v ..., -b, (porta), NULL
    int         n_args;
    char*       cmd_ini;         // Windows: le stesse impostazioni nella riga di comando
    int         n;
    FcgiWorker* w;
    int         next;            // giro dei processi (round robin)
    fc_mutex    mx;
    char        desc[600];
#ifdef _WIN32
    HANDLE      job;             // i processi figli muoiono con SegVault
#else
    char**      envp;            // ambiente dei figli (preparato prima di fork)
    int         max_fd;
    // PR_SET_PDEATHSIG vale per il *thread* che fa fork: un php-cgi avviato dal thread di una connessione
    // morirebbe con quella. Tutti i processi nascono quindi da un thread che vive quanto il gruppo.
    pthread_t       spawner;
    bool            spawner_on, spawner_stop;
    pthread_mutex_t sp_mx, sp_req_mx;   // sp_req_mx: una richiesta di avvio alla volta
    pthread_cond_t  sp_cv;
    FcgiWorker*     sp_w;               // processo da avviare (NULL = niente da fare)
    bool            sp_done, sp_ok;
    char            sp_err[256];
#endif
};

// ── socket ──────────────────────────────────────────────────────

static void set_timeouts(sock_t fd, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static bool send_all(sock_t fd, const void* data, size_t n) {
    const char* p = data;
    while (n > 0) {
        int w = (int)send(fd, p, n > (1u << 30) ? (1 << 30) : (int)n, SEND_FLAGS);
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// 1 = letti n byte, 0 = connessione chiusa, -1 = errore / tempo scaduto
static int recv_all(sock_t fd, void* data, size_t n) {
    char* p = data;
    while (n > 0) {
        int r = (int)recv(fd, p, n > (1u << 30) ? (1 << 30) : (int)n, 0);
        if (r == 0) return 0;
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static sock_t connect_tcp(const char* host, int port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return BAD_SOCK;
    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd != BAD_SOCK && connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) { sock_close(fd); fd = BAD_SOCK; }
    freeaddrinfo(res);
    if (fd != BAD_SOCK) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    return fd;
}

#ifndef _WIN32
static sock_t connect_unix(const char* path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) return BAD_SOCK;
    strcpy(a.sun_path, path);
    sock_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd != BAD_SOCK && connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); fd = BAD_SOCK; }
    return fd;
}
#endif

// una porta TCP libera su 127.0.0.1 (il sistema ne sceglie una, poi la si lascia a php-cgi)
static int free_port(void) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    int port = 0;
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) == 0 && getsockname(fd, (struct sockaddr*)&a, &al) == 0) port = ntohs(a.sin_port);
    sock_close(fd);
    return port;
}

// ── processi php-cgi ────────────────────────────────────────────

static bool worker_alive(FcgiWorker* w) {
#ifdef _WIN32
    return w->proc && WaitForSingleObject(w->proc, 0) == WAIT_TIMEOUT;
#else
    if (w->pid <= 0) return false;
    int st;
    pid_t r = waitpid(w->pid, &st, WNOHANG);
    if (r == 0) return true;
    w->pid = 0;                                 // uscito (o non più nostro figlio)
    return false;
#endif
}

static void worker_kill(FcgiWorker* w) {
#ifdef _WIN32
    if (w->proc) {
        TerminateProcess(w->proc, 0);
        WaitForSingleObject(w->proc, 2000);
        CloseHandle(w->proc);
        w->proc = NULL;
    }
#else
    if (w->pid > 0) {
        kill(w->pid, SIGTERM);
        int st;
        for (int i = 0; i < 100 && waitpid(w->pid, &st, WNOHANG) == 0; i++) sleep_ms(10);
        if (waitpid(w->pid, &st, WNOHANG) == 0) { kill(w->pid, SIGKILL); waitpid(w->pid, &st, 0); }
        w->pid = 0;
    }
#endif
}

static bool worker_start(FcgiPool* p, FcgiWorker* w, char* err, size_t errsz) {
    worker_kill(w);
    w->port = free_port();
    if (!w->port) { snprintf(err, errsz, "no free port for php-cgi"); return false; }
    char bind_arg[32];
    snprintf(bind_arg, sizeof(bind_arg), "127.0.0.1:%d", w->port);
#ifdef _WIN32
    char cmd[4400];
    snprintf(cmd, sizeof(cmd), "\"%s\"%s -b %s", p->exe, p->cmd_ini ? p->cmd_ini : "", bind_arg);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        snprintf(err, errsz, "cannot start %s (error %lu)", p->exe, (unsigned long)GetLastError());
        return false;
    }
    if (p->job) AssignProcessToJobObject(p->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    w->proc = pi.hProcess;
#else
    char* argv[80];
    int na = 0;
    for (int i = 0; i < p->n_args && na < 76; i++) argv[na++] = p->args[i];
    argv[na++] = "-b";
    argv[na++] = bind_arg;
    argv[na] = NULL;
    int max_fd = p->max_fd;
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, errsz, "fork failed: %s", strerror(errno)); return false; }
    if (pid == 0) {
        // figlio: solo chiamate sicure dopo fork in un processo con più thread
        prctl(PR_SET_PDEATHSIG, SIGTERM);          // se SegVault muore, muore anche php-cgi
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); }
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int fd = 3; fd < max_fd; fd++) close(fd);   // socket del server: non vanno ereditati
        execve(argv[0], argv, p->envp);
        _exit(127);
    }
    w->pid = pid;
#endif
    w->started = true;
    return true;
}

#ifndef _WIN32
static void* spawner_main(void* arg) {
    FcgiPool* p = arg;
    pthread_mutex_lock(&p->sp_mx);
    while (!p->spawner_stop) {
        if (p->sp_w && !p->sp_done) {
            FcgiWorker* w = p->sp_w;
            pthread_mutex_unlock(&p->sp_mx);
            char err[256] = "";
            bool ok = worker_start(p, w, err, sizeof(err));
            pthread_mutex_lock(&p->sp_mx);
            p->sp_ok = ok;
            snprintf(p->sp_err, sizeof(p->sp_err), "%s", err);
            p->sp_done = true;
            pthread_cond_broadcast(&p->sp_cv);
        } else pthread_cond_wait(&p->sp_cv, &p->sp_mx);
    }
    pthread_mutex_unlock(&p->sp_mx);
    return NULL;
}
#endif

// avvia (o riavvia) un processo; su Linux lo fa il thread spawner
static bool worker_launch(FcgiPool* p, FcgiWorker* w, char* err, size_t errsz) {
#ifdef _WIN32
    return worker_start(p, w, err, errsz);
#else
    pthread_mutex_lock(&p->sp_req_mx);
    pthread_mutex_lock(&p->sp_mx);
    p->sp_w = w;
    p->sp_done = false;
    pthread_cond_broadcast(&p->sp_cv);
    while (!p->sp_done) pthread_cond_wait(&p->sp_cv, &p->sp_mx);
    bool ok = p->sp_ok;
    snprintf(err, errsz, "%s", p->sp_err);
    p->sp_w = NULL;
    pthread_mutex_unlock(&p->sp_mx);
    pthread_mutex_unlock(&p->sp_req_mx);
    return ok;
#endif
}

// ── record FastCGI ──────────────────────────────────────────────

static void rec_head(uint8_t h[8], int type, size_t len) {
    h[0] = 1;                      // versione
    h[1] = (uint8_t)type;
    h[2] = 0; h[3] = 1;            // id della richiesta: 1 (una richiesta per connessione)
    h[4] = (uint8_t)(len >> 8); h[5] = (uint8_t)len;
    h[6] = 0; h[7] = 0;            // niente padding
}

static bool send_stream(sock_t fd, int type, const char* data, size_t n) {
    uint8_t h[8];
    while (n > 0) {
        size_t k = n > FCGI_MAX_CONTENT ? FCGI_MAX_CONTENT : n;
        rec_head(h, type, k);
        if (!send_all(fd, h, 8) || !send_all(fd, data, k)) return false;
        data += k;
        n -= k;
    }
    rec_head(h, type, 0);          // record vuoto: fine dello stream
    return send_all(fd, h, 8);
}

static void put_len(JsonBuf* b, size_t n) {
    if (n < 128) { char c = (char)n; jb_raw(b, &c, 1); return; }
    char c[4] = { (char)(0x80 | ((n >> 24) & 0x7f)), (char)(n >> 16), (char)(n >> 8), (char)n };
    jb_raw(b, c, 4);
}

// conversazione su una connessione aperta
static int fcgi_talk(sock_t fd, const FcgiParam* params, int nparams, const char* body, size_t body_len,
                     size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz,
                     FcgiOnOutput on_output, void* ctx) {
    size_t total = 0;
    uint8_t begin[16];
    rec_head(begin, FCGI_BEGIN_REQUEST, 8);
    memset(begin + 8, 0, 8);
    begin[9] = FCGI_RESPONDER;     // ruolo; flag 0 = il backend chiude la connessione alla fine
    JsonBuf pb = {0};
    for (int i = 0; i < nparams; i++) {
        const char* v = params[i].value ? params[i].value : "";
        size_t nl = strlen(params[i].name), vl = strlen(v);
        put_len(&pb, nl);
        put_len(&pb, vl);
        jb_raw(&pb, params[i].name, nl);
        jb_raw(&pb, v, vl);
    }
    if (pb.oom) { free(pb.data); snprintf(err, errsz, "out of memory"); return 500; }
    bool ok = send_all(fd, begin, 16) && send_stream(fd, FCGI_PARAMS, pb.data, pb.len) &&
              send_stream(fd, FCGI_STDIN, body, body_len);
    free(pb.data);
    if (!ok) { snprintf(err, errsz, "PHP closed the connection"); return 502; }

    char content[FCGI_MAX_CONTENT + 256];
    for (;;) {
        uint8_t h[8];
        int r = recv_all(fd, h, 8);
        if (r == 0) { snprintf(err, errsz, "PHP closed the connection before the end of the response"); return 502; }
        if (r < 0) { snprintf(err, errsz, "PHP did not answer in time"); return 504; }
        size_t len = (size_t)h[4] << 8 | h[5];
        size_t pad = h[6];
        if (h[0] != 1) { snprintf(err, errsz, "PHP sent a malformed FastCGI record"); return 502; }
        r = recv_all(fd, content, len + pad);
        if (r <= 0) { snprintf(err, errsz, r < 0 ? "PHP did not answer in time" : "PHP closed the connection"); return r < 0 ? 504 : 502; }
        if (h[1] == FCGI_STDOUT) {
            total += len;
            if ((on_output ? out->len + len : total) > max_out) { snprintf(err, errsz, "PHP response larger than %zu MB", max_out >> 20); return 502; }
            jb_raw(out, content, len);
            if (on_output && len && !out->oom && !on_output(ctx, out)) { snprintf(err, errsz, "the client went away"); return 499; }
        } else if (h[1] == FCGI_STDERR) {
            if (errout->len < (1u << 20)) jb_raw(errout, content, len);
        } else if (h[1] == FCGI_END_REQUEST) {
            if (len >= 5 && content[4] != 0) {      // protocolStatus: 1 = troppe connessioni, 2 = ruolo, 3 = sovraccarico
                snprintf(err, errsz, "PHP refused the request (FastCGI status %d)", content[4]);
                return 503;
            }
            if (out->oom) { snprintf(err, errsz, "out of memory"); return 500; }
            return 0;
        }
        // altri tipi (FCGI_GET_VALUES_RESULT, FCGI_UNKNOWN_TYPE): ignorati
    }
}

// ── API ─────────────────────────────────────────────────────────

FcgiPool* fcgi_pool_spawn(const char* php_cgi, int workers, const char* const* ini, int n_ini, char* err, size_t errsz) {
    if (workers <= 0) workers = 4;
    if (workers > 64) workers = 64;
    FcgiPool* p = calloc(1, sizeof(FcgiPool));
    if (!p) { snprintf(err, errsz, "out of memory"); return NULL; }
    p->exe = strdup(php_cgi);
    if (n_ini > 36) n_ini = 36;
    p->args = calloc((size_t)(1 + 2 * n_ini + 1), sizeof(char*));
    if (p->args) {
        p->args[p->n_args++] = p->exe;
        JsonBuf cmd = {0};
        for (int i = 0; i < n_ini; i++) {
            p->args[p->n_args++] = strdup("-d");
            p->args[p->n_args++] = strdup(ini[i]);
            jb_lit(&cmd, " -d \"");
            jb_raw(&cmd, ini[i], strlen(ini[i]));
            jb_lit(&cmd, "\"");
        }
        p->cmd_ini = cmd.data;
    }
    p->n = workers;
    p->w = calloc((size_t)workers, sizeof(FcgiWorker));
    mx_init(&p->mx);
    snprintf(p->desc, sizeof(p->desc), "php-cgi x%d (%.500s)", workers, php_cgi);
    if (!p->exe || !p->w) { snprintf(err, errsz, "out of memory"); fcgi_pool_free(p); return NULL; }
    struct stat st;
    if (stat(php_cgi, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) {
        snprintf(err, errsz, "php-cgi not found: %.300s", php_cgi);
        fcgi_pool_free(p);
        return NULL;
    }
#ifdef _WIN32
    // PHP_FCGI_MAX_REQUESTS=0: un processo serve richieste senza limite (se esce viene riavviato)
    SetEnvironmentVariableA("PHP_FCGI_MAX_REQUESTS", "0");
    SetEnvironmentVariableA("PHP_FCGI_CHILDREN", "0");
    p->job = CreateJobObjectA(NULL, NULL);
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
#else
    // ambiente dei figli: quello di SegVault con PHP_FCGI_* impostati (un processo = una richiesta alla volta)
    int ne = 0;
    while (environ && environ[ne]) ne++;
    p->envp = calloc((size_t)ne + 3, sizeof(char*));
    if (!p->envp) { snprintf(err, errsz, "out of memory"); fcgi_pool_free(p); return NULL; }
    int k = 0;
    for (int i = 0; i < ne; i++)
        if (strncmp(environ[i], "PHP_FCGI_CHILDREN=", 18) && strncmp(environ[i], "PHP_FCGI_MAX_REQUESTS=", 22))
            p->envp[k++] = environ[i];
    p->envp[k++] = "PHP_FCGI_CHILDREN=0";
    p->envp[k++] = "PHP_FCGI_MAX_REQUESTS=0";
    long mf = sysconf(_SC_OPEN_MAX);
    p->max_fd = mf > 0 && mf < 65536 ? (int)mf : 65536;
    pthread_mutex_init(&p->sp_mx, NULL);
    pthread_mutex_init(&p->sp_req_mx, NULL);
    pthread_cond_init(&p->sp_cv, NULL);
    if (pthread_create(&p->spawner, NULL, spawner_main, p) != 0) {
        snprintf(err, errsz, "cannot start the PHP supervisor thread");
        pthread_mutex_destroy(&p->sp_mx);
        pthread_mutex_destroy(&p->sp_req_mx);
        pthread_cond_destroy(&p->sp_cv);
        fcgi_pool_free(p);
        return NULL;
    }
    p->spawner_on = true;
#endif
    for (int i = 0; i < workers; i++)
        if (!worker_launch(p, &p->w[i], err, errsz)) { fcgi_pool_free(p); return NULL; }
    // il primo processo deve accettare connessioni: altrimenti php-cgi non è un php-cgi (o manca una libreria)
    double t0 = now_ms();
    for (;;) {
        sock_t fd = connect_tcp("127.0.0.1", p->w[0].port);
        if (fd != BAD_SOCK) { sock_close(fd); break; }
        if (!worker_alive(&p->w[0])) {
            snprintf(err, errsz, "%.300s exited right after starting (is it php-cgi, not php?)", php_cgi);
            fcgi_pool_free(p);
            return NULL;
        }
        if (now_ms() - t0 > START_WAIT_MS) {
            snprintf(err, errsz, "%.300s does not accept FastCGI connections", php_cgi);
            fcgi_pool_free(p);
            return NULL;
        }
        sleep_ms(20);
    }
    return p;
}

FcgiPool* fcgi_pool_external(const char* addr, char* err, size_t errsz) {
    FcgiPool* p = calloc(1, sizeof(FcgiPool));
    if (!p) { snprintf(err, errsz, "out of memory"); return NULL; }
    p->external = true;
    mx_init(&p->mx);
    snprintf(p->desc, sizeof(p->desc), "FastCGI %.500s", addr);
    if (!strncmp(addr, "unix:", 5) || addr[0] == '/') {
#ifdef _WIN32
        snprintf(err, errsz, "unix sockets are not available on Windows: use host:port");
        fcgi_pool_free(p);
        return NULL;
#else
        p->unix_sock = true;
        snprintf(p->host, sizeof(p->host), "%s", addr[0] == '/' ? addr : addr + 5);
        return p;
#endif
    }
    const char* colon = strrchr(addr, ':');
    if (!colon || colon == addr || (size_t)(colon - addr) >= sizeof(p->host) || atoi(colon + 1) <= 0 || atoi(colon + 1) > 65535) {
        snprintf(err, errsz, "FastCGI address must be host:port or unix:/path (got '%.200s')", addr);
        fcgi_pool_free(p);
        return NULL;
    }
    memcpy(p->host, addr, (size_t)(colon - addr));
    p->host[colon - addr] = '\0';
    p->port = atoi(colon + 1);
    return p;
}

void fcgi_pool_free(FcgiPool* p) {
    if (!p) return;
    for (int i = 0; p->w && i < p->n; i++) worker_kill(&p->w[i]);
#ifdef _WIN32
    if (p->job) CloseHandle(p->job);
#else
    if (p->spawner_on) {
        pthread_mutex_lock(&p->sp_mx);
        p->spawner_stop = true;
        pthread_cond_broadcast(&p->sp_cv);
        pthread_mutex_unlock(&p->sp_mx);
        pthread_join(p->spawner, NULL);
        pthread_mutex_destroy(&p->sp_mx);
        pthread_mutex_destroy(&p->sp_req_mx);
        pthread_cond_destroy(&p->sp_cv);
    }
    free(p->envp);
#endif
    mx_destroy(&p->mx);
    free(p->w);
    for (int i = 1; p->args && i < p->n_args; i++) free(p->args[i]);   // args[0] è exe
    free(p->args);
    free(p->cmd_ini);
    free(p->exe);
    free(p);
}

const char* fcgi_pool_describe(const FcgiPool* p) { return p ? p->desc : ""; }

int fcgi_request(FcgiPool* p, const FcgiParam* params, int nparams, const char* body, size_t body_len,
                 int timeout_ms, size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz,
                 FcgiOnOutput on_output, void* ctx) {
    err[0] = '\0';
    if (!max_out) max_out = (size_t)256 << 20;
    if (p->external) {
        sock_t fd;
#ifndef _WIN32
        if (p->unix_sock) fd = connect_unix(p->host); else
#endif
        fd = connect_tcp(p->host, p->port);
        if (fd == BAD_SOCK) { snprintf(err, errsz, "cannot connect to PHP at %s", p->desc + 8); return 502; }
        if (timeout_ms > 0) set_timeouts(fd, timeout_ms);
        int r = fcgi_talk(fd, params, nparams, body, body_len, max_out, out, errout, err, errsz, on_output, ctx);
        sock_close(fd);
        return r;
    }

    // un processo libero (ognuno serve una richiesta alla volta)
    double t0 = now_ms();
    FcgiWorker* w = NULL;
    while (!w) {
        mx_lock(&p->mx);
        for (int i = 0; i < p->n && !w; i++) {
            FcgiWorker* c = &p->w[(p->next + i) % p->n];
            if (!c->busy) { c->busy = true; w = c; p->next = (int)(c - p->w + 1) % p->n; }
        }
        mx_unlock(&p->mx);
        if (w) break;
        if (now_ms() - t0 > BUSY_WAIT_MS) { snprintf(err, errsz, "all %d PHP processes are busy", p->n); return 503; }
        sleep_ms(2);
    }

    int result;
    sock_t fd = BAD_SOCK;
    double c0 = now_ms();
    bool restarted = false;
    for (;;) {                                         // connessione, riavviando il processo se è uscito
        if (!worker_alive(w)) {
            if (restarted && now_ms() - c0 > START_WAIT_MS) break;
            if (!worker_launch(p, w, err, errsz)) break;
            restarted = true;
            c0 = now_ms();
        }
        fd = connect_tcp("127.0.0.1", w->port);
        if (fd != BAD_SOCK || now_ms() - c0 > START_WAIT_MS) break;
        sleep_ms(10);
    }
    if (fd == BAD_SOCK) {
        if (!err[0]) snprintf(err, errsz, "PHP (php-cgi) does not accept connections");
        result = 502;
    } else {
        if (timeout_ms > 0) set_timeouts(fd, timeout_ms);
        result = fcgi_talk(fd, params, nparams, body, body_len, max_out, out, errout, err, errsz, on_output, ctx);
        sock_close(fd);
        // tempo scaduto: il processo sta ancora eseguendo lo script, si riavvia
        if (result == 504 || result == 499) worker_kill(w);
    }
    mx_lock(&p->mx);
    w->busy = false;
    mx_unlock(&p->mx);
    return result;
}

static bool is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

bool fcgi_find_php_cgi(char* out, size_t outsz) {
    char exe_dir[1024] = "";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    if (n == 0 || n >= sizeof(exe_dir)) exe_dir[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    exe_dir[n > 0 ? n : 0] = '\0';
#endif
    char* slash = strrchr(exe_dir, '/');
#ifdef _WIN32
    char* bs = strrchr(exe_dir, '\\');
    if (bs > slash) slash = bs;
#endif
    if (slash) *slash = '\0'; else exe_dir[0] = '\0';

    const char* env = getenv("SEGVAULT_PHP_CGI");
    if (env && is_file(env)) { snprintf(out, outsz, "%s", env); return true; }
    if (exe_dir[0]) {
        snprintf(out, outsz, "%s/php/" PHP_CGI_NAME, exe_dir);
        if (is_file(out)) return true;
    }
    snprintf(out, outsz, "php/" PHP_CGI_NAME);
    if (is_file(out)) return true;
    const char* path = getenv("PATH");
    while (path && *path) {
        const char* e = strchr(path, PATH_SEP);
        size_t l = e ? (size_t)(e - path) : strlen(path);
        if (l > 0 && l < 900) {
            snprintf(out, outsz, "%.*s/" PHP_CGI_NAME, (int)l, path);
            if (is_file(out)) return true;
        }
        path = e ? e + 1 : NULL;
    }
#ifdef _WIN32
    static const char* const common[] = { "C:\\php\\php-cgi.exe", "C:\\xampp\\php\\php-cgi.exe", "C:\\UniServerZ\\core\\php83\\php-cgi.exe" };
#else
    static const char* const common[] = { "/usr/bin/php-cgi", "/usr/local/bin/php-cgi", "/opt/homebrew/bin/php-cgi" };
#endif
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
        if (is_file(common[i])) { snprintf(out, outsz, "%s", common[i]); return true; }
    out[0] = '\0';
    return false;
}
