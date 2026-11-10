#include "result_export.h"
#include "grid_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

typedef struct { char* p; size_t n, cap; bool ok; } Buf;

static void put(Buf* b, const char* s, size_t n) {
    if (!b->ok) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap * 2 + n + 1024;
        char* p = realloc(b->p, cap);
        if (!p) { b->ok = false; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void puts_(Buf* b, const char* s) { put(b, s, strlen(s)); }
static void putc_(Buf* b, char c) { put(b, &c, 1); }

static const char* const EXT[]   = { "csv", "tsv", "json", "md", "html", "sql" };
static const char* const NAMES[] = { "CSV", "TSV", "JSON", "Markdown", "HTML", "SQL INSERT" };

const char* export_format_ext(ExportFormat f)  { return (f >= 0 && f < EXPORT_FORMAT_COUNT) ? EXT[f] : "csv"; }
const char* export_format_name(ExportFormat f) { return (f >= 0 && f < EXPORT_FORMAT_COUNT) ? NAMES[f] : "CSV"; }

ExportFormat export_format_from_path(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return EXPORT_CSV;
    dot++;
    if (!strcasecmp(dot, "txt"))      return EXPORT_TSV;
    if (!strcasecmp(dot, "markdown")) return EXPORT_MARKDOWN;
    if (!strcasecmp(dot, "htm"))      return EXPORT_HTML;
    for (int f = 0; f < EXPORT_FORMAT_COUNT; f++)
        if (!strcasecmp(dot, EXT[f])) return (ExportFormat)f;
    return EXPORT_CSV;
}

static bool is_num_cell(const Cell* c) {
    if (c->type != CELL_TYPE_INT && c->type != CELL_TYPE_FLOAT) return false;
    // solo numeri che JSON/SQL accettano così come sono
    const char* s = c->value;
    if (*s == '-') s++;
    if (!isdigit((unsigned char)*s)) return false;
    char* end = NULL;
    strtod(c->value, &end);
    return end && *end == '\0';
}

// nome di colonna senza il prefisso "tabella."
static const char* short_name(const char* n) {
    const char* dot = n ? strrchr(n, '.') : NULL;
    return dot ? dot + 1 : (n ? n : "");
}

char* export_sql_literal(const Cell* c) {
    return grid_edit_cell_literal(c);    // stesso quoting della griglia: '' e \\ (il lexer tratta \ come escape)
}

static void csv_field(Buf* b, const char* v, char delim) {
    bool quote = strchr(v, delim) || strpbrk(v, "\"\n\r") || v[0] == ' ' || (v[0] && v[strlen(v) - 1] == ' ');
    if (!quote) { puts_(b, v); return; }
    putc_(b, '"');
    for (const char* p = v; *p; p++) { if (*p == '"') putc_(b, '"'); putc_(b, *p); }
    putc_(b, '"');
}

static void json_string(Buf* b, const char* s) {
    putc_(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  puts_(b, "\\\""); break;
            case '\\': puts_(b, "\\\\"); break;
            case '\n': puts_(b, "\\n"); break;
            case '\r': puts_(b, "\\r"); break;
            case '\t': puts_(b, "\\t"); break;
            default:
                if (c < 0x20) { char u[8]; snprintf(u, sizeof(u), "\\u%04x", c); puts_(b, u); }
                else putc_(b, (char)c);
        }
    }
    putc_(b, '"');
}

static void html_text(Buf* b, const char* s) {
    for (; *s; s++) {
        switch (*s) {
            case '<': puts_(b, "&lt;"); break;
            case '>': puts_(b, "&gt;"); break;
            case '&': puts_(b, "&amp;"); break;
            case '"': puts_(b, "&quot;"); break;
            case '\n': puts_(b, "<br>"); break;
            default: putc_(b, *s);
        }
    }
}

static void md_text(Buf* b, const char* s) {
    for (; *s; s++) {
        if (*s == '|') puts_(b, "\\|");
        else if (*s == '\n') puts_(b, "<br>");
        else if (*s != '\r') putc_(b, *s);
    }
}

char* export_result(const TableView* tv, ExportFormat f, const ExportOptions* opt) {
    if (!tv || !tv->result || tv->result->num_cols <= 0 || tv->msg_kind != TABLE_MSG_NONE) return NULL;
    const QueryResult* r = tv->result;
    int nc = r->num_cols, nr = table_view_row_count(tv);
    ExportOptions def = { ',', true, false, true, "my_table" };
    if (!opt) opt = &def;
    Buf b = { NULL, 0, 0, true };
    const char* nl = opt->json_pretty ? "\n" : "";

    switch (f) {
        case EXPORT_CSV: case EXPORT_TSV: {
            char d = f == EXPORT_TSV ? '\t' : opt->delimiter;
            if (opt->header) {
                for (int c = 0; c < nc; c++) { if (c) putc_(&b, d); csv_field(&b, r->col_names[c] ? r->col_names[c] : "", d); }
                puts_(&b, "\r\n");
            }
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    if (c) putc_(&b, d);
                    const Cell* cell = &row->cells[c];
                    if (cell->is_null) { if (opt->null_as_text || f == EXPORT_TSV) puts_(&b, "NULL"); }
                    else csv_field(&b, cell->value, d);
                }
                puts_(&b, "\r\n");
            }
            break;
        }
        case EXPORT_JSON:
            putc_(&b, '[');
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                puts_(&b, i ? "," : "");
                puts_(&b, nl);
                puts_(&b, opt->json_pretty ? "  {" : "{");
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    const Cell* cell = &row->cells[c];
                    if (c) putc_(&b, ',');
                    if (opt->json_pretty) puts_(&b, "\n    ");
                    json_string(&b, short_name(r->col_names[c]));
                    puts_(&b, opt->json_pretty ? ": " : ":");
                    if (cell->is_null) puts_(&b, "null");
                    else if (is_num_cell(cell)) puts_(&b, cell->value);
                    else if (cell->type == CELL_TYPE_BOOL && (!strcmp(cell->value, "true") || !strcmp(cell->value, "false")))
                        puts_(&b, cell->value);
                    else json_string(&b, cell->value);
                }
                puts_(&b, nl);
                puts_(&b, opt->json_pretty ? "  }" : "}");
            }
            puts_(&b, nl);
            puts_(&b, "]\n");
            break;

        case EXPORT_MARKDOWN:
            puts_(&b, "|");
            for (int c = 0; c < nc; c++) { putc_(&b, ' '); md_text(&b, r->col_names[c] ? r->col_names[c] : ""); puts_(&b, " |"); }
            puts_(&b, "\n|");
            for (int c = 0; c < nc; c++) {
                bool num = r->num_rows > 0 && is_num_cell(&r->rows[0].cells[c]);
                puts_(&b, num ? " ---: |" : " --- |");
            }
            putc_(&b, '\n');
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                puts_(&b, "|");
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    putc_(&b, ' ');
                    md_text(&b, row->cells[c].is_null ? "NULL" : row->cells[c].value);
                    puts_(&b, " |");
                }
                putc_(&b, '\n');
            }
            break;

        case EXPORT_HTML:
            puts_(&b, "<!doctype html>\n<meta charset=\"utf-8\">\n<title>SegVault export</title>\n"
                      "<style>body{font:14px system-ui,sans-serif;margin:24px}table{border-collapse:collapse}"
                      "th,td{border:1px solid #ccc;padding:4px 10px;text-align:left}th{background:#f2f4f7}"
                      "td.n{text-align:right}td.null{color:#999}</style>\n<table>\n<tr>");
            for (int c = 0; c < nc; c++) { puts_(&b, "<th>"); html_text(&b, r->col_names[c] ? r->col_names[c] : ""); puts_(&b, "</th>"); }
            puts_(&b, "</tr>\n");
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                puts_(&b, "<tr>");
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    const Cell* cell = &row->cells[c];
                    puts_(&b, cell->is_null ? "<td class=\"null\">NULL" : is_num_cell(cell) ? "<td class=\"n\">" : "<td>");
                    if (!cell->is_null) html_text(&b, cell->value);
                    puts_(&b, "</td>");
                }
                puts_(&b, "</tr>\n");
            }
            puts_(&b, "</table>\n");
            break;

        case EXPORT_SQL: {
            char head[1024];
            int n = snprintf(head, sizeof(head), "INSERT INTO %s (", opt->table && opt->table[0] ? opt->table : "my_table");
            for (int c = 0; c < nc && n < (int)sizeof(head) - 80; c++)
                n += snprintf(head + n, sizeof(head) - n, "%s%s", c ? ", " : "", short_name(r->col_names[c]));
            snprintf(head + n, sizeof(head) - n, ") VALUES (");
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                puts_(&b, head);
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    char* lit = export_sql_literal(&row->cells[c]);
                    if (c) puts_(&b, ", ");
                    puts_(&b, lit ? lit : "NULL");
                    free(lit);
                }
                puts_(&b, ");\n");
            }
            break;
        }
        default:
            break;
    }
    if (!b.ok) { free(b.p); return NULL; }
    if (!b.p) b.p = calloc(1, 1);
    return b.p;
}
