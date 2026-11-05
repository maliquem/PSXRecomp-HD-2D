// Shadow map check. Written for psxrecomp. Each scenery
// pixel is rebuilt from `depth` and looked up in the framework's `shadow` map
// (u_smx..u_sms): green = the map holds this very surface (the mapping
// agrees), red = something nearer the light in the map (shadowed), blue = the
// map is empty there; the picture dimmed underneath.
// u_view.x = 1 shows the map's depth instead; 2 how far behind the map this pixel
// is (0.5 = on it, 1 unit = 1/400; 0 = map empty).

uniform vec4 u_view;
uniform vec4 u_proj;
uniform vec2 u_pp;    // the projection centre from the picture centre, native px, y down (framework; 0 = the centre)
uniform float u_natx;
uniform vec4 u_shadowdir;
uniform vec4 u_smx, u_smy, u_smz, u_sms;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb * 0.35;
    float d = texelFetch(u_d, p, 0).r;
    if (d >= 0.99999) { o = vec4(src, 1.0); return; }
    float z = 256.0 / max(1.0 - d, 1e-7) - 256.0;
    vec2 nat = vec2(u_proj.y * u_size.x / u_size.y * (u_natx > 0.0 ? u_natx : 1.0), u_proj.y);
    vec3 P = vec3(((v_uv - 0.5) * nat + vec2(-u_pp.x, u_pp.y)) * z / u_proj.x, z);
    vec2 suv = vec2((dot(P, u_smx.xyz) - u_smx.w) * u_sms.x, (dot(P, u_smy.xyz) - u_smy.w) * u_sms.y);
    float own = (u_smz.w - dot(P, u_smz.xyz)) * u_sms.z;
    float m = texture(u_sm, suv).r;
    if (u_view.x > 2.5) { float v = u_view.x > 3.5 ? m / u_sms.z - u_smz.w : z; v = abs(v); o = vec4(floor(v / 16.0) / 255.0, fract(v / 16.0), 0.0, 1.0); return; }   // 3: this pixel's Z, 4: the map's (light at the camera): x 16 in red, the rest in green
    if (u_view.x > 1.5) { o = vec4(vec3(m >= 0.99999 ? 0.0 : 0.5 + (own - m) / u_sms.z / 400.0), 1.0); return; }   // how far behind the map, +-200 units around 0.5
    if (u_view.x > 0.5) { o = vec4(vec3(m), 1.0); return; }
    vec3 c;
    float tol = 0.004;
    if (m >= 0.99999) c = vec3(0.0, 0.0, 1.0);
    else if (m < own - tol) c = vec3(1.0, 0.0, 0.0);
    else if (abs(m - own) <= tol) c = vec3(0.0, 1.0, 0.0);
    else c = vec3(1.0, 1.0, 0.0);
    o = vec4(src + c * 0.6, 1.0);
}
