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

// bin: risultati in formato binario (prepared statement): INT diventa BIGINT, perché un intero di
// SegVault può non stare in 32 bit e nel formato binario la larghezza è quella del tipo
// "alias.colonna" (nome qualificato di SegVault quando due tabelle hanno colonne omonime): MySQL dà
// il nome della colonna da solo e l'alias come tabella (PHP fetch_object / fetch_assoc usano il nome)
static bool split_qualified(const char* name, char* tbl, size_t tn, const char** col) {
    const char* dot = strchr(name, '.');
    if (!dot || dot == name || !dot[1] || strchr(dot + 1, '.')) return false;
    for (const char* p = name; *p; p++)
        if (p != dot && !(isalnum((unsigned char)*p) || *p == '_' || *p == '$' || (unsigned char)*p >= 0x80)) return false;
    snprintf(tbl, tn, "%.*s", (int)(dot - name), name);
    *col = dot + 1;
    return true;
}

static void column_def_ex(MwOut* o, const char* db, const char* table, const char* name, const char* type, bool bin) {
    char qtable[80];
    const char* col;
    if (name && split_qualified(name, qtable, sizeof(qtable), &col)) {
        if (!table || !table[0]) table = qtable;
        name = col;
    }
    unsigned ty, fl, cs, dec;
    uint32_t len;
    map_type(type, &ty, &fl, &cs, &len, &dec);
    if (bin && ty == MT_LONG) { ty = MT_LONGLONG; len = 20; }
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

static void column_def(MwOut* o, const char* db, const char* table, const char* name, const char* type) {
    column_def_ex(o, db, table, name, type, false);
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

// ── prepared statement (COM_STMT_PREPARE / EXECUTE) ──────────────
// Il server non compila il piano: conserva il testo, e a ogni EXECUTE sostituisce i '?' con i
// valori arrivati in formato binario, scritti come letterali SQL (stringhe con gli apici
// raddoppiati e i backslash protetti: nessuna SQL injection possibile dai valori).

// scorre l'SQL saltando stringhe, identificatori tra backtick e commenti; per ogni '?' di codice
// chiama fn (se c'è) e restituisce quanti sono
static int scan_params(const char* sql, void (*fn)(void* ctx, size_t at), void* ctx) {
    int count = 0;
    size_t n = strlen(sql), i = 0;
    while (i < n) {
        char c = sql[i];
        if (c == '\'' || c == '"' || c == '`') {
            i++;
            while (i < n) {
                if (sql[i] == '\\' && c != '`' && i + 1 < n) { i += 2; continue; }
                if (sql[i] == c) { if (i + 1 < n && sql[i + 1] == c) { i += 2; continue; } i++; break; }
                i++;
            }
        } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') i++;
        } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            const char* e = strstr(sql + i + 2, "*/");
            i = e ? (size_t)(e - sql) + 2 : n;
        } else {
            if (c == '?') { if (fn) fn(ctx, i); count++; }
            i++;
        }
    }
    return count;
}

int mw_count_params(const char* sql) { return scan_params(sql, NULL, NULL); }

void mw_prepare_ok(MwOut* o, uint32_t id, int nparams, const QueryResult* meta, const char* db, unsigned status) {
    int ncols = meta && meta->success ? meta->num_cols : 0;
    mw_begin(o);
    mw_u8(o, 0);
    mw_u32(o, id);
    mw_u16(o, (unsigned)ncols);
    mw_u16(o, (unsigned)nparams);
    mw_u8(o, 0);
    mw_u16(o, 0);
    mw_end(o);
    if (nparams > 0) {
        for (int i = 0; i < nparams; i++) column_def_ex(o, "", "", "?", NULL, true);
        mw_eof(o, status);
    }
    if (ncols > 0) {
        for (int c = 0; c < ncols; c++)
            column_def_ex(o, db, meta->col_tables && meta->col_tables[c] ? meta->col_tables[c] : "",
                          meta->col_names[c], meta->col_types ? meta->col_types[c] : NULL, true);
        mw_eof(o, status);
    }
}

static void put_le(MwOut* o, uint64_t v, int bytes) {
    uint8_t b[8];
    for (int i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i));
    mw_bytes(o, b, (size_t)bytes);
}

static long long cell_int(const char* v) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "t")) return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "f")) return 0;
    char* e;
    long long x = strtoll(v, &e, 10);
    if (*e == '.' || *e == 'e' || *e == 'E') x = (long long)strtod(v, NULL);
    return x;
}

// "AAAA-MM-GG[ HH:MM:SS[.ffffff]]" -> formato binario di DATE / DATETIME
static void put_datetime(MwOut* o, const char* v) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, k = 0;
    long us = 0;
    int got = sscanf(v, "%d-%d-%d%n", &y, &mo, &d, &k);
    if (got < 3) { mw_u8(o, 0); return; }                 // non è una data: 0000-00-00
    const char* t = v + k;
    if (*t == ' ' || *t == 'T') {
        int k2 = 0;
        if (sscanf(t + 1, "%d:%d:%d%n", &h, &mi, &se, &k2) == 3 && t[1 + k2] == '.') {
            const char* f = t + 2 + k2;
            int digits = 0;
            while (isdigit((unsigned char)*f) && digits < 6) { us = us * 10 + (*f++ - '0'); digits++; }
            while (digits++ < 6) us *= 10;
        }
    }
    int len = us ? 11 : (h || mi || se) ? 7 : 4;
    mw_u8(o, (unsigned)len);
    mw_u16(o, (unsigned)y);
    mw_u8(o, (unsigned)mo);
    mw_u8(o, (unsigned)d);
    if (len >= 7) { mw_u8(o, (unsigned)h); mw_u8(o, (unsigned)mi); mw_u8(o, (unsigned)se); }
    if (len == 11) mw_u32(o, (uint32_t)us);
}

void mw_resultset_binary(MwOut* o, const QueryResult* r, const char* db, unsigned status) {
    mw_begin(o);
    mw_lenenc(o, (uint64_t)r->num_cols);
    mw_end(o);
    unsigned* types = calloc((size_t)(r->num_cols ? r->num_cols : 1), sizeof(unsigned));
    if (!types) { o->oom = true; return; }
    for (int c = 0; c < r->num_cols; c++) {
        const char* t = r->col_types ? r->col_types[c] : NULL;
        column_def_ex(o, db, r->col_tables && r->col_tables[c] ? r->col_tables[c] : "", r->col_names[c], t, true);
        unsigned fl, cs, dec;
        uint32_t len;
        map_type(t, &types[c], &fl, &cs, &len, &dec);
        if (types[c] == MT_LONG) types[c] = MT_LONGLONG;
    }
    mw_eof(o, status & ~MW_STATUS_MORE_RESULTS);
    size_t nb = ((size_t)r->num_cols + 7 + 2) / 8;
    uint8_t* bitmap = calloc(nb, 1);
    if (!bitmap) { free(types); o->oom = true; return; }
    for (int i = 0; i < r->num_rows && !o->oom; i++) {
        mw_begin(o);
        mw_u8(o, 0);
        memset(bitmap, 0, nb);
        for (int c = 0; c < r->num_cols; c++)
            if (r->rows[i].cells[c].is_null) bitmap[(c + 2) / 8] |= (uint8_t)(1u << ((c + 2) % 8));
        mw_bytes(o, bitmap, nb);
        for (int c = 0; c < r->num_cols; c++) {
            const Cell* cell = &r->rows[i].cells[c];
            if (cell->is_null) continue;
            const char* v = cell->value ? cell->value : "";
            switch (types[c]) {
                case MT_TINY:     mw_u8(o, (unsigned)(cell_int(v) & 0xff)); break;
                case MT_LONGLONG: put_le(o, (uint64_t)cell_int(v), 8); break;
                case MT_DOUBLE: { double d = strtod(v, NULL); uint64_t b; memcpy(&b, &d, 8); put_le(o, b, 8); break; }
                case MT_DATE: case MT_DATETIME: put_datetime(o, v); break;
                default:          mw_lenz(o, v); break;
            }
        }
        mw_end(o);
    }
    free(bitmap);
    free(types);
    mw_eof(o, status);
}

// ── valori dei parametri ─────────────────────────────────────────

typedef struct { char* p; size_t n, cap; bool oom; } Sb;
static void sb_add(Sb* b, const char* s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char* q = realloc(b->p, nc);
        if (!q) { b->oom = true; return; }
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void sb_str(Sb* b, const char* s) { sb_add(b, s, strlen(s)); }

static bool sb_quoted(Sb* b, const uint8_t* s, size_t n) {
    if (memchr(s, 0, n)) return false;
    sb_add(b, "'", 1);
    size_t st = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\'' && s[i] != '\\') continue;
        sb_add(b, (const char*)s + st, i - st);
        sb_add(b, s[i] == '\'' ? "''" : "\\\\", 2);
        st = i + 1;
    }
    sb_add(b, (const char*)s + st, n - st);
    sb_add(b, "'", 1);
    return true;
}

static uint64_t rd_le(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// un valore in formato binario -> letterale SQL in lit; false se il pacchetto è troncato
static bool param_literal(Sb* lit, unsigned type, bool is_unsigned, const uint8_t* p, size_t n, size_t* pos, char* err, size_t en) {
    char tmp[96];
    int w = 0;
    switch (type) {
        case 0x06: sb_str(lit, "NULL"); return true;                  // MYSQL_TYPE_NULL
        case 0x01: w = 1; break;                                       // TINY
        case 0x02: case 0x0d: w = 2; break;                            // SHORT, YEAR
        case 0x03: case 0x09: w = 4; break;                            // LONG, INT24
        case 0x08: w = 8; break;                                       // LONGLONG
        case 0x04: {                                                   // FLOAT
            if (*pos + 4 > n) goto trunc;
            uint32_t b = (uint32_t)rd_le(p + *pos, 4); float f; memcpy(&f, &b, 4); *pos += 4;
            snprintf(tmp, sizeof(tmp), "%.9g", (double)f);
            goto number;
        }
        case 0x05: {                                                   // DOUBLE
            if (*pos + 8 > n) goto trunc;
            uint64_t b = rd_le(p + *pos, 8); double d; memcpy(&d, &b, 8); *pos += 8;
            snprintf(tmp, sizeof(tmp), "%.17g", d);
            goto number;
        }
        case 0x07: case 0x0a: case 0x0c: {                             // TIMESTAMP, DATE, DATETIME
            if (*pos >= n) goto trunc;
            unsigned l = p[(*pos)++];
            if (*pos + l > n) goto trunc;
            const uint8_t* d = p + *pos;
            unsigned y = l >= 4 ? (unsigned)rd_le(d, 2) : 0, mo = l >= 4 ? d[2] : 0, dd = l >= 4 ? d[3] : 0;
            unsigned h = l >= 7 ? d[4] : 0, mi = l >= 7 ? d[5] : 0, s = l >= 7 ? d[6] : 0;
            unsigned long us = l >= 11 ? (unsigned long)rd_le(d + 7, 4) : 0;
            *pos += l;
            if (type == 0x0a) snprintf(tmp, sizeof(tmp), "'%04u-%02u-%02u'", y, mo, dd);
            else if (us) snprintf(tmp, sizeof(tmp), "'%04u-%02u-%02u %02u:%02u:%02u.%06lu'", y, mo, dd, h, mi, s, us);
            else snprintf(tmp, sizeof(tmp), "'%04u-%02u-%02u %02u:%02u:%02u'", y, mo, dd, h, mi, s);
            sb_str(lit, tmp);
            return true;
        }
        case 0x0b: {                                                   // TIME
            if (*pos >= n) goto trunc;
            unsigned l = p[(*pos)++];
            if (*pos + l > n) goto trunc;
            const uint8_t* d = p + *pos;
            bool neg = l >= 8 && d[0];
            unsigned long days = l >= 8 ? (unsigned long)rd_le(d + 1, 4) : 0;
            unsigned h = l >= 8 ? d[5] : 0, mi = l >= 8 ? d[6] : 0, s = l >= 8 ? d[7] : 0;
            *pos += l;
            snprintf(tmp, sizeof(tmp), "'%s%02lu:%02u:%02u'", neg ? "-" : "", days * 24 + h, mi, s);
            sb_str(lit, tmp);
            return true;
        }
        default: {                                                     // stringhe, BLOB, DECIMAL, JSON, ...
            uint64_t l;
            if (!rd_lenenc(p, n, pos, &l) || *pos + l > n) goto trunc;
            const uint8_t* s = p + *pos;
            *pos += (size_t)l;
            if (type == 0xf6 || type == 0x00) {                        // DECIMAL: numero, se lo è davvero
                bool num = l > 0 && l < sizeof(tmp);
                for (uint64_t i = 0; num && i < l; i++)
                    if (!isdigit(s[i]) && s[i] != '.' && s[i] != '-' && s[i] != '+' && s[i] != 'e' && s[i] != 'E') num = false;
                if (num) { memcpy(tmp, s, (size_t)l); tmp[l] = '\0'; goto number; }
            }
            if (!sb_quoted(lit, s, (size_t)l)) { snprintf(err, en, "parameter values cannot contain NUL bytes"); return false; }
            return true;
        }
    }
    if (*pos + (size_t)w > n) goto trunc;
    {
        uint64_t u = rd_le(p + *pos, w);
        *pos += (size_t)w;
        if (is_unsigned) snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)u);
        else {
            long long v = w == 1 ? (int8_t)u : w == 2 ? (int16_t)u : w == 4 ? (int32_t)u : (long long)u;
            snprintf(tmp, sizeof(tmp), "%lld", v);
        }
    }
number:
    if (!strcmp(tmp, "inf") || !strcmp(tmp, "-inf") || !strcmp(tmp, "nan") || !strcmp(tmp, "-nan")) { sb_str(lit, "NULL"); return true; }
    if (tmp[0] == '-') { sb_str(lit, "("); sb_str(lit, tmp); sb_str(lit, ")"); }   // "x-?" con -5 non diventa un commento
    else sb_str(lit, tmp);
    return true;
trunc:
    snprintf(err, en, "malformed COM_STMT_EXECUTE packet");
    return false;
}

typedef struct { const char* sql; Sb* out; size_t last; char** lits; int k; } Subst;
static void subst_at(void* ctx, size_t at) {
    Subst* s = ctx;
    sb_add(s->out, s->sql + s->last, at - s->last);
    sb_str(s->out, s->lits[s->k++]);
    s->last = at + 1;
}

char* mw_bind_execute(const char* sql, const uint8_t* p, size_t n, int nparams, uint8_t* types,
                      bool* types_set, MwLongData* ld, char* err, size_t en) {
    // p: dopo il comando e l'id (flag u8, iteration_count u32, poi i parametri)
    size_t pos = 5;
    if (n < pos) { snprintf(err, en, "malformed COM_STMT_EXECUTE packet"); return NULL; }
    char** lits = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(char*));
    if (!lits) { snprintf(err, en, "out of memory"); return NULL; }
    char* res = NULL;
    if (nparams > 0) {
        size_t nb = ((size_t)nparams + 7) / 8;
        if (pos + nb + 1 > n) { snprintf(err, en, "malformed COM_STMT_EXECUTE packet"); goto out; }
        const uint8_t* nulls = p + pos;
        pos += nb;
        if (p[pos++]) {                                    // new_params_bound_flag: tipi nuovi
            if (pos + 2 * (size_t)nparams > n) { snprintf(err, en, "malformed COM_STMT_EXECUTE packet"); goto out; }
            memcpy(types, p + pos, 2 * (size_t)nparams);
            pos += 2 * (size_t)nparams;
            *types_set = true;
        }
        if (!*types_set) { snprintf(err, en, "no parameter types were sent for the prepared statement"); goto out; }
        for (int i = 0; i < nparams; i++) {
            Sb lit = {0};
            if (ld && ld[i].set) {                         // valore mandato con COM_STMT_SEND_LONG_DATA
                if (!sb_quoted(&lit, (const uint8_t*)(ld[i].p ? ld[i].p : ""), ld[i].n)) {
                    snprintf(err, en, "parameter values cannot contain NUL bytes"); free(lit.p); goto out;
                }
            } else if (nulls[i / 8] & (1u << (i % 8))) sb_str(&lit, "NULL");
            else if (!param_literal(&lit, types[2 * i], (types[2 * i + 1] & 0x80) != 0, p, n, &pos, err, en)) { free(lit.p); goto out; }
            if (lit.oom || !lit.p) { free(lit.p); snprintf(err, en, "out of memory"); goto out; }
            lits[i] = lit.p;
        }
    }
    {
        Sb o = {0};
        Subst st = { sql, &o, 0, lits, 0 };
        int found = scan_params(sql, subst_at, &st);
        sb_add(&o, sql + st.last, strlen(sql + st.last));
        if (found != nparams) { free(o.p); snprintf(err, en, "the statement changed: %d placeholders, %d expected", found, nparams); goto out; }
        if (o.oom) { free(o.p); snprintf(err, en, "out of memory"); goto out; }
        res = o.p ? o.p : strdup("");
    }
out:
    for (int i = 0; i < nparams; i++) free(lits[i]);
    free(lits);
    return res;
}

typedef struct { const char* sql; Sb* out; size_t last; } Nulls;
static void null_at(void* ctx, size_t at) {
    Nulls* s = ctx;
    sb_add(s->out, s->sql + s->last, at - s->last);
    sb_str(s->out, "NULL");
    s->last = at + 1;
}

char* mw_with_nulls(const char* sql) {
    Sb o = {0};
    Nulls st = { sql, &o, 0 };
    scan_params(sql, null_at, &st);
    sb_add(&o, sql + st.last, strlen(sql + st.last));
    if (o.oom) { free(o.p); return NULL; }
    return o.p ? o.p : strdup("");
}

// ── autocommit ───────────────────────────────────────────────────

int mw_set_autocommit(const char* sql) {
    const char* p = sql;
    if (!word(&p, "SET")) return -1;
    if (!word(&p, "SESSION")) word(&p, "LOCAL");
    p = skip_ws(p);
    if (p[0] == '@' && p[1] == '@') {
        p += 2;
        if (!strncasecmp(p, "session.", 8)) p += 8;
        else if (!strncasecmp(p, "local.", 6)) p += 6;
    }
    if (!word(&p, "autocommit")) return -1;
    p = skip_ws(p);
    if (*p == '=') p++;
    else if (p[0] == ':' && p[1] == '=') p += 2;
    else return -1;
    int v = -1;
    const char* q = p;
    if (word(&q, "1") || word(&q, "ON") || word(&q, "TRUE")) v = 1;
    else if (q = p, word(&q, "0") || word(&q, "OFF") || word(&q, "FALSE")) v = 0;
    else {
        q = skip_ws(p);
        if (*q == '\'') {
            if (!strncasecmp(q, "'ON'", 4) || !strncmp(q, "'1'", 3)) v = 1;
            else if (!strncasecmp(q, "'OFF'", 5) || !strncmp(q, "'0'", 3)) v = 0;
            if (v >= 0) q = strchr(q + 1, '\'') + 1;
        }
    }
    if (v < 0) return -1;
    q = skip_ws(q);
    if (*q == ';') q = skip_ws(q + 1);
    return *q ? -1 : v;                                    // SET autocommit = 0, x = 1: non qui
}

int mw_stmt_kind(const char* sql) {
    const char* p = sql;
    if (word(&p, "COMMIT")) return MW_KIND_COMMIT;
    p = sql;
    if (word(&p, "ROLLBACK")) { word(&p, "WORK"); return word(&p, "TO") ? MW_KIND_OTHER : MW_KIND_ROLLBACK; }
    static const char* const DML[] = { "INSERT", "UPDATE", "DELETE", "REPLACE", "CALL" };
    for (size_t i = 0; i < sizeof(DML) / sizeof(DML[0]); i++) { p = sql; if (word(&p, DML[i])) return MW_KIND_DML; }
    return MW_KIND_OTHER;
}
