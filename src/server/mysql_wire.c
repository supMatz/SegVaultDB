#include "mysql_wire.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── buffer dei pacchetti ─────────────────────────────────────────

static void grow(MwOut* o, size_t add) {
    if (o->oom || o->n + add <= o->cap) return;
    size_t nc = o->cap ? o->cap * 2 : 1024;
    while (nc < o->n + add) nc *= 2;
    uint8_t* q = realloc(o->p, nc);
    if (!q) { o->oom = true; return; }
    o->p = q;
    o->cap = nc;
}

void mw_bytes(MwOut* o, const void* d, size_t n) {
    grow(o, n);
    if (o->oom || !n) return;
    memcpy(o->p + o->n, d, n);
    o->n += n;
}

void mw_u8(MwOut* o, unsigned v) { uint8_t b = (uint8_t)v; mw_bytes(o, &b, 1); }
void mw_u16(MwOut* o, unsigned v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; mw_bytes(o, b, 2); }
void mw_u32(MwOut* o, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; mw_bytes(o, b, 4); }

void mw_lenenc(MwOut* o, uint64_t v) {
    if (v < 251) { mw_u8(o, (unsigned)v); return; }
    if (v < 65536) { mw_u8(o, 0xfc); mw_u16(o, (unsigned)v); return; }
    if (v < 16777216) { mw_u8(o, 0xfd); uint8_t b[3] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16) }; mw_bytes(o, b, 3); return; }
    mw_u8(o, 0xfe);
    mw_u32(o, (uint32_t)v);
    mw_u32(o, (uint32_t)(v >> 32));
}

void mw_lenstr(MwOut* o, const char* s, size_t n) { mw_lenenc(o, n); mw_bytes(o, s, n); }
static void mw_lenz(MwOut* o, const char* s) { mw_lenstr(o, s ? s : "", s ? strlen(s) : 0); }

void mw_begin(MwOut* o) {
    o->start = o->n;
    uint8_t h[4] = { 0, 0, 0, o->seq++ };
    mw_bytes(o, h, 4);
}

// un pacchetto più lungo di 16 MB - 1 andrebbe spezzato: qui non succede (righe e testi più piccoli)
void mw_end(MwOut* o) {
    if (o->oom) return;
    size_t len = o->n - o->start - 4;
    if (len > 0xffffff) len = 0xffffff;
    o->p[o->start] = (uint8_t)len;
    o->p[o->start + 1] = (uint8_t)(len >> 8);
    o->p[o->start + 2] = (uint8_t)(len >> 16);
}

// ── pacchetti del server ─────────────────────────────────────────

void mw_handshake(MwOut* o, uint32_t conn_id, const uint8_t scramble[20]) {
    mw_begin(o);
    mw_u8(o, 10);                                          // protocollo 10
    const char* ver = "8.0.35-SegVault";
    mw_bytes(o, ver, strlen(ver) + 1);
    mw_u32(o, conn_id);
    mw_bytes(o, scramble, 8);
    mw_u8(o, 0);
    mw_u16(o, MW_SERVER_CAPS & 0xffff);
    mw_u8(o, 45);                                          // utf8mb4_general_ci
    mw_u16(o, MW_STATUS_AUTOCOMMIT);
    mw_u16(o, MW_SERVER_CAPS >> 16);
    mw_u8(o, 21);                                          // lunghezza dei dati di autenticazione
    uint8_t zero[10] = {0};
    mw_bytes(o, zero, 10);
    mw_bytes(o, scramble + 8, 12);
    mw_u8(o, 0);
    const char* plugin = "mysql_native_password";
    mw_bytes(o, plugin, strlen(plugin) + 1);
    mw_end(o);
}

void mw_auth_switch(MwOut* o, const uint8_t scramble[20]) {
    mw_begin(o);
    mw_u8(o, 0xfe);
    const char* plugin = "mysql_native_password";
    mw_bytes(o, plugin, strlen(plugin) + 1);
    mw_bytes(o, scramble, 20);
    mw_u8(o, 0);
    mw_end(o);
}

void mw_ok(MwOut* o, uint64_t affected, uint64_t insert_id, unsigned status, const char* info) {
    mw_begin(o);
    mw_u8(o, 0);
    mw_lenenc(o, affected);
    mw_lenenc(o, insert_id);
    mw_u16(o, status);
    mw_u16(o, 0);                                          // avvisi
    if (info && info[0]) mw_bytes(o, info, strlen(info));
    mw_end(o);
}

void mw_err(MwOut* o, unsigned code, const char* sqlstate, const char* msg) {
    mw_begin(o);
    mw_u8(o, 0xff);
    mw_u16(o, code);
    mw_u8(o, '#');
    mw_bytes(o, sqlstate && strlen(sqlstate) == 5 ? sqlstate : "HY000", 5);
    size_t ml = msg ? strlen(msg) : 0;
    mw_bytes(o, msg, ml > 1000 ? 1000 : ml);
    mw_end(o);
}

void mw_eof(MwOut* o, unsigned status) {
    mw_begin(o);
    mw_u8(o, 0xfe);
    mw_u16(o, 0);
    mw_u16(o, status);
    mw_end(o);
}

// tipo SegVault -> tipo MySQL, flag, lunghezza, decimali
enum { MT_TINY = 1, MT_LONG = 3, MT_DOUBLE = 5, MT_LONGLONG = 8, MT_DATE = 10, MT_DATETIME = 12,
       MT_NEWDECIMAL = 0xf6, MT_BLOB = 0xfc, MT_VAR_STRING = 0xfd };

static void map_type(const char* t, unsigned* type, unsigned* flags, unsigned* charset, uint32_t* len, unsigned* dec) {
    *flags = 0; *charset = 45; *len = 65535; *dec = 0; *type = MT_VAR_STRING;
    if (!t) return;
    if (!strncasecmp(t, "BIGINT", 6))              { *type = MT_LONGLONG; *len = 20; }
    else if (!strncasecmp(t, "INT", 3))            { *type = MT_LONG; *len = 11; }
    else if (!strncasecmp(t, "BOOL", 4))           { *type = MT_TINY; *len = 1; }
    else if (!strncasecmp(t, "FLOAT", 5) || !strncasecmp(t, "DOUBLE", 6) || !strncasecmp(t, "REAL", 4)) { *type = MT_DOUBLE; *len = 22; *dec = 31; }
    else if (!strncasecmp(t, "DECIMAL", 7) || !strncasecmp(t, "NUMERIC", 7)) { *type = MT_NEWDECIMAL; *len = 66; *dec = 30; }
    else if (!strncasecmp(t, "DATETIME", 8) || !strncasecmp(t, "TIMESTAMP", 9)) { *type = MT_DATETIME; *len = 19; *charset = 63; }
    else if (!strncasecmp(t, "DATE", 4))           { *type = MT_DATE; *len = 10; *charset = 63; }
    else if (!strncasecmp(t, "BLOB", 4))           { *type = MT_BLOB; *flags = 128 | 16; *charset = 63; }
    else if (!strncasecmp(t, "TEXT", 4))           { *type = MT_BLOB; *flags = 16; *len = 16777215; }
    if (*type == MT_LONG || *type == MT_LONGLONG || *type == MT_TINY || *type == MT_DOUBLE || *type == MT_NEWDECIMAL) {
        *flags |= 32768;                                   // NUM_FLAG
        *charset = 63;                                     // binary
    }
}

static void column_def(MwOut* o, const char* db, const char* table, const char* name, const char* type) {
    unsigned ty, fl, cs, dec;
    uint32_t len;
    map_type(type, &ty, &fl, &cs, &len, &dec);
    mw_begin(o);
    mw_lenz(o, "def");
    mw_lenz(o, db);
    mw_lenz(o, table);
    mw_lenz(o, table);
    mw_lenz(o, name);
    mw_lenz(o, name);
    mw_u8(o, 0x0c);
    mw_u16(o, cs);
    mw_u32(o, len);
    mw_u8(o, ty);
    mw_u16(o, fl);
    mw_u8(o, dec);
    mw_u16(o, 0);
    mw_end(o);
}

void mw_resultset(MwOut* o, const QueryResult* r, const char* db, unsigned status) {
    mw_begin(o);
    mw_lenenc(o, (uint64_t)r->num_cols);
    mw_end(o);
    for (int c = 0; c < r->num_cols; c++)
        column_def(o, db, r->col_tables && r->col_tables[c] ? r->col_tables[c] : "",
                   r->col_names[c], r->col_types ? r->col_types[c] : NULL);
    mw_eof(o, status & ~MW_STATUS_MORE_RESULTS);
    for (int i = 0; i < r->num_rows && !o->oom; i++) {
        mw_begin(o);
        for (int c = 0; c < r->num_cols; c++) {
            const Cell* cell = &r->rows[i].cells[c];
            if (cell->is_null) mw_u8(o, 0xfb);
            else mw_lenz(o, cell->value);
        }
        mw_end(o);
    }
    mw_eof(o, status);
}

// ── login ────────────────────────────────────────────────────────

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool rd_lenenc(const uint8_t* p, size_t n, size_t* pos, uint64_t* v) {
    if (*pos >= n) return false;
    uint8_t b = p[(*pos)++];
    if (b < 0xfb) { *v = b; return true; }
    size_t k = b == 0xfc ? 2 : b == 0xfd ? 3 : b == 0xfe ? 8 : 0;
    if (!k || *pos + k > n) return false;
    *v = 0;
    for (size_t i = 0; i < k; i++) *v |= (uint64_t)p[*pos + i] << (8 * i);
    *pos += k;
    return true;
}

// stringa terminata da 0 dentro il pacchetto
static bool rd_z(const uint8_t* p, size_t n, size_t* pos, char* out, size_t on) {
    size_t s = *pos;
    while (*pos < n && p[*pos]) (*pos)++;
    if (*pos >= n) return false;
    size_t l = *pos - s;
    if (l >= on) l = on - 1;
    memcpy(out, p + s, l);
    out[l] = '\0';
    (*pos)++;
    return true;
}

bool mw_parse_login(const uint8_t* p, size_t n, MwLogin* out) {
    memset(out, 0, sizeof(*out));
    if (n < 32) return false;
    out->caps = rd32(p);
    if (!(out->caps & MW_CLIENT_PROTOCOL_41)) return false;
    size_t pos = 32;                                       // caps, max packet, charset, 23 byte riservati
    if (!rd_z(p, n, &pos, out->user, sizeof(out->user))) return false;
    uint64_t al = 0;
    if (out->caps & MW_CLIENT_PLUGIN_AUTH_LENENC) { if (!rd_lenenc(p, n, &pos, &al)) return false; }
    else if (out->caps & MW_CLIENT_SECURE_CONNECTION) { if (pos >= n) return false; al = p[pos++]; }
    else { size_t s = pos; while (pos < n && p[pos]) pos++; al = pos - s; pos = s; }
    if (al > sizeof(out->auth) || pos + al > n) return false;
    memcpy(out->auth, p + pos, (size_t)al);
    out->auth_len = (size_t)al;
    pos += (size_t)al;
    if (!(out->caps & (MW_CLIENT_PLUGIN_AUTH_LENENC | MW_CLIENT_SECURE_CONNECTION)) && pos < n) pos++;
    if ((out->caps & MW_CLIENT_CONNECT_WITH_DB) && pos < n) rd_z(p, n, &pos, out->db, sizeof(out->db));
    if ((out->caps & MW_CLIENT_PLUGIN_AUTH) && pos < n) rd_z(p, n, &pos, out->plugin, sizeof(out->plugin));
    return true;
}

// ── istruzioni dei driver ────────────────────────────────────────

typedef struct { const char* name; const char* value; } Var;
static const Var VARS[] = {
    { "version", "8.0.35-SegVault" }, { "version_comment", "SegVault" }, { "version_compile_os", "SegVault" },
    { "max_allowed_packet", "67108864" }, { "sql_mode", "" }, { "autocommit", "1" },
    { "character_set_client", "utf8mb4" }, { "character_set_connection", "utf8mb4" }, { "character_set_results", "utf8mb4" },
    { "character_set_server", "utf8mb4" }, { "character_set_database", "utf8mb4" }, { "character_set_system", "utf8mb3" },
    { "collation_connection", "utf8mb4_general_ci" }, { "collation_server", "utf8mb4_general_ci" },
    { "collation_database", "utf8mb4_general_ci" }, { "init_connect", "" }, { "interactive_timeout", "28800" },
    { "wait_timeout", "28800" }, { "net_write_timeout", "60" }, { "lower_case_table_names", "1" },
    { "transaction_isolation", "SERIALIZABLE" }, { "tx_isolation", "SERIALIZABLE" }, { "transaction_read_only", "0" },
    { "tx_read_only", "0" }, { "time_zone", "SYSTEM" }, { "system_time_zone", "UTC" }, { "license", "MIT" },
    { "auto_increment_increment", "1" }, { "auto_increment_offset", "1" }, { "net_buffer_length", "16384" },
    { "query_cache_size", "0" }, { "query_cache_type", "OFF" }, { "performance_schema", "0" }, { "have_ssl", "DISABLED" },
    { "default_storage_engine", "SegVault" }, { "storage_engine", "SegVault" }, { "sql_auto_is_null", "0" },
    { "sql_select_limit", "18446744073709551615" }, { "max_connections", "64" }, { "innodb_version", "" },
};
#define NVARS (int)(sizeof(VARS) / sizeof(VARS[0]))

static const char* var_value(const char* name) {
    for (int i = 0; i < NVARS; i++) if (!strcasecmp(VARS[i].name, name)) return VARS[i].value;
    return NULL;
}

static const char* skip_ws(const char* p) {
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '*') { const char* e = strstr(p + 2, "*/"); if (!e) return p + strlen(p); p = e + 2; continue; }
        return p;
    }
}

static bool word(const char** pp, const char* w) {
    const char* p = skip_ws(*pp);
    size_t l = strlen(w);
    if (strncasecmp(p, w, l) || isalnum((unsigned char)p[l]) || p[l] == '_') return false;
    *pp = p + l;
    return true;
}

// LIKE semplice: % e _ , senza distinzione maiuscole/minuscole
static bool like(const char* s, const char* pat) {
    if (!*pat) return !*s;
    if (*pat == '%') { for (;; s++) { if (like(s, pat + 1)) return true; if (!*s) return false; } }
    if (!*s) return false;
    if (*pat == '_' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) return like(s + 1, pat + 1);
    return false;
}

static void text_header(MwOut* o, int ncols, const char* const* names) {
    mw_begin(o);
    mw_lenenc(o, (uint64_t)ncols);
    mw_end(o);
    for (int i = 0; i < ncols; i++) column_def(o, "", "", names[i], "VARCHAR");
}

static void text_row(MwOut* o, int n, const char* const* v) {
    mw_begin(o);
    for (int i = 0; i < n; i++) { if (v[i]) mw_lenz(o, v[i]); else mw_u8(o, 0xfb); }
    mw_end(o);
}

bool mw_client_noise(MwOut* o, const char* sql, unsigned status) {
    const char* p = skip_ws(sql);
    // SET NAMES / SET autocommit / SET sql_mode / SET SESSION ...: accettati senza effetto
    if (word(&p, "SET")) {
        const char* q = p;
        if (word(&q, "PASSWORD")) return false;
        mw_ok(o, 0, 0, status, NULL);
        return true;
    }
    p = skip_ws(sql);
    if (word(&p, "SHOW")) {
        if (!word(&p, "SESSION") && !word(&p, "GLOBAL")) word(&p, "FULL");
        if (word(&p, "VARIABLES")) {
            char pat[128] = "%";
            if (word(&p, "LIKE")) {
                p = skip_ws(p);
                if (*p == '\'') {
                    const char* e = strchr(p + 1, '\'');
                    size_t l = e ? (size_t)(e - p - 1) : 0;
                    if (l >= sizeof(pat)) l = sizeof(pat) - 1;
                    memcpy(pat, p + 1, l);
                    pat[l] = '\0';
                }
            }
            static const char* const N[] = { "Variable_name", "Value" };
            text_header(o, 2, N);
            mw_eof(o, status);
            for (int i = 0; i < NVARS; i++)
                if (like(VARS[i].name, pat)) { const char* v[2] = { VARS[i].name, VARS[i].value }; text_row(o, 2, v); }
            mw_eof(o, status);
            return true;
        }
        static const char* const W[] = { "Level", "Code", "Message" };
        static const char* const C[] = { "Collation", "Charset", "Id", "Default", "Compiled", "Sortlen" };
        static const char* const E[] = { "Engine", "Support", "Comment", "Transactions", "XA", "Savepoints" };
        static const char* const S[] = { "Charset", "Description", "Default collation", "Maxlen" };
        const char* const* names = NULL;
        int nn = 0;
        if (word(&p, "WARNINGS") || word(&p, "ERRORS")) { names = W; nn = 3; }
        else if (word(&p, "COLLATION")) { names = C; nn = 6; }
        else if (word(&p, "ENGINES")) { names = E; nn = 6; }
        else if (word(&p, "CHARACTER") || word(&p, "CHARSET")) { names = S; nn = 4; }
        if (!names) return false;
        text_header(o, nn, names);
        mw_eof(o, status);
        if (names == E) { const char* v[6] = { "SegVault", "DEFAULT", "SegVault storage engine", "YES", "NO", "YES" }; text_row(o, 6, v); }
        if (names == S) { const char* v[4] = { "utf8mb4", "UTF-8 Unicode", "utf8mb4_general_ci", "4" }; text_row(o, 4, v); }
        if (names == C) { const char* v[6] = { "utf8mb4_general_ci", "utf8mb4", "45", "Yes", "Yes", "1" }; text_row(o, 6, v); }
        mw_eof(o, status);
        return true;
    }
    // SELECT @@a [AS x], @@session.b ... [LIMIT n]: solo variabili
    p = skip_ws(sql);
    if (!word(&p, "SELECT")) return false;
    char names[16][64], vals[16][128];
    int n = 0;
    for (;;) {
        p = skip_ws(p);
        if (p[0] != '@' || p[1] != '@' || n >= 16) return false;
        p += 2;
        const char* q = p;
        if (!strncasecmp(q, "session.", 8)) q += 8;
        else if (!strncasecmp(q, "global.", 7)) q += 7;
        else if (!strncasecmp(q, "local.", 6)) q += 6;
        char var[64];
        size_t l = 0;
        while ((isalnum((unsigned char)*q) || *q == '_') && l < sizeof(var) - 1) var[l++] = *q++;
        var[l] = '\0';
        const char* v = var_value(var);
        snprintf(names[n], sizeof(names[n]), "@@%.*s", (int)(q - p), p);
        snprintf(vals[n], sizeof(vals[n]), "%s", v ? v : "");
        p = q;
        const char* a = p;
        if (word(&a, "AS")) {
            a = skip_ws(a);
            l = 0;
            char quote = (*a == '`' || *a == '\'' || *a == '"') ? *a++ : 0;
            while (*a && (quote ? *a != quote : (isalnum((unsigned char)*a) || *a == '_')) && l < 63) names[n][l++] = *a++;
            names[n][l] = '\0';
            if (quote && *a == quote) a++;
            p = a;
        }
        n++;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    if (word(&p, "LIMIT")) { p = skip_ws(p); while (isdigit((unsigned char)*p)) p++; }
    p = skip_ws(p);
    if (*p == ';') p = skip_ws(p + 1);
    if (*p) return false;
    const char* cn[16];
    const char* cv[16];
    for (int i = 0; i < n; i++) { cn[i] = names[i]; cv[i] = vals[i]; }
    text_header(o, n, cn);
    mw_eof(o, status);
    text_row(o, n, cv);
    mw_eof(o, status);
    return true;
}
