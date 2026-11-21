// Water: reflection, ripples and shimmer. Written for psxrecomp.
// u_w is the framework's water mask (1 on the 3D pixels painted in a colour the
// game listed as water, gl_renderer_set_water_texels). For each water pixel the
// pass walks up the screen to the shore (the first pixel above that is not water)
// and mirrors the picture about it, so the scenery and the sprites standing on the
// bank show upside down in the water, fading with the distance from the shore.
// The shore is 3D scenery (u_m mask3d: green, no 2D over it, blue) dry for two
// more steps up: a small dry hole in the water, or a HUD window over it, is
// walked through, and 2D pixels (HUD, text) are never mirrored.
// u_water: x = reflection strength (0 = none), y = ripple size (PS1 lines),
//          z = shimmer strength, w = reach of the reflection (fraction of the height).
// u_wave:  x = ripple speed, y = ripple length (PS1 lines), zw = 0.
// Pixels outside the mask are copied unchanged.

uniform vec4 u_water;
uniform vec4 u_wave;

float wet(vec2 uv) { return texture(u_w, uv).r; }
float solid(vec2 uv) { vec2 m = texture(u_m, uv).gb; return step(0.5, m.x) * step(m.y, 0.5); }

// The shore above (x, y0): walk up in steps to the first 3D pixel that is dry
// for two more steps, then refine; -1 when none within the reach.
float shore(float x, float y0, float stp, float reach)
{
    float ys = -1.0;
    for (int i = 1; i <= 64; i++) {
        float y = y0 + float(i) * stp;
        if (y > 1.0 || float(i) * stp > reach) break;
        if (wet(vec2(x, y)) < 0.5 && solid(vec2(x, y)) > 0.5 &&
            wet(vec2(x, y + stp)) < 0.5 && wet(vec2(x, y + 2.0 * stp)) < 0.5) { ys = y; break; }
    }
    if (ys < 0.0) return -1.0;
    float lo = ys - stp, hi = ys;                 // lo is water, hi is not
    for (int i = 0; i < 5; i++) {
        float m = 0.5 * (lo + hi);
        if (wet(vec2(x, m)) < 0.5) hi = m; else lo = m;
    }
    return hi;
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main()
{
    vec3 src = texture(u_src, v_uv).rgb;
    if (wet(v_uv) < 0.5) { o = vec4(src, 1.0); return; }

    vec2 px = 1.0 / u_size;
    float line = u_size.y / 240.0;                 // target pixels per PS1 line
    float t = u_time * u_wave.x;
    vec2 sp = v_uv * u_size / line;                // position in PS1 pixels
    float lw = max(u_wave.y, 1.0);
    // two crossing wave trains: a horizontal sway that grows down the screen (nearer water)
    float d = sin(sp.y * 6.2832 / lw + t * 2.1) * 0.6 + sin((sp.x * 0.35 + sp.y) * 6.2832 / (lw * 1.7) - t * 1.3) * 0.4;
    vec2 off = vec2(d * u_water.y * line * px.x, 0.0);

    // the water itself, swayed (only from other water pixels, so the shore does not smear in)
    vec2 uv1 = v_uv + off;
    vec3 c = wet(uv1) > 0.5 ? texture(u_src, uv1).rgb : src;

    // shore: found on the columns every 8 PS1 pixels either side and blended
    // between them, so the line is smooth, not jagged texel by texel
    float reach = u_water.w;
    float stp = 4.0 * line * px.y;
    float bw = 8.0 * line * px.x;
    float bx = v_uv.x / bw - 0.5, w = fract(bx);
    float xl = (floor(bx) + 0.5) * bw;
    float hl = shore(xl, v_uv.y, stp, reach), hr = shore(xl + bw, v_uv.y, stp, reach);
    // a side with no shore weighs nothing, and fades the reflection: no hard edge
    float wl = hl < 0.0 ? 0.0 : 1.0 - w, wr = hr < 0.0 ? 0.0 : w;
    float hi = (hl * wl + hr * wr) / max(wl + wr, 1e-4);
    float dist = hi - v_uv.y;                      // distance below the shore
    if (wl + wr > 0.0 && dist > 0.0) {
        // the reflection sways more the farther it is from the shore
        vec2 ruv = vec2(v_uv.x + off.x * (1.5 + 2.0 * dist / reach), hi + dist);
        if (ruv.y <= 1.0) {
            vec3 r = texture(u_src, ruv).rgb;
            float f = u_water.x * (1.0 - smoothstep(0.0, reach, dist)) * solid(ruv) * (wl + wr);
            // the reflection darkens and takes the water's tint a little
            c = mix(c, r * mix(vec3(1.0), c / max(max(c.r, max(c.g, c.b)), 0.05), 0.35), f);
        }
    }

    // shimmer: small glints on the crests that come and go
    vec2 cell = floor(sp / vec2(3.0, 2.0));
    float g = hash(cell + floor(t * 0.7));
    float crest = smoothstep(0.75, 1.0, d);
    float glint = step(0.985, g) * crest * (0.5 + 0.5 * sin(t * 5.0 + g * 40.0));
    c += vec3(0.9, 0.95, 1.0) * glint * u_water.z;

    o = vec4(clamp(c, 0.0, 1.0), 1.0);
}
