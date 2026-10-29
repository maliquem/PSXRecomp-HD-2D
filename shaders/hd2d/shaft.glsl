// Light shafts through the windows placed by hand (area_lights.h), the air's
// part. Written for psxrecomp. A window is a rectangle in the room's wall (u_win
// a corner, view space y up, w how far its light goes in; u_wu, u_wv its
// width and height edges, w its panes); its light travels along u_wd; u_wc its
// colour (w: how dusty). Each pixel's camera ray crosses the beam (the
// window's rectangle pushed along u_wd) between two depths, found exactly;
// samples along that stretch, up to the scenery (u_d) or a sprite (u_spr) in front, sum how much
// air is lit: brighter near the window, the panes' bars a little darker, a
// slow drifting haze (3D value noise in the window's own frame, so it stays
// put when the camera turns), and in a = the dust: sparse bright motes.
// u_wn: x the windows, y the quality (1 Low: 14 samples, no dust; 2 High: 26
// and dust). Composited by light-out.glsl.

uniform vec4 u_proj;
uniform vec4 u_win[4], u_wu[4], u_wv[4], u_wd[4], u_wc[4], u_wn;

float hash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.11, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float noise3(vec3 x)
{
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3(1, 0, 0)), f.x), mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x), mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}
// the stretch of z (along the ray: X = R z) where lo <= f0 + f1 z <= hi
vec2 band(float f0, float f1, float lo, float hi, vec2 zz)
{
    if (abs(f1) < 1e-7) return (f0 >= lo && f0 <= hi) ? zz : vec2(1.0, 0.0);
    float a = (lo - f0) / f1, b = (hi - f0) / f1;
    return vec2(max(zz.x, min(a, b)), min(zz.y, max(a, b)));
}

void main()
{
    o = vec4(0.0);
    if (u_wn.x < 0.5 || u_wn.y < 0.5) return;
    vec2 g_nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
    float d = texture(u_d, v_uv).r;
    float zend = d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 20000.0;
    float zp = texture(u_spr, v_uv).r;   // a sprite in front: the air behind it is hidden
    if (zp > 0.0 && zp < 50000.0) zend = min(zend, zp);
    vec3 R = vec3((v_uv - 0.5) * g_nat / u_proj.x, 1.0);   // the ray: X = R z
    int N = u_wn.y > 1.5 ? 26 : 14;
    float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 acc = vec3(0.0);
    float dust = 0.0;
    for (int i = 0; i < 4; i++) {
        if (float(i) >= u_wn.x) break;
        vec3 O = u_win[i].xyz, U = u_wu[i].xyz, V = u_wv[i].xyz, D = u_wd[i].xyz;
        vec3 Nw = normalize(cross(U, V));
        float dn = dot(D, Nw);
        if (abs(dn) < 1e-4) continue;
        float uu = dot(U, U), vv = dot(V, V);
        // t (along D from the window), s, r (across it, 0..1) at X = R z: f0 + f1 z
        float t0 = dot(-O, Nw) / dn, t1 = dot(R, Nw) / dn;
        float s0 = (dot(-O, U) - t0 * dot(D, U)) / uu, s1 = (dot(R, U) - t1 * dot(D, U)) / uu;
        float r0 = (dot(-O, V) - t0 * dot(D, V)) / vv, r1 = (dot(R, V) - t1 * dot(D, V)) / vv;
        vec2 zz = vec2(1.0, zend);
        zz = band(t0, t1, 0.0, u_win[i].w, zz);
        zz = band(s0, s1, -0.04, 1.04, zz);
        zz = band(r0, r1, -0.04, 1.04, zz);
        if (zz.y <= zz.x) continue;
        float dz = (zz.y - zz.x) / float(N);
        float lu = sqrt(uu), lv = sqrt(vv);
        for (int k = 0; k < 26; k++) {
            if (k >= N) break;
            float z = zz.x + (float(k) + jit) * dz;
            float t = t0 + t1 * z, s = s0 + s1 * z, r = r0 + r1 * z;
            float edge = smoothstep(-0.04, 0.04, s) * smoothstep(1.04, 0.96, s) * smoothstep(-0.04, 0.04, r) * smoothstep(1.04, 0.96, r);
            // the panes' bars: a little darker streaks
            float bu = abs(fract(s * u_wu[i].w + 0.5) - 0.5) * lu / u_wu[i].w;
            float bv = abs(fract(r * u_wv[i].w + 0.5) - 0.5) * lv / u_wv[i].w;
            float bars = mix(1.0, min(smoothstep(1.5, 4.0, bu), smoothstep(1.5, 4.0, bv)), 0.45);
            float fade = smoothstep(0.0, 30.0, t) * exp(-t / (0.6 * u_win[i].w + 150.0)) * (1.0 - smoothstep(0.8, 1.0, t / u_win[i].w));
            vec3 L = vec3(s * lu, r * lv, t);   // the window's own frame (world units)
            float haze = 0.55 + 0.6 * noise3(L / 46.0 + vec3(0.0, u_time * 0.05, u_time * 0.03)) + 0.25 * noise3(L / 17.0 - u_time * 0.04);
            float a = edge * bars * fade;
            acc += u_wc[i].rgb * a * haze * dz;
            if (u_wn.y > 1.5 && u_wc[i].w > 0.0) {   // motes: sparse sparks of a fine noise, drifting down slowly
                float m = noise3(L / 3.2 + vec3(0.0, u_time * 0.6, u_time * 0.15));
                dust += u_wc[i].w * a * pow(m, 18.0) * 6.0 * dz;
            }
        }
    }
    o = vec4(acc / 120.0, dust / 25.0);
}
