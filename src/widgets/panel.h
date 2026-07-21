#ifndef PANEL_H
#define PANEL_H

#include "widget.h"

#define PANEL_MAX_CHILDREN 128

typedef struct {
    Widget base;
    Widget* children[PANEL_MAX_CHILDREN];
    int num_children;
    Color color_bg;
    Color color_border;
    int border_width;
    int padding;
} Panel;

Panel* panel_create(int x, int y, int w, int h);
void panel_add_child(Panel* p, Widget* child);
bool panel_remove_child(Panel* p, Widget* child);
void panel_clear(Panel* p);

#endif
