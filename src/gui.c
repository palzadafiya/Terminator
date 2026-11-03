/*
 * gui.c: Pure Xlib graphical user interface and event loop.
 *
 * Implements the native X11 interface:
 * - Double-buffered offscreen pixmap rendering eliminating visual flicker.
 * - Interactive tab strip with tab creation (+), switching, closing (x), and title formatting.
 * - Event-driven poll() loop over ConnectionNumber(dpy) and tab IPC pipes.
 * - KeyPress dispatch through configurable keymaps and modal state machines (Ctrl+R, Tab completion).
 * - Mouse drag-selection and X11 PRIMARY and CLIPBOARD copy/paste integration.
 */

#define _GNU_SOURCE
#include "gui.h"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include "complete.h"

static bool is_all_whitespace(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

#define DEFAULT_WIDTH 900
#define DEFAULT_HEIGHT 560
#define MIN_WIDTH 400
#define MIN_HEIGHT 240
#define TAB_STRIP_HEIGHT 32
#define PADDING 10

static unsigned long alloc_rgb(Display *dpy, Colormap cmap, uint8_t r, uint8_t g, uint8_t b) {
    XColor xc;
    xc.red = (unsigned short)((r << 8) | r);
    xc.green = (unsigned short)((g << 8) | g);
    xc.blue = (unsigned short)((b << 8) | b);
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &xc)) {
        return xc.pixel;
    }
    return BlackPixel(dpy, DefaultScreen(dpy));
}

static void init_theme(Gui *gui) {
    Colormap cmap = DefaultColormap(gui->dpy, gui->screen);
    const char *theme_env = getenv("TERMINATOR_THEME");
    bool is_light = (theme_env && strcmp(theme_env, "light") == 0);

    if (is_light) {
        gui->colors.bg = alloc_rgb(gui->dpy, cmap, 0xFF, 0xFF, 0xFF);
        gui->colors.text = alloc_rgb(gui->dpy, cmap, 0x1D, 0x1D, 0x1F);
        gui->colors.dim_text = alloc_rgb(gui->dpy, cmap, 0x8E, 0x8E, 0x93);
        gui->colors.tab_bar_bg = alloc_rgb(gui->dpy, cmap, 0xE8, 0xE8, 0xE8);
        gui->colors.tab_active_bg = alloc_rgb(gui->dpy, cmap, 0xFF, 0xFF, 0xFF);
        gui->colors.tab_inactive_bg = alloc_rgb(gui->dpy, cmap, 0xE8, 0xE8, 0xE8);
        gui->colors.separator = alloc_rgb(gui->dpy, cmap, 0xD1, 0xD1, 0xD6);
        gui->colors.selection = alloc_rgb(gui->dpy, cmap, 0xB3, 0xD4, 0xFF);
        gui->colors.cursor = alloc_rgb(gui->dpy, cmap, 0x1D, 0x1D, 0x1F);
    } else {
        gui->colors.bg = alloc_rgb(gui->dpy, cmap, 0x1E, 0x1E, 0x1E);
        gui->colors.text = alloc_rgb(gui->dpy, cmap, 0xE6, 0xE6, 0xE6);
        gui->colors.dim_text = alloc_rgb(gui->dpy, cmap, 0x8E, 0x8E, 0x93);
        gui->colors.tab_bar_bg = alloc_rgb(gui->dpy, cmap, 0x2B, 0x2B, 0x2D);
        gui->colors.tab_active_bg = alloc_rgb(gui->dpy, cmap, 0x3C, 0x3C, 0x3E);
        gui->colors.tab_inactive_bg = alloc_rgb(gui->dpy, cmap, 0x2B, 0x2B, 0x2D);
        gui->colors.separator = alloc_rgb(gui->dpy, cmap, 0x3A, 0x3A, 0x3C);
        gui->colors.selection = alloc_rgb(gui->dpy, cmap, 0x3E, 0x5C, 0x8A);
        gui->colors.cursor = alloc_rgb(gui->dpy, cmap, 0xE6, 0xE6, 0xE6);
    }
}

static int s_font_probe_error = 0;
static int font_probe_error_handler(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    s_font_probe_error = 1;
    return 0;
}

static XFontStruct *try_load_font(Display *dpy, const char *name) {
    if (!name || !name[0]) return NULL;

    s_font_probe_error = 0;
    XSync(dpy, False);
    XErrorHandler old_h = XSetErrorHandler(font_probe_error_handler);
    XFontStruct *f = XLoadQueryFont(dpy, name);
    XSync(dpy, False);
    XSetErrorHandler(old_h);

    if (s_font_probe_error || !f) {
        if (f) {
            XFreeFont(dpy, f);
        }
        return NULL;
    }
    return f;
}

static XFontStruct *load_simple_font(Display *dpy) {
    const char *names[] = {
        "fixed",
        "7x13",
        "8x13",
        "9x15",
        "6x13",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        XFontStruct *f = try_load_font(dpy, names[i]);
        if (f) return f;
    }
    return NULL;
}

static uint32_t gui_resolve_keysym(const char *name) {
    if (!name || !name[0]) return 0;
    KeySym k = XStringToKeysym(name);
    if (k == NoSymbol) {
        if (strcasecmp(name, "enter") == 0 || strcasecmp(name, "return") == 0) return (uint32_t)XK_Return;
        if (strcasecmp(name, "backspace") == 0) return (uint32_t)XK_BackSpace;
        if (strcasecmp(name, "delete") == 0) return (uint32_t)XK_Delete;
        if (strcasecmp(name, "tab") == 0) return (uint32_t)XK_Tab;
        if (strcasecmp(name, "left") == 0) return (uint32_t)XK_Left;
        if (strcasecmp(name, "right") == 0) return (uint32_t)XK_Right;
        if (strcasecmp(name, "up") == 0) return (uint32_t)XK_Up;
        if (strcasecmp(name, "down") == 0) return (uint32_t)XK_Down;
        if (strcasecmp(name, "home") == 0) return (uint32_t)XK_Home;
        if (strcasecmp(name, "end") == 0) return (uint32_t)XK_End;
        if (strcasecmp(name, "page_up") == 0 || strcasecmp(name, "pageup") == 0) return (uint32_t)XK_Page_Up;
        if (strcasecmp(name, "page_down") == 0 || strcasecmp(name, "pagedown") == 0) return (uint32_t)XK_Page_Down;
    }
    return (uint32_t)k;
}

void gui_reload_keymap(Gui *gui) {
    if (!gui) return;
    char warns[1024] = {0};
    keymap_load_file(&gui->keymap, gui->config_path, warns, sizeof(warns));
    keymap_resolve_keysyms(&gui->keymap, gui_resolve_keysym);
    if (warns[0] != '\0' && gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tabbuf_append_line(tab->buf, warns);
    }
}

static void update_backbuffer(Gui *gui) {
    if (gui->backbuffer) {
        XFreePixmap(gui->dpy, gui->backbuffer);
    }
    gui->backbuffer = XCreatePixmap(gui->dpy, gui->win,
                                    (unsigned int)gui->width,
                                    (unsigned int)gui->height,
                                    (unsigned int)DefaultDepth(gui->dpy, gui->screen));
}

Gui *gui_create(TabRuntime *rt) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "terminator: cannot connect to X server %s\n", XDisplayName(NULL));
        return NULL;
    }

    Gui *gui = calloc(1, sizeof(Gui));
    if (!gui) {
        XCloseDisplay(dpy);
        return NULL;
    }

    gui->dpy = dpy;
    gui->screen = DefaultScreen(dpy);
    gui->rt = rt;
    gui->width = DEFAULT_WIDTH;
    gui->height = DEFAULT_HEIGHT;
    gui->is_focused = true;
    gui->running = true;
    gui->hovered_tab = -1;
    gui->hovered_close = false;
    gui->hovered_plus = false;

    init_theme(gui);

    gui->atom_primary = XA_PRIMARY;
    gui->atom_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    gui->atom_targets = XInternAtom(dpy, "TARGETS", False);
    gui->atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    gui->atom_terminator_selection = XInternAtom(dpy, "TERMINATOR_SELECTION", False);

    gui->font = load_simple_font(dpy);
    if (gui->font) {
        gui->font_width = gui->font->max_bounds.width > 0 ? gui->font->max_bounds.width : XTextWidth(gui->font, "M", 1);
        gui->font_ascent = gui->font->ascent;
        gui->font_height = gui->font->ascent + gui->font->descent + 2;
    } else {
        gui->font_width = 8;
        gui->font_ascent = 12;
        gui->font_height = 16;
    }

    Window root = RootWindow(dpy, gui->screen);
    gui->win = XCreateSimpleWindow(dpy, root, 100, 100,
                                   (unsigned int)gui->width,
                                   (unsigned int)gui->height,
                                   0, gui->colors.bg, gui->colors.bg);

    XSelectInput(dpy, gui->win,
                 KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                 ExposureMask | StructureNotifyMask | FocusChangeMask);

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    if (gui->font) gcv.font = gui->font->fid;
    gui->gc = XCreateGC(dpy, gui->win, gui->font ? GCFont : 0, &gcv);

    update_backbuffer(gui);

    /* WM Integration */
    XStoreName(dpy, gui->win, "Terminator");
    XClassHint class_hint = {"terminator", "Terminator"};
    XSetClassHint(dpy, gui->win, &class_hint);

    gui->wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    gui->wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, gui->win, &gui->wm_delete_window, 1);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PResizeInc;
    hints.min_width = MIN_WIDTH;
    hints.min_height = MIN_HEIGHT;
    hints.width_inc = gui->font_width;
    hints.height_inc = gui->font_height;
    XSetWMNormalHints(dpy, gui->win, &hints);

    XMapWindow(dpy, gui->win);

    keymap_get_config_path(gui->config_path, sizeof(gui->config_path));
    char initial_warns[1024] = {0};
    keymap_load_file(&gui->keymap, gui->config_path, initial_warns, sizeof(initial_warns));
    keymap_resolve_keysyms(&gui->keymap, gui_resolve_keysym);

    if (initial_warns[0] != '\0' && gui->rt->tab_count > 0) {
        tabbuf_append_line(gui->rt->tabs[0]->buf, initial_warns);
    }

    return gui;
}

void gui_destroy(Gui *gui) {
    if (!gui) return;
    if (gui->primary_selection_text) free(gui->primary_selection_text);
    if (gui->clipboard_selection_text) free(gui->clipboard_selection_text);
    if (gui->backbuffer) XFreePixmap(gui->dpy, gui->backbuffer);
    if (gui->gc) XFreeGC(gui->dpy, gui->gc);
    if (gui->font) XFreeFont(gui->dpy, gui->font);
    if (gui->win) XDestroyWindow(gui->dpy, gui->win);
    if (gui->dpy) XCloseDisplay(gui->dpy);
    free(gui);
}

static int get_tab_width(const Gui *gui) {
    if (!gui->rt || gui->rt->tab_count == 0) return 120;
    int plus_w = 32;
    int w = (gui->width - plus_w) / gui->rt->tab_count;
    if (w > 200) w = 200;
    if (w < 80) w = 80;
    return w;
}

static void draw_rounded_tab(Gui *gui, int x, int y, int w, int h, unsigned long color) {
    int r = 6;
    XSetForeground(gui->dpy, gui->gc, color);
    /* Top-left corner */
    XFillArc(gui->dpy, gui->backbuffer, gui->gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    /* Top-right corner */
    XFillArc(gui->dpy, gui->backbuffer, gui->gc, x + w - 2 * r, y, 2 * r, 2 * r, 0 * 64, 90 * 64);
    /* Top center rectangle */
    XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, x + r, y, (unsigned int)(w - 2 * r), (unsigned int)r);
    /* Body rectangle */
    XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, x, y + r, (unsigned int)w, (unsigned int)(h - r));
}

static void draw_truncated_string(Gui *gui, int x, int y, int max_w, const char *text, unsigned long color) {
    XSetForeground(gui->dpy, gui->gc, color);
    int len = (int)strlen(text);
    int text_w = XTextWidth(gui->font, text, len);

    if (text_w <= max_w) {
        XDrawString(gui->dpy, gui->backbuffer, gui->gc, x, y, text, len);
        return;
    }

    /* Truncate with "..." */
    char buf[128];
    strncpy(buf, text, sizeof(buf) - 4);
    buf[sizeof(buf) - 4] = '\0';
    int cur_len = (int)strlen(buf);

    while (cur_len > 0) {
        buf[cur_len] = '\0';
        strcat(buf, "...");
        if (XTextWidth(gui->font, buf, (int)strlen(buf)) <= max_w) {
            XDrawString(gui->dpy, gui->backbuffer, gui->gc, x, y, buf, (int)strlen(buf));
            return;
        }
        buf[cur_len] = '\0';
        cur_len--;
    }
}

static void gui_redraw(Gui *gui) {
    if (!gui || !gui->backbuffer) return;

    /* Fill background */
    XSetForeground(gui->dpy, gui->gc, gui->colors.bg);
    XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, 0, 0,
                   (unsigned int)gui->width, (unsigned int)gui->height);

    /* Draw Tab Strip */
    XSetForeground(gui->dpy, gui->gc, gui->colors.tab_bar_bg);
    XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, 0, 0,
                   (unsigned int)gui->width, TAB_STRIP_HEIGHT);

    int tab_w = get_tab_width(gui);
    int tab_count = gui->rt->tab_count;

    for (int i = 0; i < tab_count; i++) {
        Tab *tab = gui->rt->tabs[i];
        int tx = i * tab_w;
        int ty = 4;
        int th = TAB_STRIP_HEIGHT - 4;
        bool is_active = (i == gui->rt->active_idx);
        bool is_hovered = (i == gui->hovered_tab);

        if (is_active) {
            draw_rounded_tab(gui, tx, ty, tab_w, th, gui->colors.tab_active_bg);
        } else if (is_hovered) {
            XSetForeground(gui->dpy, gui->gc, gui->colors.tab_active_bg);
            XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, tx, ty, (unsigned int)tab_w, (unsigned int)th);
        }

        /* Draw tab label */
        int label_x = tx + 10;
        int label_y = ty + gui->font_ascent + 4;
        int max_label_w = tab_w - 32;
        unsigned long text_col = is_active ? gui->colors.text : gui->colors.dim_text;
        draw_truncated_string(gui, label_x, label_y, max_label_w, tab->label, text_col);

        /* Draw close glyph 'x' if active or hovered */
        if (is_active || is_hovered) {
            int cx = tx + tab_w - 18;
            int cy = ty + 7;
            unsigned long close_col = (is_hovered && gui->hovered_close) ? gui->colors.text : gui->colors.dim_text;
            XSetForeground(gui->dpy, gui->gc, close_col);
            XDrawLine(gui->dpy, gui->backbuffer, gui->gc, cx, cy, cx + 7, cy + 7);
            XDrawLine(gui->dpy, gui->backbuffer, gui->gc, cx + 7, cy, cx, cy + 7);
        }
    }

    /* Draw '+' button */
    int plus_x = tab_count * tab_w + 6;
    int plus_y = 6;
    unsigned long plus_col = gui->hovered_plus ? gui->colors.text : gui->colors.dim_text;
    XSetForeground(gui->dpy, gui->gc, plus_col);
    XDrawLine(gui->dpy, gui->backbuffer, gui->gc, plus_x + 8, plus_y + 4, plus_x + 8, plus_y + 14);
    XDrawLine(gui->dpy, gui->backbuffer, gui->gc, plus_x + 3, plus_y + 9, plus_x + 13, plus_y + 9);

    /* Draw separator under tab strip */
    XSetForeground(gui->dpy, gui->gc, gui->colors.separator);
    XDrawLine(gui->dpy, gui->backbuffer, gui->gc, 0, TAB_STRIP_HEIGHT - 1, gui->width, TAB_STRIP_HEIGHT - 1);

    /* Render Active Tab's Terminal Area */
    if (gui->rt->active_idx >= 0 && gui->rt->active_idx < tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        int term_x = PADDING;
        int term_y = TAB_STRIP_HEIGHT + PADDING;
        int avail_w = gui->width - 2 * PADDING;
        int avail_h = gui->height - term_y - PADDING;

        int cols = avail_w / gui->font_width;
        int rows = avail_h / gui->font_height;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;

        /* Build live input line text */
        char full_input[LINEEDIT_MAX + 512];
        snprintf(full_input, sizeof(full_input), "%s%s", tab->prompt, lineedit_get_text(&tab->input));
        size_t full_len = strlen(full_input);
        size_t prompt_len = strlen(tab->prompt);

        size_t vlines_in = (full_len == 0) ? 1 : ((full_len + (size_t)cols - 1) / (size_t)cols);
        size_t total_vlines_sb = tabbuf_total_visual_lines(tab->buf, (size_t)cols);
        size_t total_vlines = total_vlines_sb + vlines_in;

        size_t visible_rows = (size_t)rows;
        size_t start_v = 0;

        if (total_vlines > visible_rows) {
            size_t max_scroll = total_vlines - visible_rows;
            if (tab->buf->scroll_offset > max_scroll) {
                tab->buf->scroll_offset = max_scroll;
            }
            start_v = total_vlines - visible_rows - tab->buf->scroll_offset;
        } else {
            tab->buf->scroll_offset = 0;
            start_v = 0;
        }

        size_t rows_to_draw = total_vlines < visible_rows ? total_vlines : visible_rows;

        /* Draw visual lines sequentially */
        for (size_t r = 0; r < rows_to_draw; r++) {
            size_t v_idx = start_v + r;
            int line_y = term_y + (int)r * gui->font_height + gui->font_ascent;

            if (gui->has_selection) {
                size_t r1 = (size_t)gui->sel_start_vr, c1 = (size_t)gui->sel_start_vc;
                size_t r2 = (size_t)gui->sel_end_vr, c2 = (size_t)gui->sel_end_vc;
                if (r1 > r2 || (r1 == r2 && c1 > c2)) {
                    size_t tr = r1; r1 = r2; r2 = tr;
                    size_t tc = c1; c1 = c2; c2 = tc;
                }
                if (v_idx >= r1 && v_idx <= r2) {
                    size_t sc = (v_idx == r1) ? c1 : 0;
                    size_t ec = (v_idx == r2) ? c2 : (size_t)cols;
                    if (ec > sc) {
                        int hl_x = term_x + (int)sc * gui->font_width;
                        int hl_y = term_y + (int)r * gui->font_height;
                        int hl_w = (int)(ec - sc) * gui->font_width;
                        XSetForeground(gui->dpy, gui->gc, gui->colors.selection);
                        XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, hl_x, hl_y,
                                       (unsigned int)hl_w, (unsigned int)gui->font_height);
                    }
                }
            }

            if (v_idx < total_vlines_sb) {
                /* Scrollback line from buffer */
                const char *str = NULL;
                size_t slen = 0;
                tabbuf_get_visual_line(tab->buf, (size_t)cols, v_idx, &str, &slen);
                if (str && slen > 0) {
                    XSetForeground(gui->dpy, gui->gc, gui->colors.text);
                    XDrawString(gui->dpy, gui->backbuffer, gui->gc, term_x, line_y, str, (int)slen);
                }
            } else {
                /* Live input line */
                size_t input_vr = v_idx - total_vlines_sb;
                size_t char_offset = input_vr * (size_t)cols;
                if (char_offset < full_len) {
                    size_t chunk_len = full_len - char_offset;
                    if (chunk_len > (size_t)cols) chunk_len = (size_t)cols;
                    XSetForeground(gui->dpy, gui->gc, gui->colors.text);
                    XDrawString(gui->dpy, gui->backbuffer, gui->gc, term_x, line_y,
                                full_input + char_offset, (int)chunk_len);
                }

                /* Cursor on this visual row */
                size_t cursor_col = prompt_len + tab->input.cursor;
                size_t cur_vr = cursor_col / (size_t)cols;
                if (cur_vr == input_vr) {
                    size_t cur_vc = cursor_col % (size_t)cols;
                    int cur_x = term_x + (int)cur_vc * gui->font_width;
                    int cur_box_y = term_y + (int)r * gui->font_height;

                    if (gui->is_focused) {
                        XSetForeground(gui->dpy, gui->gc, gui->colors.cursor);
                        XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, cur_x, cur_box_y,
                                       (unsigned int)gui->font_width, (unsigned int)gui->font_height);

                        char under = ' ';
                        if (tab->input.cursor < tab->input.len) {
                            under = tab->input.buf[tab->input.cursor];
                        }
                        XSetForeground(gui->dpy, gui->gc, gui->colors.bg);
                        XDrawString(gui->dpy, gui->backbuffer, gui->gc, cur_x, line_y, &under, 1);
                    } else {
                        XSetForeground(gui->dpy, gui->gc, gui->colors.cursor);
                        XDrawRectangle(gui->dpy, gui->backbuffer, gui->gc, cur_x, cur_box_y,
                                       (unsigned int)(gui->font_width - 1), (unsigned int)(gui->font_height - 1));
                    }
                }
            }
        }

        /* Draw slim scrollbar thumb if scrolled up */
        if (tab->buf->scroll_offset > 0 && total_vlines > visible_rows) {
            int sb_track_h = avail_h;
            int sb_thumb_h = (int)((visible_rows * (size_t)sb_track_h) / total_vlines);
            if (sb_thumb_h < 20) sb_thumb_h = 20;

            int max_thumb_travel = sb_track_h - sb_thumb_h;
            size_t max_scroll = total_vlines - visible_rows;
            int sb_thumb_y = term_y + max_thumb_travel - (int)((tab->buf->scroll_offset * (size_t)max_thumb_travel) / max_scroll);

            XSetForeground(gui->dpy, gui->gc, gui->colors.dim_text);
            XFillRectangle(gui->dpy, gui->backbuffer, gui->gc, gui->width - 6, sb_thumb_y, 4, (unsigned int)sb_thumb_h);
        }
    }

    /* Copy offscreen backbuffer to window */
    XCopyArea(gui->dpy, gui->backbuffer, gui->win, gui->gc,
              0, 0, (unsigned int)gui->width, (unsigned int)gui->height, 0, 0);
}

/* Key Action Handlers for extensible Key Dispatch Table */
static void action_spawn_tab(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    tabrt_spawn(gui->rt, ConnectionNumber(gui->dpy));
}

static void action_close_tab(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->tab_count > 0) {
        if (tabrt_close(gui->rt, gui->rt->active_idx)) {
            gui->running = false;
        }
    }
}

static void action_switch_tab(Gui *gui, KeySym k, unsigned int state) {
    (void)state;
    int target = (int)(k - XK_1);
    if (target >= 0 && target < gui->rt->tab_count) {
        gui->rt->active_idx = target;
    }
}

static void action_prev_tab(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->tab_count > 1) {
        gui->rt->active_idx = (gui->rt->active_idx + gui->rt->tab_count - 1) % gui->rt->tab_count;
    }
}

static void action_next_tab(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->tab_count > 1) {
        gui->rt->active_idx = (gui->rt->active_idx + 1) % gui->rt->tab_count;
    }
}

static size_t get_tab_total_visual_lines(const Tab *tab, int cols) {
    if (!tab) return 0;
    size_t full_len = strlen(tab->prompt) + tab->input.len;
    size_t vlines_in = (full_len == 0) ? 1 : ((full_len + (size_t)cols - 1) / (size_t)cols);
    return tabbuf_total_visual_lines(tab->buf, (size_t)cols) + vlines_in;
}

static void action_scroll_page_up(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        int avail_h = gui->height - TAB_STRIP_HEIGHT - 2 * PADDING;
        int rows = avail_h / gui->font_height;
        int cols = (gui->width - 2 * PADDING) / gui->font_width;
        size_t total_v = get_tab_total_visual_lines(tab, cols > 0 ? cols : 80);
        tabbuf_scroll_up(tab->buf, rows > 0 ? (size_t)rows : 1, total_v, rows > 0 ? (size_t)rows : 1);
    }
}

static void action_scroll_page_down(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        int avail_h = gui->height - TAB_STRIP_HEIGHT - 2 * PADDING;
        int rows = avail_h / gui->font_height;
        tabbuf_scroll_down(tab->buf, rows > 0 ? (size_t)rows : 1);
    }
}

static void action_return(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];

    /* Reset history browsing state */
    tab->hist_browsing = false;
    tab->hist_seq = 0;
    if (tab->compl_state != COMPL_STATE_NONE) {
        completion_result_free(&tab->compl_result);
        tab->compl_state = COMPL_STATE_NONE;
        tab->compl_digits[0] = '\0';
        tab->compl_digits_len = 0;
    }

    if (tab->search_mode) {
        const char *term = lineedit_get_text(&tab->input);
        char committed[LINEEDIT_MAX + 512];
        snprintf(committed, sizeof(committed), "%s%s", tab->prompt, term);
        tabbuf_append_line(tab->buf, committed);

        bool empty = is_all_whitespace(term);
        if (!empty) {
            HistSearchResult res;
            memset(&res, 0, sizeof(res));
            if (history_search(gui->rt->shared, term, &res) == 0) {
                if (res.match_type == HIST_MATCH_EXACT) {
                    if (res.count > 0 && res.entries[0]) {
                        tabbuf_append_line(tab->buf, res.entries[0]);
                    }
                } else if (res.match_type == HIST_MATCH_SUBSTRING) {
                    for (size_t i = 0; i < res.count; i++) {
                        if (res.entries[i]) {
                            tabbuf_append_line(tab->buf, res.entries[i]);
                        }
                    }
                } else {
                    tabbuf_append_line(tab->buf, "No match for search term in history");
                }
                hist_search_result_free(&res);
            } else {
                tabbuf_append_line(tab->buf, "No match for search term in history");
            }
        }

        tab->search_mode = false;
        strncpy(tab->prompt, tab->saved_prompt, sizeof(tab->prompt) - 1);
        tab->prompt[sizeof(tab->prompt) - 1] = '\0';
        lineedit_clear(&tab->input);
        tabbuf_scroll_to_bottom(tab->buf);
        return;
    }

    const char *line = lineedit_get_text(&tab->input);
    char committed[LINEEDIT_MAX + 512];
    snprintf(committed, sizeof(committed), "%s%s", tab->prompt, line);
    tabbuf_append_line(tab->buf, committed);

    /* Send LINE frame to tab */
    proto_queue_frame(&tab->pending_out, FRAME_LINE, line, (uint32_t)strlen(line));
    proto_flush_queue(tab->to_tab_fd, &tab->pending_out);

    lineedit_clear(&tab->input);
    tab->prompt[0] = '\0';
    tabbuf_scroll_to_bottom(tab->buf);
}

static void action_backspace(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        lineedit_backspace(&tab->input);
        tabbuf_scroll_to_bottom(tab->buf);
    }
}

static void action_delete(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        lineedit_delete(&tab->input);
    }
}

static void action_left(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        lineedit_move_left(&gui->rt->tabs[gui->rt->active_idx]->input);
    }
}

static void action_right(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        lineedit_move_right(&gui->rt->tabs[gui->rt->active_idx]->input);
    }
}

static void action_home(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        lineedit_line_start(&gui->rt->tabs[gui->rt->active_idx]->input);
    }
}

static void action_end(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        lineedit_line_end(&gui->rt->tabs[gui->rt->active_idx]->input);
    }
}

static void action_kill_to_start(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        lineedit_kill_to_start(&tab->input);
    }
}

static void action_kill_to_end(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        lineedit_kill_to_end(&tab->input);
    }
}

static void action_delete_prev_word(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        lineedit_delete_prev_word(&tab->input);
    }
}

static void action_clear_screen(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tabbuf_clear(tab->buf);
        tabbuf_scroll_to_bottom(tab->buf);
    }
}

static void action_search(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];
    if (tab->search_mode) return;

    tab->search_mode = true;
    tab->hist_browsing = false;
    tab->hist_seq = 0;

    strncpy(tab->saved_prompt, tab->prompt, sizeof(tab->saved_prompt) - 1);
    tab->saved_prompt[sizeof(tab->saved_prompt) - 1] = '\0';
    strncpy(tab->saved_input_before_search, lineedit_get_text(&tab->input), sizeof(tab->saved_input_before_search) - 1);
    tab->saved_input_before_search[sizeof(tab->saved_input_before_search) - 1] = '\0';

    snprintf(tab->prompt, sizeof(tab->prompt), "Enter search term: ");
    lineedit_clear(&tab->input);
    tabbuf_scroll_to_bottom(tab->buf);
}

static void action_tab_complete(Gui *gui) {
    if (gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];
    if (tab->search_mode) return;

    /* Cancel previous completion state if any */
    if (tab->compl_state != COMPL_STATE_NONE) {
        completion_result_free(&tab->compl_result);
        tab->compl_state = COMPL_STATE_NONE;
        tab->compl_digits[0] = '\0';
        tab->compl_digits_len = 0;
    }

    CompletionResult cr;
    memset(&cr, 0, sizeof(cr));

    const char *line = lineedit_get_text(&tab->input);
    size_t cursor = tab->input.cursor;

    if (complete_file_candidates(line, cursor, tab->cwd, &cr) != 0 || cr.count == 0) {
        completion_result_free(&cr);
        return;
    }

    if (cr.count == 1) {
        const char *cand = cr.items[0];
        char escaped[2048];
        complete_escape_filename(cand, escaped, sizeof(escaped));
        char repl[4096];
        snprintf(repl, sizeof(repl), "%s%s", cr.dir_prefix, escaped);
        lineedit_replace_range(&tab->input, cr.word_start, cr.word_start + cr.word_len, repl);
        tabbuf_scroll_to_bottom(tab->buf);
        completion_result_free(&cr);
        return;
    }

    /* Several matches */
    size_t lcp_len = strlen(cr.lcp);
    size_t typed_len = strlen(cr.unescaped_prefix);

    if (lcp_len > typed_len) {
        char esc_lcp[2048];
        complete_escape_filename(cr.lcp, esc_lcp, sizeof(esc_lcp));
        char repl[4096];
        snprintf(repl, sizeof(repl), "%s%s", cr.dir_prefix, esc_lcp);
        lineedit_replace_range(&tab->input, cr.word_start, cr.word_start + cr.word_len, repl);
        tabbuf_scroll_to_bottom(tab->buf);
        completion_result_free(&cr);
        return;
    }

    /* LCP == current word (ambiguous) */
    if (cr.count > 100) {
        char committed[LINEEDIT_MAX + 512];
        snprintf(committed, sizeof(committed), "%s%s", tab->prompt, lineedit_get_text(&tab->input));
        tabbuf_append_line(tab->buf, committed);

        char ask[128];
        snprintf(ask, sizeof(ask), "Display all %zu possibilities? (y or n)", cr.count);
        tabbuf_append_line(tab->buf, ask);

        tab->compl_result = cr;
        tab->compl_state = COMPL_STATE_ASK_CONFIRM;
        tabbuf_scroll_to_bottom(tab->buf);
        return;
    }

    /* Print numbered list on output as "1. abc.txt  2. abcd.txt" */
    char committed[LINEEDIT_MAX + 512];
    snprintf(committed, sizeof(committed), "%s%s", tab->prompt, lineedit_get_text(&tab->input));
    tabbuf_append_line(tab->buf, committed);

    int avail_w = gui->width - 2 * PADDING;
    int cols = avail_w / (gui->font_width > 0 ? gui->font_width : 8);
    if (cols < 20) cols = 80;

    char *list_str = complete_format_numbered_list(&cr, cols);
    if (list_str) {
        tabbuf_append_line(tab->buf, list_str);
        free(list_str);
    }

    tab->compl_result = cr;
    tab->compl_state = COMPL_STATE_SELECT;
    tab->compl_digits[0] = '\0';
    tab->compl_digits_len = 0;
    tabbuf_scroll_to_bottom(tab->buf);
}

static void action_history_prev(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];
    if (!gui->rt->shared || tab->search_mode) return;

    if (!tab->hist_browsing) {
        strncpy(tab->saved_input, lineedit_get_text(&tab->input), sizeof(tab->saved_input) - 1);
        tab->saved_input[sizeof(tab->saved_input) - 1] = '\0';
        uint64_t newest = history_newest_seq(gui->rt->shared);
        if (newest > 0) {
            tab->hist_seq = newest;
            tab->hist_browsing = true;
            char hbuf[HISTORY_MAX_LINE];
            if (history_get_by_seq(gui->rt->shared, tab->hist_seq, hbuf, sizeof(hbuf)) == 0) {
                lineedit_set_text(&tab->input, hbuf);
                lineedit_move_end(&tab->input);
            }
        }
    } else {
        uint64_t oldest = history_oldest_seq(gui->rt->shared);
        if (tab->hist_seq > oldest) {
            uint64_t target = tab->hist_seq - 1;
            char hbuf[HISTORY_MAX_LINE];
            while (target >= oldest) {
                if (history_get_by_seq(gui->rt->shared, target, hbuf, sizeof(hbuf)) == 0) {
                    tab->hist_seq = target;
                    lineedit_set_text(&tab->input, hbuf);
                    lineedit_move_end(&tab->input);
                    break;
                }
                if (target == 0) break;
                target--;
            }
        }
    }
}

static void action_history_next(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];
    if (!gui->rt->shared || tab->search_mode) return;

    if (tab->hist_browsing) {
        uint64_t newest = history_newest_seq(gui->rt->shared);
        if (tab->hist_seq < newest) {
            uint64_t target = tab->hist_seq + 1;
            char hbuf[HISTORY_MAX_LINE];
            bool found = false;
            while (target <= newest) {
                if (history_get_by_seq(gui->rt->shared, target, hbuf, sizeof(hbuf)) == 0) {
                    tab->hist_seq = target;
                    lineedit_set_text(&tab->input, hbuf);
                    lineedit_move_end(&tab->input);
                    found = true;
                    break;
                }
                target++;
            }
            if (!found) {
                lineedit_set_text(&tab->input, tab->saved_input);
                lineedit_move_end(&tab->input);
                tab->hist_browsing = false;
                tab->hist_seq = 0;
            }
        } else {
            lineedit_set_text(&tab->input, tab->saved_input);
            lineedit_move_end(&tab->input);
            tab->hist_browsing = false;
            tab->hist_seq = 0;
        }
    }
}

static void gui_extract_selection(Gui *gui) {
    if (!gui || !gui->has_selection || gui->rt->active_idx >= gui->rt->tab_count) return;
    Tab *tab = gui->rt->tabs[gui->rt->active_idx];
    if (!tab) return;

    int cols = (gui->width - 2 * PADDING) / gui->font_width;
    if (cols < 1) cols = 1;

    char full_input[LINEEDIT_MAX + 512];
    snprintf(full_input, sizeof(full_input), "%s%s", tab->prompt, lineedit_get_text(&tab->input));
    size_t full_len = strlen(full_input);
    size_t total_vlines_sb = tabbuf_total_visual_lines(tab->buf, (size_t)cols);

    size_t r1 = (size_t)gui->sel_start_vr, c1 = (size_t)gui->sel_start_vc;
    size_t r2 = (size_t)gui->sel_end_vr, c2 = (size_t)gui->sel_end_vc;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        size_t tr = r1; r1 = r2; r2 = tr;
        size_t tc = c1; c1 = c2; c2 = tc;
    }

    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) return;
    size_t len = 0;

    for (size_t v = r1; v <= r2 && len < cap - 2; v++) {
        const char *str = NULL;
        size_t slen = 0;

        if (v < total_vlines_sb) {
            tabbuf_get_visual_line(tab->buf, (size_t)cols, v, &str, &slen);
        } else {
            size_t in_vr = v - total_vlines_sb;
            size_t off = in_vr * (size_t)cols;
            if (off < full_len) {
                str = full_input + off;
                slen = full_len - off;
                if (slen > (size_t)cols) slen = (size_t)cols;
            }
        }

        size_t sc = (v == r1) ? c1 : 0;
        size_t ec = (v == r2) ? c2 : slen;
        if (ec > slen) ec = slen;

        if (str && ec > sc) {
            size_t chunk = ec - sc;
            if (len + chunk >= cap - 2) chunk = cap - 2 - len;
            memcpy(buf + len, str + sc, chunk);
            len += chunk;
        }

        if (v < r2 && len < cap - 1) {
            buf[len++] = '\n';
        }
    }

    buf[len] = '\0';

    if (gui->primary_selection_text) free(gui->primary_selection_text);
    gui->primary_selection_text = buf;

    XSetSelectionOwner(gui->dpy, XA_PRIMARY, gui->win, CurrentTime);
    XFlush(gui->dpy);
}

static void action_copy(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->has_selection && gui->primary_selection_text) {
        if (gui->clipboard_selection_text) free(gui->clipboard_selection_text);
        gui->clipboard_selection_text = strdup(gui->primary_selection_text);
        XSetSelectionOwner(gui->dpy, gui->atom_clipboard, gui->win, CurrentTime);
        XFlush(gui->dpy);
    }
}

static void action_paste(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    XConvertSelection(gui->dpy, gui->atom_clipboard, gui->atom_utf8_string,
                      gui->atom_terminator_selection, gui->win, CurrentTime);
    XFlush(gui->dpy);
}

static void action_eof(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        if (tab->input.len == 0) {
            proto_queue_frame(&tab->pending_out, FRAME_EOF, NULL, 0);
            proto_flush_queue(tab->to_tab_fd, &tab->pending_out);
        } else {
            tab->hist_browsing = false;
            tab->hist_seq = 0;
            lineedit_delete(&tab->input);
        }
    }
}

static void action_intr(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        if (tab->search_mode) {
            char committed[LINEEDIT_MAX + 512];
            snprintf(committed, sizeof(committed), "%s%s^C", tab->prompt, lineedit_get_text(&tab->input));
            tabbuf_append_line(tab->buf, committed);
            tab->search_mode = false;
            strncpy(tab->prompt, tab->saved_prompt, sizeof(tab->prompt) - 1);
            tab->prompt[sizeof(tab->prompt) - 1] = '\0';
            lineedit_clear(&tab->input);
            tabbuf_scroll_to_bottom(tab->buf);
            return;
        }
        char committed[LINEEDIT_MAX + 512];
        snprintf(committed, sizeof(committed), "%s%s^C", tab->prompt, lineedit_get_text(&tab->input));
        tabbuf_append_line(tab->buf, committed);
        lineedit_clear(&tab->input);
        tab->prompt[0] = '\0';
        tabbuf_scroll_to_bottom(tab->buf);
        proto_queue_frame(&tab->pending_out, FRAME_INTR, NULL, 0);
        proto_flush_queue(tab->to_tab_fd, &tab->pending_out);
    }
}

static void action_tstp(Gui *gui, KeySym k, unsigned int state) {
    (void)k; (void)state;
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        if (tab->search_mode) return;
        tab->hist_browsing = false;
        tab->hist_seq = 0;
        if (tab->compl_state != COMPL_STATE_NONE) {
            completion_result_free(&tab->compl_result);
            tab->compl_state = COMPL_STATE_NONE;
            tab->compl_digits[0] = '\0';
            tab->compl_digits_len = 0;
        }
        if (tab->prompt[0] || tab->input.len > 0) {
            char committed[LINEEDIT_MAX + 512];
            snprintf(committed, sizeof(committed), "%s%s^Z", tab->prompt, lineedit_get_text(&tab->input));
            tabbuf_append_line(tab->buf, committed);
        } else {
            tabbuf_append_bytes(tab->buf, "^Z", 2);
        }
        lineedit_clear(&tab->input);
        tab->prompt[0] = '\0';
        tabbuf_scroll_to_bottom(tab->buf);
        proto_queue_frame(&tab->pending_out, FRAME_TSTP, NULL, 0);
        proto_flush_queue(tab->to_tab_fd, &tab->pending_out);
    }
}

typedef void (*ActionHandler)(Gui *gui, KeySym k, unsigned int state);
static const ActionHandler s_action_handlers[ACTION_COUNT] = {
    [ACTION_NONE] = NULL,
    [ACTION_LINE_START] = action_home,
    [ACTION_LINE_END] = action_end,
    [ACTION_KILL_TO_START] = action_kill_to_start,
    [ACTION_KILL_TO_END] = action_kill_to_end,
    [ACTION_DELETE_PREV_WORD] = action_delete_prev_word,
    [ACTION_CLEAR_SCREEN] = action_clear_screen,
    [ACTION_HISTORY_PREV] = action_history_prev,
    [ACTION_HISTORY_NEXT] = action_history_next,
    [ACTION_COPY] = action_copy,
    [ACTION_PASTE] = action_paste,
    [ACTION_EOF] = action_eof,
    [ACTION_MOVE_LEFT] = action_left,
    [ACTION_MOVE_RIGHT] = action_right,
    [ACTION_BACKSPACE] = action_backspace,
    [ACTION_DELETE] = action_delete,
    [ACTION_RETURN] = action_return
};

static void handle_key_press(Gui *gui, XKeyEvent *e) {
    char buf[64];
    KeySym k = 0;
    int n = XLookupString(e, buf, sizeof(buf), &k, NULL);

    /* Normalize modifiers mask to relevant modifiers (ignore NumLock Mod2Mask and CapsLock LockMask) */
    unsigned int norm_mods = KM_MOD_NONE;
    if (e->state & ControlMask) norm_mods |= KM_MOD_CTRL;
    if (e->state & ShiftMask) norm_mods |= KM_MOD_SHIFT;
    if (e->state & Mod1Mask) norm_mods |= KM_MOD_ALT;
    if (e->state & Mod4Mask) norm_mods |= KM_MOD_SUPER;

    /* Base keysym independent of Shift: convert uppercase letter keysym to lowercase */
    if (k >= XK_A && k <= XK_Z) {
        k = (k - XK_A) + XK_a;
    }

    /* 1. Reserved Chords (tab management & system signals) */
    if (norm_mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && (k == XK_t || k == XK_T)) {
        action_spawn_tab(gui, k, norm_mods);
        return;
    }
    if (norm_mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && (k == XK_w || k == XK_W)) {
        action_close_tab(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_ALT && k >= XK_1 && k <= XK_9) {
        action_switch_tab(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_CTRL && k == XK_Page_Up) {
        action_prev_tab(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_CTRL && k == XK_Page_Down) {
        action_next_tab(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_SHIFT && k == XK_Page_Up) {
        action_scroll_page_up(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_SHIFT && k == XK_Page_Down) {
        action_scroll_page_down(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_CTRL && (k == XK_c || k == XK_C)) {
        action_intr(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_CTRL && (k == XK_z || k == XK_Z)) {
        action_tstp(gui, k, norm_mods);
        return;
    }
    if (norm_mods == KM_MOD_CTRL && (k == XK_r || k == XK_R)) {
        action_search(gui, k, norm_mods);
        return;
    }

    /* Modal completion mode special key handling */
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        if (tab->compl_state == COMPL_STATE_ASK_CONFIRM) {
            if (n > 0 && (buf[0] == 'y' || buf[0] == 'Y')) {
                int avail_w = gui->width - 2 * PADDING;
                int cols = avail_w / (gui->font_width > 0 ? gui->font_width : 8);
                if (cols < 20) cols = 80;
                char *list_str = complete_format_numbered_list(&tab->compl_result, cols);
                if (list_str) {
                    tabbuf_append_line(tab->buf, list_str);
                    free(list_str);
                }
                tab->compl_state = COMPL_STATE_SELECT;
                tab->compl_digits[0] = '\0';
                tab->compl_digits_len = 0;
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            } else {
                completion_result_free(&tab->compl_result);
                tab->compl_state = COMPL_STATE_NONE;
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            }
        } else if (tab->compl_state == COMPL_STATE_SELECT) {
            if (n > 0 && isdigit((unsigned char)buf[0])) {
                if (tab->compl_digits_len + 1 < sizeof(tab->compl_digits)) {
                    tab->compl_digits[tab->compl_digits_len++] = buf[0];
                    tab->compl_digits[tab->compl_digits_len] = '\0';
                }
                return;
            } else if (k == XK_Return || k == XK_KP_Enter || (n > 0 && (buf[0] == '\r' || buf[0] == '\n'))) {
                if (tab->compl_digits_len > 0) {
                    long sel = strtol(tab->compl_digits, NULL, 10);
                    if (sel >= 1 && (size_t)sel <= tab->compl_result.count) {
                        const char *chosen = tab->compl_result.items[sel - 1];
                        char escaped[2048];
                        complete_escape_filename(chosen, escaped, sizeof(escaped));
                        char repl[4096];
                        snprintf(repl, sizeof(repl), "%s%s", tab->compl_result.dir_prefix, escaped);
                        lineedit_replace_range(&tab->input, tab->compl_result.word_start,
                                               tab->compl_result.word_start + tab->compl_result.word_len,
                                               repl);
                    }
                }
                completion_result_free(&tab->compl_result);
                tab->compl_state = COMPL_STATE_NONE;
                tab->compl_digits[0] = '\0';
                tab->compl_digits_len = 0;
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            } else if (k == XK_Escape) {
                completion_result_free(&tab->compl_result);
                tab->compl_state = COMPL_STATE_NONE;
                tab->compl_digits[0] = '\0';
                tab->compl_digits_len = 0;
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            } else {
                /* Any non-digit key cancels selection mode and leaves line unchanged */
                completion_result_free(&tab->compl_result);
                tab->compl_state = COMPL_STATE_NONE;
                tab->compl_digits[0] = '\0';
                tab->compl_digits_len = 0;
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            }
        }
    }

    /* Tab file-name completion trigger */
    if (k == XK_Tab || (n > 0 && buf[0] == '\t')) {
        action_tab_complete(gui);
        return;
    }

    /* Modal search mode special key handling */
    if (gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        if (tab->search_mode) {
            if (k == XK_Escape) {
                char committed[LINEEDIT_MAX + 512];
                snprintf(committed, sizeof(committed), "%s%s", tab->prompt, lineedit_get_text(&tab->input));
                tabbuf_append_line(tab->buf, committed);
                tab->search_mode = false;
                strncpy(tab->prompt, tab->saved_prompt, sizeof(tab->prompt) - 1);
                tab->prompt[sizeof(tab->prompt) - 1] = '\0';
                lineedit_set_text(&tab->input, tab->saved_input_before_search);
                tabbuf_scroll_to_bottom(tab->buf);
                return;
            }
            if (k == XK_Up || k == XK_Down) {
                return;
            }
        }
    }

    /* 2. Dispatch through configured keymap */
    KeyAction act = keymap_find_action(&gui->keymap, norm_mods, (uint32_t)k);
    if (act != ACTION_NONE && act < ACTION_COUNT && s_action_handlers[act]) {
        s_action_handlers[act](gui, k, norm_mods);
        return;
    }

    /* 3. Built-in defaults fallback for standard navigation/edit keys */
    if (k == XK_Return || k == XK_KP_Enter || (n > 0 && (buf[0] == '\r' || buf[0] == '\n'))) {
        action_return(gui, k, norm_mods);
        return;
    }
    if (k == XK_BackSpace || (n > 0 && (buf[0] == '\b' || buf[0] == 127))) {
        action_backspace(gui, k, norm_mods);
        return;
    }
    if (k == XK_Delete) {
        action_delete(gui, k, norm_mods);
        return;
    }
    if (k == XK_Home) {
        action_home(gui, k, norm_mods);
        return;
    }
    if (k == XK_End) {
        action_end(gui, k, norm_mods);
        return;
    }
    if (k == XK_Left) {
        action_left(gui, k, norm_mods);
        return;
    }
    if (k == XK_Right) {
        action_right(gui, k, norm_mods);
        return;
    }
    if (k == XK_Up) {
        action_history_prev(gui, k, norm_mods);
        return;
    }
    if (k == XK_Down) {
        action_history_next(gui, k, norm_mods);
        return;
    }

    /* 4. Fallback: insert printable text (only if not modified by Ctrl/Alt/Super) */
    if ((norm_mods & (KM_MOD_CTRL | KM_MOD_ALT | KM_MOD_SUPER)) == 0 && n > 0 &&
        (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] != 127) {
        if (gui->rt->active_idx < gui->rt->tab_count) {
            Tab *tab = gui->rt->tabs[gui->rt->active_idx];
            tab->hist_browsing = false;
            tab->hist_seq = 0;
            lineedit_insert_text(&tab->input, buf, (size_t)n);
            tabbuf_scroll_to_bottom(tab->buf);
        }
    }
}

static void handle_button_press(Gui *gui, XButtonEvent *e) {
    if (e->button == Button4) {
        /* Wheel Up: scroll up 3 lines */
        if (gui->rt->active_idx < gui->rt->tab_count) {
            Tab *tab = gui->rt->tabs[gui->rt->active_idx];
            int avail_h = gui->height - TAB_STRIP_HEIGHT - 2 * PADDING;
            int rows = avail_h / gui->font_height;
            int cols = (gui->width - 2 * PADDING) / gui->font_width;
            size_t total_v = get_tab_total_visual_lines(tab, cols > 0 ? cols : 80);
            tabbuf_scroll_up(tab->buf, 3, total_v, rows > 0 ? (size_t)rows : 1);
        }
        return;
    }
    if (e->button == Button5) {
        /* Wheel Down: scroll down 3 lines */
        if (gui->rt->active_idx < gui->rt->tab_count) {
            Tab *tab = gui->rt->tabs[gui->rt->active_idx];
            tabbuf_scroll_down(tab->buf, 3);
        }
        return;
    }

    if (e->button == Button1) {
        if (e->y < TAB_STRIP_HEIGHT) {
            int tab_w = get_tab_width(gui);
            int tab_count = gui->rt->tab_count;

            /* Check '+' button click */
            int plus_x = tab_count * tab_w + 6;
            if (e->x >= plus_x && e->x <= plus_x + 20) {
                tabrt_spawn(gui->rt, ConnectionNumber(gui->dpy));
                return;
            }

            /* Check tab click */
            int clicked_tab = e->x / tab_w;
            if (clicked_tab >= 0 && clicked_tab < tab_count) {
                int tx = clicked_tab * tab_w;
                int close_x = tx + tab_w - 22;
                if (e->x >= close_x && e->x <= close_x + 18) {
                    /* Close tab */
                    if (tabrt_close(gui->rt, clicked_tab)) {
                        gui->running = false;
                    }
                    return;
                }
                /* Switch active tab */
                gui->rt->active_idx = clicked_tab;
            }
        } else {
            /* Click inside terminal area: initiate text selection */
            if (gui->rt->active_idx < gui->rt->tab_count) {
                Tab *tab = gui->rt->tabs[gui->rt->active_idx];
                int cols = (gui->width - 2 * PADDING) / gui->font_width;
                if (cols < 1) cols = 1;
                int rows = (gui->height - TAB_STRIP_HEIGHT - 2 * PADDING) / gui->font_height;
                if (rows < 1) rows = 1;

                size_t total_v = get_tab_total_visual_lines(tab, cols);
                size_t visible_rows = (size_t)rows;
                size_t start_v = (total_v > visible_rows) ? (total_v - visible_rows - tab->buf->scroll_offset) : 0;

                int r = (e->y - (TAB_STRIP_HEIGHT + PADDING)) / gui->font_height;
                int c = (e->x - PADDING) / gui->font_width;
                if (r < 0) r = 0;
                if (c < 0) c = 0;

                gui->selecting = true;
                gui->has_selection = false;
                gui->sel_start_vr = (int)(start_v + (size_t)r);
                gui->sel_start_vc = c;
                gui->sel_end_vr = gui->sel_start_vr;
                gui->sel_end_vc = c;
            }
        }
    } else if (e->button == Button2) {
        /* Middle Click in terminal: paste PRIMARY selection */
        if (e->y >= TAB_STRIP_HEIGHT) {
            XConvertSelection(gui->dpy, XA_PRIMARY, gui->atom_utf8_string,
                              gui->atom_terminator_selection, gui->win, CurrentTime);
            XFlush(gui->dpy);
        }
    }
}

static void handle_button_release(Gui *gui, XButtonEvent *e) {
    if (e->button == Button1 && gui->selecting) {
        gui->selecting = false;
        if (gui->has_selection) {
            gui_extract_selection(gui);
        }
    }
}

static void handle_selection_request(Gui *gui, XSelectionRequestEvent *req) {
    XSelectionEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SelectionNotify;
    ev.display = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.property = None;
    ev.time = req->time;

    const char *sel_text = NULL;
    if (req->selection == XA_PRIMARY) {
        sel_text = gui->primary_selection_text;
    } else if (req->selection == gui->atom_clipboard) {
        sel_text = gui->clipboard_selection_text;
    }

    if (sel_text != NULL) {
        if (req->target == gui->atom_targets) {
            Atom targets[] = { gui->atom_targets, gui->atom_utf8_string, XA_STRING };
            XChangeProperty(gui->dpy, req->requestor, req->property,
                            XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)targets, (int)(sizeof(targets) / sizeof(targets[0])));
            ev.property = req->property;
        } else if (req->target == gui->atom_utf8_string || req->target == XA_STRING) {
            size_t len = strlen(sel_text);
            XChangeProperty(gui->dpy, req->requestor, req->property,
                            req->target, 8, PropModeReplace,
                            (unsigned char *)sel_text, (int)len);
            ev.property = req->property;
        }
    }

    XSendEvent(gui->dpy, req->requestor, False, 0, (XEvent *)&ev);
    XFlush(gui->dpy);
}

static void handle_selection_clear(Gui *gui, XSelectionClearEvent *e) {
    if (e->selection == XA_PRIMARY) {
        if (gui->primary_selection_text) {
            free(gui->primary_selection_text);
            gui->primary_selection_text = NULL;
        }
        gui->has_selection = false;
        gui_redraw(gui);
    } else if (e->selection == gui->atom_clipboard) {
        if (gui->clipboard_selection_text) {
            free(gui->clipboard_selection_text);
            gui->clipboard_selection_text = NULL;
        }
    }
}

static void handle_selection_notify(Gui *gui, XSelectionEvent *e) {
    if (e->property == None) return;

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop_data = NULL;

    /* Cap at 64 KiB: 65536 bytes */
    int status = XGetWindowProperty(gui->dpy, gui->win, e->property,
                                   0, 16384 /* 16384 * 4 = 65536 bytes */,
                                   False, AnyPropertyType,
                                   &actual_type, &actual_format,
                                   &nitems, &bytes_after, &prop_data);

    if (status == Success && prop_data && nitems > 0) {
        size_t len = nitems * (actual_format / 8);
        if (len > 65536) len = 65536;

        /* Process pasted text: replace inner newlines with spaces and strip trailing newline */
        char *cleaned = malloc(len + 1);
        if (cleaned) {
            size_t out_len = 0;
            for (size_t i = 0; i < len; i++) {
                char c = (char)prop_data[i];
                if (c == '\r') continue;
                cleaned[out_len++] = c;
            }
            while (out_len > 0 && cleaned[out_len - 1] == '\n') {
                out_len--;
            }
            for (size_t i = 0; i < out_len; i++) {
                if (cleaned[i] == '\n') cleaned[i] = ' ';
            }
            cleaned[out_len] = '\0';

            if (out_len > 0 && gui->rt->active_idx < gui->rt->tab_count) {
                Tab *tab = gui->rt->tabs[gui->rt->active_idx];
                tab->hist_browsing = false;
                tab->hist_seq = 0;
                lineedit_insert_text(&tab->input, cleaned, out_len);
                tabbuf_scroll_to_bottom(tab->buf);
            }
            free(cleaned);
        }
        XFree(prop_data);
    }
    XDeleteProperty(gui->dpy, gui->win, e->property);
}

static void handle_pointer_motion(Gui *gui, XMotionEvent *e) {
    int prev_hovered = gui->hovered_tab;
    bool prev_close = gui->hovered_close;
    bool prev_plus = gui->hovered_plus;

    gui->hovered_tab = -1;
    gui->hovered_close = false;
    gui->hovered_plus = false;

    if (e->y < TAB_STRIP_HEIGHT) {
        int tab_w = get_tab_width(gui);
        int tab_count = gui->rt->tab_count;

        int plus_x = tab_count * tab_w + 6;
        if (e->x >= plus_x && e->x <= plus_x + 20) {
            gui->hovered_plus = true;
        } else {
            int idx = e->x / tab_w;
            if (idx >= 0 && idx < tab_count) {
                gui->hovered_tab = idx;
                int tx = idx * tab_w;
                int close_x = tx + tab_w - 22;
                if (e->x >= close_x && e->x <= close_x + 18) {
                    gui->hovered_close = true;
                }
            }
        }
    } else if (gui->selecting && gui->rt->active_idx < gui->rt->tab_count) {
        Tab *tab = gui->rt->tabs[gui->rt->active_idx];
        int cols = (gui->width - 2 * PADDING) / gui->font_width;
        if (cols < 1) cols = 1;
        int rows = (gui->height - TAB_STRIP_HEIGHT - 2 * PADDING) / gui->font_height;
        if (rows < 1) rows = 1;

        size_t total_v = get_tab_total_visual_lines(tab, cols);
        size_t visible_rows = (size_t)rows;
        size_t start_v = (total_v > visible_rows) ? (total_v - visible_rows - tab->buf->scroll_offset) : 0;

        int r = (e->y - (TAB_STRIP_HEIGHT + PADDING)) / gui->font_height;
        int c = (e->x - PADDING) / gui->font_width;
        if (r < 0) r = 0;
        if (c < 0) c = 0;

        gui->sel_end_vr = (int)(start_v + (size_t)r);
        gui->sel_end_vc = c;
        if (gui->sel_end_vr != gui->sel_start_vr || gui->sel_end_vc != gui->sel_start_vc) {
            gui->has_selection = true;
        }
        gui_redraw(gui);
    }

    if (gui->hovered_tab != prev_hovered ||
        gui->hovered_close != prev_close ||
        gui->hovered_plus != prev_plus) {
        gui_redraw(gui);
    }
}

typedef struct {
    Gui *gui;
    Tab *tab;
} TabFrameContext;

static void on_tab_frame_received(const Frame *f, void *user_data) {
    TabFrameContext *ctx = (TabFrameContext *)user_data;
    Tab *tab = ctx->tab;
    Gui *gui = ctx->gui;

    if (f->type == FRAME_OUTPUT) {
        tabbuf_append_bytes(tab->buf, f->payload, f->len);
    } else if (f->type == FRAME_PROMPT) {
        const char *cwd = f->payload;
        size_t cwd_len = strlen(cwd);
        const char *prompt = cwd + cwd_len + 1;

        strncpy(tab->cwd, cwd, sizeof(tab->cwd) - 1);
        tab->cwd[sizeof(tab->cwd) - 1] = '\0';
        strncpy(tab->prompt, prompt, sizeof(tab->prompt) - 1);
        tab->prompt[sizeof(tab->prompt) - 1] = '\0';

        tab_update_label(tab);
    } else if (f->type == FRAME_CLEAR) {
        tabbuf_clear(tab->buf);
        tabbuf_scroll_to_bottom(tab->buf);
    } else if (f->type == FRAME_RELOAD_KEYS) {
        gui_reload_keymap(gui);
    } else if (f->type == FRAME_EXIT) {
        tab->alive = false;
        tab->should_close = true;
    }
}

int gui_run(Gui *gui) {
    if (!gui) return 1;

    bool dirty = true;
    int x_fd = ConnectionNumber(gui->dpy);

    while (gui->running && gui->rt->tab_count > 0) {
        /* Drain all pending X events */
        while (XPending(gui->dpy)) {
            XEvent ev;
            XNextEvent(gui->dpy, &ev);

            switch (ev.type) {
            case KeyPress:
                handle_key_press(gui, &ev.xkey);
                dirty = true;
                break;
            case ButtonPress:
                handle_button_press(gui, &ev.xbutton);
                dirty = true;
                break;
            case ButtonRelease:
                handle_button_release(gui, &ev.xbutton);
                dirty = true;
                break;
            case MotionNotify:
                handle_pointer_motion(gui, &ev.xmotion);
                break;
            case SelectionRequest:
                handle_selection_request(gui, &ev.xselectionrequest);
                break;
            case SelectionClear:
                handle_selection_clear(gui, &ev.xselectionclear);
                dirty = true;
                break;
            case SelectionNotify:
                handle_selection_notify(gui, &ev.xselection);
                dirty = true;
                break;
            case LeaveNotify:
                gui->hovered_tab = -1;
                gui->hovered_close = false;
                gui->hovered_plus = false;
                dirty = true;
                break;
            case FocusIn:
                gui->is_focused = true;
                dirty = true;
                break;
            case FocusOut:
                gui->is_focused = false;
                dirty = true;
                break;
            case Expose:
                if (ev.xexpose.count == 0) {
                    dirty = true;
                }
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != gui->width ||
                    ev.xconfigure.height != gui->height) {
                    gui->width = ev.xconfigure.width;
                    gui->height = ev.xconfigure.height;
                    update_backbuffer(gui);
                    dirty = true;
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == gui->wm_delete_window) {
                    gui->running = false;
                }
                break;
            default:
                break;
            }
        }

        if (!gui->running || gui->rt->tab_count == 0) {
            break;
        }

        /* Redraw at most once per loop iteration if dirty, before flushing and blocking in poll */
        if (dirty) {
            gui_redraw(gui);
            dirty = false;
        }

        /* Flush X buffer before blocking */
        XFlush(gui->dpy);

        /* Build pollfd array */
        struct pollfd pfds[1 + MAX_TABS * 2];
        int fd_map_tab[1 + MAX_TABS * 2];
        bool fd_is_write[1 + MAX_TABS * 2];

        int nfds = 0;

        /* X connection fd */
        pfds[nfds].fd = x_fd;
        pfds[nfds].events = POLLIN;
        fd_map_tab[nfds] = -1;
        fd_is_write[nfds] = false;
        nfds++;

        /* Tab fds */
        for (int i = 0; i < gui->rt->tab_count; i++) {
            Tab *tab = gui->rt->tabs[i];
            if (tab->from_tab_fd >= 0) {
                pfds[nfds].fd = tab->from_tab_fd;
                pfds[nfds].events = POLLIN | POLLHUP;
                fd_map_tab[nfds] = i;
                fd_is_write[nfds] = false;
                nfds++;
            }
            if (tab->to_tab_fd >= 0 && proto_queue_has_data(&tab->pending_out)) {
                pfds[nfds].fd = tab->to_tab_fd;
                pfds[nfds].events = POLLOUT;
                fd_map_tab[nfds] = i;
                fd_is_write[nfds] = true;
                nfds++;
            }
        }

        /* Blocking wait on poll */
        int ret = poll(pfds, (nfds_t)nfds, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("gui poll");
            break;
        }

        /* Handle poll events */
        for (int i = 1; i < nfds; i++) {
            int tab_idx = fd_map_tab[i];
            if (tab_idx < 0 || tab_idx >= gui->rt->tab_count) continue;
            Tab *tab = gui->rt->tabs[tab_idx];

            if (!fd_is_write[i]) {
                /* Tab read end */
                if (pfds[i].revents & POLLIN) {
                    bool eof_reached = false;
                    TabFrameContext ctx = { gui, tab };
                    proto_read_frames(tab->from_tab_fd, &tab->in_buf,
                                      on_tab_frame_received, &ctx, &eof_reached);
                    dirty = true;
                    if (eof_reached) {
                        tab->alive = false;
                        if (tab->from_tab_fd >= 0) {
                            close(tab->from_tab_fd);
                            tab->from_tab_fd = -1;
                        }
                        if (tab->to_tab_fd >= 0) {
                            close(tab->to_tab_fd);
                            tab->to_tab_fd = -1;
                        }
                        if (!tab->should_close && !tab->process_exited_printed) {
                            tabbuf_append_line(tab->buf, "[process exited]");
                            tab->process_exited_printed = true;
                            dirty = true;
                        }
                    }
                }
                if (pfds[i].revents & (POLLHUP | POLLERR)) {
                    if (tab->alive) {
                        tab->alive = false;
                        if (tab->from_tab_fd >= 0) {
                            close(tab->from_tab_fd);
                            tab->from_tab_fd = -1;
                        }
                        if (tab->to_tab_fd >= 0) {
                            close(tab->to_tab_fd);
                            tab->to_tab_fd = -1;
                        }
                        if (!tab->should_close && !tab->process_exited_printed) {
                            tabbuf_append_line(tab->buf, "[process exited]");
                            tab->process_exited_printed = true;
                            dirty = true;
                        }
                    }
                }
            } else {
                /* Tab write end */
                if (pfds[i].revents & POLLOUT) {
                    proto_flush_queue(tab->to_tab_fd, &tab->pending_out);
                }
            }
        }

        /* Close any tabs that exited voluntarily */
        for (int i = gui->rt->tab_count - 1; i >= 0; i--) {
            if (gui->rt->tabs[i]->should_close) {
                if (tabrt_close(gui->rt, i)) {
                    gui->running = false;
                    break;
                }
                dirty = true;
            }
        }
    }

    return 0;
}
