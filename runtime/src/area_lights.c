/* Lights placed by hand per area: the table (area_lights.h). The emulation
 * thread (the file at start), the debug server (a tuning tool) and the render
 * thread (each present) meet here: a short spin lock guards the table, and a
 * reader takes a copy of one area's set. */
#include "area_lights.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AL_AREAS_MAX = 512 };
static AreaLightSet   *s_set[AL_AREAS_MAX];
static int             s_nset = 0;
static atomic_flag     s_lock = ATOMIC_FLAG_INIT;
static _Atomic uint32_t s_ver = 1;

static void al_lock(void) { while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) { } }
static void al_unlock(void) { atomic_flag_clear_explicit(&s_lock, memory_order_release); }

static AreaLightSet *al_find(uint32_t area) {
    for (int i = 0; i < s_nset; i++) if (s_set[i]->area == area) return s_set[i];
    return NULL;
}
uint32_t area_lights_version(void) { return atomic_load(&s_ver); }
int area_lights_get(uint32_t area, AreaLightSet *out) {
    int any = 0;
    al_lock();
    const AreaLightSet *s = area ? al_find(area) : NULL;
    if (s) { *out = *s; any = s->has_amb || s->n > 0; }
    al_unlock();
    if (!s) { memset(out, 0, sizeof *out); out->area = area; }
    return any;
}
int area_lights_areas(uint32_t *areas, int max) {
    int n = 0;
    al_lock();
    for (int i = 0; i < s_nset && n < max; i++)
        if (s_set[i]->has_amb || s_set[i]->n > 0) areas[n++] = s_set[i]->area;
    al_unlock();
    return n;
}

/* ---- reading lines ---------------------------------------------------- */
typedef struct { char *p; } Words;
static char *al_word(Words *w) {
    char *s = w->p;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) { w->p = s; return NULL; }
    char *b = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = '\0';
    w->p = s;
    return b;
}
static int al_num(Words *w, float *v) {
    char *t = al_word(w), *e;
    if (!t) return 0;
    *v = strtof(t, &e);
    return *e == '\0';
}
static int al_nums(Words *w, float *v, int n) {
    for (int i = 0; i < n; i++) if (!al_num(w, &v[i])) return 0;
    return 1;
}
static void al_note(char *dst, const char *src) {
    while (*src && isspace((unsigned char)*src)) src++;
    snprintf(dst, AREA_LIGHT_NOTE, "%s", src);
    for (int k = (int)strlen(dst) - 1; k >= 0 && isspace((unsigned char)dst[k]); k--) dst[k] = '\0';
}
static void al_defaults(AreaLight *l, int kind) {
    memset(l, 0, sizeof *l);
    l->kind = kind;
    l->col[0] = 1.0f; l->col[1] = 0.85f; l->col[2] = 0.6f;
    l->power = 1.0f;
    l->reach = kind == AREA_LIGHT_WINDOW ? 0.0f : 600.0f;
    l->cone = 60.0f;
    l->cols = l->rows = 1;
}
/* One line into the set; *area_out its area word (0 = none). 1 = read (or
 * blank), 0 = an error in err. */
static int al_parse_line(char *line, AreaLightSet *set, uint32_t *area_out, char *err, int errcap) {
    char note[AREA_LIGHT_NOTE] = "";
    char *hash = strchr(line, '#');
    if (hash) { *hash = '\0'; al_note(note, hash + 1); }
    Words w = {line};
    char *t = al_word(&w);
    *area_out = 0;
    if (!t) return 1;
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        *area_out = (uint32_t)strtoul(t, NULL, 16);
        t = al_word(&w);
        if (!t) return 1;
    }
    if (strcmp(t, "amb") == 0) {
        if (!al_nums(&w, set->amb, 3)) { snprintf(err, (size_t)errcap, "amb: R G B"); return 0; }
        memcpy(set->amb_night, set->amb, sizeof set->amb);
        char *k = al_word(&w);
        if (k && (strcmp(k, "night") != 0 || !al_nums(&w, set->amb_night, 3))) {
            snprintf(err, (size_t)errcap, "amb: R G B [night R G B]"); return 0;
        }
        set->has_amb = 1;
        memcpy(set->amb_note, note, sizeof note);
        return 1;
    }
    const int kind = strcmp(t, "point") == 0 ? AREA_LIGHT_POINT : strcmp(t, "spot") == 0 ? AREA_LIGHT_SPOT
                   : strcmp(t, "window") == 0 ? AREA_LIGHT_WINDOW : 0;
    if (!kind) { snprintf(err, (size_t)errcap, "unknown word '%s' (amb, point, spot, window)", t); return 0; }
    if (set->n >= AREA_LIGHT_MAX) { snprintf(err, (size_t)errcap, "at most %d lights per area", AREA_LIGHT_MAX); return 0; }
    AreaLight l;
    al_defaults(&l, kind);
    if (!al_nums(&w, l.p, 3)) { snprintf(err, (size_t)errcap, "%s: X Y Z", t); return 0; }
    if (kind == AREA_LIGHT_WINDOW && (!al_nums(&w, l.a, 3) || !al_nums(&w, l.b, 3))) {
        snprintf(err, (size_t)errcap, "window: three corners X Y Z"); return 0;
    }
    if (kind == AREA_LIGHT_SPOT) { l.a[0] = l.p[0]; l.a[1] = l.p[1] + 100.0f; l.a[2] = l.p[2]; }
    while ((t = al_word(&w)) != NULL) {
        int ok = 1;
        if (strcmp(t, "color") == 0) ok = al_nums(&w, l.col, 3);
        else if (strcmp(t, "power") == 0) ok = al_num(&w, &l.power);
        else if (strcmp(t, "reach") == 0) ok = al_num(&w, &l.reach);
        else if (strcmp(t, "flicker") == 0) ok = al_num(&w, &l.flicker);
        else if (strcmp(t, "dust") == 0) ok = al_num(&w, &l.dust);
        else if (strcmp(t, "cone") == 0) ok = al_num(&w, &l.cone);
        else if (strcmp(t, "shadow") == 0) l.shadow = 1;
        else if (strcmp(t, "at") == 0) ok = al_nums(&w, l.a, 3);
        else if (strcmp(t, "dir") == 0) { ok = al_nums(&w, l.dir, 3); l.has_dir = ok; }
        else if (strcmp(t, "panes") == 0) {
            float c[2];
            ok = al_nums(&w, c, 2);
            l.cols = c[0] < 1.0f ? 1 : c[0] > 16.0f ? 16 : (int)c[0];
            l.rows = c[1] < 1.0f ? 1 : c[1] > 16.0f ? 16 : (int)c[1];
        } else if (strcmp(t, "when") == 0) {
            char *v = al_word(&w);
            ok = v != NULL;
            if (ok) l.when = strcmp(v, "day") == 0 ? AREA_WHEN_DAY : strcmp(v, "night") == 0 ? AREA_WHEN_NIGHT
                           : strcmp(v, "always") == 0 ? AREA_WHEN_ALWAYS : -1;
            ok = ok && l.when >= 0;
        } else if (strcmp(t, "light") == 0) {
            char *v = al_word(&w);
            ok = v != NULL;
            if (ok) l.src = strcmp(v, "sun") == 0 ? AREA_SRC_SUN : strcmp(v, "moon") == 0 ? AREA_SRC_MOON
                          : strcmp(v, "fixed") == 0 ? AREA_SRC_FIXED : -1;
            ok = ok && l.src >= 0;
            /* a direction of its own (a fixed light's, or one over the area's sun) */
            if (ok) {
                const char *q = w.p;   /* looked at without cutting it: a number next? */
                while (*q && isspace((unsigned char)*q)) q++;
                char *e;
                (void)strtof(q, &e);
                float d[2];
                if (e != q && (*e == '\0' || isspace((unsigned char)*e))) {
                    ok = al_nums(&w, d, 2);
                    if (ok) { l.az = d[0]; l.el = d[1]; l.own_dir = 1; }
                } else ok = l.src != AREA_SRC_FIXED;
            }
        } else ok = 0;
        if (!ok) { snprintf(err, (size_t)errcap, "%s: bad or unknown '%s'", kind == AREA_LIGHT_WINDOW ? "window" : "light", t); return 0; }
    }
    memcpy(l.note, note, sizeof note);
    set->l[set->n++] = l;
    return 1;
}
/* the set into the table (made when new); takes the lock */
static int al_store(uint32_t area, const AreaLightSet *set) {
    int ok = 1;
    al_lock();
    AreaLightSet *s = al_find(area);
    if (!s && s_nset < AL_AREAS_MAX && (s = (AreaLightSet *)malloc(sizeof *s)) != NULL) s_set[s_nset++] = s;
    if (s) { *s = *set; s->area = area; } else ok = 0;
    al_unlock();
    atomic_fetch_add(&s_ver, 1u);
    return ok;
}
int area_lights_set_text(uint32_t area, const char *lines, char *err, int errcap) {
    if (!area) { snprintf(err, (size_t)errcap, "no area"); return 0; }
    AreaLightSet *set = (AreaLightSet *)calloc(1, sizeof *set);
    char *buf = (char *)malloc(strlen(lines) + 1);
    if (!set || !buf) { free(set); free(buf); snprintf(err, (size_t)errcap, "out of memory"); return 0; }
    strcpy(buf, lines);
    set->area = area;
    int ok = 1, ln = 0;
    for (char *line = buf, *next; line && ok; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) *next++ = '\0';
        ln++;
        uint32_t a;
        char e[160];
        if (!al_parse_line(line, set, &a, e, (int)sizeof e)) { snprintf(err, (size_t)errcap, "line %d: %s", ln, e); ok = 0; }
        else if (a && a != area) { snprintf(err, (size_t)errcap, "line %d: another area", ln); ok = 0; }
    }
    if (ok) ok = al_store(area, set);
    if (!ok && !err[0]) snprintf(err, (size_t)errcap, "light table full");
    free(set); free(buf);
    return ok;
}
int area_lights_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    AreaLightSet *set = (AreaLightSet *)calloc(1, sizeof *set);
    if (!set) { fclose(f); return -1; }
    /* the file's areas in order; an area's lines need not stand together */
    while (fgets(line, sizeof line, f)) {
        uint32_t area = 0;
        char e[160];
        char copy[512];
        snprintf(copy, sizeof copy, "%s", line);
        Words w = {copy};
        char *t = al_word(&w);
        if (!t || t[0] == '#' || !(t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))) continue;
        area = (uint32_t)strtoul(t, NULL, 16);
        if (!area) continue;
        area_lights_get(area, set);
        uint32_t a;
        if (!al_parse_line(line, set, &a, e, (int)sizeof e)) {
            fprintf(stdout, "psxrecomp: area_lights: 0x%08x: %s\n", (unsigned)area, e);
            continue;
        }
        if (al_store(area, set)) n++;
    }
    fclose(f);
    free(set);
    return n;
}

/* ---- writing lines ---------------------------------------------------- */
static int al_put(char *out, int cap, int k, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int al_put(char *out, int cap, int k, const char *fmt, ...) {
    if (k >= cap) return k;
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(out + k, (size_t)(cap - k), fmt, ap);
    va_end(ap);
    return r < 0 ? k : k + r > cap ? cap : k + r;
}
static int al_put_light(char *out, int cap, int k, uint32_t area, const AreaLight *l) {
    static const char *const kn[] = {"", "point", "spot", "window"};
    static const char *const wn[] = {"always", "day", "night"};
    static const char *const sn[] = {"sun", "moon", "fixed"};
    k = al_put(out, cap, k, "0x%08x %s %.1f %.1f %.1f", (unsigned)area, kn[l->kind], l->p[0], l->p[1], l->p[2]);
    if (l->kind == AREA_LIGHT_WINDOW)
        k = al_put(out, cap, k, "  %.1f %.1f %.1f  %.1f %.1f %.1f panes %d %d light %s", l->a[0], l->a[1], l->a[2],
                   l->b[0], l->b[1], l->b[2], l->cols, l->rows, sn[l->src]);
    if (l->kind == AREA_LIGHT_WINDOW && l->own_dir)
        k = al_put(out, cap, k, " %.1f %.1f", l->az, l->el);
    if (l->kind == AREA_LIGHT_WINDOW && l->has_dir) k = al_put(out, cap, k, " dir %.3f %.3f %.3f", l->dir[0], l->dir[1], l->dir[2]);
    if (l->kind == AREA_LIGHT_SPOT)
        k = al_put(out, cap, k, " at %.1f %.1f %.1f cone %.1f", l->a[0], l->a[1], l->a[2], l->cone);
    k = al_put(out, cap, k, " color %.2f %.2f %.2f power %.2f", l->col[0], l->col[1], l->col[2], l->power);
    if (l->kind != AREA_LIGHT_WINDOW) k = al_put(out, cap, k, " reach %.0f", l->reach);
    if (l->shadow) k = al_put(out, cap, k, " shadow");
    if (l->flicker > 0.0f) k = al_put(out, cap, k, " flicker %.2f", l->flicker);
    if (l->dust > 0.0f) k = al_put(out, cap, k, " dust %.2f", l->dust);
    if (l->when) k = al_put(out, cap, k, " when %s", wn[l->when]);
    if (l->note[0]) k = al_put(out, cap, k, "  # %s", l->note);
    return al_put(out, cap, k, "\n");
}
int area_lights_text(uint32_t area, char *out, int cap) {
    AreaLightSet *s = (AreaLightSet *)malloc(sizeof *s);
    int k = 0;
    if (cap > 0) out[0] = '\0';
    if (!s) return 0;
    area_lights_get(area, s);
    if (s->has_amb) {
        k = al_put(out, cap, k, "0x%08x amb %.2f %.2f %.2f", (unsigned)area, s->amb[0], s->amb[1], s->amb[2]);
        if (memcmp(s->amb, s->amb_night, sizeof s->amb) != 0)
            k = al_put(out, cap, k, " night %.2f %.2f %.2f", s->amb_night[0], s->amb_night[1], s->amb_night[2]);
        if (s->amb_note[0]) k = al_put(out, cap, k, "  # %s", s->amb_note);
        k = al_put(out, cap, k, "\n");
    }
    for (int i = 0; i < s->n; i++) k = al_put_light(out, cap, k, area, &s->l[i]);
    free(s);
    return k;
}
