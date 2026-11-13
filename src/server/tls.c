/*
SCOPO: HTTPS con OpenSSL caricato all'avvio (vedi tls.h).
*/
#include "tls.h"
#include "h2.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <direct.h>
  #undef X509_NAME                     // wincrypt.h usa gli stessi nomi di OpenSSL
  #undef X509_EXTENSIONS
  #undef X509_CERT_PAIR
  #undef PKCS7_ISSUER_AND_SERIAL
  #undef OCSP_REQUEST
  #undef OCSP_RESPONSE
  #define BAD_SOCK INVALID_SOCKET
  #define sock_close closesocket
  #define SEND_FLAGS 0
  typedef HMODULE lib_t;
  static lib_t lib_open(const char* n) { return LoadLibraryA(n); }
  static void* lib_sym(lib_t l, const char* n) { return (void*)GetProcAddress(l, n); }
  static void lib_close(lib_t l) { FreeLibrary(l); }
  typedef CRITICAL_SECTION tls_mx;
  static void mx_init(tls_mx* m) { InitializeCriticalSection(m); }
  static void mx_lock(tls_mx* m) { EnterCriticalSection(m); }
  static void mx_unlock(tls_mx* m) { LeaveCriticalSection(m); }
  static void mx_destroy(tls_mx* m) { DeleteCriticalSection(m); }
  #define mkdir_p(d) _mkdir(d)
  static void set_nonblock(tls_sock fd, bool on) { u_long m = on ? 1 : 0; ioctlsocket(fd, FIONBIO, &m); }
#else
  #include <arpa/inet.h>
  #include <dlfcn.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <pthread.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define BAD_SOCK (-1)
  #define sock_close close
  #define SEND_FLAGS MSG_NOSIGNAL
  typedef void* lib_t;
  static lib_t lib_open(const char* n) { return dlopen(n, RTLD_NOW | RTLD_LOCAL); }
  static void* lib_sym(lib_t l, const char* n) { return dlsym(l, n); }
  static void lib_close(lib_t l) { dlclose(l); }
  typedef pthread_mutex_t tls_mx;
  static void mx_init(tls_mx* m) { pthread_mutex_init(m, NULL); }
  static void mx_lock(tls_mx* m) { pthread_mutex_lock(m); }
  static void mx_unlock(tls_mx* m) { pthread_mutex_unlock(m); }
  static void mx_destroy(tls_mx* m) { pthread_mutex_destroy(m); }
  #define mkdir_p(d) mkdir(d, 0755)
  static void set_nonblock(tls_sock fd, bool on) {
      int fl = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
  }
#endif

// ── OpenSSL: solo le funzioni che servono, prese a runtime ──────

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;
typedef struct x509_st X509;
typedef struct X509_name_st X509_NAME;
typedef struct X509_extension_st X509_EXTENSION;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_md_st EVP_MD;
typedef struct bio_st BIO;
typedef struct asn1_string_st ASN1_INTEGER;
typedef struct asn1_string_st ASN1_TIME;

#define SSL_ERROR_WANT_READ   2
#define SSL_ERROR_WANT_WRITE  3
#define SSL_ERROR_ZERO_RETURN 6
#define SSL_CTRL_SET_TLSEXT_SERVERNAME_CB  53
#define SSL_CTRL_SET_TLSEXT_SERVERNAME_ARG 54
#define SSL_CTRL_SET_MIN_PROTO_VERSION     123
#define SSL_CTRL_MODE                      33
#define SSL_MODE_ENABLE_PARTIAL_WRITE      0x1
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 0x2
#define TLS1_2_VERSION 0x0303
#define TLSEXT_NAMETYPE_host_name 0
#define SSL_TLSEXT_ERR_OK 0
#define SSL_TLSEXT_ERR_NOACK 3
#define OPENSSL_NPN_NEGOTIATED 1
#define MBSTRING_ASC 0x1001
#define NID_basic_constraints 87
#define NID_key_usage 83
#define NID_ext_key_usage 126
#define NID_subject_alt_name 85

static struct {
    lib_t ssl, crypto;
    char  desc[96];
    // libssl
    const SSL_METHOD* (*TLS_server_method)(void);
    SSL_CTX* (*SSL_CTX_new)(const SSL_METHOD*);
    void     (*SSL_CTX_free)(SSL_CTX*);
    int      (*SSL_CTX_use_certificate)(SSL_CTX*, X509*);
    int      (*SSL_CTX_use_PrivateKey)(SSL_CTX*, EVP_PKEY*);
    long     (*SSL_CTX_ctrl)(SSL_CTX*, int, long, void*);
    long     (*SSL_CTX_callback_ctrl)(SSL_CTX*, int, void (*)(void));
    void     (*SSL_CTX_set_alpn_select_cb)(SSL_CTX*, int (*)(SSL*, const unsigned char**, unsigned char*, const unsigned char*, unsigned int, void*), void*);
    SSL*     (*SSL_new)(SSL_CTX*);
    void     (*SSL_free)(SSL*);
    int      (*SSL_set_fd)(SSL*, int);
    int      (*SSL_accept)(SSL*);
    int      (*SSL_read)(SSL*, void*, int);
    int      (*SSL_write)(SSL*, const void*, int);
    int      (*SSL_pending)(const SSL*);
    int      (*SSL_shutdown)(SSL*);
    int      (*SSL_get_error)(const SSL*, int);
    const char* (*SSL_get_servername)(const SSL*, int);
    SSL_CTX* (*SSL_set_SSL_CTX)(SSL*, SSL_CTX*);
    void     (*SSL_get0_alpn_selected)(const SSL*, const unsigned char**, unsigned int*);
    int      (*SSL_select_next_proto)(unsigned char**, unsigned char*, const unsigned char*, unsigned int, const unsigned char*, unsigned int);
    // libcrypto
    EVP_PKEY* (*EVP_PKEY_Q_keygen)(void*, const char*, const char*, ...);
    void      (*EVP_PKEY_free)(EVP_PKEY*);
    const EVP_MD* (*EVP_sha256)(void);
    X509*     (*X509_new)(void);
    void      (*X509_free)(X509*);
    int       (*X509_set_version)(X509*, long);
    ASN1_INTEGER* (*X509_get_serialNumber)(X509*);
    int       (*ASN1_INTEGER_set)(ASN1_INTEGER*, long);
    ASN1_TIME* (*X509_getm_notBefore)(const X509*);
    ASN1_TIME* (*X509_getm_notAfter)(const X509*);
    ASN1_TIME* (*X509_gmtime_adj)(ASN1_TIME*, long);
    int       (*X509_set_pubkey)(X509*, EVP_PKEY*);
    X509_NAME* (*X509_get_subject_name)(const X509*);
    int       (*X509_set_issuer_name)(X509*, const X509_NAME*);
    int       (*X509_NAME_add_entry_by_txt)(X509_NAME*, const char*, int, const unsigned char*, int, int, int);
    X509_EXTENSION* (*X509V3_EXT_nconf_nid)(void*, void*, int, const char*);
    int       (*X509_add_ext)(X509*, X509_EXTENSION*, int);
    void      (*X509_EXTENSION_free)(X509_EXTENSION*);
    int       (*X509_sign)(X509*, EVP_PKEY*, const EVP_MD*);
    BIO*      (*BIO_new_file)(const char*, const char*);
    int       (*BIO_free)(BIO*);
    int       (*PEM_write_bio_X509)(BIO*, const X509*);
    int       (*PEM_write_bio_PrivateKey)(BIO*, const EVP_PKEY*, const void*, const unsigned char*, int, void*, void*);
    X509*     (*PEM_read_bio_X509)(BIO*, X509**, void*, void*);
    EVP_PKEY* (*PEM_read_bio_PrivateKey)(BIO*, EVP_PKEY**, void*, void*);
    const char* (*OpenSSL_version)(int);
    int       (*RAND_bytes)(unsigned char*, int);
} O;

static tls_mx g_load_mx;
static int g_loaded;                  // 0 = da provare, 1 = caricato, -1 = assente
static char g_load_err[300];

#ifdef _WIN32
static void load_mx_init(void) {
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) mx_init(&g_load_mx);
}
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void load_mx_init_real(void) { mx_init(&g_load_mx); }
static void load_mx_init(void) { pthread_once(&g_once, load_mx_init_real); }
#endif

static bool load_pair(const char* ssl_name, const char* crypto_name) {
    lib_t c = lib_open(crypto_name);
    if (!c) return false;
    lib_t s = lib_open(ssl_name);
    if (!s) { lib_close(c); return false; }
    O.ssl = s;
    O.crypto = c;
    return true;
}

static bool openssl_load(char* err, size_t errsz) {
    load_mx_init();
    mx_lock(&g_load_mx);
    if (g_loaded) {
        bool ok = g_loaded > 0;
        if (!ok) snprintf(err, errsz, "%s", g_load_err);
        mx_unlock(&g_load_mx);
        return ok;
    }
    bool found = false;
#ifdef _WIN32
    char dir[1024] = "";
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof(dir));
    if (n > 0 && n < sizeof(dir)) { char* sl = strrchr(dir, '\\'); if (sl) *sl = '\0'; else dir[0] = '\0'; }
    static const char* const V[] = { "4", "3" };
    static const char* const SUB[] = { "", "\\php", "" };
    for (int k = 0; k < 2 && !found; k++)
        for (int d = 0; d < 3 && !found; d++) {
            char a[1200], b[1200];
            if (d < 2 && dir[0]) {
                snprintf(a, sizeof(a), "%s%s\\libssl-%s-x64.dll", dir, SUB[d], V[k]);
                snprintf(b, sizeof(b), "%s%s\\libcrypto-%s-x64.dll", dir, SUB[d], V[k]);
            } else if (d == 2) {
                snprintf(a, sizeof(a), "libssl-%s-x64.dll", V[k]);
                snprintf(b, sizeof(b), "libcrypto-%s-x64.dll", V[k]);
            } else continue;
            found = load_pair(a, b);
        }
#elif defined(__APPLE__)
    static const char* const P[][2] = { { "libssl.3.dylib", "libcrypto.3.dylib" },
                                        { "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib", "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib" },
                                        { "/usr/local/opt/openssl@3/lib/libssl.3.dylib", "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib" } };
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]) && !found; i++) found = load_pair(P[i][0], P[i][1]);
#else
    static const char* const P[][2] = { { "libssl.so.3", "libcrypto.so.3" }, { "libssl.so.4", "libcrypto.so.4" }, { "libssl.so", "libcrypto.so" } };
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]) && !found; i++) found = load_pair(P[i][0], P[i][1]);
#endif
    if (!found) {
        snprintf(g_load_err, sizeof(g_load_err),
#ifdef _WIN32
                 "OpenSSL not found: HTTPS needs libssl-3-x64.dll and libcrypto-3-x64.dll (they come with PHP for Windows: put PHP in a php folder next to SegVault)"
#else
                 "OpenSSL 3 not found (install the openssl package)"
#endif
                 );
        g_loaded = -1;
        snprintf(err, errsz, "%s", g_load_err);
        mx_unlock(&g_load_mx);
        return false;
    }
    bool ok = true;
    const char* missing = NULL;
    #define S(name) do { *(void**)&O.name = lib_sym(O.ssl, #name); if (!O.name && ok) { ok = false; missing = #name; } } while (0)
    #define C(name) do { *(void**)&O.name = lib_sym(O.crypto, #name); if (!O.name && ok) { ok = false; missing = #name; } } while (0)
    S(TLS_server_method); S(SSL_CTX_new); S(SSL_CTX_free); S(SSL_CTX_use_certificate); S(SSL_CTX_use_PrivateKey);
    S(SSL_CTX_ctrl); S(SSL_CTX_callback_ctrl); S(SSL_CTX_set_alpn_select_cb); S(SSL_new); S(SSL_free); S(SSL_set_fd);
    S(SSL_accept); S(SSL_read); S(SSL_write); S(SSL_pending); S(SSL_shutdown); S(SSL_get_error); S(SSL_get_servername);
    S(SSL_set_SSL_CTX); S(SSL_get0_alpn_selected); S(SSL_select_next_proto);
    C(EVP_PKEY_Q_keygen); C(EVP_PKEY_free); C(EVP_sha256); C(X509_new); C(X509_free); C(X509_set_version);
    C(X509_get_serialNumber); C(ASN1_INTEGER_set); C(X509_getm_notBefore); C(X509_getm_notAfter); C(X509_gmtime_adj);
    C(X509_set_pubkey); C(X509_get_subject_name); C(X509_set_issuer_name); C(X509_NAME_add_entry_by_txt);
    C(X509V3_EXT_nconf_nid); C(X509_add_ext); C(X509_EXTENSION_free); C(X509_sign); C(BIO_new_file); C(BIO_free);
    C(PEM_write_bio_X509); C(PEM_write_bio_PrivateKey); C(PEM_read_bio_X509); C(PEM_read_bio_PrivateKey);
    C(OpenSSL_version); C(RAND_bytes);
    #undef S
    #undef C
    if (!ok) {
        snprintf(g_load_err, sizeof(g_load_err), "the OpenSSL library found is too old (missing %s): OpenSSL 3 or later is needed", missing);
        lib_close(O.ssl);
        lib_close(O.crypto);
        g_loaded = -1;
        snprintf(err, errsz, "%s", g_load_err);
        mx_unlock(&g_load_mx);
        return false;
    }
    snprintf(O.desc, sizeof(O.desc), "%s", O.OpenSSL_version(0));
    g_loaded = 1;
    mx_unlock(&g_load_mx);
    return true;
}

// ── certificati ─────────────────────────────────────────────────

#define LEAF_CACHE 256

typedef struct {
    char     name[256];
    SSL_CTX* ctx;
} Leaf;

struct TlsCtx {
    char      dir[1024];
    char      ca_path[1100];
    X509*     ca;
    EVP_PKEY* ca_key;
    EVP_PKEY* leaf_key;              // una chiave per tutti i certificati dei siti
    SSL_CTX*  def;                   // "localhost" (anche senza SNI)
    tls_mx    mx;
    Leaf      leaves[LEAF_CACHE];
    int       nleaves;
};

static long rand_serial(void) {
    unsigned char b[8];
    long v = 0;
    if (O.RAND_bytes(b, sizeof(b)) == 1) memcpy(&v, b, sizeof(v));
    else v = (long)time(NULL);
    if (v < 0) v = -v;
    return v ? v : 1;
}

static bool add_ext(X509* x, int nid, const char* value) {
    X509_EXTENSION* e = O.X509V3_EXT_nconf_nid(NULL, NULL, nid, value);
    if (!e) return false;
    bool ok = O.X509_add_ext(x, e, -1) == 1;
    O.X509_EXTENSION_free(e);
    return ok;
}

// certificato firmato da issuer (o autofirmato se issuer è NULL)
static X509* make_cert(EVP_PKEY* key, const char* cn, X509* issuer, EVP_PKEY* issuer_key, long days, bool ca, const char* san) {
    X509* x = O.X509_new();
    if (!x) return NULL;
    bool ok = O.X509_set_version(x, 2) == 1 &&
              O.ASN1_INTEGER_set(O.X509_get_serialNumber(x), rand_serial()) == 1 &&
              O.X509_gmtime_adj(O.X509_getm_notBefore(x), -86400L) &&
              O.X509_gmtime_adj(O.X509_getm_notAfter(x), days * 86400L) &&
              O.X509_set_pubkey(x, key) == 1;
    X509_NAME* nm = O.X509_get_subject_name(x);
    ok = ok && O.X509_NAME_add_entry_by_txt(nm, "O", MBSTRING_ASC, (const unsigned char*)"SegVault development", -1, -1, 0) == 1 &&
         O.X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0) == 1;
    ok = ok && O.X509_set_issuer_name(x, issuer ? O.X509_get_subject_name(issuer) : nm) == 1;
    if (ca) ok = ok && add_ext(x, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") && add_ext(x, NID_key_usage, "critical,keyCertSign,cRLSign");
    else ok = ok && add_ext(x, NID_basic_constraints, "critical,CA:FALSE") && add_ext(x, NID_key_usage, "critical,digitalSignature,keyEncipherment") &&
              add_ext(x, NID_ext_key_usage, "serverAuth") && add_ext(x, NID_subject_alt_name, san);
    ok = ok && O.X509_sign(x, issuer_key ? issuer_key : key, O.EVP_sha256()) > 0;
    if (!ok) { O.X509_free(x); return NULL; }
    return x;
}

static bool write_pem(const char* path, X509* x, EVP_PKEY* k) {
    BIO* b = O.BIO_new_file(path, "w");
    if (!b) return false;
    bool ok = x ? O.PEM_write_bio_X509(b, x) == 1 : O.PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL) == 1;
    O.BIO_free(b);
#ifndef _WIN32
    if (k) chmod(path, 0600);                 // la chiave della CA: solo per questo utente
#endif
    return ok;
}

static X509* read_cert(const char* path) {
    BIO* b = O.BIO_new_file(path, "r");
    if (!b) return NULL;
    X509* x = O.PEM_read_bio_X509(b, NULL, NULL, NULL);
    O.BIO_free(b);
    return x;
}

static EVP_PKEY* read_key(const char* path) {
    BIO* b = O.BIO_new_file(path, "r");
    if (!b) return NULL;
    EVP_PKEY* k = O.PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    O.BIO_free(b);
    return k;
}

static int alpn_cb(SSL* s, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void* arg) {
    (void)s; (void)arg;
    // h2 se il client lo propone, altrimenti HTTP/1.1
    static const unsigned char pref_h2[] = "\x02h2\x08http/1.1";
    static const unsigned char pref_h1[] = "\x08http/1.1";
    const unsigned char* pref = h2_enabled() ? pref_h2 : pref_h1;
    unsigned int pl = h2_enabled() ? sizeof(pref_h2) - 1 : sizeof(pref_h1) - 1;
    unsigned char* o = NULL;
    if (O.SSL_select_next_proto(&o, outlen, pref, pl, in, inlen) != OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_NOACK;
    *out = o;
    return SSL_TLSEXT_ERR_OK;
}

static int sni_cb(SSL* s, int* ad, void* arg);

static SSL_CTX* new_ctx(TlsCtx* t, X509* cert) {
    SSL_CTX* c = O.SSL_CTX_new(O.TLS_server_method());
    if (!c) return NULL;
    if (O.SSL_CTX_use_certificate(c, cert) != 1 || O.SSL_CTX_use_PrivateKey(c, t->leaf_key) != 1) { O.SSL_CTX_free(c); return NULL; }
    O.SSL_CTX_ctrl(c, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);
    O.SSL_CTX_ctrl(c, SSL_CTRL_MODE, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, NULL);
    O.SSL_CTX_set_alpn_select_cb(c, alpn_cb, NULL);
    O.SSL_CTX_callback_ctrl(c, SSL_CTRL_SET_TLSEXT_SERVERNAME_CB, (void (*)(void))sni_cb);
    O.SSL_CTX_ctrl(c, SSL_CTRL_SET_TLSEXT_SERVERNAME_ARG, 0, t);
    return c;
}

// nome accettabile per un certificato al volo: lettere, cifre, '-', '.', niente di strano
static bool good_name(const char* n) {
    size_t l = strlen(n);
    if (l == 0 || l > 200 || n[0] == '.' || n[0] == '-') return false;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' && c != '.') return false;
        if (c == '.' && n[i + 1] == '.') return false;
    }
    return true;
}

static SSL_CTX* ctx_for(TlsCtx* t, const char* name) {
    char lower[256];
    size_t l = strlen(name);
    if (l >= sizeof(lower)) return NULL;
    for (size_t i = 0; i <= l; i++) lower[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    if (!good_name(lower) || !strcmp(lower, "localhost")) return NULL;
    mx_lock(&t->mx);
    for (int i = 0; i < t->nleaves; i++)
        if (!strcmp(t->leaves[i].name, lower)) { SSL_CTX* c = t->leaves[i].ctx; mx_unlock(&t->mx); return c; }
    SSL_CTX* c = NULL;
    if (t->nleaves < LEAF_CACHE) {
        char san[600];
        snprintf(san, sizeof(san), "DNS:%s", lower);
        X509* x = make_cert(t->leaf_key, lower, t->ca, t->ca_key, 397, false, san);
        if (x) {
            c = new_ctx(t, x);
            O.X509_free(x);                       // il contesto ne tiene una copia
            if (c) {
                snprintf(t->leaves[t->nleaves].name, sizeof(t->leaves[0].name), "%s", lower);
                t->leaves[t->nleaves].ctx = c;
                t->nleaves++;
            }
        }
    }
    mx_unlock(&t->mx);
    return c;
}

static int sni_cb(SSL* s, int* ad, void* arg) {
    (void)ad;
    TlsCtx* t = arg;
    const char* name = O.SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
    if (name) {
        SSL_CTX* c = ctx_for(t, name);
        if (c) O.SSL_set_SSL_CTX(s, c);
    }
    return SSL_TLSEXT_ERR_OK;
}

TlsCtx* tls_create(const char* cert_dir, char* err, size_t errsz) {
    if (!openssl_load(err, errsz)) return NULL;
    TlsCtx* t = calloc(1, sizeof(TlsCtx));
    if (!t) { snprintf(err, errsz, "out of memory"); return NULL; }
    mx_init(&t->mx);
    snprintf(t->dir, sizeof(t->dir), "%s", cert_dir && cert_dir[0] ? cert_dir : "certs");
    struct stat st;
    if (stat(t->dir, &st) != 0) mkdir_p(t->dir);
    char kp[1100], lp[1100];
    snprintf(t->ca_path, sizeof(t->ca_path), "%s/ca.crt", t->dir);
    snprintf(kp, sizeof(kp), "%s/ca.key", t->dir);
    snprintf(lp, sizeof(lp), "%s/sites.key", t->dir);
    t->ca = read_cert(t->ca_path);
    t->ca_key = read_key(kp);
    if (!t->ca || !t->ca_key) {                   // prima volta: nuova CA (10 anni)
        if (t->ca) O.X509_free(t->ca);
        if (t->ca_key) O.EVP_PKEY_free(t->ca_key);
        t->ca_key = O.EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
        char cn[128], host[64] = "";
#ifdef _WIN32
        DWORD hl = sizeof(host);
        if (!GetComputerNameA(host, &hl)) host[0] = '\0';
#else
        if (gethostname(host, sizeof(host)) != 0) host[0] = '\0';
        host[sizeof(host) - 1] = '\0';
#endif
        snprintf(cn, sizeof(cn), "SegVault local CA%s%s", host[0] ? " " : "", host);
        t->ca = t->ca_key ? make_cert(t->ca_key, cn, NULL, NULL, 3650, true, NULL) : NULL;
        if (!t->ca || !write_pem(kp, NULL, t->ca_key) || !write_pem(t->ca_path, t->ca, NULL)) {
            snprintf(err, errsz, "cannot create the local certificate authority in %.200s", t->dir);
            tls_free(t);
            return NULL;
        }
    }
    t->leaf_key = read_key(lp);
    if (!t->leaf_key) {
        t->leaf_key = O.EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
        if (!t->leaf_key || !write_pem(lp, NULL, t->leaf_key)) { snprintf(err, errsz, "cannot create the site key in %.200s", t->dir); tls_free(t); return NULL; }
    }
    X509* x = make_cert(t->leaf_key, "localhost", t->ca, t->ca_key, 397, false, "DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1");
    t->def = x ? new_ctx(t, x) : NULL;
    if (x) O.X509_free(x);
    if (!t->def) { snprintf(err, errsz, "cannot set up TLS (certificate)"); tls_free(t); return NULL; }
    return t;
}

void tls_free(TlsCtx* t) {
    if (!t) return;
    for (int i = 0; i < t->nleaves; i++) O.SSL_CTX_free(t->leaves[i].ctx);
    if (t->def) O.SSL_CTX_free(t->def);
    if (t->ca) O.X509_free(t->ca);
    if (t->ca_key) O.EVP_PKEY_free(t->ca_key);
    if (t->leaf_key) O.EVP_PKEY_free(t->leaf_key);
    mx_destroy(&t->mx);
    free(t);
}

const char* tls_ca_path(const TlsCtx* t) { return t->ca_path; }
const char* tls_library(const TlsCtx* t) { (void)t; return O.desc; }

// ── connessione ─────────────────────────────────────────────────

static bool raw_send(tls_sock fd, const char* p, size_t n) {
    while (n > 0) {
        int w = (int)send(fd, p, (int)n, SEND_FLAGS);
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
                struct timeval tv = { 15, 0 };
                if (select((int)fd + 1, NULL, &wr, NULL, &tv) <= 0) return false;
                continue;
            }
#else
            if (w < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
                struct timeval tv = { 15, 0 };
                if (select((int)fd + 1, NULL, &wr, NULL, &tv) <= 0) return false;
                continue;
            }
#endif
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// SSL_write di tutto il buffer su un socket non bloccante
static bool ssl_send_all(SSL* s, tls_sock fd, const char* p, size_t n) {
    while (n > 0) {
        int w = O.SSL_write(s, p, n > (1u << 30) ? (1 << 30) : (int)n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        int e = O.SSL_get_error(s, w);
        if (e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ) return false;
        fd_set rd, wr;
        FD_ZERO(&rd); FD_ZERO(&wr);
        if (e == SSL_ERROR_WANT_WRITE) FD_SET(fd, &wr); else FD_SET(fd, &rd);
        struct timeval tv = { 15, 0 };
        if (select((int)fd + 1, &rd, &wr, NULL, &tv) <= 0) return false;
    }
    return true;
}

// lettura non bloccante: >0 byte, 0 = niente per ora, -1 = chiusa / errore
static int ssl_recv_some(SSL* s, char* buf, int n) {
    int r = O.SSL_read(s, buf, n);
    if (r > 0) return r;
    int e = O.SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
    return -1;
}

static tls_sock connect_inner(int port) {
    tls_sock fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return BAD_SOCK;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { sock_close(fd); return BAD_SOCK; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    return fd;
}

// HTTP/1.1 su TLS: byte decifrati <-> server interno
static void bridge_h1(SSL* s, tls_sock cfd, tls_sock ifd, atomic_int* stop) {
    char buf[65536];
    set_nonblock(cfd, true);
    for (;;) {
        if (*stop) return;
        // dati già decifrati nel buffer di OpenSSL: select non li vede
        while (O.SSL_pending(s) > 0) {
            int r = ssl_recv_some(s, buf, sizeof(buf));
            if (r < 0) return;
            if (r == 0) break;
            if (!raw_send(ifd, buf, (size_t)r)) return;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(cfd, &rd);
        FD_SET(ifd, &rd);
        struct timeval tv = { 0, 200000 };
        int r = select((int)(cfd > ifd ? cfd : ifd) + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return;
        }
        if (r == 0) continue;
        if (FD_ISSET(cfd, &rd)) {
            int g = ssl_recv_some(s, buf, sizeof(buf));
            if (g < 0) return;
            if (g > 0 && !raw_send(ifd, buf, (size_t)g)) return;
        }
        if (FD_ISSET(ifd, &rd)) {
            int g = (int)recv(ifd, buf, sizeof(buf), 0);
            if (g <= 0) return;
            if (!ssl_send_all(s, cfd, buf, (size_t)g)) return;
        }
    }
}

static int ssl_recv_some_cb(void* s, char* b, int n) { return ssl_recv_some((SSL*)s, b, n); }
static bool ssl_send_all_cb(void* s, tls_sock fd, const char* p, size_t n) { return ssl_send_all((SSL*)s, fd, p, n); }
static int ssl_pending_cb(void* s) { return O.SSL_pending((SSL*)s); }

void tls_serve(TlsCtx* t, tls_sock fd, const char* client_ip, int client_port, int inner_port,
               const char* secret, atomic_int* stop) {
    SSL* s = O.SSL_new(t->def);
    if (!s) return;
    O.SSL_set_fd(s, (int)fd);
    if (O.SSL_accept(s) != 1) { O.SSL_free(s); return; }     // handshake fallito (CA non fidata, client che chiude, ...)
    const unsigned char* proto = NULL;
    unsigned int pl = 0;
    O.SSL_get0_alpn_selected(s, &proto, &pl);
    bool h2 = pl == 2 && !memcmp(proto, "h2", 2);
    char marker[200];
    snprintf(marker, sizeof(marker), "SVTLS %s %s %d\r\n", secret, client_ip, client_port);
    if (h2) {
        set_nonblock(fd, true);
        H2Io io = { s, fd, ssl_recv_some_cb, ssl_send_all_cb, ssl_pending_cb };
        h2_serve(&io, inner_port, marker, stop);
    } else {
        tls_sock ifd = connect_inner(inner_port);
        if (ifd != BAD_SOCK) {
            if (raw_send(ifd, marker, strlen(marker))) bridge_h1(s, fd, ifd, stop);
            sock_close(ifd);
        }
    }
    set_nonblock(fd, false);
    O.SSL_shutdown(s);
    O.SSL_free(s);
}

// ── fiducia nella CA ────────────────────────────────────────────

#ifndef _WIN32
static bool have_tool(const char* name) {
    {
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
    }
}
#endif

int tls_trust_ca(const char* cert_dir, char* out, size_t outsz) {
    char err[300];
    TlsCtx* t = tls_create(cert_dir, err, sizeof(err));
    if (!t) { snprintf(out, outsz, "%s\n", err); return 1; }
    char ca[1200];
    snprintf(ca, sizeof(ca), "%s", t->ca_path);
    tls_free(t);
    for (const char* q = ca; *q; q++)
        if (*q == '"' || *q == '\'' || *q == '`' || *q == '$') { snprintf(out, outsz, "unusual characters in the path %s\n", ca); return 1; }
    size_t k = 0;
    int done = 0;
    char cmd[3000];
#define TOUT(...) do { if (k < outsz) k += (size_t)snprintf(out + k, outsz - k, __VA_ARGS__); } while (0)
    TOUT("Certificate authority: %s\n", ca);
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "certutil -user -addstore Root \"%s\" >NUL", ca);
    if (system(cmd) == 0) { TOUT("Added to the trusted roots of this Windows user (Chrome, Edge; Firefox: set security.enterprise_roots.enabled)\n"); done++; }
    else TOUT("certutil failed: run  certutil -user -addstore Root \"%s\"\n", ca);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd), "security add-trusted-cert -r trustRoot -k \"$HOME/Library/Keychains/login.keychain-db\" '%s'", ca);
    if (system(cmd) == 0) { TOUT("Added to the login keychain (Safari, Chrome)\n"); done++; }
    else TOUT("Could not add it: run  %s\n", cmd);
#else
    const char* home = getenv("HOME");
    if (have_tool("certutil") && home) {
        // Chrome / Chromium / Edge: archivio NSS dell'utente; Firefox: ogni profilo
        snprintf(cmd, sizeof(cmd), "mkdir -p \"$HOME/.pki/nssdb\" && certutil -d sql:\"$HOME/.pki/nssdb\" -A -t C,, -n 'SegVault local CA' -i '%s' 2>/dev/null", ca);
        if (system(cmd) == 0) { TOUT("Added for Chrome / Chromium (~/.pki/nssdb)\n"); done++; }
        snprintf(cmd, sizeof(cmd),
                 "n=0; for d in \"$HOME\"/.mozilla/firefox/*/ \"$HOME\"/snap/firefox/common/.mozilla/firefox/*/ \"$HOME\"/.var/app/org.mozilla.firefox/.mozilla/firefox/*/; do "
                 "[ -f \"$d/cert9.db\" ] && certutil -d sql:\"$d\" -A -t C,, -n 'SegVault local CA' -i '%s' 2>/dev/null && n=$((n+1)); done; [ $n -gt 0 ]", ca);
        if (system(cmd) == 0) { TOUT("Added to the Firefox profiles\n"); done++; }
    } else TOUT("certutil not found (package nss-tools / libnss3-tools): browsers will not trust the CA until you add it\n");
    if (getuid() == 0) {
        if (have_tool("trust")) snprintf(cmd, sizeof(cmd), "trust anchor --store '%s'", ca);
        else snprintf(cmd, sizeof(cmd), "cp '%s' /usr/local/share/ca-certificates/segvault-local-ca.crt && update-ca-certificates", ca);
        if (system(cmd) == 0) { TOUT("Added to the system trust store (curl, PHP, Node, Python)\n"); done++; }
    } else
        TOUT("System store (curl, PHP, Node, Python): run as root  %s '%s'\n",
            have_tool("trust") ? "trust anchor --store" : "cp ... /usr/local/share/ca-certificates/ && update-ca-certificates", ca);
#endif
#undef TOUT
    return done ? 0 : 1;
}
