#ifndef TUPLE_H
#define TUPLE_H

// ─────────────────────────────────────────────────────────────────
// FILE: src/table/tuple.h
// SCOPO: Rappresenta una riga del DB in memoria.
//        Gestisce serializzazione/deserializzazione da/verso pagine.
// ─────────────────────────────────────────────────────────────────

#include "../catalog/schema.h"

// RID = Record IDentifier: indirizzo fisico di una tuple
typedef struct {
    uint32_t page_id;  // In quale pagina
    uint16_t slot_id;  // In quale slot della pagina
} RID;

// Valore di una singola cella (una colonna in una riga)
typedef struct {
    bool is_null;
    union {
        int32_t  int_val;
        int64_t  bigint_val;
        double   float_val;
        bool     bool_val;
        char*    str_val;   // Allocato dinamicamente (da liberare)
        uint8_t* blob_val;
    };
    int blob_len; // Solo per BLOB
} Value;

// Una riga completa
typedef struct {
    Value* values;      // Array di valori (num_cols elementi)
    int      num_values;
    RID    rid;         // Indirizzo fisico (valido dopo insert/get)
} Tuple;

Tuple* tuple_create(int num_cols);
void     tuple_free(Tuple* t);

// Serializza la tuple in un byte array (da inserire in una pagina)
uint8_t* tuple_serialize(Tuple* t, TableSchema* s, size_t* out_len);

// Deserializza un byte array in una tuple
Tuple* tuple_deserialize(const uint8_t* data, size_t len, TableSchema* s);

// Crea una tuple da un array di stringhe (usato dall'executor)
Tuple* tuple_from_strings(const char** values, int count, TableSchema* s);

// Converte un valore in stringa (per la GUI)
void value_to_string(Value* v, DataType type, char* buf, size_t buf_len);

void tuple_print(Tuple* t, TableSchema* s); // Debug

#endif