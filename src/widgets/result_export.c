#include "../../include/sv_strbuf.h"
#include "result_export.h"
#include "../table/binesc.h"
#include "grid_edit.h"
#include "export_bin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

typedef SvStrBuf Buf;                                        // buffer comune (CR-B2)
#define put svsb_put
#define puts_ svsb_puts
#define putc_ svsb_putc

static const char* const EXT[]   = { "csv", "tsv", "json", "md", "html", "sql", "xlsx", "pdf" };
static const char* const NAMES[] = { "CSV", "TSV", "JSON", "Markdown", "HTML", "SQL INSERT", "Excel XLSX", "PDF" };

bool export_is_binary(ExportFormat f) { return f == EXPORT_XLSX || f == EXPORT_PDF; }

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
// `nome` per l'SQL esportato: un alias del risultato può contenere qualunque cosa, e il lexer non ha
// escape per il backtick dentro `...`: backtick e caratteri di controllo diventano '_'
static void put_ident(Buf* b, const char* name, size_t len) {
    puts_(b, "`");
    char tmp[2];
    tmp[1] = '\0';
    for (size_t i = 0; i < len && name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        tmp[0] = (c < 0x20 || c == 0x7F || c == '`') ? '_' : (char)c;
        puts_(b, tmp);
    }
    puts_(b, "`");
}

static const char* short_name(const char* n) {
    const char* dot = n ? strrchr(n, '.') : NULL;
    return dot ? dot + 1 : (n ? n : "");
}

char* export_sql_literal(const Cell* c) {
    return grid_edit_cell_literal(c);    // stesso quoting della griglia: '' e \\ (il lexer tratta \ come escape)
}

// byte veri di un valore in forma interna (binesc.h): per CSV, JSON, Markdown, HTML
static char* decoded(const char* v, size_t* n) {
    size_t l = strlen(v);
    char* d = malloc(l + 1);
    if (!d) { *n = 0; return NULL; }
    *n = sv_bin_decode(v, l, d);
    d[*n] = '\0';
    return d;
}

static void csv_field(Buf* b, const char* v, char delim) {
    if (sv_bin_has(v, strlen(v))) {                            // binario: i byte veri, tra virgolette
        size_t n;
        char* d = decoded(v, &n);
        if (!d) return;
        putc_(b, '"');
        for (size_t i = 0; i < n; i++) { if (d[i] == '"') putc_(b, '"'); putc_(b, d[i]); }
        putc_(b, '"');
        free(d);
        return;
    }
    bool quote = strchr(v, delim) || strpbrk(v, "\"\n\r") || v[0] == ' ' || (v[0] && v[strlen(v) - 1] == ' ');
    if (!quote) { puts_(b, v); return; }
    putc_(b, '"');
    for (const char* p = v; *p; p++) { if (*p == '"') putc_(b, '"'); putc_(b, *p); }
    putc_(b, '"');
}

static void json_string(Buf* b, const char* s0) {
    size_t n;
    char* d = sv_bin_has(s0, strlen(s0)) ? decoded(s0, &n) : NULL;
    const char* s = d ? d : s0;
    size_t len = d ? n : strlen(s0);
    putc_(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
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
    free(d);
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

unsigned char* export_result_bin(const TableView* tv, ExportFormat f, const ExportOptions* opt, const char* title, size_t* len) {
    if (!export_is_binary(f)) {
        char* t = export_result(tv, f, opt);
        if (t) *len = strlen(t);
        return (unsigned char*)t;
    }
    if (!tv || !tv->result || tv->result->num_cols <= 0 || tv->msg_kind != TABLE_MSG_NONE) return NULL;
    int nr = table_view_row_count(tv);
    int* order = malloc(sizeof(int) * (size_t)(nr ? nr : 1));
    if (!order) return NULL;
    for (int i = 0; i < nr; i++) order[i] = table_view_source_row(tv, i);
    uint8_t* b = f == EXPORT_XLSX ? export_xlsx(tv->result, order, nr, len) : export_pdf(tv->result, order, nr, title, len);
    free(order);
    return b;
}

char* export_result(const TableView* tv, ExportFormat f, const ExportOptions* opt) {
    if (export_is_binary(f)) return NULL;
    if (!tv || !tv->result || tv->result->num_cols <= 0 || tv->msg_kind != TABLE_MSG_NONE) return NULL;
    const QueryResult* r = tv->result;
    int nc = r->num_cols, nr = table_view_row_count(tv);
    ExportOptions def = { ',', true, false, true, "my_table" };
    if (!opt) opt = &def;
    Buf b = {0};
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
            // intestazione di ogni INSERT: tutte le colonne, tra backtick (prima 1 KB fisso: con molte colonne le
            // ultime sparivano e l'INSERT aveva più valori che colonne; un nome come "order" non era quotato)
            Buf head = {0};
            puts_(&head, "INSERT INTO ");
            const char* tn = opt->table && opt->table[0] ? opt->table : "my_table";
            const char* dot = strchr(tn, '.');                  // "db.tabella": due nomi
            if (dot) { put_ident(&head, tn, (size_t)(dot - tn)); puts_(&head, "."); put_ident(&head, dot + 1, strlen(dot + 1)); }
            else put_ident(&head, tn, strlen(tn));
            puts_(&head, " (");
            for (int c = 0; c < nc; c++) {
                if (c) puts_(&head, ", ");
                const char* cn = short_name(r->col_names[c]);
                put_ident(&head, cn, strlen(cn));
            }
            puts_(&head, ") VALUES (");
            if (head.oom) { free(head.p); b.oom = true; break; }
            for (int i = 0; i < nr; i++) {
                const Row* row = &r->rows[table_view_source_row(tv, i)];
                puts_(&b, head.p);
                for (int c = 0; c < nc && c < row->num_cells; c++) {
                    char* lit = export_sql_literal(&row->cells[c]);
                    if (c) puts_(&b, ", ");
                    puts_(&b, lit ? lit : "NULL");
                    free(lit);
                }
                puts_(&b, ");\n");
            }
            free(head.p);
            break;
        }
        default:
            break;
    }
    if (b.oom) { free(b.p); return NULL; }
    if (!b.p) b.p = calloc(1, 1);
    return b.p;
}
