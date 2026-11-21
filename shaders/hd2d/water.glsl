// Water: the bank and the sprites standing on it mirrored in the water, with a
// slow, light ripple. Written for psxrecomp.
// u_w is the framework's water mask (1 on the 3D pixels painted in a colour the
// game listed as water, gl_renderer_set_water_texels). For each water pixel the
// pass walks up the screen to the bank (the first dry 3D pixel above) and mirrors
// the picture about it, fading with the distance from it; a sprite met on the way
// (standing at the water) is mirrored about its feet instead. The water itself is
// never moved; only the mirrored picture sways a little.
// Nothing leaks out of the water: a pixel changes only when it is water with no
// 2D over it (u_m mask3d: green 3D, blue 2D over it), and only dry 3D scenery with
// no 2D over it and the sprites standing in the scene (u_spr, the framework's
// `sprite` input: no HUD, no window) are mirrored, never the HUD or text.
// u_water: x = reflection strength (0 = none), y = ripple (PS1 pixels of sway),
//          z = reach of the sprites' reflection, w = reach of the scenery's
//          (fractions of the height; a sprite often stands well above the shore).
// u_wave:  x = ripple speed, y = ripple wave length (PS1 lines), zw = 0.
// The mirrored picture is kept clean: only the water's own colour filters it.

uniform vec4 u_water;
uniform vec4 u_wave;

float wet(vec2 uv) { return texture(u_w, uv).r; }
float solid(vec2 uv) { vec2 m = texture(u_m, uv).gb; return step(0.5, m.x) * step(m.y, 0.5); }
float spr(vec2 uv) { return step(texture(u_spr, uv).r, 59999.0); }

// Walk up from (x, y0) in steps: .x = the bank, the first dry 3D scenery pixel
// that stays dry for two more steps (sprites and small dry holes are walked
// through), .y = the first sprite pixel met before it (a sprite standing at the
// water); both refined, -1 when none within the reach.
vec2 walk(float x, float y0, float stp, float reach)
{
    float yb = -1.0, ys = -1.0;
    for (int i = 1; i <= 64; i++) {
        float y = y0 + float(i) * stp;
        if (y > 1.0 || float(i) * stp > reach) break;
        vec2 p = vec2(x, y);
        if (ys < 0.0 && spr(p) > 0.5) ys = y;
        if (wet(p) < 0.5 && solid(p) > 0.5 &&
            wet(vec2(x, y + stp)) < 0.5 && wet(vec2(x, y + 2.0 * stp)) < 0.5) { yb = y; break; }
    }
    if (yb >= 0.0) {
        float lo = yb - stp, hi = yb;             // lo is water, hi is not
        for (int i = 0; i < 5; i++) {
            float m = 0.5 * (lo + hi);
            if (wet(vec2(x, m)) < 0.5) hi = m; else lo = m;
        }
        yb = hi;
    }
    if (ys >= 0.0) {
        float lo = ys - stp, hi = ys;             // lo is no sprite, hi is
        for (int i = 0; i < 5; i++) {
            float m = 0.5 * (lo + hi);
            if (spr(vec2(x, m)) > 0.5) hi = m; else lo = m;
        }
        ys = hi;
    }
    return vec2(yb, ys);
}

void main()
{
    vec3 src = texture(u_src, v_uv).rgb;
    o = vec4(src, 1.0);
    vec2 px = 1.0 / u_size;
    float line = u_size.y / 240.0;                 // target pixels per PS1 line
    if (wet(v_uv) < 0.5 || solid(v_uv) < 0.5) return;

    float reach = u_water.w, sreach = u_water.z;
    float stp = 4.0 * line * px.y;

    // the columns here and 8, 16 and 24 PS1 pixels either side: the bank is
    // their weighted average (1 2 3 4 3 2 1; a column that finds none weighs
    // nothing), so the mirror line runs smooth along it; a sprite's line is the
    // lowest sprite pixel among them (its feet), one line for the whole sprite,
    // so the gaps between its legs and its outline do not slice its reflection
    float dx = 8.0 * line * px.x, hs = 0.0, ws = 0.0, fs = 2.0;
    for (int k = -3; k <= 3; k++) {
        vec2 h = walk(v_uv.x + float(k) * dx, v_uv.y, stp, max(reach, sreach));
        float wk = (4.0 - abs(float(k))) * step(0.0, h.x);
        hs += h.x * wk; ws += wk;
        if (h.y >= 0.0) fs = min(fs, h.y);
    }

    // a slow ripple of long waves: the mirrored picture only bends a little
    float yl = v_uv.y * u_size.y / line;           // PS1 lines
    float sway = sin(yl * 6.2832 / max(u_wave.y, 1.0) + u_time * u_wave.x) * u_water.y * line * px.x;

    vec3 r = src;
    float f = 0.0;
    if (fs < 1.5) {                                // a sprite at the water: mirrored about its feet
        float d = fs - v_uv.y;
        vec2 ruv = vec2(v_uv.x + sway, fs + d);
        if (ruv.y <= 1.0 && spr(ruv) > 0.5) {
            r = texture(u_src, ruv).rgb;
            f = 1.0 - smoothstep(0.5 * sreach, sreach, d);
        }
    }
    if (f <= 0.0 && ws > 0.5) {                    // the bank, and a sprite standing on it
        float hb = hs / ws, d = hb - v_uv.y;
        vec2 ruv = vec2(v_uv.x + sway, hb + d);
        // a sprite already mirrored about its feet is not mirrored again here
        float s = spr(ruv) * step(1.5, fs);
        if (d > 0.0 && ruv.y <= 1.0 && (s > 0.5 || (solid(ruv) > 0.5 && wet(ruv) < 0.5))) {
            r = texture(u_src, ruv).rgb;
            float R = mix(reach, sreach, s);
            f = (1.0 - smoothstep(0.5 * R, R, d)) * clamp(ws / 8.0, 0.0, 1.0);
        }
    }
    if (f <= 0.0) return;

    // the water's colour, averaged over a few PS1 pixels (its texture's grain
    // stays out of the reflection), as a filter over the clean mirrored picture
    vec2 q = 6.0 * line * px;
    vec3 wc = (src + texture(u_src, v_uv + vec2(q.x, 0.0)).rgb + texture(u_src, v_uv - vec2(q.x, 0.0)).rgb
                   + texture(u_src, v_uv + vec2(0.0, q.y)).rgb + texture(u_src, v_uv - vec2(0.0, q.y)).rgb) / 5.0;
    vec3 tint = wc / max(max(wc.r, max(wc.g, wc.b)), 0.05);
    o = vec4(mix(src, r * mix(vec3(1.0), tint, 0.6), u_water.x * f), 1.0);
}
