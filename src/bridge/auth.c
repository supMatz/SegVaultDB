#ifdef _WIN32
  #define _CRT_RAND_S                 // rand_s(): numeri casuali del sistema
#endif
#include "auth.h"
#include "db_api.h"
#include "../query/lexer.h"
#include "../query/parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifndef _WIN32
  #include <fcntl.h>
  #include <unistd.h>
#endif

// ── SHA-256 ─────────────────────────────────────────────────────

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void auth_sha256(const void* data, size_t n, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t* p = data;
    size_t left = n;
    while (left >= 64) { sha_block(h, p); p += 64; left -= 64; }
    uint8_t last[128] = {0};
    memcpy(last, p, left);
    last[left] = 0x80;
    size_t blocks = left + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < blocks; i++) sha_block(h, last + 64 * i);
    for (int i = 0; i < 8; i++) { out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16); out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i]; }
}

static void hex(const uint8_t* b, size_t n, char* out) {
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
    out[n * 2] = '\0';
}

#define HASH_ROUNDS 20000

// hash della password: SHA-256(sale + password), poi ripetuto (rallenta i tentativi a forza bruta)
static void password_hash(const char* salt_hex, const char* pw, char out[65]) {
    size_t sl = strlen(salt_hex), pl = strlen(pw);
    uint8_t* buf = malloc(sl + pl + 32);
    uint8_t d[32];
    if (!buf) { out[0] = '\0'; return; }
    memcpy(buf, salt_hex, sl);
    memcpy(buf + sl, pw, pl);
    auth_sha256(buf, sl + pl, d);
    for (int i = 1; i < HASH_ROUNDS; i++) {
        memcpy(buf, d, 32);
        memcpy(buf + 32, salt_hex, sl);
        auth_sha256(buf, 32 + sl, d);
    }
    free(buf);
    hex(d, 32, out);
}

static void random_salt(char out[33]) {
    uint8_t r[16];
    bool ok = false;
#ifdef _WIN32
    ok = true;
    for (int i = 0; i < 16 && ok; i += 4) {
        unsigned int v;
        ok = rand_s(&v) == 0;
        memcpy(r + i, &v, 4);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { ok = read(fd, r, sizeof(r)) == (ssize_t)sizeof(r); close(fd); }
#endif
    if (!ok) { uint64_t t = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)out; for (int i = 0; i < 16; i++) { t = t * 6364136223846793005ULL + 1; r[i] = (uint8_t)(t >> 56); } }
    hex(r, 16, out);
}

// ── archivio degli utenti ───────────────────────────────────────

typedef struct { char db[64], table[64]; unsigned privs; } Grant;
typedef struct { char name[64], salt[33], hash[65]; bool admin; Grant* g; int ng; } User;

static User* g_users;
static int   g_nusers;
static char  g_path[600];

static User* find_user(const char* name) {
    for (int i = 0; name && i < g_nusers; i++) if (!strcasecmp(g_users[i].name, name)) return &g_users[i];
    return NULL;
}

static bool save_users(void) {
    if (!g_path[0]) return false;
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# SegVault users: do not edit by hand while the database is running\n");
    for (int i = 0; i < g_nusers; i++) {
        User* u = &g_users[i];
        fprintf(f, "user %s %s %s %d\n", u->name, u->salt, u->hash, u->admin ? 1 : 0);
        for (int k = 0; k < u->ng; k++) fprintf(f, "grant %s %s %s %u\n", u->name, u->g[k].db, u->g[k].table, u->g[k].privs);
    }
    bool ok = fclose(f) == 0;
#ifdef _WIN32
    if (ok) { remove(g_path); ok = rename(tmp, g_path) == 0; }
#else
    if (ok) ok = rename(tmp, g_path) == 0;
#endif
    return ok;
}

static User* add_user(const char* name) {
    User* nu = realloc(g_users, sizeof(User) * (size_t)(g_nusers + 1));
    if (!nu) return NULL;
    g_users = nu;
    User* u = &g_users[g_nusers++];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%.63s", name);
    return u;
}

void auth_init(const char* data_dir) {
    auth_shutdown();
    snprintf(g_path, sizeof(g_path), "%.570s/users.dat", data_dir ? data_dir : ".");
    FILE* f = fopen(g_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char a[64], b[64], c[80], d[80];
        int adm;
        unsigned pr;
        if (sscanf(line, "user %63s %63s %79s %d", a, b, c, &adm) == 4) {
            User* u = find_user(a) ? NULL : add_user(a);
            if (!u) continue;
            snprintf(u->salt, sizeof(u->salt), "%.32s", b);
            snprintf(u->hash, sizeof(u->hash), "%.64s", c);
            u->admin = adm != 0;
        } else if (sscanf(line, "grant %63s %63s %79s %u", a, b, c, &pr) == 4) {
            User* u = find_user(a);
            Grant* ng = u ? realloc(u->g, sizeof(Grant) * (size_t)(u->ng + 1)) : NULL;
            if (!ng) continue;
            u->g = ng;
            snprintf(ng[u->ng].db, 64, "%.63s", b);
            snprintf(ng[u->ng].table, 64, "%.63s", c);
            ng[u->ng].privs = pr & AUTH_ALL;
            u->ng++;
        }
        (void)d;
    }
    fclose(f);
}

void auth_shutdown(void) {
    for (int i = 0; i < g_nusers; i++) free(g_users[i].g);
    free(g_users);
    g_users = NULL;
    g_nusers = 0;
}

bool auth_user_exists(const char* user) { return find_user(user) != NULL; }
bool auth_is_admin(const char* user) { User* u = find_user(user); return u && u->admin; }
int  auth_user_count(void) { return g_nusers; }
const char* auth_user_name(int i) { return i >= 0 && i < g_nusers ? g_users[i].name : NULL; }

bool auth_login(const char* user, const char* password) {
    User* u = find_user(user);
    if (!u || !password) return false;
    char h[65];
    password_hash(u->salt, password, h);
    // confronto a tempo costante
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) diff |= (unsigned)(h[i] ^ u->hash[i]);
    return diff == 0 && h[0];
}

unsigned auth_privs(const char* user, const char* db, const char* table) {
    if (!user || !user[0]) return AUTH_ALL;
    User* u = find_user(user);
    if (!u) return 0;
    if (u->admin) return AUTH_ALL;
    unsigned p = 0;
    for (int k = 0; k < u->ng; k++) {
        const Grant* g = &u->g[k];
        bool dbok = !strcmp(g->db, "*") || (db && !strcasecmp(g->db, db));
        bool tok = !strcmp(g->table, "*") || (table && strcmp(table, "*") && !strcasecmp(g->table, table));
        if (dbok && tok) p |= g->privs;
    }
    return p;
}

unsigned auth_grant_on(const char* user, const char* db, const char* table) {
    User* u = find_user(user);
    for (int k = 0; u && k < u->ng; k++)
        if (!strcasecmp(u->g[k].db, db) && !strcasecmp(u->g[k].table, table)) return u->g[k].privs;
    return 0;
}

int auth_grant_count(const char* user) { User* u = find_user(user); return u ? u->ng : 0; }

bool auth_grant_at(const char* user, int i, char* db, char* table, unsigned* privs) {
    User* u = find_user(user);
    if (!u || i < 0 || i >= u->ng) return false;
    snprintf(db, 64, "%s", u->g[i].db);
    snprintf(table, 64, "%s", u->g[i].table);
    *privs = u->g[i].privs;
    return true;
}

// qualche permesso nel database (anche solo su una tabella)?
static bool any_in_db(const char* user, const char* db) {
    if (!user || !user[0]) return true;
    User* u = find_user(user);
    if (!u) return false;
    if (u->admin) return true;
    for (int k = 0; k < u->ng; k++)
        if (u->g[k].privs && (!strcmp(u->g[k].db, "*") || !strcasecmp(u->g[k].db, db))) return true;
    return false;
}

bool auth_can_see_db(const char* user, const char* db) { return any_in_db(user, db); }

static const char* const PRIV_NAMES[7] = { "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER" };

const char* auth_priv_name(unsigned bit) {
    for (int i = 0; i < 7; i++) if (bit == (1u << i)) return PRIV_NAMES[i];
    return "?";
}

static void privs_text(unsigned p, char* out, size_t n) {
    if ((p & AUTH_ALL) == AUTH_ALL) { snprintf(out, n, "ALL PRIVILEGES"); return; }
    size_t l = 0;
    out[0] = '\0';
    for (int i = 0; i < 7; i++)
        if (p & (1u << i)) l += (size_t)snprintf(out + l, l < n ? n - l : 0, "%s%s", l ? ", " : "", PRIV_NAMES[i]);
    if (!l) snprintf(out, n, "USAGE");
}

// ── risultati ───────────────────────────────────────────────────

static QueryResult* res_new(int ncols, const char* const* names) {
    QueryResult* r = calloc(1, sizeof(QueryResult));
    if (!r) return NULL;
    r->success = true;
    if (ncols) {
        r->col_names = calloc((size_t)ncols, sizeof(char*));
        r->col_types = calloc((size_t)ncols, sizeof(char*));
        r->num_cols = ncols;
        for (int i = 0; i < ncols && r->col_names && r->col_types; i++) { r->col_names[i] = strdup(names[i]); r->col_types[i] = strdup("TEXT"); }
    }
    return r;
}

static void res_row(QueryResult* r, const char* const* v) {
    Row* rows = realloc(r->rows, sizeof(Row) * (size_t)(r->num_rows + 1));
    if (!rows) return;
    r->rows = rows;
    if (db_row_set(&r->rows[r->num_rows], r->num_cols, v, NULL)) r->num_rows++;
}

static QueryResult* res_err(const char* fmt, const char* a) {
    QueryResult* r = res_new(0, NULL);
    if (!r) return NULL;
    r->success = false;
    snprintf(r->error, sizeof(r->error), fmt, a ? a : "");
    return r;
}

// ── istruzioni di gestione ──────────────────────────────────────

typedef struct { const TokenList* t; int i; } Cur;

static const Token* tk(Cur* c) { return c->i < c->t->count ? &c->t->tokens[c->i] : NULL; }
static bool at_end(Cur* c) { const Token* t = tk(c); return !t || t->type == TOK_EOF || t->type == TOK_SEMICOLON; }
static bool word(Cur* c, const char* w) {
    const Token* t = tk(c);
    if (t && t->type != TOK_STRING && t->text && !strcasecmp(t->text, w)) { c->i++; return true; }
    return false;
}
static bool name(Cur* c, char* out, size_t n) {          // nome: identificatore o stringa
    const Token* t = tk(c);
    if (!t || at_end(c) || !t->text || !t->text[0]) return false;
    if (t->type != TOK_IDENTIFIER && t->type != TOK_STRING && !isalpha((unsigned char)t->text[0])) return false;
    snprintf(out, n, "%.63s", t->text);
    c->i++;
    return true;
}
static bool valid_user_name(const char* s) {
    if (!s[0] || strlen(s) > 63) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') return false;
    return true;
}

// lista di permessi: ALL [PRIVILEGES] | SELECT, INSERT, ...
static bool priv_list(Cur* c, unsigned* out) {
    *out = 0;
    if (word(c, "ALL")) { word(c, "PRIVILEGES"); *out = AUTH_ALL; return true; }
    do {
        bool found = false;
        for (int i = 0; i < 7 && !found; i++) if (word(c, PRIV_NAMES[i])) { *out |= 1u << i; found = true; }
        if (!found) return false;
    } while (tk(c) && tk(c)->type == TOK_COMMA && (c->i++, true));
    return true;
}

// destinazione: *.* | db.* | db.tabella | tabella (database corrente)
static bool target(Cur* c, const char* cur_db, char* db, char* table) {
    const Token* t = tk(c);
    if (!t) return false;
    char first[64];
    if (t->type == TOK_STAR) { snprintf(first, sizeof(first), "*"); c->i++; }
    else if (!name(c, first, sizeof(first))) return false;
    t = tk(c);
    if (t && t->type == TOK_DOT) {
        c->i++;
        t = tk(c);
        if (t && t->type == TOK_STAR) { snprintf(table, 64, "*"); c->i++; }
        else if (!name(c, table, 64)) return false;
        snprintf(db, 64, "%s", first);
        return !(db[0] == '*' && table[0] != '*');
    }
    if (!strcmp(first, "*")) {            // * = tutte le tabelle del database corrente
        if (!cur_db || !cur_db[0]) return false;
        snprintf(db, 64, "%s", cur_db);
        snprintf(table, 64, "*");
        return true;
    }
    if (!cur_db || !cur_db[0]) return false;
    snprintf(db, 64, "%s", cur_db);
    snprintf(table, 64, "%s", first);
    return true;
}

static void set_grant(User* u, const char* db, const char* table, unsigned add, unsigned remove) {
    for (int k = 0; k < u->ng; k++)
        if (!strcasecmp(u->g[k].db, db) && !strcasecmp(u->g[k].table, table)) {
            u->g[k].privs = (u->g[k].privs | add) & ~remove;
            if (!u->g[k].privs) { u->g[k] = u->g[u->ng - 1]; u->ng--; }
            return;
        }
    if (!add) return;
    Grant* ng = realloc(u->g, sizeof(Grant) * (size_t)(u->ng + 1));
    if (!ng) return;
    u->g = ng;
    snprintf(ng[u->ng].db, 64, "%s", db);
    snprintf(ng[u->ng].table, 64, "%s", table);
    ng[u->ng].privs = add;
    u->ng++;
}

static void grant_rows(QueryResult* r, const User* u) {
    char line[400], p[120];
    if (u->admin) {
        snprintf(line, sizeof(line), "GRANT ALL PRIVILEGES ON *.* TO %s WITH GRANT OPTION", u->name);
        const char* v[1] = { line };
        res_row(r, v);
    }
    for (int k = 0; k < u->ng; k++) {
        privs_text(u->g[k].privs, p, sizeof(p));
        snprintf(line, sizeof(line), "GRANT %s ON %s.%s TO %s", p, u->g[k].db, u->g[k].table, u->name);
        const char* v[1] = { line };
        res_row(r, v);
    }
    if (!u->admin && !u->ng) {
        snprintf(line, sizeof(line), "GRANT USAGE ON *.* TO %s", u->name);
        const char* v[1] = { line };
        res_row(r, v);
    }
}

bool auth_statement(const void* tokens, const char* session_user, const char* cur_db, void** out) {
    const TokenList* tl = tokens;
    Cur c = { tl, 0 };
    *out = NULL;
    if (!tl || tl->count == 0) return false;
    bool root = !session_user || !session_user[0];
    bool admin = root || auth_is_admin(session_user);
    char u[64], pw[256], db[64], table[64];

    // SHOW USERS | SHOW GRANTS [FOR utente]
    if (word(&c, "SHOW")) {
        if (word(&c, "USERS")) {
            if (!admin) { *out = res_err("Access denied: only administrators can list users%s", ""); return true; }
            static const char* const N[3] = { "User", "Administrator", "Grants" };
            QueryResult* r = res_new(3, N);
            for (int i = 0; r && i < g_nusers; i++) {
                char n[16];
                snprintf(n, sizeof(n), "%d", g_users[i].ng);
                const char* v[3] = { g_users[i].name, g_users[i].admin ? "yes" : "no", n };
                res_row(r, v);
            }
            *out = r;
            return true;
        }
        if (word(&c, "GRANTS")) {
            const char* who = session_user;
            if (word(&c, "FOR")) { if (!name(&c, u, sizeof(u))) { *out = res_err("Expected a user name after FOR%s", ""); return true; } who = u; }
            if (!who || !who[0]) { *out = res_err("SHOW GRANTS needs FOR user (the desktop session is the administrator)%s", ""); return true; }
            if (!admin && strcasecmp(who, session_user)) { *out = res_err("Access denied: you can only see your own grants%s", ""); return true; }
            User* us = find_user(who);
            if (!us) { *out = res_err("User '%.63s' does not exist", who); return true; }
            static const char* const N[1] = { "Grants" };
            QueryResult* r = res_new(1, N);
            if (r) grant_rows(r, us);
            *out = r;
            return true;
        }
        return false;
    }

    bool is_create = false, is_alter = false, is_drop = false;
    c.i = 0;
    if (word(&c, "CREATE")) is_create = true;
    else if (word(&c, "ALTER")) is_alter = true;
    else if (word(&c, "DROP")) is_drop = true;
    if (is_create || is_alter || is_drop) {
        if (!word(&c, "USER")) return false;
        if (!admin) { *out = res_err("Access denied: only administrators can manage users%s", ""); return true; }
        bool if_exists = false, if_not_exists = false;
        if (word(&c, "IF")) {
            if (word(&c, "NOT")) if_not_exists = true;
            else if_exists = true;
            word(&c, "EXISTS");
        }
        if (!name(&c, u, sizeof(u)) || !valid_user_name(u)) { *out = res_err("Expected a user name (letters, digits, _ . -)%s", ""); return true; }
        User* us = find_user(u);
        if (is_drop) {
            if (!at_end(&c)) { *out = res_err("Unexpected text after DROP USER %.63s", u); return true; }
            if (!us) { *out = if_exists ? res_new(0, NULL) : res_err("User '%.63s' does not exist", u); return true; }
            if (session_user && !strcasecmp(session_user, u)) { *out = res_err("You cannot drop your own user%s", ""); return true; }
            free(us->g);
            *us = g_users[--g_nusers];
            save_users();
            *out = res_new(0, NULL);
            return true;
        }
        if (!word(&c, "IDENTIFIED") || !word(&c, "BY") || !tk(&c) || tk(&c)->type != TOK_STRING) {
            *out = res_err("Expected IDENTIFIED BY 'password'%s", ""); return true;
        }
        snprintf(pw, sizeof(pw), "%s", tk(&c)->text);
        c.i++;
        if (!at_end(&c)) { *out = res_err("Unexpected text after the password%s", ""); return true; }
        if (strlen(pw) < 4) { *out = res_err("The password needs at least 4 characters%s", ""); return true; }
        if (is_create && us) { *out = if_not_exists ? res_new(0, NULL) : res_err("User '%.63s' already exists", u); return true; }
        if (is_alter && !us) { *out = res_err("User '%.63s' does not exist", u); return true; }
        if (!us) us = add_user(u);
        if (!us) { *out = res_err("Out of memory%s", ""); return true; }
        random_salt(us->salt);
        password_hash(us->salt, pw, us->hash);
        memset(pw, 0, sizeof(pw));
        *out = save_users() ? res_new(0, NULL) : res_err("Cannot write the users file%s", "");
        return true;
    }

    // GRANT privilegi ON destinazione TO utente [WITH GRANT OPTION]
    // REVOKE privilegi ON destinazione FROM utente  |  REVOKE GRANT OPTION ON *.* FROM utente
    c.i = 0;
    bool grant = word(&c, "GRANT"), revoke = !grant && word(&c, "REVOKE");
    if (!grant && !revoke) return false;
    if (!admin) { *out = res_err("Access denied: only administrators can grant or revoke%s", ""); return true; }
    unsigned privs = 0;
    bool option_only = false;
    if (revoke && word(&c, "GRANT")) { if (!word(&c, "OPTION")) { *out = res_err("Expected GRANT OPTION%s", ""); return true; } option_only = true; }
    else if (!priv_list(&c, &privs)) { *out = res_err("Expected privileges: ALL or SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER%s", ""); return true; }
    if (!word(&c, "ON") || !target(&c, cur_db, db, table)) { *out = res_err("Expected ON *.*, ON db.*, ON db.table or ON table%s", ""); return true; }
    if (!word(&c, grant ? "TO" : "FROM") || !name(&c, u, sizeof(u))) { *out = res_err(grant ? "Expected TO user%s" : "Expected FROM user%s", ""); return true; }
    User* us = find_user(u);
    if (!us) { *out = res_err("User '%.63s' does not exist: CREATE USER first", u); return true; }
    bool with_option = false;
    if (grant && word(&c, "WITH")) {
        if (!word(&c, "GRANT") || !word(&c, "OPTION")) { *out = res_err("Expected WITH GRANT OPTION%s", ""); return true; }
        with_option = true;
    }
    if (!at_end(&c)) { *out = res_err("Unexpected text at the end of the statement%s", ""); return true; }
    if (with_option || option_only) {
        if (strcmp(db, "*") || strcmp(table, "*")) { *out = res_err("GRANT OPTION applies to *.* only%s", ""); return true; }
        if (option_only && session_user && !strcasecmp(session_user, u)) { *out = res_err("You cannot remove your own administrator rights%s", ""); return true; }
        us->admin = with_option;
    }
    if (privs) set_grant(us, db, table, grant ? privs : 0, revoke ? privs : 0);
    *out = save_users() ? res_new(0, NULL) : res_err("Cannot write the users file%s", "");
    return true;
}

// ── controllo degli statement ───────────────────────────────────

typedef struct {
    const char* user;
    const char* db;
    char  ctes[16][64];
    int   nctes;
    char* err;
    size_t errn;
    bool  ok;
} Chk;

static bool is_cte(Chk* k, const char* t) {
    for (int i = 0; i < k->nctes; i++) if (!strcasecmp(k->ctes[i], t)) return true;
    return false;
}

static void need(Chk* k, unsigned priv, const char* table) {
    if (!k->ok || !table || !table[0] || is_cte(k, table)) return;
    if (!k->db || !k->db[0]) {
        if (auth_privs(k->user, "*", "*") & priv) return;
        snprintf(k->err, k->errn, "No database selected");
        k->ok = false;
        return;
    }
    if (auth_privs(k->user, k->db, table) & priv) return;
    snprintf(k->err, k->errn, "Access denied for user '%.63s': %s on %.63s.%.63s", k->user, auth_priv_name(priv), k->db,
             strcmp(table, "*") ? table : "*");
    k->ok = false;
}

static void walk_node(Chk* k, const ASTNode* n);

static void walk_expr(Chk* k, const ASTExpr* e) {
    if (!e || !k->ok) return;
    if (e->type == EXPR_SUBQUERY && e->subq) walk_node(k, e->subq->node);
    walk_expr(k, e->left);
    walk_expr(k, e->right);
    for (int i = 0; i < e->nargs; i++) walk_expr(k, e->args[i]);
}

static void walk_select(Chk* k, const ASTNode* n) {
    for (const ASTNode* b = n; b && k->ok; b = b->select_stmt.set_next) {
        for (int i = 0; i < b->select_stmt.num_ctes && k->nctes < 16; i++)
            snprintf(k->ctes[k->nctes++], 64, "%s", b->select_stmt.ctes[i].name);
        for (int i = 0; i < b->select_stmt.num_ctes; i++) walk_node(k, b->select_stmt.ctes[i].node);
        if (b->select_stmt.from_sub) walk_node(k, b->select_stmt.from_sub);
        else need(k, AUTH_SELECT, b->select_stmt.table_name);
        for (int j = 0; j < b->select_stmt.num_joins; j++) {
            if (b->select_stmt.joins[j].sub) walk_node(k, b->select_stmt.joins[j].sub);
            else need(k, AUTH_SELECT, b->select_stmt.joins[j].table_name);
            walk_expr(k, b->select_stmt.joins[j].condition);
        }
        for (int i = 0; i < b->select_stmt.num_cols; i++) walk_expr(k, b->select_stmt.columns[i]);
        walk_expr(k, b->select_stmt.where);
        walk_expr(k, b->select_stmt.having);
        for (int i = 0; i < b->select_stmt.num_group; i++) walk_expr(k, b->select_stmt.group_exprs[i]);
        for (int i = 0; i < b->select_stmt.num_order; i++) walk_expr(k, b->select_stmt.order_exprs[i]);
    }
}

static void walk_node(Chk* k, const ASTNode* n) {
    if (!n || !k->ok) return;
    switch (n->type) {
        case NODE_SELECT: walk_select(k, n); break;
        case NODE_INSERT:
            need(k, AUTH_INSERT, n->insert_stmt.table_name);
            for (int i = 0; i < n->insert_stmt.num_rows * n->insert_stmt.num_values; i++) walk_expr(k, n->insert_stmt.values[i]);
            walk_node(k, n->insert_stmt.select);
            break;
        case NODE_UPDATE:
            need(k, AUTH_UPDATE, n->update_stmt.table_name);
            for (int i = 0; i < n->update_stmt.num_sets; i++) walk_expr(k, n->update_stmt.set_exprs[i]);
            walk_expr(k, n->update_stmt.where);
            break;
        case NODE_DELETE:
            need(k, AUTH_DELETE, n->delete_stmt.table_name);
            walk_expr(k, n->delete_stmt.where);
            break;
        case NODE_EXPLAIN: walk_node(k, (const ASTNode*)n->explain.stmt); break;
        default: break;
    }
}

bool auth_check(const char* user, const ASTNode* stmt, const char* cur_db, char* err, size_t errn) {
    if (!user || !user[0] || !stmt) return true;
    User* u = find_user(user);
    if (!u) { snprintf(err, errn, "User '%.63s' no longer exists", user); return false; }
    if (u->admin) return true;
    Chk k = { user, cur_db, {{0}}, 0, err, errn, true };
    switch (stmt->type) {
        case NODE_SELECT: case NODE_INSERT: case NODE_UPDATE: case NODE_DELETE: case NODE_EXPLAIN:
            walk_node(&k, stmt);
            break;
        case NODE_BEGIN: case NODE_COMMIT: case NODE_ROLLBACK: case NODE_SAVEPOINT: case NODE_ROLLBACK_TO:
            break;
        case NODE_USE:
            if (!any_in_db(user, stmt->use_stmt.db_name)) {
                snprintf(err, errn, "Access denied for user '%.63s' to database '%.63s'", user, stmt->use_stmt.db_name);
                k.ok = false;
            }
            break;
        case NODE_SHOW: case NODE_DESCRIBE:
            if (cur_db && cur_db[0] && !any_in_db(user, cur_db)) {
                snprintf(err, errn, "Access denied for user '%.63s' to database '%.63s'", user, cur_db);
                k.ok = false;
            }
            break;
        case NODE_CREATE_DB: {
            const char* d = stmt->create_db.db_name;
            if (!(auth_privs(user, d, "*") & AUTH_CREATE)) { snprintf(err, errn, "Access denied for user '%.63s': CREATE on %.63s.*", user, d); k.ok = false; }
            break;
        }
        case NODE_DROP_DB: {
            const char* d = stmt->drop_db.db_name;
            if (!(auth_privs(user, d, "*") & AUTH_DROP)) { snprintf(err, errn, "Access denied for user '%.63s': DROP on %.63s.*", user, d); k.ok = false; }
            break;
        }
        case NODE_CREATE_TABLE: need(&k, AUTH_CREATE, stmt->create_table.table_name); break;
        case NODE_CREATE_VIEW:  need(&k, AUTH_CREATE, stmt->create_view.view_name); break;
        case NODE_DROP_TABLE:   need(&k, AUTH_DROP, stmt->drop_table.table_name); break;
        case NODE_DROP_VIEW:    need(&k, AUTH_DROP, stmt->drop_view.view_name); break;
        case NODE_TRUNCATE:     need(&k, AUTH_DROP, stmt->delete_stmt.table_name); break;
        case NODE_ALTER_TABLE:  need(&k, AUTH_ALTER, stmt->alter_table.table_name); break;
        case NODE_CREATE_INDEX: need(&k, AUTH_ALTER, stmt->create_index.table_name); break;
        case NODE_DROP_INDEX:   need(&k, AUTH_ALTER, stmt->drop_index.table_name); break;
        case NODE_CREATE_TRIGGER: need(&k, AUTH_ALTER, stmt->create_trigger.table_name); break;
        case NODE_DROP_TRIGGER: need(&k, AUTH_ALTER, "*"); break;
        case NODE_CREATE_PROCEDURE: need(&k, AUTH_CREATE, "*"); break;
        case NODE_DROP_PROCEDURE: need(&k, AUTH_DROP, "*"); break;
        case NODE_CALL:          // le istruzioni della procedura si controllano una per una (auth_hook)
            if (!cur_db || !cur_db[0] || !any_in_db(user, cur_db)) {
                snprintf(err, errn, "Access denied for user '%.63s' to database '%.63s'", user, cur_db ? cur_db : "");
                k.ok = false;
            }
            break;
        default:
            snprintf(err, errn, "Access denied for user '%.63s': this statement needs an administrator", user);
            k.ok = false;
            break;
    }
    return k.ok;
}
