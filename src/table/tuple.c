#include "tuple.h"
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
    for (int i = 0; i < t->num_values; i++) {
        Value* v = &t->values[i];
        if (!v->is_null) {
            // Libera le stringhe allocate dinamicamente
        }
    }
    free(t->values);
    free(t);
}

// Formato di serializzazione:
//   [null_bitmap: ceil(num_cols/8) byte]
//   [col0_data][col1_data]...
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
            size_t slen = col->type == SV_TYPE_BLOB ? t->values[i].blob_len : strlen(t->values[i].str_val ? t->values[i].str_val : "");
            total += 2 + slen; // 2 byte per la lunghezza
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
            case SV_TYPE_DATETIME:
                memcpy(buf + pos, &v->bigint_val, 8);
                pos += 8; break;
            case SV_TYPE_FLOAT:
                memcpy(buf + pos, &v->float_val, 8);
                pos += 8; break;
            case SV_TYPE_BOOL:
            case SV_TYPE_DATE:
                buf[pos++] = v->bool_val ? 1 : 0; break;
            case SV_TYPE_VARCHAR:
            case SV_TYPE_CHAR:
            case SV_TYPE_TEXT: {
                const char* s = v->str_val ? v->str_val : "";
                uint16_t slen = (uint16_t)strlen(s);
                memcpy(buf + pos, &slen, 2); pos += 2;
                memcpy(buf + pos, s, slen);  pos += slen;
                break;
            }
            default: break;
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

        switch (col->type) {
            case SV_TYPE_INT:
                memcpy(&v->int_val, data + pos, 4); pos += 4; break;
            case SV_TYPE_BIGINT:
            case SV_TYPE_DATETIME:
                memcpy(&v->bigint_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_FLOAT:
                memcpy(&v->float_val, data + pos, 8); pos += 8; break;
            case SV_TYPE_BOOL:
            case SV_TYPE_DATE:
                v->bool_val = data[pos++]; break;
            case SV_TYPE_VARCHAR:
            case SV_TYPE_CHAR:
            case SV_TYPE_TEXT: {
                uint16_t slen;
                memcpy(&slen, data + pos, 2); pos += 2;
                v->str_val = (char*)malloc(slen + 1);
                if (v->str_val) {
                    memcpy(v->str_val, data + pos, slen);
                    v->str_val[slen] = '\0';
                }
                pos += slen;
                break;
            }
            default: break;
        }
    }
    return t;
}

Tuple* tuple_from_strings(const char** values, int count,
                              TableSchema* s) {
    Tuple* t = tuple_create(count);
    if (!t) return NULL;
    for (int i = 0; i < count && i < s->num_columns; i++) {
        Column* col = &s->columns[i];
        Value*  v   = &t->values[i];
        if (!values[i] || strcmp(values[i], "NULL") == 0) {
            v->is_null = true;
            continue;
        }
        v->is_null = false;
        switch (col->type) {
            case SV_TYPE_INT:
                v->int_val = atoi(values[i]); break;
            case SV_TYPE_BIGINT:
                v->bigint_val = atoll(values[i]); break;
            case SV_TYPE_FLOAT:
                v->float_val = atof(values[i]); break;
            case SV_TYPE_BOOL:
                v->bool_val = (strcmp(values[i], "1") == 0 ||
                               strcmp(values[i], "true") == 0); break;
            default:
                v->str_val = strdup(values[i]); break;
        }
    }
    return t;
}

void value_to_string(Value* v, DataType type,
                      char* buf, size_t buf_len) {
    if (v->is_null) { snprintf(buf, buf_len, "NULL"); return; }
    switch (type) {
        case SV_TYPE_INT:    snprintf(buf, buf_len, "%d",  v->int_val);   break;
        case SV_TYPE_BIGINT: snprintf(buf, buf_len, "%lld", (long long)v->bigint_val); break;
        case SV_TYPE_FLOAT:  snprintf(buf, buf_len, "%g",  v->float_val); break;
        case SV_TYPE_BOOL:   snprintf(buf, buf_len, "%s",  v->bool_val ? "1" : "0"); break;
        case SV_TYPE_VARCHAR:
        case SV_TYPE_CHAR:
        case SV_TYPE_TEXT:
            snprintf(buf, buf_len, "%s", v->str_val ? v->str_val : ""); break;
        default:             snprintf(buf, buf_len, "?"); break;
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