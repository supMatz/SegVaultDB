#include "tree_view.h"
#include "theme.h"
#include "../bridge/db_api.h"
#include <stdio.h>
#include <string.h>

// ── navigazione delle righe visibili ─────────────────────────────

static int count_rows(TreeNode* node) {
    int n = 0;
    for (int i = 0; i < node->num_children; i++) {
        n++;
        if (node->children[i].expanded) n += count_rows(&node->children[i]);
    }
    return n;
}

static TreeNode* find_node_at_row(TreeNode* node, int target, int* current, int depth, int* out_depth) {
    for (int i = 0; i < node->num_children; i++) {
        if (*current == target) { if (out_depth) *out_depth = depth; return &node->children[i]; }
        (*current)++;
        if (node->children[i].expanded) {
            TreeNode* found = find_node_at_row(&node->children[i], target, current, depth + 1, out_depth);
            if (found) return found;
        }
    }
    return NULL;
}

static int row_at(TreeView* tv, int my) {
    int rel = my - tv->base.bounds.y - TREE_PAD_TOP;
    if (rel < 0) return -1;
    return tv->scroll_y + rel / TREE_ROW_H;
}

// ── disegno ──────────────────────────────────────────────────────

static void draw_type_icon(PlatformWindow* win, TreeNode* n, int cx, int cy, TreeView* tv) {
    switch (n->type) {
        case TREE_NODE_DATABASE:  ui_icon_database(win, cx, cy, 14, tv->color_icon_db); break;
        case TREE_NODE_TABLE:     ui_icon_table(win, cx, cy, 14, tv->color_icon_table); break;
        case TREE_NODE_VIEW:      ui_icon_table(win, cx, cy, 14, tv->color_icon_view); break;
        case TREE_NODE_PROCEDURE:
        case TREE_NODE_FUNCTION:  ui_icon_play(win, cx, cy, 10, tv->color_icon_proc); break;
        case TREE_NODE_COLUMN:
            if (strcmp(n->extra, "PK") == 0 || strstr(n->extra, "PK")) ui_icon_key(win, cx, cy, 13, TH_WARNING);
            else ui_icon_column(win, cx, cy, 13, TH_ICON_COLUMN);
            break;
        default: ui_circle(win, cx, cy, 2, TH_TEXT_DIM); break;
    }
}

static int draw_node(TreeView* tv, PlatformWindow* win, TreeNode* node, int depth, int row, int first_visible) {
    Rect b = tv->base.bounds;
    int visible_rows = tree_view_visible_rows(tv) + 1;
    int last_visible = first_visible + visible_rows;

    for (int i = 0; i < node->num_children; i++) {
        TreeNode* child = &node->children[i];

        if (row >= first_visible && row < last_visible) {
            int y  = b.y + TREE_PAD_TOP + (row - first_visible) * TREE_ROW_H;
            Rect rr = {b.x + 6, y + 1, b.w - 12, TREE_ROW_H - 2};
            int cy = y + TREE_ROW_H / 2;

            if (child == tv->selected) {
                platform_fill_rect_rounded(win, rr, tv->color_row_selected, 5);
                platform_fill_rect_rounded(win, (Rect){rr.x, rr.y + 5, 3, rr.h - 10}, TH_ACCENT, 1);
            } else if (row == tv->hover_row) {
                platform_fill_rect_rounded(win, rr, tv->color_row_hover, 5);
            }

            // guide di indentazione
            for (int d = 1; d <= depth; d++) {
                int gx = b.x + 18 + (d - 1) * TREE_INDENT;
                platform_fill_rect(win, (Rect){gx, y, 1, TREE_ROW_H}, TH_BORDER);
            }

            int ix = b.x + 12 + depth * TREE_INDENT;
            if (child->has_children) {
                Color cc = (row == tv->hover_row || child == tv->selected) ? TH_TEXT_MUTED : TH_TEXT_DIM;
                ui_icon_chevron(win, ix + 6, cy, 10, child->expanded ? CHEVRON_DOWN : CHEVRON_RIGHT, cc);
            }
            draw_type_icon(win, child, ix + 22, cy, tv);

            int lx = ix + 34;
            FontStyle fs = (child->type == TREE_NODE_DATABASE) ? FONT_UI_BOLD : FONT_UI;
            Color tc = (child->type == TREE_NODE_COLUMN) ? TH_TEXT_MUTED : tv->color_text;
            if (child == tv->selected) tc = TH_TEXT;
            bool in_use = child->type == TREE_NODE_DATABASE && tv->current_db[0] &&
                          strcmp(child->label, tv->current_db) == 0;
            if (in_use) tc = TH_ACCENT;
            int ty = ui_text_y(win, (Rect){0, y, 0, TREE_ROW_H}, tv->font_size, fs);

            // info extra (tipo colonna) allineata a destra
            int right = b.x + b.w - 14;
            int extra_w = 0;
            const char* extra = child->extra;
            if (child->type == TREE_NODE_COLUMN && child->extra[0]) {
                // l'extra può essere "INT" oppure "PK"
                extra_w = platform_measure_text_ex(win, extra, -1, TH_FONT_SMALL, FONT_MONO);
                int max_extra = (right - lx) / 2;
                if (extra_w > max_extra) extra_w = max_extra;
                int ey = ui_text_y(win, (Rect){0, y, 0, TREE_ROW_H}, TH_FONT_SMALL, FONT_MONO);
                ui_text_clipped(win, extra, right - extra_w, ey, extra_w, tv->color_text_extra, TH_FONT_SMALL, FONT_MONO);
                extra_w += 8;
            } else if (child->type == TREE_NODE_DATABASE && child->expanded) {
                char cnt[16];
                snprintf(cnt, sizeof(cnt), "%d", child->num_children);
                int cw = platform_measure_text_ex(win, cnt, -1, TH_FONT_SMALL, FONT_UI) + 12;
                Rect pr = {right - cw, cy - 8, cw, 16};
                platform_fill_rect_rounded(win, pr, TH_ELEVATED, 8);
                platform_draw_text_ex(win, cnt, -1, (Point){pr.x + 6, ui_text_y(win, pr, TH_FONT_SMALL, FONT_UI)},
                                      TH_TEXT_DIM, TH_FONT_SMALL, FONT_UI);
                extra_w = cw + 8;
            }

            if (in_use) {                       // punto "in uso", come l'indicatore in alto a destra
                platform_fill_rect_rounded(win, (Rect){right - extra_w - 8, cy - 4, 8, 8}, TH_ACCENT, 4);
                extra_w += 14;
            }
            ui_text_clipped(win, child->label, lx, ty, right - lx - extra_w, tc, tv->font_size, fs);
        }

        row++;

        if (child->expanded && child->num_children > 0)
            row = draw_node(tv, win, child, depth + 1, row, first_visible);

        if (row >= last_visible + 10) break;
    }
    return row;
}

static void tree_view_draw(Widget* self, PlatformWindow* win) {
    TreeView* tv = (TreeView*)self;
    Rect b = self->bounds;

    platform_fill_rect(win, b, tv->color_bg);
    tree_view_clamp_scroll(tv);

    if (tv->root->num_children == 0) {
        // stato vuoto
        int cy = b.y + 60;
        ui_icon_database(win, b.x + b.w / 2, cy, 28, TH_TEXT_FAINT);
        const char* t1 = "No databases yet";
        const char* t2 = "CREATE DATABASE name;";
        int w1 = platform_measure_text_ex(win, t1, -1, TH_FONT_UI, FONT_UI_BOLD);
        int w2 = platform_measure_text_ex(win, t2, -1, TH_FONT_SMALL + 1, FONT_MONO);
        platform_draw_text_ex(win, t1, -1, (Point){b.x + (b.w - w1) / 2, cy + 26}, TH_TEXT_MUTED, TH_FONT_UI, FONT_UI_BOLD);
        platform_draw_text_ex(win, t2, -1, (Point){b.x + (b.w - w2) / 2, cy + 48}, TH_TEXT_DIM, TH_FONT_SMALL + 1, FONT_MONO);
        return;
    }

    platform_set_clip(win, b);
    draw_node(tv, win, tv->root, 0, 0, tv->scroll_y);
    platform_reset_clip(win);
}

// ── eventi ───────────────────────────────────────────────────────

static bool tree_view_handle_event(Widget* self, sEvent* evt) {
    TreeView* tv = (TreeView*)self;

    switch (evt->type) {
        case EVT_MOUSE_MOVE: {
            int old = tv->hover_row;
            if (widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
                int r = row_at(tv, evt->mouse_y);
                tv->hover_row = (r >= 0 && r < count_rows(tv->root)) ? r : -1;
            } else {
                tv->hover_row = -1;
            }
            (void)old;
            return false;
        }

        case EVT_MOUSE_WHEEL:
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y)) return false;
            tv->scroll_y -= evt->wheel_delta * 3;
            tree_view_clamp_scroll(tv);
            return true;

        case EVT_MOUSE_DOWN: {
            if (!widget_contains_point(self, evt->mouse_x, evt->mouse_y)) return false;
            int clicked = row_at(tv, evt->mouse_y);
            int cur = 0, depth = 0;
            TreeNode* node = clicked >= 0 ? find_node_at_row(tv->root, clicked, &cur, 0, &depth) : NULL;
            if (!node) return true;

            uint64_t now = platform_time_ms();
            bool dbl = (node == tv->last_click_node) && (now - tv->last_click_ms < 400);
            tv->last_click_ms   = dbl ? 0 : now;
            tv->last_click_node = node;

            tree_view_select(tv, node);

            if (dbl) {
                // doppio click: azione sul nodo (il primo click ha già espanso/collassato)
                if (tv->on_activate) tv->on_activate(node, self->user_data);
                return true;
            }

            if (node->has_children && evt->mouse_button == SV_MOUSE_LEFT)
                tree_node_toggle(tv, node);

            if (tv->on_select)
                tv->on_select(node, self->user_data);
            return true;
        }

        default:
            break;
    }
    return false;
}

static void free_children_rec(TreeNode* node) {
    for (int i = 0; i < node->num_children; i++)
        free_children_rec(&node->children[i]);
    free(node->children);
    node->children     = NULL;
    node->num_children = 0;
    node->children_cap = 0;
}

static void tree_view_destroy_fn(Widget* self) {
    TreeView* tv = (TreeView*)self;
    free_children_rec(tv->root);
    free(tv->root);
}

// ── API pubblica ─────────────────────────────────────────────────

TreeView* tree_view_create(int x, int y, int w, int h) {
    TreeView* tv = SV_ALLOC(TreeView);
    if (!tv) return NULL;

    tv->base.type         = WIDGET_TREE_VIEW;
    tv->base.state        = WIDGET_STATE_NORMAL;
    tv->base.bounds       = (Rect){x, y, w, h};

    tv->base.visible      = true;
    tv->base.enabled      = true;
    tv->base.draw         = tree_view_draw;
    tv->base.handle_event = tree_view_handle_event;
    tv->base.destroy      = tree_view_destroy_fn;

    tv->root       = SV_ALLOC(TreeNode);
    tv->root->type = TREE_NODE_ROOT;
    tv->selected   = NULL;
    tv->scroll_y   = 0;
    tv->hover_row  = -1;
    tv->font_size  = TH_FONT_UI;

    tree_view_apply_theme(tv);

    return tv;
}

TreeNode* tree_node_add_child(TreeNode* parent, TreeNodeType type, const char* label, const char* extra) {
    if (!parent) return NULL;

    // espandi l'array figli se necessario
    if (parent->num_children >= parent->children_cap) {
        int cap = SV_MAX(8, parent->children_cap * 2);
        TreeNode* nc = realloc(parent->children, cap * sizeof(TreeNode));
        if (!nc) return NULL;
        parent->children     = nc;
        parent->children_cap = cap;
        // il realloc può spostare i figli: riallinea i puntatori parent dei nipoti
        for (int i = 0; i < parent->num_children; i++)
            for (int j = 0; j < nc[i].num_children; j++)
                nc[i].children[j].parent = &nc[i];
    }

    TreeNode* child = &parent->children[parent->num_children++];
    memset(child, 0, sizeof(TreeNode));
    child->type   = type;
    child->parent = parent;
    strncpy(child->label, label, sizeof(child->label) - 1);
    if (extra) strncpy(child->extra, extra, sizeof(child->extra) - 1);

    // I nodi non-foglia hanno figli potenziali
    child->has_children = (type != TREE_NODE_COLUMN && type != TREE_NODE_INDEX);
    parent->has_children = true;

    return child;
}

void tree_node_toggle(TreeView* tv, TreeNode* node) {
    (void)tv;
    if (!node->has_children) return;
    node->expanded = !node->expanded;
    // I figli vengono popolati da on_tree_select in window.c
}

void tree_node_clear_children(TreeNode* node) {
    if (!node) return;
    free_children_rec(node);
    node->loaded   = false;
    node->expanded = false;
}

void tree_view_refresh(TreeView* tv) {
    if (!tv) return;
    // i puntatori ai vecchi nodi non sono più validi
    tv->selected        = NULL;
    tv->last_click_node = NULL;
    tv->hover_row       = -1;

    tree_node_clear_children(tv->root);

    // ricarica la lista dei database dal bridge
    NameList* dbs = db_list_databases();
    if (!dbs) return;

    for (int i = 0; i < dbs->count; i++)
        tree_node_add_child(tv->root, TREE_NODE_DATABASE, dbs->names[i], NULL);
    db_namelist_free(dbs);
}

void tree_view_select(TreeView* tv, TreeNode* node) {
    if (!tv || !node) return;
    if (tv->selected) tv->selected->selected = false;
    node->selected = true;
    tv->selected   = node;
}

TreeNode* tree_view_set_current_db(TreeView* tv, const char* db) {
    if (!tv) return NULL;
    snprintf(tv->current_db, sizeof(tv->current_db), "%s", db ? db : "");
    if (!tv->root || !tv->current_db[0]) return NULL;
    for (int i = 0; i < tv->root->num_children; i++)
        if (tv->root->children[i].type == TREE_NODE_DATABASE && !strcmp(tv->root->children[i].label, tv->current_db))
            return &tv->root->children[i];
    return NULL;
}

int tree_view_row_count(TreeView* tv) { return tv ? count_rows(tv->root) : 0; }

int tree_view_visible_rows(TreeView* tv) {
    return SV_MAX(1, (tv->base.bounds.h - TREE_PAD_TOP) / TREE_ROW_H);
}

void tree_view_clamp_scroll(TreeView* tv) {
    int max = SV_MAX(0, count_rows(tv->root) - tree_view_visible_rows(tv));
    tv->scroll_y = SV_CLAMP(tv->scroll_y, 0, max);
}

void tree_view_apply_theme(TreeView* tv) {
    if (!tv) return;
    tv->color_bg           = TH_SURFACE;
    tv->color_row_hover    = TH_HOVER;
    tv->color_row_selected = TH_ACCENT_SOFT;
    tv->color_text         = TH_CELL_TEXT;
    tv->color_text_extra   = TH_TEXT_DIM;
    tv->color_icon_db      = TH_ICON_DB;
    tv->color_icon_table   = TH_ICON_TABLE;
    tv->color_icon_view    = TH_ICON_VIEW;
    tv->color_icon_proc    = TH_ICON_PROC;
}
