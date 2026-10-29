#ifndef PSXRECOMP_AREA_LIGHTS_H
#define PSXRECOMP_AREA_LIGHTS_H
/* Lights placed by hand per area (the area of [video.shadow] sun_areas: a
 * mod's map id or the scene_keys' number): point lights, spot lights and
 * windows (an opening the sun or the moon shines through, drawn as a shaft and
 * the patch it lights), and how dark the area is where none of them reaches
 * (an indoor room under no sun). Positions are in the game's world (the
 * coordinates the frame camera maps to view space, as the fire lights' and the
 * shadow world's). A title's file (the runtime reads area_lights.txt next to
 * the exe) and a tuning tool (TCP area_lights) fill it; the light chains read
 * the current area's set at each present. One line per light:
 *
 *   0xAREA amb R G B [night R G B]
 *   0xAREA point X Y Z [color R G B] [power F] [reach A] [shadow] [flicker F] [when day|night|always]
 *   0xAREA spot X Y Z at X Y Z [cone DEG] (then as point)
 *   0xAREA window X Y Z  X Y Z  X Y Z [panes C R] [light sun|moon|fixed [AZ EL]] [dir X Y Z]
 *                [color R G B] [power F] [dust F] [when ...]
 *
 * A window is the corner X Y Z, the corner along its width, the corner along
 * its height; the light goes through it along dir (the world way it travels),
 * else from the area's sun (or moon), else straight in and down. Its colour
 * is the day's; at night (the night's share) it turns to moonlight. '#'
 * starts a note, kept with the light. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AREA_LIGHT_MAX = 16, AREA_LIGHT_NOTE = 48 };
enum { AREA_LIGHT_POINT = 1, AREA_LIGHT_SPOT, AREA_LIGHT_WINDOW };
enum { AREA_WHEN_ALWAYS = 0, AREA_WHEN_DAY, AREA_WHEN_NIGHT };
enum { AREA_SRC_SUN = 0, AREA_SRC_MOON, AREA_SRC_FIXED };

typedef struct {
    int   kind;
    float p[3];           /* the light; a window's first corner */
    float a[3], b[3];     /* spot: the point it faces (a); window: its corners along the width (a), the height (b) */
    float col[3], power, reach;
    float flicker;        /* 0 steady .. 1 a flame's */
    float cone;           /* spot: its full angle, degrees */
    int   shadow, when;
    int   cols, rows;     /* window: its panes */
    int   src;            /* window: sun, moon or a fixed direction */
    int   own_dir;        /* its own direction (az, el, degrees as sun_areas'), over the area's sun or moon */
    float az, el, dust;
    int   has_dir;        /* window: dir set */
    float dir[3];         /* window: the way its light travels (world) */
    char  note[AREA_LIGHT_NOTE];
} AreaLight;

typedef struct {
    uint32_t  area;
    int       has_amb;
    float     amb[3], amb_night[3];
    char      amb_note[AREA_LIGHT_NOTE];
    int       n;
    AreaLight l[AREA_LIGHT_MAX];
} AreaLightSet;

/* The area's set (1: it has an ambient or a light). Any thread. */
int      area_lights_get(uint32_t area, AreaLightSet *out);
/* Replaces the area's set with these file lines (the area word optional on
 * each; "" = none). 0 and a message when a line does not read. */
int      area_lights_set_text(uint32_t area, const char *lines, char *err, int errcap);
/* The area's set as file lines, with the area word. */
int      area_lights_text(uint32_t area, char *out, int cap);
/* The areas that have a set, up to max. */
int      area_lights_areas(uint32_t *areas, int max);
/* A file of such lines (several areas): the lights read, -1 none. */
int      area_lights_load(const char *path);
/* Bumped at each change (a reader's cache). */
uint32_t area_lights_version(void);

#ifdef __cplusplus
}
#endif
#endif
