// Light from the game's own sources, composite. Written
// for this project. Brightens and tints the 3D scenery by the gathered light:
// mostly multiplied (the texture stays readable), a little added (u_light.z,
// so a dark floor still picks up the colour of the spell). Opaque HUD and
// menus are left as they are. Scenery seen through something
// see-through (the spell itself) is lit too: leaving it out drew a dark
// outline around every effect. The light goes on screen-like (a pixel that is
// already bright takes less), so the spell itself does not burn out to white
// and the scenery seen through it is lit as much as the scenery beside it.
// Sprites standing in the scene (u_spr, the framework's `sprite` input) take
// it too, by u_lspr.x (0 = left as they are), with half the added tint (a
// dark costume still picks up the fire's colour).
// u_flicker: x = how much the light flickers (0 = steady), y = its pace: a
// flame's slow, uneven pulse (three slow waves of unrelated frequencies over
// u_time), the same for every light in the picture.
// The lights that cast (the framework's `firemap`, u_fl their places in
// view space, y up, w the map's depth): fire_light gives how much of a light
// reaches a point, by its distance, how the surface faces it and whether
// something nearer the light (the scenery, a sprite) stands in the way.
// Each fire light takes its glow's colour (u_flc rgb, the pieces the
// game draws glowing there: a flame orange, a staff's orb white; none known:
// u_firecol.rgb) and size (w: a small glow lights less and less far,
// gl_postfx.inc). u_flx: x its reach over the map's depth, y its flicker
// (< 0: u_flicker's), z the flicker's phase, w the placed light's index (< 0:
// a fire's); u_fld a spot's direction and half-angle cosine (w 0: all ways).
// The lights placed by hand in the area (gl_postfx.inc, area_lights.h; on by
// u_placed.x): up to 2 among the casting ones (u_flx.w >= 0), the others
// with no shadow in u_al (place, reach), u_alc (colour, strength), u_ald,
// u_alx (as u_fld, u_flx), u_aln.x of them. They add their light to the
// scenery and the sprites (not to the fire's darkening). u_alamb: the area's
// ambient (rgb; w 1 = set): what the 3D scenery and the sprites keep where
// no light reaches (an indoor room under no sun). u_aln.y = 1 draws a marker
// on each placed light, u_aln.z the picked one (a tuning tool).
// The windows placed by hand (u_win.. as shaft.glsl; u_wn.y > 0: shafts on):
// the patch each one's light lands on (back along the light to the window's
// rectangle: inside it, its panes' bars dark, edges softer the farther it
// goes; what stands between, from the depth, shades it), and the lit air and
// dust of the shaft pass (u_sh, blurred) over the scenery and the sprites.
// u_lightview.x = 1 shows the light alone; .y = 1 the fire lights alone
// (white lit, black in their shadow or out of reach).

uniform vec4 u_light;
uniform vec4 u_lightview;
uniform vec4 u_lspr;
uniform vec4 u_flicker;
uniform vec4 u_proj;
uniform vec4 u_fl[4], u_fls[4], u_flc[4], u_flx[4], u_fld[4], u_flmap;
uniform vec4 u_al[8], u_alc[8], u_ald[8], u_alx[8], u_aln, u_alamb;
uniform vec4 u_win[4], u_wu[4], u_wv[4], u_wd[4], u_wc[4], u_wn;
uniform vec4 u_firecol, u_firecfg;

float flicker_by(float t, float amount)
{
    float n = 0.5 * sin(t * 7.3) + 0.3 * sin(t * 13.1 + 1.7) + 0.2 * sin(t * 23.7 + 0.4);
    return 1.0 + amount * n;
}
float flicker(float t) { return flicker_by(t, u_flicker.x); }
// a light's flicker (x: u_flx, u_alx: y the amount, < 0 u_flicker's; z its phase)
float light_flicker(vec4 x)
{
    float pace = max(u_flicker.y, 0.0);
    return x.y < 0.0 ? flicker(u_time * pace) : flicker_by(u_time * max(pace, 1.0) + x.z, x.y);
}
// a spot's share at d (the unit way from the light to the point)
float spot(vec4 s, vec3 d)
{
    return s.w > 0.0 ? smoothstep(s.w, mix(s.w, 1.0, 0.3), dot(d, s.xyz)) : 1.0;
}

// the cube face of direction d (largest axis), its axes as in gl_postfx.inc
const vec3 FF[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 FR[6] = vec3[6](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(-1, 0, 0));
const vec3 FU[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
int fire_face(vec3 d)
{
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x > 0.0 ? 0 : 1;
    if (a.y >= a.z) return d.y > 0.0 ? 2 : 3;
    return d.z > 0.0 ? 4 : 5;
}
// visible from light li at P (1 lit .. 0 in its shadow): 5x5 samples over
// u_firecfg.w world units at P (a flame is no point: soft edges, wider far
// off), turned per pixel so the steps of the map's texels do not show
float fire_vis(vec3 P, vec4 fl, int li)
{
    vec3 d = P - fl.xyz;
    float dist = length(d);
    int f = fire_face(d);
    float z = dot(d, FF[f]);
    vec2 xy = vec2(dot(d, FR[f]), dot(d, FU[f])) / z;
    float t = 1.0 / u_flmap.x;
    float bias = 6.0 + 0.015 * dist;
    float spread = max(u_firecfg.w / max(z, 1.0), 1.5 * t);   // face units per step
    float a = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    float sum = 0.0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            vec2 q = clamp(xy + rot * vec2(i, j) * spread, vec2(-1.0 + t), vec2(1.0 - t)) * 0.5 + 0.5;
            vec2 uv = (vec2(float(f % 3), float(f / 3 + 2 * li)) + q) / vec2(3.0, u_flmap.z);
            sum += texture(u_fm, uv).r * fl.w + bias >= dist ? 1.0 : 0.0;
        }
    return sum / 25.0;
}
// the scenery's depth (SZ, 0 = none) and place in view space, read at the
// centres of the internal pixels (as sun.glsl)
vec2 g_nat, g_grid;
float sz_at(vec2 uv)
{
    float d = texture(u_d, (floor(uv * g_grid) + 0.5) / g_grid).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
}
vec3 pos_at(vec2 uv, float z) { return vec3((uv - 0.5) * g_nat * z / u_proj.x, z); }
// Two native pixels along +d from P on its own face, as a step forward (as
// sun.glsl: the side whose 1/Z runs straightest). The nearest neighbours'
// difference stepped once per native line on a floor (its depth moves in
// small steps line by line), which tilted the normal there: a thin dark
// line across the firelit floor on every native line.
vec3 face_step(vec2 uv0, vec3 P, float z, vec2 d)
{
    float f1 = sz_at(uv0 + d), f2 = sz_at(uv0 + 2.0 * d);
    float b1 = sz_at(uv0 - d), b2 = sz_at(uv0 - 2.0 * d);
    float ef = (f1 > 0.0 && f2 > 0.0) ? abs(1.0 / f2 - 2.0 / f1 + 1.0 / z) : 1e9;
    float eb = (b1 > 0.0 && b2 > 0.0) ? abs(1.0 / b2 - 2.0 / b1 + 1.0 / z) : 1e9;
    if (ef >= 1e9 && eb >= 1e9) return vec3(d * g_nat * z / u_proj.x * 2.0, 0.0);   // alone: facing the camera
    return ef <= eb ? pos_at(uv0 + 2.0 * d, f2) - P : P - pos_at(uv0 - 2.0 * d, b2);
}
// how much of a light at L reaching R reaches P with normal n (n 0: a
// sprite, facing all ways), by distance and facing; s a spot's way
float light_reach(vec3 P, vec3 n, vec3 L, float R, vec4 s)
{
    vec3 d = L - P;
    float dist = length(d);
    if (R <= 0.0 || dist >= R) return 0.0;
    float att = (1.0 - smoothstep(0.35 * R, R, dist)) / (1.0 + dist * dist / (0.08 * R * R));
    float face = dot(n, n) > 0.0 ? clamp(0.25 + 0.75 * dot(n, d / dist), 0.0, 1.0) : 0.7;
    return att * face * spot(s, -d / max(dist, 1e-4));
}
// light li at P with normal n: x = how much reaches it, y = how much would
// but something stands in the way (its shadow); k: its size (1 = a
// campfire's), its reach fl.w * k (the map's depth: fl.w)
vec2 fire_light(vec3 P, vec3 n, vec4 fl, int li, float k, vec4 s)
{
    float a = light_reach(P, n, fl.xyz, fl.w * k, s);
    if (a <= 0.0) return vec2(0.0);
    float vis = fire_vis(P, fl, li);
    return vec2(a * vis, a * (1.0 - vis));
}
// window i's light on P (normal n; 0: a sprite): inside its rectangle seen
// back along the light, the panes' bars dark, the edges softer far from it;
// shaded where the depth shows something between P and the window, or a
// sprite stands in the way
float window_light(vec3 P, vec3 n, int i)
{
    vec3 O = u_win[i].xyz, U = u_wu[i].xyz, V = u_wv[i].xyz, D = u_wd[i].xyz;
    vec3 Nw = normalize(cross(U, V));
    float dn = dot(D, Nw);
    if (abs(dn) < 1e-4) return 0.0;
    float t = dot(P - O, Nw) / dn;
    if (t <= 0.0) return 0.0;
    vec3 Q = P - D * t - O;
    float uu = dot(U, U), vv = dot(V, V), lu = sqrt(uu), lv = sqrt(vv);
    float s = dot(Q, U) / uu, r = dot(Q, V) / vv;
    float soft = 1.5 + 0.012 * t;   // world units
    float es = soft / lu, er = soft / lv;
    float inside = smoothstep(-es, es, s) * smoothstep(1.0 + es, 1.0 - es, s) * smoothstep(-er, er, r) * smoothstep(1.0 + er, 1.0 - er, r);
    if (inside <= 0.0) return 0.0;
    float bu = abs(fract(s * u_wu[i].w + 0.5) - 0.5) * lu / u_wu[i].w;
    float bv = abs(fract(r * u_wv[i].w + 0.5) - 0.5) * lv / u_wv[i].w;
    float bw = 1.6 + 0.004 * t;
    float bars = min(u_wu[i].w > 1.5 ? smoothstep(bw, bw + soft, bu) : 1.0, u_wv[i].w > 1.5 ? smoothstep(bw, bw + soft, bv) : 1.0);
    float face = dot(n, n) > 0.0 ? clamp(dot(n, -D), 0.0, 1.0) : 0.6;
    if (face <= 0.0) return 0.0;
    // something between: steps back toward the window over the depth
    float vis = 1.0;
    for (int k = 1; k <= 10; k++) {
        if (u_lightview.w > 0.5) break;
        vec3 X = P - D * (t * float(k) / 11.0);
        vec2 uv = X.xy * u_proj.x / (X.z * g_nat) + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
        float zs = sz_at(uv);
        if (zs > 0.0 && zs < X.z - (8.0 + 0.004 * X.z)) { vis = 0.0; break; }
        // a sprite standing there (its upright plane at that depth): the light
        // passes through its cut-out, so it casts into the patch
        float zp = texture(u_spr, uv).r;
        if (zp > 0.0 && zp < 50000.0 && abs(zp - X.z) < 22.0) { vis = 0.0; break; }
    }
    return inside * bars * face * vis * (0.65 + 0.35 * exp(-t / 900.0));
}
// a marker on the placed light at L (view space): 1 on its ring, picked bigger
float marker(vec3 L, bool picked)
{
    if (L.z < 1.0) return 0.0;
    vec2 uv = L.xy * u_proj.x / (L.z * g_nat) + 0.5;
    float r = length((v_uv - uv) * u_size), rad = picked ? 13.0 : 9.0;
    return max(1.0 - smoothstep(1.0, 2.2, abs(r - rad)), 1.0 - smoothstep(1.2, 2.4, r));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    float m = texelFetch(u_m3, p, 0).g;
    float zs = texelFetch(u_spr, p, 0).r;
    bool is_spr = m < 0.5 && zs > 0.0 && zs < 50000.0;
    float spr = is_spr ? u_lspr.x : 0.0;
    vec3 l = texture(u_l, v_uv).rgb * flicker(u_time * max(u_flicker.y, 0.0));

    // the fire lights at this point (scenery from its depth, a sprite on its plane)
    vec2 fire = vec2(0.0);
    vec3 firec = vec3(0.0);   // the lights' colours, each by how much of it reaches here
    vec3 placed = vec3(0.0);  // the lights placed by hand, coloured
    vec3 wpatch = vec3(0.0);   // the windows' light where it lands
    g_nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
    g_grid = g_nat * max(u_proj.z, 1.0);
    if ((u_flmap.y > 0.0 || u_aln.x > 0.0 || u_wn.y * u_wn.x > 0.0) && (m > 0.5 || is_spr)) {
        float z = zs;
        if (!is_spr) {
            float dd = texelFetch(u_d, p, 0).r;
            z = dd < 0.99999 ? 256.0 / max(1.0 - dd, 1e-7) - 256.0 : 0.0;
        }
        if (z > 0.0) {
            vec3 P = pos_at(v_uv, z);
            vec3 n = vec3(0.0);
            if (!is_spr) {
                vec2 p1 = 1.0 / g_nat;
                n = normalize(cross(face_step(v_uv, P, z, vec2(p1.x, 0.0)),
                                    face_step(v_uv, P, z, vec2(0.0, p1.y))) + vec3(0.0, 0.0, -1e-6));
                if (dot(n, P) > 0.0) n = -n;   // face the camera, along its ray
            }
            for (int i = 0; i < 4; i++) {
                if (float(i) >= u_flmap.y) break;
                vec2 f = fire_light(P, n, u_fl[i], i, u_flx[i].x, u_fld[i]);
                if (u_flx[i].w < 0.0) { fire += f; firec += u_flc[i].rgb * u_flc[i].w * f.x; }
                else placed += u_flc[i].rgb * u_flc[i].w * f.x * light_flicker(u_flx[i]);
            }
            for (int i = 0; i < 8; i++) {
                if (float(i) >= u_aln.x) break;
                placed += u_alc[i].rgb * u_alc[i].w * light_flicker(u_alx[i])
                        * light_reach(P, n, u_al[i].xyz, u_al[i].w, u_ald[i]);
            }
            if (u_wn.y > 0.5)
                for (int i = 0; i < 4; i++) {
                    if (float(i) >= u_wn.x) break;
                    wpatch += u_wc[i].rgb * window_light(P, n, i);
                }
        }
    }
    float mk = 0.0, mkp = 0.0;   // the tool's markers: the lights, the picked one
    if (u_aln.y > 0.5) {
        for (int i = 0; i < 4; i++) {
            if (float(i) >= u_flmap.y) break;
            if (u_flx[i].w < 0.0) continue;
            bool p = abs(u_flx[i].w - u_aln.z) < 0.5;
            float k = marker(u_fl[i].xyz, p);
            if (p) mkp = max(mkp, k); else mk = max(mk, k);
        }
        for (int i = 0; i < 8; i++) {
            if (float(i) >= u_aln.x) break;
            bool p = abs(u_alx[i].w - u_aln.z) < 0.5;
            float k = marker(u_al[i].xyz, p);
            if (p) mkp = max(mkp, k); else mk = max(mk, k);
        }
    }
    if (u_lightview.y > 0.5) { o = vec4(src * 0.15 + vec3(min(fire.x, 1.0)) + placed, 1.0); return; }
    if (u_lightview.z > 0.5) { o = vec4(src * 0.15 + wpatch, 1.0); return; }

    if (u_lightview.x > 0.5) { o = vec4(src * 0.25 + l * max(m, spr), 1.0); return; }
    // the area's ambient: the scenery and the sprites darker where no light
    // reaches; the placed lights bring back their light (from the colours as drawn)
    vec3 src0 = src;
    if (u_alamb.w > 0.5 && (m > 0.5 || is_spr)) src *= u_alamb.rgb;
    vec3 add = (src * l + u_light.z * l) * m + (src * l + 0.5 * u_light.z * l) * spr;
    // the fire lights: their shadow darkens (u_firecfg.z), their light
    // brightens and tints by each light's colour (u_firecol.w the strength), flickering
    float fw = (m > 0.5 ? 1.0 : spr) * flicker(u_time * max(u_flicker.y, 0.0));
    vec3 fc = firec * (min(fire.x, 1.5) / max(fire.x, 1e-4)) * u_firecol.w * fw;
    src *= 1.0 - u_firecfg.z * min(fire.y, 1.0) * (m > 0.5 ? 1.0 : spr);
    add += src * fc + 0.15 * fc;
    vec3 pc = placed * (m > 0.5 ? 1.0 : spr);
    add += src0 * pc + 0.08 * pc;
    vec3 wp = wpatch * (m > 0.5 ? 1.0 : 0.5 * spr);
    add += src0 * wp * 1.7 + 0.10 * wp;
    vec3 c = src + add * (1.0 - src);
    if (u_wn.y > 0.5 && (m > 0.5 || is_spr)) {   // the shafts' lit air and dust, screened over
        vec4 sh = texture(u_sh, v_uv);
        vec3 air = sh.rgb + sh.a * (u_wn.x > 0.5 ? u_wc[0].rgb : vec3(1.0)) * 1.5;
        c = 1.0 - (1.0 - c) * (1.0 - clamp(air, 0.0, 1.0));
    }
    c = mix(c, vec3(1.0), mk * 0.85);
    c = mix(c, vec3(1.0, 0.75, 0.2), mkp);
    o = vec4(c, 1.0);
}
