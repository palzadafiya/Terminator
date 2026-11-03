#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Modifiers bitmask (X-free) */
#define KM_MOD_NONE   0
#define KM_MOD_SHIFT  (1 << 0)
#define KM_MOD_CTRL   (1 << 1)
#define KM_MOD_ALT    (1 << 2)
#define KM_MOD_SUPER  (1 << 3)

typedef enum {
    ACTION_NONE = 0,
    ACTION_LINE_START,        /* line_start: Ctrl+A / Home */
    ACTION_LINE_END,          /* line_end: Ctrl+E / End */
    ACTION_KILL_TO_START,     /* kill_to_start: Ctrl+U */
    ACTION_KILL_TO_END,       /* kill_to_end: Ctrl+K */
    ACTION_DELETE_PREV_WORD,  /* delete_prev_word: Ctrl+W */
    ACTION_CLEAR_SCREEN,      /* clear_screen: Ctrl+L */
    ACTION_HISTORY_PREV,      /* history_prev: Up */
    ACTION_HISTORY_NEXT,      /* history_next: Down */
    ACTION_COPY,              /* copy: Ctrl+Shift+C */
    ACTION_PASTE,             /* paste: Ctrl+Shift+V */
    ACTION_EOF,               /* eof: Ctrl+D */
    ACTION_MOVE_LEFT,         /* move_left: Left */
    ACTION_MOVE_RIGHT,        /* move_right: Right */
    ACTION_BACKSPACE,         /* backspace: Backspace */
    ACTION_DELETE,            /* delete: Delete */
    ACTION_RETURN,            /* return: Return */
    ACTION_COUNT
} KeyAction;

typedef struct {
    unsigned int mods; /* KM_MOD_* */
    char key[64];      /* Normalized key string (e.g. "a", "c", "Left", "Up") */
    uint32_t keysym;   /* Resolved keysym (0 if unresolved) */
} KeyChord;

typedef struct {
    KeyAction action;
    KeyChord chord;
} KeyBinding;

typedef struct {
    KeyBinding bindings[ACTION_COUNT];
} KeyMap;

/* Function pointer types for mapping between key names and X keysyms */
typedef uint32_t (*KeyResolveFn)(const char *name);
typedef const char *(*KeyNameFn)(uint32_t keysym);

/* Initialize a KeyMap with the default bindings */
void keymap_init_defaults(KeyMap *km);

/* Action introspection */
const char *keyaction_to_name(KeyAction action);
KeyAction keyaction_from_name(const char *name);
const char *keyaction_default_chord_str(KeyAction action);

/* Chord operations */
bool keymap_parse_chord(const char *str, KeyChord *out);
void keymap_format_chord(const KeyChord *chord, char *buf, size_t cap);
bool keymap_chords_equal(const KeyChord *a, const KeyChord *b);

/* Returns true if chord matches a hard-reserved Terminator chord (Ctrl+C, Ctrl+Z, Tab, etc.) */
bool keymap_is_reserved(const KeyChord *chord);

/* File configuration: ~/.terminatorrc */
const char *keymap_get_config_path(char *buf, size_t cap);

/* Load keymap from file. If file does not exist, defaults are retained.
 * Any parsing errors/warnings are written into warnings buffer. */
int keymap_load_file(KeyMap *km, const char *path, char *warnings, size_t warnings_cap);

/* Save keymap to file atomically (write to temp + rename) */
int keymap_save_file(const KeyMap *km, const char *path, char *err_buf, size_t err_cap);

/* Rebind a single action */
int keymap_set_binding(KeyMap *km, KeyAction action, const KeyChord *chord, char *err_buf, size_t err_cap);
int keymap_reset_binding(KeyMap *km, KeyAction action);

/* Lookup bound action for given normalized modifiers and keysym */
KeyAction keymap_find_action(const KeyMap *km, unsigned int mods, uint32_t keysym);

/* Lookup bound action by chord matching (for headless tests or before keysyms are resolved) */
KeyAction keymap_find_action_by_chord(const KeyMap *km, const KeyChord *chord);

/* Resolve keysyms in keymap using the GUI's X11 resolver */
void keymap_resolve_keysyms(KeyMap *km, KeyResolveFn resolver);

#endif /* KEYMAP_H */
