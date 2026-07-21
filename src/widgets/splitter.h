#ifndef SPLITTER_WIDGET_H
#define SPLITTER_WIDGET_H

#include "widget.h"

typedef enum { SPLITTER_HORIZONTAL, SPLITTER_VERTICAL } SplitterDir;

typedef struct {
    Widget base;
    SplitterDir direction;
    int thickness;
    int min_pos;
    int max_pos;
    int position;
    bool dragging;
    int drag_offset;
    Color color_bar;
    Color color_hover;
    Color color_drag;
    void (*on_drag_end)(int new_position, void* user_data);
} Splitter;

Splitter* splitter_create(int x, int y, int w, int h, SplitterDir dir);

#endif
