// Anti-aliasing of the 3D geometry only. Written for psxrecomp.
// A pixel is smoothed only when it is 3D scenery (mask3d green, nothing 2D
// drawn over it: blue) AND it sits on an edge of the geometry, found in the
// PGXP depth, not in the colours: a silhouette (the depth jumps, or the 3D
// meets the background). Creases between faces are not looked for: 1/depth
// is too coarse on faces seen edge-on (a whole wall showed as a crease).
// Texture detail inside a face, sprites, the HUD and menus are
// left exactly as drawn. The smoothing itself is a short directional blend
// along the edge (luma gradient, two taps each side, clamped to the 3x3
// range of the neighbourhood): one pass, ~14 taps.
// u_aa: x = jump threshold (fraction of the nearer depth), y unused,
//       z = strength (0..1), w = 1: show the edges.

uniform vec4 u_aa;

float sz_at(ivec2 q, ivec2 mx, out float m3, out float cov)
{
    q = clamp(q, ivec2(0), mx);
    vec4 mk = texelFetch(u_m, q, 0);
    m3 = mk.g;
    cov = mk.b;
    float d = texelFetch(u_d, q, 0).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : -1.0;   // -1: no depth
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    ivec2 mx = textureSize(u_d, 0) - 1;
    vec3 src = texture(u_src, v_uv).rgb;
    float m3, cov;
    float c = sz_at(p, mx, m3, cov);
    if (m3 < 0.5 || cov > 0.5 || c < 0.0) { o = vec4(src, 1.0); return; }

    // edge of the geometry: going round the 8 neighbours, the depth jumps
    // (judged against the nearer of the two depths, so both sides of a
    // silhouette count) or the 3D meets the background (no depth and nothing
    // 2D over it). A face's edge or corner gives one run of such neighbours;
    // a texture's cut-out (leaves, grass, a sliver of a branch) gives several
    // and is left alone, as is everything inside a face.
    const ivec2 ring[8] = ivec2[8](ivec2(1, 0), ivec2(1, 1), ivec2(0, 1), ivec2(-1, 1),
                                   ivec2(-1, 0), ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1));
    bool jmp[8];
    int njmp = 0;
    for (int k = 0; k < 8; k++) {
        float nm, nc;
        float n = sz_at(p + ring[k], mx, nm, nc);
        jmp[k] = n < 0.0 ? nc < 0.5 : abs(n - c) > u_aa.x * min(n, c) + 1.0;
        njmp += jmp[k] ? 1 : 0;
    }
    int runs = 0;
    for (int k = 0; k < 8; k++) runs += (jmp[k] && !jmp[(k + 7) % 8]) ? 1 : 0;
    if (njmp == 0 || runs != 1) { o = vec4(src, 1.0); return; }
    if (u_aa.w > 0.5) { o = vec4(mix(src, vec3(1.0, 0.1, 0.1), 0.7), 1.0); return; }

    // directional blend along the edge
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    vec3 nw = texture(u_src, v_uv + vec2(-1.0,  1.0) * px).rgb;
    vec3 ne = texture(u_src, v_uv + vec2( 1.0,  1.0) * px).rgb;
    vec3 sw = texture(u_src, v_uv + vec2(-1.0, -1.0) * px).rgb;
    vec3 se = texture(u_src, v_uv + vec2( 1.0, -1.0) * px).rgb;
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lc = luma(src);
    float lmin = min(lc, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lc, max(max(lnw, lne), max(lsw, lse)));
    if (lmax - lmin < 0.02) { o = vec4(src, 1.0); return; }   // no visible step
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-4.0), vec2(4.0)) * px;
    vec3 a = 0.5 * (texture(u_src, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                    texture(u_src, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = 0.5 * a + 0.25 * (texture(u_src, v_uv - dir * 0.5).rgb + texture(u_src, v_uv + dir * 0.5).rgb);
    float lb = luma(b);
    vec3 aa = (lb < lmin || lb > lmax) ? a : b;
    o = vec4(mix(src, aa, u_aa.z), 1.0);
}
