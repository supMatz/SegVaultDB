#include "tuple.h"
#include <strings.h>
#include <stdio.h>

Tuple* tuple_create(int num_cols) {
    Tuple* t = SV_ALLOC(Tuple);
    if (!t) return NULL;
    t->values     = SV_ALLOC_N(Value, num_cols);
    t->num_values = num_cols;
    t->rid        = (RID){0, 0};
    return t;
}

void tuple_free(Tuple* t) {
    if (!t) return;
    for (int i = 0; t->values && i < t->num_values; i++)
        if (t->values[i].owns_str) free(t->values[i].str_val);
    free(t->values);
    free(t);
}

void tuple_free_with_schema(Tuple* t, TableSchema* s) {
    (void)s;
    tuple_free(t);
}

// Formato di serializzazione:
//   [null_bitmap: ceil(num_cols/8) byte]
//   [col0_data][col1_data]...
size_t tuple_serialized_size(const Tuple* t, const TableSchema* s, bool* too_long) {
    size_t total = (size_t)(t->num_values + 7) / 8;
    if (too_long) *too_long = false;
    for (int i = 0; i < t->num_values; i++) {
        if (t->values[i].is_null) continue;
        int sz = datatype_size(s->columns[i].type);
        if (sz > 0) { total += (size_t)sz; continue; }
        size_t slen = strlen(t->values[i].str_val ? t->values[i].str_val : "");
        if (slen > SV_MAX_TEXT_BYTES && too_long) *too_long = true;
        total += (slen >= 0xFFFF ? 6 : 2) + slen;
    }
    return total;
}

//   Per VARCHAR/TEXT: [uint16_t len][bytes...]
uint8_t* tuple_serialize(Tuple* t, TableSchema* s, size_t* out_len) {
    // Prima passata: calcola la dimensione totale
    int null_bitmap_size = (t->num_values + 7) / 8;
    size_t total = null_bitmap_size;

    for (int i = 0; i < t->num_values; i++) {
        if (t->values[i].is_null) continue;
        Column* col = &s->columns[i];
        int sz = datatype_size(col->type);
        if (sz > 0) {
            total += sz;
        } else {
            // VARCHAR/TEXT: lunghezza + contenuto
            size_t slen = strlen(t->values[i].str_val ? t->values[i].str_val : "");
            if (slen > SV_MAX_TEXT_BYTES) slen = SV_MAX_TEXT_BYTES;
            total += (slen >= 0xFFFF ? 6 : 2) + slen; // lunghezza: 2 byte, o 0xFFFF + 4 byte
        }
    }

    uint8_t* buf = (uint8_t*)calloc(1, total);
    if (!buf) return NULL;

    // Scrivi il null bitmap
    for (int i = 0; i < t->num_values; i++)
        if (t->values[i].is_null)
            buf[i / 8] |= (1 << (i % 8));

    size_t pos = null_bitmap_size;
    for (int i = 0; i < t->num_values; i++) {
        if (t->values[i].is_null) continue;
        Column* col = &s->columns[i];
        Value*  v   = &t->values[i];

        switch (col->type) {
            case SV_TYPE_INT:
                memcpy(buf + pos, &v->int_val, 4);
                pos += 4; break;
            case SV_TYPE_BIGINT:
                memcpy(buf + pos, &v->bigint_val, 8);
                pos += 8; break;
            case SV_TYPE_FLOAT:
                memcpy(buf + pos, &v->float_val, 8);
                pos += 8; break;
            case SV_TYPE_BOOL:
                buf[pos++] = v->bool_val ? 1 : 0; break;
            default: {   // testo: VARCHAR, CHAR, TEXT, DATE, DATETIME, DECIMAL, BLOB
                const char* s = v->str_val ? v->str_val : "";
                size_t full = strlen(s);
                uint32_t slen = (uint32_t)(full > SV_MAX_TEXT_BYTES ? SV_MAX_TEXT_BYTES : full);
                if (slen >= 0xFFFF) {                 // testo lungo: 0xFFFF poi la lunghezza a 32 bit
                    uint16_t esc = 0xFFFF;
                    memcpy(buf + pos, &esc, 2); pos += 2;
                    memcpy(buf + pos, &slen, 4); pos += 4;
                } else {
                    uint16_t s16 = (uint16_t)slen;
                    memcpy(buf + pos, &s16, 2); pos += 2;
                }
                memcpy(buf + pos, s, slen);  pos += slen;
                break;
            }
        }
    }

    *out_len = total;
    return buf;
}

Tuple* tuple_deserialize(const uint8_t* data, size_t len,
                            TableSchema* s) {
    Tuple* t = tuple_create(s->num_columns);
    if (!t) return NULL;

    int null_bitmap_size = (s->num_columns + 7) / 8;
    if ((size_t)null_bitmap_size > len) { tuple_free(t); return NULL; }

    size_t pos = null_bitmap_size;
    for (int i = 0; i < s->num_columns; i++) {
        // Controlla il null bitmap
        if (data[i / 8] & (1 << (i % 8))) {
            t->values[i].is_null = true;
            continue;
        }
        Column* col = &s->columns[i];
        Value*  v   = &t->values[i];
        v->is_null = false;

        // ogni lettura verifica i limiti: una riga danneggiata viene scartata invece di
        // leggere oltre la fine del buffer
        #define NEED(n) do { if (pos + (size_t)(n) > len) { tuple_free_with_schema(t, s); return NULL; } } while (0)
        switch (col->type) {
            case SV_TYPE_INT:
                NEED(4); memcpy(&v->int_val, data + pos, 4); pos += 4; break;
            case SV_TYPE_BIGINT:
                NEED(8); memcpy(&v->bigint_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_FLOAT:
                NEED(8); memcpy(&v->float_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_BOOL:
                NEED(1); v->bool_val = data[pos++]; break;
            default: {   // testo con prefisso di lunghezza
                uint16_t s16;
                uint32_t slen;
                NEED(2); memcpy(&s16, data + pos, 2); pos += 2;
                slen = s16;
                if (s16 == 0xFFFF) { NEED(4); memcpy(&slen, data + pos, 4); pos += 4; }
                NEED(slen);
                v->str_val = (char*)malloc(slen + 1);
                v->owns_str = v->str_val != NULL;
                if (v->str_val) {
                    memcpy(v->str_val, data + pos, slen);
                    v->str_val[slen] = '\0';
                }
                pos += slen;
                break;
            }
        }
        #undef NEED
    }
    return t;
}

bool tuple_view(Tuple* t, const uint8_t* data, size_t len, const TableSchema* s, char* strbuf, size_t strcap) {
    if (t->num_values != s->num_columns) return false;
    int nb = (s->num_columns + 7) / 8;
    if ((size_t)nb > len) return false;
    size_t pos = (size_t)nb, sp = 0;
    for (int i = 0; i < s->num_columns; i++) {
        Value* v = &t->values[i];
        v->owns_str = false;
        if (data[i / 8] & (1 << (i % 8))) { v->is_null = true; v->str_val = NULL; continue; }
        v->is_null = false;
        switch (s->columns[i].type) {
            case SV_TYPE_INT:    if (pos + 4 > len) return false; memcpy(&v->int_val, data + pos, 4); pos += 4; break;
            case SV_TYPE_BIGINT: if (pos + 8 > len) return false; memcpy(&v->bigint_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_FLOAT:  if (pos + 8 > len) return false; memcpy(&v->float_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_BOOL:   if (pos + 1 > len) return false; v->bool_val = data[pos++]; break;
            default: {
                uint16_t s16;
                uint32_t sl;
                if (pos + 2 > len) return false;
                memcpy(&s16, data + pos, 2); pos += 2;
                sl = s16;
                if (s16 == 0xFFFF) { if (pos + 4 > len) return false; memcpy(&sl, data + pos, 4); pos += 4; }
                if (pos + sl > len || sp + sl + 1 > strcap) return false;
                memcpy(strbuf + sp, data + pos, sl);
                strbuf[sp + sl] = '\0';
                v->str_val = strbuf + sp;
                sp += (size_t)sl + 1;
                pos += sl;
                break;
            }
        }
    }
    return true;
}

void value_set_from_text(Value* v, DataType type, const char* text) {
    if (v->owns_str) free(v->str_val);
    memset(v, 0, sizeof(*v));
    if (!text) { v->is_null = true; return; }   // NULL SQL = puntatore nullo (la stringa 'NULL' è testo)
    switch (type) {
        case SV_TYPE_INT:    v->int_val = atoi(text); break;
        case SV_TYPE_BIGINT: v->bigint_val = atoll(text); break;
        case SV_TYPE_FLOAT:  v->float_val = atof(text); break;
        case SV_TYPE_BOOL:
            v->bool_val = strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0;
            break;
        default:
            v->str_val = strdup(text);
            v->owns_str = v->str_val != NULL;
            break;
    }
}

Tuple* tuple_from_strings(const char** values, int count,
                              TableSchema* s) {
    Tuple* t = tuple_create(count);
    if (!t) return NULL;
    for (int i = 0; i < count && i < s->num_columns; i++)
        value_set_from_text(&t->values[i], s->columns[i].type, values[i]);
    return t;
}

void value_to_string(Value* v, DataType type,
                      char* buf, size_t buf_len) {
    if (v->is_null) { snprintf(buf, buf_len, "NULL"); return; }
    switch (type) {
        case SV_TYPE_INT:    snprintf(buf, buf_len, "%d",  v->int_val);   break;
        case SV_TYPE_BIGINT: snprintf(buf, buf_len, "%lld", (long long)v->bigint_val); break;
        case SV_TYPE_FLOAT:  snprintf(buf, buf_len, "%.15g", v->float_val); break;   // %g teneva solo 6 cifre
        case SV_TYPE_BOOL:   snprintf(buf, buf_len, "%s",  v->bool_val ? "1" : "0"); break;
        default:             snprintf(buf, buf_len, "%s", v->str_val ? v->str_val : ""); break;   // testo
    }
}

void tuple_print(Tuple* t, TableSchema* s) {
    for (int i = 0; i < t->num_values; i++) {
        char buf[256];
        value_to_string(&t->values[i], s->columns[i].type,
                        buf, sizeof(buf));
        printf("%s=%s ", s->columns[i].name, buf);
    }
    printf("\n");
}