/*
SCOPO: programmi CGI (vedi cgi.h).
       stdin e stderr del programma sono file temporanei (niente blocchi tra pipe piene), stdout una pipe
       letta con un limite di tempo: oltre, il processo viene terminato.
*/
#include "cgi.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/select.h>
  #include <sys/syscall.h>
  #include <sys/wait.h>
  #include <time.h>
  #include <unistd.h>
  extern char** environ;
#endif

static const char* const CGI_EXT[] = { ".cgi", ".pl", ".py", ".rb" };

bool cgi_is_script(const char* path, size_t len) {
    for (size_t i = 0; i < sizeof(CGI_EXT) / sizeof(CGI_EXT[0]); i++) {
        size_t el = strlen(CGI_EXT[i]);
        if (len > el && !strncasecmp(path + len - el, CGI_EXT[i], el)) return true;
    }
    return false;
}

// interprete dalla prima riga "#!" (interp + un argomento facoltativo); "" se manca
static void shebang(const char* script, char* interp, size_t in, char* arg, size_t an) {
    interp[0] = arg[0] = '\0';
    FILE* f = fopen(script, "rb");
    if (!f) return;
    char line[512];
    if (fgets(line, sizeof(line), f) && line[0] == '#' && line[1] == '!') {
        char* p = line + 2;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strcspn(p, " \t\r\n");
        snprintf(interp, in, "%.*s", (int)l, p);
        p += l;
        while (*p == ' ' || *p == '\t') p++;
        snprintf(arg, an, "%.*s", (int)strcspn(p, "\r\n"), p);
        size_t al = strlen(arg);
        while (al > 0 && (arg[al - 1] == ' ' || arg[al - 1] == '\t')) arg[--al] = '\0';
    }
    fclose(f);
}

static const char* base_of(const char* p) {
    const char* b = strrchr(p, '/');
#ifdef _WIN32
    const char* bs = strrchr(p, '\\');
    if (bs > b) b = bs;
#endif
    return b ? b + 1 : p;
}

// interprete per estensione (senza "#!")
static const char* by_ext(const char* script) {
    const char* dot = strrchr(script, '.');
    if (!dot) return NULL;
    if (!strcasecmp(dot, ".pl")) return "perl";
    if (!strcasecmp(dot, ".py")) return "python3";
    if (!strcasecmp(dot, ".rb")) return "ruby";
    return NULL;
}

#ifndef _WIN32

static bool find_in_path(const char* name, char* out, size_t n) {
    if (strchr(name, '/')) { snprintf(out, n, "%s", name); return access(out, X_OK) == 0; }
    const char* path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char* e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        snprintf(out, n, "%.*s/%s", (int)l, path, name);
        if (l && access(out, X_OK) == 0) return true;
        path = e ? e + 1 : path + l;
    }
    return false;
}

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

int cgi_exec(const char* script, const char* const* env, const char* body, size_t body_len, int timeout_ms,
             size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz) {
    // programma da eseguire: "#!" (con l'interprete cercato nel PATH se il percorso non esiste qui), estensione,
    // oppure lo script stesso se è eseguibile
    char interp[512], arg[256], exe[1024] = "";
    shebang(script, interp, sizeof(interp), arg, sizeof(arg));
    const char* argv[5];
    int ac = 0;
    if (interp[0]) {
        if (!find_in_path(interp, exe, sizeof(exe)) && !find_in_path(base_of(interp), exe, sizeof(exe))) {
            snprintf(err, errsz, "interpreter not found: %s", interp);
            return 502;
        }
        argv[ac++] = exe;
        if (arg[0]) argv[ac++] = arg;
    } else if (by_ext(script)) {
        const char* name = by_ext(script);
        if (!find_in_path(name, exe, sizeof(exe)) && !(strcmp(name, "python3") == 0 && find_in_path("python", exe, sizeof(exe)))) {
            snprintf(err, errsz, "%s not found in the PATH", name);
            return 502;
        }
        argv[ac++] = exe;
    } else if (access(script, X_OK) != 0) {
        snprintf(err, errsz, "the script is not executable and has no #! line");
        return 502;
    }
    argv[ac++] = script;
    argv[ac] = NULL;
    // ambiente: le variabili CGI più PATH e la lingua di SegVault
    int ne = 0;
    while (env[ne]) ne++;
    const char** envp = calloc((size_t)ne + 8, sizeof(char*));
    if (!envp) { snprintf(err, errsz, "out of memory"); return 500; }
    int k = 0;
    for (int i = 0; i < ne; i++) envp[k++] = env[i];
    for (char** e = environ; e && *e && k < ne + 7; e++)
        if (!strncmp(*e, "PATH=", 5) || !strncmp(*e, "LANG=", 5) || !strncmp(*e, "LC_ALL=", 7) || !strncmp(*e, "TZ=", 3) ||
            !strncmp(*e, "TMPDIR=", 7) || !strncmp(*e, "PERL5LIB=", 9) || !strncmp(*e, "PYTHONPATH=", 11)) envp[k++] = *e;
    envp[k] = NULL;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", script);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = '\0'; else snprintf(dir, sizeof(dir), ".");

    FILE* in = tmpfile();
    FILE* er = tmpfile();
    int pfd[2] = { -1, -1 };
    if (!in || !er || pipe(pfd) != 0) {
        if (in) fclose(in);
        if (er) fclose(er);
        free(envp);
        snprintf(err, errsz, "cannot create the CGI pipes");
        return 500;
    }
    if (body_len) fwrite(body, 1, body_len, in);
    fflush(in);
    rewind(in);
    int infd = fileno(in), erfd = fileno(er);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(infd, 0);
        dup2(pfd[1], 1);
        dup2(erfd, 2);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int fd = 3; fd < 4096; fd++) close(fd);
        if (chdir(dir)) {}
        execve(argv[0], (char* const*)argv, (char* const*)envp);
        _exit(127);
    }
    close(pfd[1]);
    free(envp);
    int rc = 0;
    if (pid < 0) { snprintf(err, errsz, "fork failed: %s", strerror(errno)); rc = 502; }
    else {
        double t0 = now_ms();
        char buf[65536];
        for (;;) {
            int left = timeout_ms > 0 ? (int)(timeout_ms - (now_ms() - t0)) : 1000;
            if (timeout_ms > 0 && left <= 0) { kill(pid, SIGKILL); snprintf(err, errsz, "the CGI program took longer than %d ms", timeout_ms); rc = 504; break; }
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(pfd[0], &rd);
            struct timeval tv = { left / 1000, (left % 1000) * 1000 };
            int r = select(pfd[0] + 1, &rd, NULL, NULL, &tv);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) continue;
            ssize_t g = read(pfd[0], buf, sizeof(buf));
            if (g < 0 && errno == EINTR) continue;
            if (g <= 0) break;
            if (out->len + (size_t)g > max_out) { kill(pid, SIGKILL); snprintf(err, errsz, "CGI output larger than %zu MB", max_out >> 20); rc = 502; break; }
            jb_raw(out, buf, (size_t)g);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (!rc && WIFEXITED(st) && WEXITSTATUS(st) == 127 && out->len == 0) { snprintf(err, errsz, "cannot run %s", argv[0]); rc = 502; }
    }
    close(pfd[0]);
    // stderr (avvisi, errori del programma)
    fflush(er);
    rewind(er);
    char eb[4096];
    size_t g;
    while ((g = fread(eb, 1, sizeof(eb), er)) > 0 && errout->len < (64u << 10)) jb_raw(errout, eb, g);
    fclose(in);
    fclose(er);
    return rc;
}

#else   // Windows

static bool find_exe(const char* name, char* out, size_t n) {
    char nm[300];
    snprintf(nm, sizeof(nm), "%s", base_of(name));
    if (!strchr(nm, '.')) strncat(nm, ".exe", sizeof(nm) - strlen(nm) - 1);
    DWORD r = SearchPathA(NULL, nm, NULL, (DWORD)n, out, NULL);
    if (r > 0 && r < n) return true;
    // /usr/bin/perl non esiste su Windows: le installazioni comuni
    static const char* const DIRS[] = { "C:\\Strawberry\\perl\\bin\\", "C:\\xampp\\perl\\bin\\", "C:\\Perl64\\bin\\", "C:\\Ruby33-x64\\bin\\" };
    for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); i++) {
        snprintf(out, n, "%s%s", DIRS[i], nm);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

static HANDLE temp_file(void) {
    char dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(sizeof(dir), dir) || !GetTempFileNameA(dir, "svc", 0, path)) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
}

int cgi_exec(const char* script, const char* const* env, const char* body, size_t body_len, int timeout_ms,
             size_t max_out, JsonBuf* out, JsonBuf* errout, char* err, size_t errsz) {
    char interp[512], arg[256], exe[MAX_PATH] = "";
    shebang(script, interp, sizeof(interp), arg, sizeof(arg));
    const char* name = NULL;
    if (interp[0]) {
        // "#!/usr/bin/env python3": l'interprete è l'argomento
        if (!strcmp(base_of(interp), "env") && arg[0]) { name = arg; arg[strcspn(arg, " ")] = '\0'; }
        else name = base_of(interp);
    } else name = by_ext(script);
    if (!name) { snprintf(err, errsz, "no #! line and no known extension: cannot run it on Windows"); return 502; }
    bool found = find_exe(name, exe, sizeof(exe));
    if (!found && !strncmp(name, "python", 6)) found = find_exe("python", exe, sizeof(exe)) || find_exe("py", exe, sizeof(exe));
    if (!found) { snprintf(err, errsz, "%s not found in the PATH", name); return 502; }
    char cmd[8300];
    if (interp[0] && strcmp(base_of(interp), "env") && arg[0]) snprintf(cmd, sizeof(cmd), "\"%s\" %s \"%s\"", exe, arg, script);
    else snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", exe, script);
    // ambiente: quello di SegVault con le variabili CGI sopra
    JsonBuf eb = {0};
    for (int i = 0; env[i]; i++) jb_raw(&eb, env[i], strlen(env[i]) + 1);
    char* cur = GetEnvironmentStringsA();
    for (char* e = cur; e && *e; e += strlen(e) + 1) {
        const char* eq = strchr(e + 1, '=');
        if (!eq) continue;
        bool dup = false;
        for (int i = 0; env[i] && !dup; i++) dup = !strncasecmp(env[i], e, (size_t)(eq - e + 1));
        if (!dup) jb_raw(&eb, e, strlen(e) + 1);
    }
    if (cur) FreeEnvironmentStringsA(cur);
    jb_raw(&eb, "\0", 1);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", script);
    char* sl = strrchr(dir, '/');
    char* bs = strrchr(dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = '\0';
    HANDLE hin = temp_file(), her = temp_file(), rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (hin == INVALID_HANDLE_VALUE || her == INVALID_HANDLE_VALUE || !CreatePipe(&rd, &wr, &sa, 0) || eb.oom) {
        snprintf(err, errsz, "cannot create the CGI pipes");
        if (hin != INVALID_HANDLE_VALUE) CloseHandle(hin);
        if (her != INVALID_HANDLE_VALUE) CloseHandle(her);
        free(eb.data);
        return 500;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    DWORD w;
    if (body_len) WriteFile(hin, body, (DWORD)body_len, &w, NULL);
    SetFilePointer(hin, 0, NULL, FILE_BEGIN);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hin;
    si.hStdOutput = wr;
    si.hStdError = her;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, eb.data, sl ? dir : NULL, &si, &pi);
    CloseHandle(wr);
    free(eb.data);
    int rc = 0;
    if (!ok) { snprintf(err, errsz, "cannot start %s (error %lu)", exe, (unsigned long)GetLastError()); rc = 502; }
    else {
        ULONGLONG t0 = GetTickCount64();
        char buf[65536];
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;          // il programma ha chiuso stdout
            if (avail) {
                DWORD got = 0;
                if (!ReadFile(rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, NULL) || !got) break;
                if (out->len + got > max_out) { TerminateProcess(pi.hProcess, 1); snprintf(err, errsz, "CGI output too large"); rc = 502; break; }
                jb_raw(out, buf, got);
                continue;
            }
            if (timeout_ms > 0 && GetTickCount64() - t0 > (ULONGLONG)timeout_ms) {
                TerminateProcess(pi.hProcess, 1);
                snprintf(err, errsz, "the CGI program took longer than %d ms", timeout_ms);
                rc = 504;
                break;
            }
            if (WaitForSingleObject(pi.hProcess, 5) == WAIT_OBJECT_0) {
                // finito: quello che resta nella pipe
                DWORD got = 0;
                while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail && ReadFile(rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, NULL) && got)
                    jb_raw(out, buf, got);
                break;
            }
        }
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    SetFilePointer(her, 0, NULL, FILE_BEGIN);
    char e2[4096];
    DWORD got;
    while (errout->len < (64u << 10) && ReadFile(her, e2, sizeof(e2), &got, NULL) && got) jb_raw(errout, e2, got);
    CloseHandle(hin);
    CloseHandle(her);
    return rc;
}

#endif
