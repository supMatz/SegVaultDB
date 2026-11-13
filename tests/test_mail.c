// Test della cattura delle email (src/server/mail.c): SMTP, archivio, MIME, pagine/API web,
// client sendmail e mail() di un php-cgi vero (se installato).
//   make test_mail && ./test_mail
#include "src/server/server.h"
#include "src/server/mail.h"
#include "src/server/fastcgi.h"
#include "src/bridge/db_api.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail = 0, g_count = 0;
#define CHECK(cond, ...) do { g_count++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void wr(int fd, const char* p) {
    size_t n = strlen(p);
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return; p += w; n -= (size_t)w; }
}

// una risposta SMTP (anche su più righe): il codice dell'ultima
static int smtp_reply(int fd, char* last, size_t n) {
    char buf[4096];
    size_t have = 0;
    for (;;) {
        ssize_t g = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (g <= 0) return -1;
        have += (size_t)g;
        buf[have] = 0;
        for (char* line = buf; ; ) {
            char* e = strstr(line, "\r\n");
            if (!e) break;
            if (line[3] == ' ') { if (last) snprintf(last, n, "%.*s", (int)(e - line), line); return atoi(line); }
            line = e + 2;
        }
    }
}

static int cmd(int fd, const char* c, char* last, size_t n) { wr(fd, c); return smtp_reply(fd, last, n); }

typedef struct { int status; char head[8192]; char* body; size_t len; } Resp;

static Resp http(int port, const char* method, const char* path, const char* extra) {
    Resp r = {0};
    int fd = connect_to(port);
    char q[2048];
    snprintf(q, sizeof(q), "%s %s HTTP/1.1\r\nHost: localhost:%d\r\nConnection: close\r\n%s\r\n", method, path, port, extra ? extra : "");
    wr(fd, q);
    size_t cap = 4 << 20, have = 0;
    char* buf = malloc(cap);
    for (;;) { ssize_t g = recv(fd, buf + have, cap - 1 - have, 0); if (g <= 0) break; have += (size_t)g; }
    buf[have] = 0;
    close(fd);
    char* he = strstr(buf, "\r\n\r\n");
    r.status = atoi(buf + 9);
    if (he) {
        snprintf(r.head, sizeof(r.head), "%.*s", (int)(he - buf), buf);
        r.len = have - (size_t)(he + 4 - buf);
        r.body = malloc(r.len + 1);
        memcpy(r.body, he + 4, r.len);
        r.body[r.len] = 0;
    } else { r.body = strdup(""); }
    free(buf);
    return r;
}

static bool has(const Resp* r, const char* s) { return r->body && strstr(r->body, s); }

static void* run_server(void* s) { sv_server_run((SvServer*)s); return NULL; }

static const char* MULTI =
    "From: =?UTF-8?B?QW5uYSBSb3NzaQ==?= <anna@example.com>\r\n"
    "To: Bob <bob@example.com>\r\n"
    "Cc: carla@example.com\r\n"
    "Subject: =?UTF-8?Q?Conferma_ordine_n=C2=B0_42?= =?UTF-8?B?IOKclA==?=\r\n"
    "Date: Fri, 13 Nov 2026 10:00:00 +0100\r\n"
    "Message-ID: <abc@example.com>\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"OUT\"\r\n"
    "\r\n"
    "preamble\r\n"
    "--OUT\r\n"
    "Content-Type: multipart/alternative;\r\n"
    "  boundary=IN\r\n"
    "\r\n"
    "--IN\r\n"
    "Content-Type: text/plain; charset=iso-8859-1\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "\r\n"
    "Caff=E8 pronto: riga lunga che va a capo =\r\n"
    "qui.\r\n"
    ".punto iniziale\r\n"
    "--IN\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "\r\n"
    "<p>Ciao <b>Bob</b><script>alert(1)</script></p>\r\n"
    "--IN--\r\n"
    "--OUT\r\n"
    "Content-Type: application/pdf; name=\"fattura.pdf\"\r\n"
    "Content-Disposition: attachment; filename*=utf-8''fattura%20n%C2%B042.pdf\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "JVBERi0xLjQK\r\n"
    "--OUT--\r\n";

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--sendmail")) return mail_sendmail_main(atoi(argv[2]), argc - 3, argv + 3);
    char self[1024];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    self[sl > 0 ? sl : 0] = 0;

    char tmp[] = "/tmp/sv_mail_XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    char data[300], www[300], mdir[300];
    snprintf(data, sizeof(data), "%s/data", tmp);
    snprintf(www, sizeof(www), "%s/www", tmp);
    snprintf(mdir, sizeof(mdir), "%s/mail", tmp);
    mkdir(data, 0755);
    mkdir(www, 0755);
    if (!db_init(data)) { printf("db_init failed\n"); return 1; }

    SvServerConfig cfg = { .port = 0, .quiet = true, .mail = true, .mail_port = 0, .mail_dir = mdir, .static_dir = www };
    char err[300];
    SvServer* s = sv_server_start(&cfg, err, sizeof(err));
    if (!s) { printf("server: %s\n", err); return 1; }
    pthread_t th;
    pthread_create(&th, NULL, run_server, s);
    int port = sv_server_port(s), mport = sv_server_mail_port(s);
    CHECK(mport > 0, "mail port: %d", mport);
    char last[512];

    // ── dialogo SMTP ──
    int fd = connect_to(mport);
    CHECK(smtp_reply(fd, last, sizeof(last)) == 220, "greeting: %s", last);
    CHECK(cmd(fd, "EHLO app.local\r\n", last, sizeof(last)) == 250, "EHLO: %s", last);
    CHECK(cmd(fd, "RCPT TO:<x@y>\r\n", last, sizeof(last)) == 503, "RCPT before MAIL: %s", last);
    CHECK(cmd(fd, "AUTH LOGIN\r\n", last, sizeof(last)) == 334, "AUTH LOGIN: %s", last);
    CHECK(cmd(fd, "dXNlcg==\r\n", last, sizeof(last)) == 334, "user: %s", last);
    CHECK(cmd(fd, "cGFzcw==\r\n", last, sizeof(last)) == 235, "any password accepted: %s", last);
    CHECK(cmd(fd, "MAIL FROM:<anna@example.com> SIZE=1000\r\n", last, sizeof(last)) == 250, "MAIL: %s", last);
    CHECK(cmd(fd, "RCPT TO:<bob@example.com>\r\n", last, sizeof(last)) == 250, "RCPT: %s", last);
    CHECK(cmd(fd, "RCPT TO:<carla@example.com>\r\n", last, sizeof(last)) == 250, "RCPT 2: %s", last);
    CHECK(cmd(fd, "RCPT TO:<segreto@example.com>\r\n", last, sizeof(last)) == 250, "RCPT bcc: %s", last);
    CHECK(cmd(fd, "DATA\r\n", last, sizeof(last)) == 354, "DATA: %s", last);
    // il client raddoppia il punto iniziale ("..punto" diventa ".punto" nel messaggio)
    char* stuffed = malloc(strlen(MULTI) * 2);
    strcpy(stuffed, MULTI);
    char* dp = strstr(stuffed, ".punto");
    memmove(dp + 1, dp, strlen(dp) + 1);
    dp[0] = '.';
    wr(fd, stuffed);
    free(stuffed);
    CHECK(cmd(fd, ".\r\n", last, sizeof(last)) == 250 && strstr(last, "queued as 1"), "end of DATA: %s", last);
    // pipelining: un secondo messaggio con i comandi mandati insieme
    wr(fd, "RSET\r\nMAIL FROM:<noreply@shop.test>\r\nRCPT TO:<zed@example.com>\r\nDATA\r\n");
    CHECK(smtp_reply(fd, last, sizeof(last)) == 250, "RSET: %s", last);
    CHECK(smtp_reply(fd, last, sizeof(last)) == 250, "MAIL pipelined: %s", last);
    CHECK(smtp_reply(fd, last, sizeof(last)) == 250, "RCPT pipelined: %s", last);
    CHECK(smtp_reply(fd, last, sizeof(last)) == 354, "DATA pipelined: %s", last);
    CHECK(cmd(fd, "Subject: Reset password\r\nFrom: Shop <noreply@shop.test>\r\n\r\nLink: http://shop.test/reset?t=1\r\n.\r\n", last, sizeof(last)) == 250, "msg 2: %s", last);
    CHECK(cmd(fd, "STARTTLS\r\n", last, sizeof(last)) == 454, "STARTTLS refused: %s", last);
    CHECK(cmd(fd, "QUIT\r\n", last, sizeof(last)) == 221, "QUIT: %s", last);
    close(fd);
    CHECK(sv_server_mail_count(s) == 2, "count: %d", sv_server_mail_count(s));

    // ── API ──
    Resp r = http(port, "GET", "/.segvault/mail/api/messages", NULL);
    CHECK(r.status == 200 && has(&r, "\"total\":2") && strstr(r.body, "Reset password") < strstr(r.body, "Conferma"),
          "list, newest first: %d %s", r.status, r.body);
    CHECK(has(&r, "\"subject\":\"Conferma ordine n\xc2\xb0 42 \xe2\x9c\x94\"") && has(&r, "Anna Rossi <anna@example.com>"),
          "RFC 2047 subject and from: %s", r.body);
    CHECK(has(&r, "bob@example.com, carla@example.com, segreto@example.com"), "envelope recipients (Bcc too): %s", r.body);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/api/messages?to=zed", NULL);
    CHECK(has(&r, "Reset password") && !has(&r, "Conferma"), "filter to=: %s", r.body);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/api/messages?subject=ORDINE", NULL);
    CHECK(has(&r, "Conferma") && !has(&r, "Reset"), "filter subject= (case-insensitive): %s", r.body);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/api/messages/latest", NULL);
    CHECK(r.status == 200 && has(&r, "http://shop.test/reset?t=1"), "latest + text body: %s", r.body);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/api/messages/1", NULL);
    CHECK(r.status == 200 && has(&r, "\"text\":\"Caff\xc3\xa8 pronto: riga lunga che va a capo qui.\\r\\n.punto iniziale\""),
          "QP + latin1 + dot-stuffing: %s", r.body);
    CHECK(has(&r, "<p>Ciao <b>Bob</b>") && has(&r, "\"cc\":\"carla@example.com\"") && has(&r, "\"message_id\":\"<abc@example.com>\""),
          "html, cc, message id: %s", r.body);
    CHECK(has(&r, "{\"name\":\"fattura n\xc2\xb0" "42.pdf\",\"type\":\"application/pdf\",\"size\":9}"), "attachment (RFC 2231 name, base64 size): %s", r.body);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/1.html", NULL);
    CHECK(r.status == 200 && has(&r, "<b>Bob</b>") && strcasestr(r.head, "Content-Security-Policy: sandbox"), "html part sandboxed: %s", r.head);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/1/att/0", NULL);
    CHECK(r.status == 200 && r.len == 9 && !memcmp(r.body, "%PDF-1.4\n", 9) && strcasestr(r.head, "attachment"), "attachment download: %d %zu", r.status, r.len);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/1.eml", NULL);
    CHECK(r.status == 200 && has(&r, "X-SegVault-Rcpt: bob@example.com") && has(&r, "\r\n.punto iniziale"), "raw .eml: %d", r.status);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/", NULL);
    CHECK(r.status == 200 && has(&r, "SegVault Mail") && has(&r, "api/messages"), "mail page: %d", r.status);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail", NULL);
    CHECK(r.status == 301, "redirect to the folder: %d", r.status);
    free(r.body);
    r = http(port, "GET", "/", NULL);
    CHECK(has(&r, "Mail catcher") && has(&r, "2 messages"), "dashboard card: %s", r.body);
    free(r.body);
    r = http(port, "DELETE", "/.segvault/mail/api/messages/2", "Origin: http://evil.example\r\n");
    CHECK(r.status == 403, "delete from another site refused: %d", r.status);
    free(r.body);
    r = http(port, "DELETE", "/.segvault/mail/api/messages/2", NULL);
    CHECK(r.status == 200 && sv_server_mail_count(s) == 1, "delete one: %d", r.status);
    free(r.body);
    r = http(port, "GET", "/.segvault/mail/api/messages/2", NULL);
    CHECK(r.status == 404, "deleted message gone: %d", r.status);
    free(r.body);

    // ── client sendmail (come lo chiama PHP: messaggio su stdin, destinatari dalle intestazioni) ──
    {
        int pfd[2];
        if (pipe(pfd)) return 1;
        pid_t pid = fork();
        if (pid == 0) {
            dup2(pfd[0], 0);
            close(pfd[1]);
            char ps[16];
            snprintf(ps, sizeof(ps), "%d", mport);
            execl(self, self, "--sendmail", ps, "-t", "-i", (char*)NULL);
            _exit(127);
        }
        close(pfd[0]);
        const char* m = "To: dan@example.com\nBcc: hidden@example.com\nSubject: Da sendmail\nFrom: app@example.com\n\nRiga uno\n.riga col punto\n";
        if (write(pfd[1], m, strlen(m))) {}
        close(pfd[1]);
        int st;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sendmail exit: %d", st);
        r = http(port, "GET", "/.segvault/mail/api/messages/latest", NULL);
        CHECK(has(&r, "Da sendmail") && has(&r, "dan@example.com, hidden@example.com") && has(&r, "Riga uno\\r\\n.riga col punto") &&
              !has(&r, "Bcc:"), "sendmail delivery: %s", r.body);
        free(r.body);
    }

    // ── riavvio: i messaggi restano ──
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);
    s = sv_server_start(&cfg, err, sizeof(err));
    pthread_create(&th, NULL, run_server, s);
    port = sv_server_port(s);
    mport = sv_server_mail_port(s);
    CHECK(sv_server_mail_count(s) == 2, "messages kept after restart: %d", sv_server_mail_count(s));
    r = http(port, "GET", "/.segvault/mail/api/messages", NULL);
    CHECK(has(&r, "Conferma ordine") && has(&r, "Da sendmail"), "summaries reloaded: %s", r.body);
    free(r.body);
    r = http(port, "DELETE", "/.segvault/mail/api/messages", NULL);
    CHECK(r.status == 200 && sv_server_mail_count(s) == 0, "delete all: %d", r.status);
    free(r.body);
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    // ── mail() di un php-cgi vero ──
    char php[1024];
    if (fcgi_find_php_cgi(php, sizeof(php))) {
        FILE* f;
        char p[400];
        snprintf(p, sizeof(p), "%s/send.php", www);
        f = fopen(p, "w");
        fputs("<?php $ok = mail('eve@example.com', 'Benvenuta', \"Ciao Eve\\nsecondo rigo\", 'From: site@example.com'); echo $ok ? 'sent' : 'failed';", f);
        fclose(f);
        SvServerConfig c2 = cfg;
        c2.php_cgi = php;
        c2.php_workers = 1;
        c2.self_exe = self;
        s = sv_server_start(&c2, err, sizeof(err));
        CHECK(s != NULL, "server with PHP: %s", err);
        if (s) {
            pthread_create(&th, NULL, run_server, s);
            port = sv_server_port(s);
            r = http(port, "GET", "/send.php", NULL);
            CHECK(r.status == 200 && has(&r, "sent"), "php mail(): %d %s", r.status, r.body);
            free(r.body);
            r = http(port, "GET", "/.segvault/mail/api/messages/latest", NULL);
            CHECK(has(&r, "\"subject\":\"Benvenuta\"") && has(&r, "eve@example.com") && has(&r, "Ciao Eve"), "caught from PHP: %s", r.body);
            free(r.body);
            sv_server_stop(s);
            pthread_join(th, NULL);
            sv_server_free(s);
        }
        printf("(real php-cgi tested: %s)\n", php);
    }

    // mail spento: niente pagine
    SvServerConfig c3 = { .port = 0, .quiet = true };
    s = sv_server_start(&c3, err, sizeof(err));
    pthread_create(&th, NULL, run_server, s);
    r = http(sv_server_port(s), "GET", "/.segvault/mail/", NULL);
    CHECK(r.status == 404 && has(&r, "--mail-port"), "mail off: %d %s", r.status, r.body);
    free(r.body);
    sv_server_stop(s);
    pthread_join(th, NULL);
    sv_server_free(s);

    db_shutdown();
    char rm[400];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmp);
    if (system(rm)) {}
    printf("test_mail: %d checks, %d failed\n", g_count, g_fail);
    return g_fail ? 1 : 0;
}
