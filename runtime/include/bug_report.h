#ifndef PSX_BUG_REPORT_H
#define PSX_BUG_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Player bug reports (docs/BUG_REPORT.md; the title API is in mod_plugins.h).
 *
 * bug_report_begin (the BugReport hotkey) takes the moment while the game
 * keeps running: the next presented picture (present_shot), the PSX picture
 * without effects, a save state in SAVESTATE_BUG_SLOT and the presented-image
 * ring frozen (the clip). bug_report_tick, every host frame on the main
 * thread, opens the form once those landed (or after a few seconds); the form
 * is a title page (psx_mod_open_title_page) run by main's page loop, which
 * also feeds it typed text, and is drawn in Dear ImGui where the build has it
 * (bug_report_ui_*, below). Saving writes the folder; discarding deletes it. */

int  bug_report_begin(void);
/* 1 when the form should open now (main opens it as a title page). */
int  bug_report_tick(void);
int  bug_report_page_render(uint32_t *argb, int w, int h);
int  bug_report_page_input(uint32_t pressed, uint32_t held, int key, int key_mods);
/* Typed text (SDL text input, UTF-8) while the form is open. */
void bug_report_page_text(const char *utf8);
/* 1 while the form is the open title page (main: text input on). */
int  bug_report_page_active(void);
/* 1 from begin until the form closes. */
int  bug_report_busy(void);
/* Back to nothing (session activation): a report being made is discarded. */
void bug_report_reset(void);

/* A freeze / crash dump was written at dump_path (any thread): leave a
 * report pointing to it. */
void bug_report_note_freeze(const char *dump_path, unsigned wedge_kind);

/* Debug server (bug_report command): {"op":"status"|"open"|"text"|"key"|
 * "set"|"save"|"discard", ...}. Writes the answer's members into out. */
int  bug_report_debug(const char *op, const char *text, int key, int row,
                      int value, char *out, size_t cap);

/* The title's word for an English one of the form (psx_mod_bug_report_translate),
 * else the English one. */
const char *bug_report_tr(const char *english);

#define BUG_REPORT_TEXT_MAX 4000   /* the description, bytes of UTF-8 */

/* A red rectangle on the picture, in 0..1 of the window. */
typedef struct BugReportMark { float x0, y0, x1, y1; } BugReportMark;
#define BUG_REPORT_MARKS_MAX 16

/* ---- the form in Dear ImGui (bug_report_ui.cpp) ---------------------------
 * Built with recomp-ui's Dear ImGui and an OpenGL window, the open form is
 * drawn over the presented frame (Unicode text, drop-down lists, mouse)
 * instead of on the page canvas; bug_report.c still runs the page's pause
 * loop and writes the report from the form's values. Without it every call
 * is a no-op and bug_report_ui_open returns 0. Main thread, except draw and
 * release_gl (the GL context's thread). */
typedef struct BugReportUiForm {
    const char *const *types; int type_count, type;            /* shown names */
    const char *const *severities; int severity_count, severity;
    const char *const *frequencies; int frequency_count, frequency;
    const uint32_t *thumb; int thumb_w, thumb_h;               /* the moment, ARGB (copied) */
    int photo, state, clip_frames;                             /* what was taken (PSXModBugReportView) */
    int can_send;                                              /* the form offers "Save and send" */
} BugReportUiForm;
enum { BUG_REPORT_UI_SAVE = 1, BUG_REPORT_UI_DISCARD = 2, BUG_REPORT_UI_SEND = 3 };

/* 1 while the player marks the picture (the form hides, the veil goes). */
int  bug_report_ui_marking(void);
/* The rectangles the player drew, at most max. */
int  bug_report_ui_marks(BugReportMark *out, int max);
/* The game's OpenGL window (NULL: none, the form stays on the canvas). */
void bug_report_ui_set_window(void *sdl_window);
/* 1: the form is up in ImGui, with an empty description. */
int  bug_report_ui_open(const BugReportUiForm *form);
void bug_report_ui_close(void);
/* An SDL event while the form is up: 1 = the form's (keys, text, mouse). */
int  bug_report_ui_event(const void *sdl_event);
/* Typed text / one key press (SDL keycode, KMOD_* mods) from the debug server. */
void bug_report_ui_text(const char *utf8);
void bug_report_ui_key(int sdlk, int mods);
/* P1's controller, PSX_MOD_PAGE_* held. */
void bug_report_ui_pad(uint32_t held);
/* The player saved or discarded (BUG_REPORT_UI_*), once; 0 = not yet. */
int  bug_report_ui_action(void);
void bug_report_ui_values(int *type, int *severity, int *frequency, char *text, int cap);
/* A choice from outside (debug server): which 0 type, 1 severity, 2 frequency. */
void bug_report_ui_set(int which, int value);
/* At the present, over everything (gpu_gl_renderer.c): 1 = drew. */
int  bug_report_ui_draw(int w, int h);
/* Before the GL context goes away, with it current. */
void bug_report_ui_release_gl(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BUG_REPORT_H */
