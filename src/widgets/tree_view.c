#include "tree_view.h"
#include "../bridge/db_api.h"
#include <string.h>

static int draw_node(TreeView* tv, PlatformWindow* win, TreeNode* node, int depth, int y_offset, int first_visible) {
    Rect b = tv->base.bounds;
    int visible_rows = b.h / TREE_ROW_H;
    int last_visible = first_visible + visible_rows;

    for(int i = 0; i < node->num_children; i++) {
        TreeNode* child = &node->children[i];

        if(y_offset >= first_visible && y_offset < last_visible) {
            int draw_y = b.y + (y_offset - first_visible) * TREE_ROW_H;
            Color bg = (child == tv->selected) ? tv->color_row_selected : tv->color_bg;

            platform_fill_rect(win, (Rect){b.x, draw_y, b.w, TREE_ROW_H}, bg);

            int ix = b.x + depth * TREE_INDENT;

            if (child->has_children) {
                const char* arrow = child->expanded ? "v" : ">";
                platform_draw_text(win, arrow,
                    (Point){ix, draw_y + (TREE_ROW_H - tv->font_size) / 2},
                    (Color){150, 150, 160, 255}, tv->font_size);
            }

            Color icon_color;
            const char* icon;
            switch (child->type) {
                case TREE_NODE_DATABASE: icon = "DB"; icon_color = tv->color_icon_db; break;
                case TREE_NODE_TABLE:    icon = "T";  icon_color = tv->color_icon_table; break;
                case TREE_NODE_VIEW:     icon = "V";  icon_color = tv->color_icon_view; break;
                case TREE_NODE_PROCEDURE:
                case TREE_NODE_FUNCTION: icon = "F";  icon_color = tv->color_icon_proc; break;
                case TREE_NODE_COLUMN:   icon = "C";  icon_color = (Color){150,150,160,255}; break;
                default:                 icon = "-";  icon_color = (Color){100,100,110,255}; break;
            }

            platform_draw_text(win, icon,
                (Point){ix + 14, draw_y + (TREE_ROW_H - tv->font_size) / 2},
                icon_color, tv->font_size - 1);

            int lx = ix + 30;
            platform_draw_text(win, child->label,
                (Point){lx, draw_y + (TREE_ROW_H - tv->font_size) / 2},
                tv->color_text, tv->font_size);

            if (child->extra[0]) {
                int lw = platform_measure_text(win, child->label, tv->font_size);
                platform_draw_text(win, child->extra,
                    (Point){lx + lw + 6, draw_y + (TREE_ROW_H - tv->font_size) / 2},
                    tv->color_text_extra, tv->font_size - 1);
            }
        }

        y_offset++;

        if (child->expanded && child->num_children > 0)
            y_offset = draw_node(tv, win, child, depth + 1, y_offset, first_visible);

        if (y_offset >= last_visible + 10) break;
    }
    return y_offset;
}

static void tree_view_draw(Widget* self, PlatformWindow* win) {
    TreeView* tv = (TreeView*)self;
    Rect b = self->bounds;

    platform_fill_rect(win, b, tv->color_bg);

    draw_node(tv, win, tv->root, 0, 0, tv->scroll_y);

    platform_draw_rect(win, b, (Color) {50, 50, 60, 255}, 1);
}

static TreeNode* find_node_at_row(TreeNode* node, int target, int* current) {
    for (int i = 0; i < node->num_children; i++) {
        if (*current == target) return &node->children[i];
        (*current)++;
        if (node->children[i].expanded)
            find_node_at_row(&node->children[i], target, current);
    }
    return NULL;
}
 
static bool tree_view_handle_event(Widget* self, sEvent* evt) {
    TreeView* tv = (TreeView*)self;
 
    if (evt->type == EVT_MOUSE_DOWN &&
        widget_contains_point(self, evt->mouse_x, evt->mouse_y)) {
 
        int rel_y   = evt->mouse_y - self->bounds.y;
        int clicked = tv->scroll_y + rel_y / TREE_ROW_H;
 
        int cur = 0;
        TreeNode* node = find_node_at_row(tv->root, clicked, &cur);
        if (node) {
            // deseleziona il precedente
            if (tv->selected) tv->selected->selected = false;
            node->selected = true;
            tv->selected   = node;
 
            // toggle expand se ha figli
            if (node->has_children)
                tree_node_toggle(tv, node);
 
            if (tv->on_select)
                tv->on_select(node, self->user_data);
        }
        return true;
    }
    return false;
}
 
static void tree_view_destroy_fn(Widget* self) {
    TreeView* tv = (TreeView*)self;
    // TODO: liberare ricorsivamente tutti i nodi
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
 
    tv->root     = SV_ALLOC(TreeNode);
    tv->root->type = TREE_NODE_ROOT;
    tv->selected = NULL;
    tv->scroll_y = 0;
    tv->font_size = 13;
 
    tv->color_bg           = (Color){22,  22,  28,  255};
    tv->color_row_hover    = (Color){38,  38,  48,  255};
    tv->color_row_selected = (Color){30,  70,  120, 255};
    tv->color_text         = (Color){200, 200, 210, 255};
    tv->color_text_extra   = (Color){100, 100, 120, 255};
    tv->color_icon_db      = (Color){80,  180, 255, 255};
    tv->color_icon_table   = (Color){80,  220, 140, 255};
    tv->color_icon_view    = (Color){255, 200, 80,  255};
    tv->color_icon_proc    = (Color){200, 120, 255, 255};
 
    return tv;
}
 
TreeNode* tree_node_add_child(TreeNode* parent, TreeNodeType type, const char* label, const char* extra) {
    if (!parent) return NULL;
 
    // espandi l'array figli se necessario
    if (parent->num_children >= parent->children_cap) {
        parent->children_cap = SV_MAX(8, parent->children_cap * 2);
        parent->children = realloc(parent->children,
            parent->children_cap * sizeof(TreeNode));
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
    free(node->children);
    node->children     = NULL;
    node->num_children = 0;
    node->children_cap = 0;
    node->loaded       = false;
    node->expanded     = false;
}
 

void tree_view_refresh(TreeView* tv) {
    if (!tv) return;
    // Rimuovi tutti i figli dalla radice
    tree_node_clear_children(tv->root);
    
    // ricarica la lista dei database dal bridge
    NameList* dbs = db_list_databases();
    if (!dbs) return;
 
    for (int i = 0; i < dbs->count; i++) {
        tree_node_add_child(tv->root, TREE_NODE_DATABASE,
                             dbs->names[i], NULL);
    }
    db_namelist_free(dbs);
}
 
void tree_view_select(TreeView* tv, TreeNode* node) {
    if (!tv || !node) return;
    if (tv->selected) tv->selected->selected = false;
    node->selected = true;
    tv->selected   = node;
}