/*
 * keymap.c: Hotkey parsing, chord validation, and persistent configuration.
 *
 * Implements configurable keybindings:
 * - String chord parsing (e.g. "Ctrl+Shift+T", "Alt+1") and normalization.
 * - Validation against hard-reserved chords (Ctrl+C, Ctrl+Z, Ctrl+R, Tab).
 * - Atomic persistence to ~/.terminatorrc with temporary files and rename().
 * - KeyAction lookup by resolved keysyms and modifier masks.
 */

#define _GNU_SOURCE
#include "keymap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
    KeyAction action;
    const char *name;
    const char *default_chord;
} ActionDef;

static const ActionDef s_action_defs[ACTION_COUNT] = {
    { ACTION_NONE,             "none",             "" },
    { ACTION_LINE_START,        "line_start",        "Ctrl+A" },
    { ACTION_LINE_END,          "line_end",          "Ctrl+E" },
    { ACTION_KILL_TO_START,     "kill_to_start",     "Ctrl+U" },
    { ACTION_KILL_TO_END,       "kill_to_end",       "Ctrl+K" },
    { ACTION_DELETE_PREV_WORD,  "delete_prev_word",  "Ctrl+W" },
    { ACTION_CLEAR_SCREEN,      "clear_screen",      "Ctrl+L" },
    { ACTION_HISTORY_PREV,      "history_prev",      "Up" },
    { ACTION_HISTORY_NEXT,      "history_next",      "Down" },
    { ACTION_COPY,              "copy",              "Ctrl+Shift+C" },
    { ACTION_PASTE,             "paste",             "Ctrl+Shift+V" },
    { ACTION_EOF,               "eof",               "Ctrl+D" },
    { ACTION_MOVE_LEFT,         "move_left",         "Left" },
    { ACTION_MOVE_RIGHT,        "move_right",        "Right" },
    { ACTION_BACKSPACE,         "backspace",         "Backspace" },
    { ACTION_DELETE,            "delete",            "Delete" },
    { ACTION_RETURN,            "return",            "Return" }
};

const char *keyaction_to_name(KeyAction action) {
    if (action > ACTION_NONE && action < ACTION_COUNT) {
        return s_action_defs[action].name;
    }
    return "unknown";
}

KeyAction keyaction_from_name(const char *name) {
    if (!name || !name[0]) return ACTION_NONE;
    for (int i = 1; i < ACTION_COUNT; i++) {
        if (strcasecmp(s_action_defs[i].name, name) == 0) {
            return s_action_defs[i].action;
        }
    }
    return ACTION_NONE;
}

const char *keyaction_default_chord_str(KeyAction action) {
    if (action > ACTION_NONE && action < ACTION_COUNT) {
        return s_action_defs[action].default_chord;
    }
    return "";
}

void keymap_init_defaults(KeyMap *km) {
    if (!km) return;
    memset(km, 0, sizeof(*km));

    for (int i = 1; i < ACTION_COUNT; i++) {
        km->bindings[i].action = s_action_defs[i].action;
        keymap_parse_chord(s_action_defs[i].default_chord, &km->bindings[i].chord);
    }
}

static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static bool is_modifier_name(const char *name) {
    if (!name) return false;
    return (strcasecmp(name, "ctrl") == 0 ||
            strcasecmp(name, "control") == 0 ||
            strcasecmp(name, "shift") == 0 ||
            strcasecmp(name, "alt") == 0 ||
            strcasecmp(name, "mod1") == 0 ||
            strcasecmp(name, "super") == 0 ||
            strcasecmp(name, "mod4") == 0 ||
            strcasecmp(name, "win") == 0);
}

bool keymap_parse_chord(const char *str, KeyChord *out) {
    if (!str || !out) return false;
    memset(out, 0, sizeof(*out));

    char tmp[128];
    strncpy(tmp, str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *s = trim_whitespace(tmp);
    if (!s || !s[0]) return false;

    size_t slen = strlen(s);
    if (s[0] == '+' && slen > 1) return false;

    const char *key_str = NULL;
    char mod_part[128] = {0};

    if (s[slen - 1] == '+') {
        if (slen >= 2 && s[slen - 2] == '+') {
            key_str = "+";
            if (slen > 2) {
                strncpy(mod_part, s, slen - 2);
                mod_part[slen - 2] = '\0';
            }
        } else {
            return false;
        }
    } else {
        char *last_plus = strrchr(s, '+');
        if (last_plus) {
            *last_plus = '\0';
            key_str = trim_whitespace(last_plus + 1);
            strncpy(mod_part, s, sizeof(mod_part) - 1);
        } else {
            key_str = s;
        }
    }

    if (!key_str || !key_str[0]) return false;
    if (is_modifier_name(key_str)) return false;

    unsigned int mods = KM_MOD_NONE;
    if (mod_part[0]) {
        char *token = strtok(mod_part, "+");
        while (token) {
            char *t = trim_whitespace(token);
            if (!t || !t[0]) return false;
            if (strcasecmp(t, "ctrl") == 0 || strcasecmp(t, "control") == 0) {
                mods |= KM_MOD_CTRL;
            } else if (strcasecmp(t, "shift") == 0) {
                mods |= KM_MOD_SHIFT;
            } else if (strcasecmp(t, "alt") == 0 || strcasecmp(t, "mod1") == 0) {
                mods |= KM_MOD_ALT;
            } else if (strcasecmp(t, "super") == 0 || strcasecmp(t, "mod4") == 0 || strcasecmp(t, "win") == 0) {
                mods |= KM_MOD_SUPER;
            } else {
                return false;
            }
            token = strtok(NULL, "+");
        }
    }

    out->mods = mods;

    /* Normalize key name: single letters stored lowercase for keysym resolution */
    if (strlen(key_str) == 1 && isalpha((unsigned char)key_str[0])) {
        out->key[0] = (char)tolower((unsigned char)key_str[0]);
        out->key[1] = '\0';
    } else {
        if (strcasecmp(key_str, "enter") == 0 || strcasecmp(key_str, "return") == 0) {
            strcpy(out->key, "Return");
        } else if (strcasecmp(key_str, "backspace") == 0) {
            strcpy(out->key, "BackSpace");
        } else if (strcasecmp(key_str, "delete") == 0) {
            strcpy(out->key, "Delete");
        } else if (strcasecmp(key_str, "up") == 0) {
            strcpy(out->key, "Up");
        } else if (strcasecmp(key_str, "down") == 0) {
            strcpy(out->key, "Down");
        } else if (strcasecmp(key_str, "left") == 0) {
            strcpy(out->key, "Left");
        } else if (strcasecmp(key_str, "right") == 0) {
            strcpy(out->key, "Right");
        } else if (strcasecmp(key_str, "home") == 0) {
            strcpy(out->key, "Home");
        } else if (strcasecmp(key_str, "end") == 0) {
            strcpy(out->key, "End");
        } else if (strcasecmp(key_str, "pageup") == 0 || strcasecmp(key_str, "page_up") == 0) {
            strcpy(out->key, "Page_Up");
        } else if (strcasecmp(key_str, "pagedown") == 0 || strcasecmp(key_str, "page_down") == 0) {
            strcpy(out->key, "Page_Down");
        } else if (strcasecmp(key_str, "tab") == 0) {
            strcpy(out->key, "Tab");
        } else {
            strncpy(out->key, key_str, sizeof(out->key) - 1);
            out->key[sizeof(out->key) - 1] = '\0';
        }
    }

    return true;
}

void keymap_format_chord(const KeyChord *chord, char *buf, size_t cap) {
    if (!chord || !buf || cap == 0) return;
    buf[0] = '\0';

    size_t off = 0;
    if (chord->mods & KM_MOD_CTRL) {
        off += snprintf(buf + off, cap > off ? cap - off : 0, "Ctrl+");
    }
    if (chord->mods & KM_MOD_ALT) {
        off += snprintf(buf + off, cap > off ? cap - off : 0, "Alt+");
    }
    if (chord->mods & KM_MOD_SUPER) {
        off += snprintf(buf + off, cap > off ? cap - off : 0, "Super+");
    }
    if (chord->mods & KM_MOD_SHIFT) {
        off += snprintf(buf + off, cap > off ? cap - off : 0, "Shift+");
    }

    if (strlen(chord->key) == 1 && isalpha((unsigned char)chord->key[0])) {
        snprintf(buf + off, cap > off ? cap - off : 0, "%c", (char)toupper((unsigned char)chord->key[0]));
    } else {
        snprintf(buf + off, cap > off ? cap - off : 0, "%s", chord->key);
    }
}

bool keymap_chords_equal(const KeyChord *a, const KeyChord *b) {
    if (!a || !b) return false;
    if (a->mods != b->mods) return false;

    if (a->keysym != 0 && b->keysym != 0) {
        return a->keysym == b->keysym;
    }

    return strcasecmp(a->key, b->key) == 0;
}

bool keymap_is_reserved(const KeyChord *chord) {
    if (!chord) return false;

    /* 1. Ctrl+C (SIGINT) */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "c") == 0) return true;

    /* 2. Ctrl+Z (SIGTSTP) */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "z") == 0) return true;

    /* 3. Ctrl+R (History search) */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "r") == 0) return true;

    /* 4. Tab (Completion) */
    if (chord->mods == KM_MOD_NONE && strcasecmp(chord->key, "Tab") == 0) return true;

    /* 5. Ctrl+Shift+T (New tab) */
    if (chord->mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && strcasecmp(chord->key, "t") == 0) return true;

    /* 6. Ctrl+Shift+W (Close tab) */
    if (chord->mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && strcasecmp(chord->key, "w") == 0) return true;

    /* 7. Alt+1 .. Alt+9 (Switch tab) */
    if (chord->mods == KM_MOD_ALT && strlen(chord->key) == 1 && chord->key[0] >= '1' && chord->key[0] <= '9') {
        return true;
    }

    /* 8. Ctrl+Page_Up / Ctrl+Page_Down (Prev/Next tab) */
    if (chord->mods == KM_MOD_CTRL &&
        (strcasecmp(chord->key, "Page_Up") == 0 || strcasecmp(chord->key, "Page_Down") == 0)) {
        return true;
    }

    /* 9. Shift+Page_Up / Shift+Page_Down (Scroll page) */
    if (chord->mods == KM_MOD_SHIFT &&
        (strcasecmp(chord->key, "Page_Up") == 0 || strcasecmp(chord->key, "Page_Down") == 0)) {
        return true;
    }

    return false;
}

const char *keymap_get_config_path(char *buf, size_t cap) {
    if (!buf || cap == 0) return NULL;

    const char *env_rc = getenv("TERMINATORRC");
    if (env_rc && env_rc[0]) {
        strncpy(buf, env_rc, cap - 1);
        buf[cap - 1] = '\0';
        return buf;
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, cap, "%s/.terminatorrc", home);
        return buf;
    }

    strncpy(buf, ".terminatorrc", cap - 1);
    buf[cap - 1] = '\0';
    return buf;
}

static void append_warning(char *warnings, size_t cap, const char *fmt, ...) {
    if (!warnings || cap == 0) return;
    size_t cur_len = strlen(warnings);
    if (cur_len >= cap - 1) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(warnings + cur_len, cap - cur_len, fmt, ap);
    va_end(ap);
}

int keymap_load_file(KeyMap *km, const char *path, char *warnings, size_t warnings_cap) {
    if (!km) return -1;
    keymap_init_defaults(km);

    if (warnings && warnings_cap > 0) warnings[0] = '\0';

    if (!path || !path[0]) return 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Missing config file is not an error; defaults apply */
        return 0;
    }

    char line_buf[512];
    int line_no = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        line_no++;
        char *line = trim_whitespace(line_buf);
        if (!line || !line[0] || line[0] == '#') {
            continue; /* Skip comments and empty lines */
        }

        /* Check for 'bind' prefix */
        if (strncasecmp(line, "bind", 4) == 0 && (isspace((unsigned char)line[4]) || line[4] == '\0')) {
            line = trim_whitespace(line + 4);
        }

        /* Parse: <action> [=] <chord> */
        char *eq = strchr(line, '=');
        char *action_str = NULL;
        char *chord_str = NULL;

        if (eq) {
            *eq = '\0';
            action_str = trim_whitespace(line);
            chord_str = trim_whitespace(eq + 1);
        } else {
            /* Split on first whitespace */
            char *p = line;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) {
                *p = '\0';
                action_str = trim_whitespace(line);
                chord_str = trim_whitespace(p + 1);
            }
        }

        if (!action_str || !action_str[0] || !chord_str || !chord_str[0]) {
            append_warning(warnings, warnings_cap,
                           "terminator: %s:%d: invalid configuration syntax '%s'\n",
                           path, line_no, line);
            continue;
        }

        KeyAction action = keyaction_from_name(action_str);
        if (action == ACTION_NONE) {
            append_warning(warnings, warnings_cap,
                           "terminator: %s:%d: unknown action '%s'\n",
                           path, line_no, action_str);
            continue;
        }

        KeyChord parsed;
        if (!keymap_parse_chord(chord_str, &parsed)) {
            char def_buf[64];
            keymap_format_chord(&km->bindings[action].chord, def_buf, sizeof(def_buf));
            append_warning(warnings, warnings_cap,
                           "terminator: %s:%d: unparsable chord '%s'; using default for '%s' (%s)\n",
                           path, line_no, chord_str, action_str, def_buf);
            continue;
        }

        /* Validation: reserved chord */
        if (keymap_is_reserved(&parsed)) {
            char chord_fmt[64];
            keymap_format_chord(&parsed, chord_fmt, sizeof(chord_fmt));
            char def_buf[64];
            keymap_format_chord(&km->bindings[action].chord, def_buf, sizeof(def_buf));
            append_warning(warnings, warnings_cap,
                           "terminator: %s:%d: chord '%s' is reserved; using default for '%s' (%s)\n",
                           path, line_no, chord_fmt, action_str, def_buf);
            continue;
        }

        /* Validation: conflicting chord already bound to another action (keep first, report second) */
        KeyAction conflict_act = ACTION_NONE;
        for (int i = 1; i < ACTION_COUNT; i++) {
            if (i != (int)action && keymap_chords_equal(&km->bindings[i].chord, &parsed)) {
                conflict_act = (KeyAction)i;
                break;
            }
        }

        if (conflict_act != ACTION_NONE) {
            char chord_fmt[64];
            keymap_format_chord(&parsed, chord_fmt, sizeof(chord_fmt));
            char def_buf[64];
            keymap_format_chord(&km->bindings[action].chord, def_buf, sizeof(def_buf));
            append_warning(warnings, warnings_cap,
                           "terminator: %s:%d: chord '%s' is already bound to '%s'; using default for '%s' (%s)\n",
                           path, line_no, chord_fmt, keyaction_to_name(conflict_act), action_str, def_buf);
            continue;
        }

        /* Successful binding */
        km->bindings[action].chord = parsed;
    }

    fclose(f);
    return 0;
}

int keymap_save_file(const KeyMap *km, const char *path, char *err_buf, size_t err_cap) {
    if (!km || !path || !path[0]) {
        if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "invalid arguments");
        return -1;
    }

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "cannot open temp file '%s': %s", tmp_path, strerror(errno));
        }
        return -1;
    }

    fprintf(f, "# Terminator Keybindings Configuration (~/.terminatorrc)\n");
    fprintf(f, "# Generated by Terminator 'bind' command\n\n");

    for (int i = 1; i < ACTION_COUNT; i++) {
        char chord_buf[64];
        keymap_format_chord(&km->bindings[i].chord, chord_buf, sizeof(chord_buf));
        fprintf(f, "bind %s = %s\n", keyaction_to_name(km->bindings[i].action), chord_buf);
    }

    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "cannot rename '%s' to '%s': %s", tmp_path, path, strerror(errno));
        }
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int keymap_set_binding(KeyMap *km, KeyAction action, const KeyChord *chord, char *err_buf, size_t err_cap) {
    if (!km || action <= ACTION_NONE || action >= ACTION_COUNT || !chord) {
        if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "invalid action or chord");
        return -1;
    }

    if (keymap_is_reserved(chord)) {
        char chord_fmt[64];
        keymap_format_chord(chord, chord_fmt, sizeof(chord_fmt));
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "chord '%s' is reserved by Terminator", chord_fmt);
        }
        return -1;
    }

    for (int i = 1; i < ACTION_COUNT; i++) {
        if (i != (int)action && keymap_chords_equal(&km->bindings[i].chord, chord)) {
            char chord_fmt[64];
            keymap_format_chord(chord, chord_fmt, sizeof(chord_fmt));
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap, "chord '%s' is already bound to '%s'",
                         chord_fmt, keyaction_to_name((KeyAction)i));
            }
            return -1;
        }
    }

    km->bindings[action].chord = *chord;
    return 0;
}

int keymap_reset_binding(KeyMap *km, KeyAction action) {
    if (!km || action <= ACTION_NONE || action >= ACTION_COUNT) return -1;
    const char *def_str = s_action_defs[action].default_chord;
    return keymap_parse_chord(def_str, &km->bindings[action].chord) ? 0 : -1;
}

KeyAction keymap_find_action(const KeyMap *km, unsigned int mods, uint32_t keysym) {
    if (!km || keysym == 0) return ACTION_NONE;

    for (int i = 1; i < ACTION_COUNT; i++) {
        if (km->bindings[i].chord.mods == mods && km->bindings[i].chord.keysym == keysym) {
            return km->bindings[i].action;
        }
    }
    return ACTION_NONE;
}

KeyAction keymap_find_action_by_chord(const KeyMap *km, const KeyChord *chord) {
    if (!km || !chord) return ACTION_NONE;

    for (int i = 1; i < ACTION_COUNT; i++) {
        if (keymap_chords_equal(&km->bindings[i].chord, chord)) {
            return km->bindings[i].action;
        }
    }
    return ACTION_NONE;
}

void keymap_resolve_keysyms(KeyMap *km, KeyResolveFn resolver) {
    if (!km || !resolver) return;

    for (int i = 1; i < ACTION_COUNT; i++) {
        km->bindings[i].chord.keysym = resolver(km->bindings[i].chord.key);
    }
}
