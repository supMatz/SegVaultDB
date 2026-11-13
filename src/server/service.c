/*
SCOPO: segvault-server come servizio dell'utente (vedi service.h).
*/
#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
#else
  #include <unistd.h>
  #include <limits.h>
#endif

// opzioni seguite da un percorso (da rendere assoluto: il servizio parte da un'altra cartella)
static bool path_option(const char* a) {
    static const char* const P[] = { "--static", "--audit", "--backup-dir", "--mail-dir", "--cert-dir" };
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); i++) if (!strcmp(a, P[i])) return true;
    return false;
}

// opzioni seguite da un valore (non un percorso)
static bool value_option(const char* a) {
    static const char* const V[] = { "-p", "--port", "-b", "--bind", "-t", "--token", "-d", "--database", "--cors",
                                     "--php-fcgi", "--php-workers", "--api-prefix", "--max-body", "--timeout",
                                     "--mysql-port", "--backup-every", "--backup-keep", "--mail-port", "--https-port" };
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) if (!strcmp(a, V[i])) return true;
    return false;
}

static void absolute(const char* in, char* out, size_t n) {
#ifdef _WIN32
    if (!_fullpath(out, in, n)) snprintf(out, n, "%s", in);
#else
    char tmp[PATH_MAX];
    if (in[0] == '/') { snprintf(out, n, "%s", in); return; }
    if (realpath(in, tmp)) { snprintf(out, n, "%s", tmp); return; }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) snprintf(out, n, "%.2000s/%.2000s", cwd, in);   // non esiste ancora (cartella dati nuova)
    else snprintf(out, n, "%s", in);
#endif
}

static bool self_path(char* out, size_t n) {
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    return k > 0 && k < n;
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) return false;
    out[k] = '\0';
    return true;
#endif
}

// argomenti per il servizio (assoluti), ognuno separato: argv_out[i] punto in buf
static int build_args(int argc, char** argv, char* buf, size_t bufsz, char** out, int max) {
    size_t used = 0;
    int n = 0;
    for (int i = 0; i < argc && n < max - 1; i++) {
        const char* a = argv[i];
        char tmp[4096];
        if (!strcmp(a, "--install-service") || !strcmp(a, "--uninstall-service") || !strcmp(a, "--background")) continue;
        if ((path_option(a) || !strcmp(a, "--php") || !strcmp(a, "--site")) && i + 1 < argc) {
            if (used + strlen(a) + 1 > bufsz) break;
            out[n++] = strcpy(buf + used, a);
            used += strlen(a) + 1;
            const char* v = argv[++i];
            if (!strcmp(a, "--php") && !strcmp(v, "auto")) snprintf(tmp, sizeof(tmp), "auto");
            else if (!strcmp(a, "--site")) {
                const char* eq = strchr(v, '=');
                const char* tg = eq ? eq + 1 : "";
                if (eq && strncmp(tg, "http", 4) && strspn(tg, "0123456789") != strlen(tg)) {
                    char ab[3000];
                    absolute(tg, ab, sizeof(ab));
                    snprintf(tmp, sizeof(tmp), "%.*s=%s", (int)(eq - v), v, ab);
                } else snprintf(tmp, sizeof(tmp), "%s", v);
            } else absolute(v, tmp, sizeof(tmp));
        } else if (value_option(a) && i + 1 < argc) {
            if (used + strlen(a) + 1 > bufsz) break;
            out[n++] = strcpy(buf + used, a);
            used += strlen(a) + 1;
            snprintf(tmp, sizeof(tmp), "%s", argv[++i]);
        } else if (a[0] == '-') snprintf(tmp, sizeof(tmp), "%s", a);
        else absolute(a, tmp, sizeof(tmp));                      // la cartella dati
        if (used + strlen(tmp) + 1 > bufsz) break;
        out[n++] = strcpy(buf + used, tmp);
        used += strlen(tmp) + 1;
    }
    out[n] = NULL;
    return n;
}

// cartella dati dagli argomenti (l'ultimo argomento che non è un'opzione), "./data" se manca
static void data_dir_of(char** args, int n, char* out, size_t sz) {
    snprintf(out, sz, "data");
    for (int i = 0; i < n; i++) {
        if (path_option(args[i]) || value_option(args[i]) || !strcmp(args[i], "--php") || !strcmp(args[i], "--site")) { i++; continue; }
        if (args[i][0] != '-') snprintf(out, sz, "%s", args[i]);
    }
}

int sv_service_install(int argc, char** argv, char* out, size_t outsz) {
    char exe[2048];
    if (!self_path(exe, sizeof(exe))) { snprintf(out, outsz, "cannot find this program's path\n"); return 1; }
    static char buf[32768];
    char* args[256];
    int n = build_args(argc, argv, buf, sizeof(buf), args, 256);
    char data[4096];
    data_dir_of(args, n, data, sizeof(data));
    bool has_data = false;
    for (int i = 0; i < n; i++) if (!strcmp(args[i], data)) has_data = true;
    char abs_data[4096];
    absolute(data, abs_data, sizeof(abs_data));
    if (!has_data && n < 255) { args[n++] = abs_data; args[n] = NULL; }
#ifdef _WIN32
    // riga di comando con le virgolette; --background toglie la console e scrive il registro in un file
    static char cmd[32768];
    size_t k = (size_t)snprintf(cmd, sizeof(cmd), "\"%s\" --background", exe);
    for (int i = 0; i < n && k < sizeof(cmd); i++) k += (size_t)snprintf(cmd + k, sizeof(cmd) - k, " \"%s\"", args[i]);
    HKEY h;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS ||
        RegSetValueExA(h, "SegVault Server", 0, REG_SZ, (const BYTE*)cmd, (DWORD)strlen(cmd) + 1) != ERROR_SUCCESS) {
        snprintf(out, outsz, "cannot write the Run key of this user\n");
        return 1;
    }
    RegCloseKey(h);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    bool started = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi) != 0;
    if (started) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    snprintf(out, outsz, "Installed: SegVault Server starts when you log in to Windows%s.\nCommand: %s\nLog: %s\\segvault-server.log\n"
             "Remove it with: segvault-server --uninstall-service\n", started ? " (and it is running now)" : "", cmd, abs_data);
    return 0;
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if (!home) { snprintf(out, outsz, "HOME is not set\n"); return 1; }
    char dir[2048], plist[2300];
    snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home);
    mkdir(dir, 0755);
    snprintf(plist, sizeof(plist), "%s/dev.segvault.server.plist", dir);
    FILE* f = fopen(plist, "w");
    if (!f) { snprintf(out, outsz, "cannot write %s\n", plist); return 1; }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n"
               "<key>Label</key><string>dev.segvault.server</string>\n<key>ProgramArguments</key><array>\n<string>%s</string>\n", exe);
    for (int i = 0; i < n; i++) fprintf(f, "<string>%s</string>\n", args[i]);
    fprintf(f, "</array>\n<key>RunAtLoad</key><true/>\n<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n"
               "<key>StandardErrorPath</key><string>%s/segvault-server.log</string>\n</dict></plist>\n", abs_data);
    fclose(f);
    char c[2600];
    snprintf(c, sizeof(c), "launchctl unload '%s' 2>/dev/null; launchctl load -w '%s'", plist, plist);
    int r = system(c);
    snprintf(out, outsz, "Installed: %s\n%s\nRemove it with: segvault-server --uninstall-service\n", plist,
             r == 0 ? "Running now and at every login." : "launchctl load failed: it starts at the next login.");
    return 0;
#else
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (!home && !xdg) { snprintf(out, outsz, "HOME is not set\n"); return 1; }
    const char* dry0 = getenv("SEGVAULT_SERVICE_DRYRUN");
    if (!(dry0 && dry0[0] == '1') && system("command -v systemctl >/dev/null 2>&1") != 0) { snprintf(out, outsz, "systemd not found (systemctl): start segvault-server from your session's autostart instead\n"); return 1; }
    char dir[2048], unit[2300];
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/systemd/user", xdg);
    else snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home);
    char mk[2200];
    snprintf(mk, sizeof(mk), "mkdir -p '%s'", dir);
    if (system(mk) != 0) { snprintf(out, outsz, "cannot create %s\n", dir); return 1; }
    snprintf(unit, sizeof(unit), "%s/segvault.service", dir);
    FILE* f = fopen(unit, "w");
    if (!f) { snprintf(out, outsz, "cannot write %s\n", unit); return 1; }
    // systemd: argomenti tra virgolette, con "\" e "%" protetti
    fprintf(f, "[Unit]\nDescription=SegVault development server (web, PHP, MySQL protocol, mail)\nAfter=network.target\n\n"
               "[Service]\nExecStart=\"%s\"", exe);
    for (int i = 0; i < n; i++) {
        fputs(" \"", f);
        for (const char* q = args[i]; *q; q++) {
            if (*q == '"' || *q == '\\') fputc('\\', f);
            if (*q == '%') fputc('%', f);
            fputc(*q, f);
        }
        fputc('"', f);
    }
    fprintf(f, "\nRestart=on-failure\nRestartSec=3\n\n[Install]\nWantedBy=default.target\n");
    fclose(f);
    // SEGVAULT_SERVICE_DRYRUN=1: solo il file dell'unità (prove), systemd non viene toccato
    const char* dry = getenv("SEGVAULT_SERVICE_DRYRUN");
    int r = dry && dry[0] == '1' ? 0 : system("systemctl --user daemon-reload && systemctl --user enable --now segvault.service >/dev/null 2>&1");
    snprintf(out, outsz, "Installed: %s\n%s\nStatus: systemctl --user status segvault   Log: journalctl --user -u segvault\n"
             "Remove it with: segvault-server --uninstall-service\n", unit,
             r == 0 ? "Running now and at every login (systemd user service)." : "systemctl --user enable failed: run it by hand.");
    return r == 0 ? 0 : 1;
#endif
}

int sv_service_uninstall(char* out, size_t outsz) {
#ifdef _WIN32
    HKEY h;
    LONG r = ERROR_FILE_NOT_FOUND;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        r = RegDeleteValueA(h, "SegVault Server");
        RegCloseKey(h);
    }
    snprintf(out, outsz, r == ERROR_SUCCESS ? "Removed: SegVault Server no longer starts at login (a running copy keeps running until you log out)\n"
                                            : "SegVault Server was not installed\n");
    return 0;
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    char plist[2300], c[2600];
    snprintf(plist, sizeof(plist), "%s/Library/LaunchAgents/dev.segvault.server.plist", home ? home : "");
    snprintf(c, sizeof(c), "launchctl unload -w '%s' 2>/dev/null", plist);
    if (system(c)) {}
    bool had = remove(plist) == 0;
    snprintf(out, outsz, had ? "Removed: %s\n" : "Not installed (%s)\n", plist);
    return 0;
#else
    if (system("systemctl --user disable --now segvault.service >/dev/null 2>&1")) {}
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    char unit[2300];
    if (xdg && xdg[0]) snprintf(unit, sizeof(unit), "%s/systemd/user/segvault.service", xdg);
    else snprintf(unit, sizeof(unit), "%s/.config/systemd/user/segvault.service", home ? home : "");
    bool had = remove(unit) == 0;
    if (system("systemctl --user daemon-reload >/dev/null 2>&1")) {}
    snprintf(out, outsz, had ? "Removed and stopped: %s\n" : "Not installed (%s)\n", unit);
    return 0;
#endif
}

void sv_service_background(const char* log_path) {
#ifdef _WIN32
    FreeConsole();                                       // la finestra della console si chiude
    if (log_path && freopen(log_path, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    if (log_path && freopen(log_path, "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
#else
    (void)log_path;
#endif
}
