#ifndef GUI_H
#define GUI_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdbool.h>
#include "tabrt.h"
#include "keymap.h"

typedef struct {
    unsigned long bg;
    unsigned long text;
    unsigned long dim_text;
    unsigned long tab_bar_bg;
    unsigned long tab_active_bg;
    unsigned long tab_inactive_bg;
    unsigned long separator;
    unsigned long selection;
    unsigned long cursor;
} ThemeColors;

typedef struct Gui {
    Display *dpy;
    int screen;
    Window win;
    Pixmap backbuffer;
    GC gc;
    XFontStruct *font;
    int font_width;
    int font_height;
    int font_ascent;

    int width;
    int height;
    bool is_focused;
    bool running;

    int hovered_tab;
    bool hovered_close;
    bool hovered_plus;

    ThemeColors colors;
    TabRuntime *rt;

    Atom wm_protocols;
    Atom wm_delete_window;

    /* Keymap configuration */
    KeyMap keymap;
    char config_path[1024];

    /* Mouse Selection & Clipboard */
    bool selecting;
    int sel_start_vr;
    int sel_start_vc;
    int sel_end_vr;
    int sel_end_vc;
    bool has_selection;
    char *primary_selection_text;
    char *clipboard_selection_text;
    bool paste_pending;

    Atom atom_primary;
    Atom atom_clipboard;
    Atom atom_targets;
    Atom atom_utf8_string;
    Atom atom_terminator_selection;
} Gui;

Gui *gui_create(TabRuntime *rt);
void gui_destroy(Gui *gui);
int gui_run(Gui *gui);
void gui_reload_keymap(Gui *gui);

#endif /* GUI_H */
