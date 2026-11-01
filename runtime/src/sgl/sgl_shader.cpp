/* sgl_shader.cpp — GLSL 330 to SPIR-V for the sgl layer, with reflection.
 *
 * The renderer's shaders (and the .pfx chains) are GL GLSL: loose uniforms,
 * no layout() on varyings, samplers numbered by glUniform1i. They are
 * compiled unchanged by glslang under its "Vulkan relaxed" rules (loose
 * uniforms become one block per stage; bindings and locations are numbered
 * automatically, varyings matched by name across the two stages of the
 * program). Then the SPIR-V is put where SDL_GPU wants it:
 *   vertex   samplers set 0 (bindings 0..n-1), uniform block set 1 binding 0
 *   fragment samplers set 2 (bindings 0..n-1), uniform block set 3 binding 0
 * and the outputs named by glBindFragDataLocation* get their locations.
 * SPIRV-Cross reads the result back: block members (offsets, strides, types),
 * samplers, vertex inputs — the uniform catalog glGetUniformLocation serves.
 *
 * GL's clip-space depth is [-1, 1], Vulkan's [0, 1]: the vertex shader's
 * main is renamed and a new main calls it and remaps gl_Position.z. And
 * SDL_GPU puts NDC y = +1 on row 0 of a target on every driver (D3D's way;
 * a flipped viewport on Vulkan) where GL puts y = -1 on row 0: the new main
 * negates gl_Position.y, so a primitive lands on the same memory rows as in
 * GL (gl_FragCoord, viewports, scissors and texel rows already agree). */
#include "sgl_internal.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_cross.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

bool g_glslang_ready = false;

void glslang_init() {
    if (!g_glslang_ready) {
        glslang::InitializeProcess();
        g_glslang_ready = true;
    }
}

/* The vertex shader's main runs inside ours, which maps GL's depth range. */
const char *kVsPostamble =
    "\n#undef main\n"
    "void main() {\n"
    "    psx_sgl_user_main();\n"
    "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n"
    "    gl_Position.y = -gl_Position.y;\n"
    "}\n";

std::string set_log(SglObj *prog, const std::string &msg) {
    free(prog->log);
    prog->log = strdup(msg.c_str());
    return msg;
}

/* ---- compile ------------------------------------------------------------ */
bool compile_program(SglObj *prog, const char *vs_src, const char *fs_src, bool gl_rules,
                     std::vector<uint32_t> spv[2]) {
    glslang_init();
    const EShLanguage stages[2] = {EShLangVertex, EShLangFragment};
    glslang::TShader vs(EShLangVertex), fs(EShLangFragment);
    glslang::TShader *sh[2] = {&vs, &fs};
    std::string pre[2];
    const char *strs[2][2];
    int nstrs[2];
    for (int s = 0; s < 2; s++) {
        glslang::TShader &t = *sh[s];
        t.setEnvInput(glslang::EShSourceGlsl, stages[s], glslang::EShClientVulkan, 100);
        t.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        t.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        t.setEntryPoint("main");
        if (gl_rules) {
            t.setEnvInputVulkanRulesRelaxed();
            t.setAutoMapBindings(true);
            t.setAutoMapLocations(true);
            /* One block per stage, named apart: GLSL would make one block
             * shared by both stages match member for member. */
            t.setGlobalUniformBlockName(s == 0 ? "psx_sgl_vs_globals" : "psx_sgl_fs_globals");
            t.setGlobalUniformSet(s == 0 ? 1 : 3);
            t.setGlobalUniformBinding(0);
            t.setResourceSetBinding({s == 0 ? "0" : "2"});
            if (s == 0) pre[s] = "#define main psx_sgl_user_main\n";
            t.setPreamble(pre[s].c_str());
        }
        strs[s][0] = s == 0 ? vs_src : fs_src;
        nstrs[s] = 1;
        if (gl_rules && s == 0) { strs[s][1] = kVsPostamble; nstrs[s] = 2; }
        t.setStrings(strs[s], nstrs[s]);
    }
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    for (int s = 0; s < 2; s++) {
        if (!sh[s]->parse(GetDefaultResources(), gl_rules ? 330 : 450, gl_rules ? ECoreProfile : ENoProfile,
                          false, false, msgs)) {
            set_log(prog, std::string(s == 0 ? "vertex: " : "fragment: ") + sh[s]->getInfoLog());
            return false;
        }
    }
    glslang::TProgram p;
    p.addShader(&vs);
    p.addShader(&fs);
    if (!p.link(msgs)) { set_log(prog, std::string("link: ") + p.getInfoLog()); return false; }
    if (!p.mapIO()) { set_log(prog, std::string("io: ") + p.getInfoLog()); return false; }
    for (int s = 0; s < 2; s++) {
        glslang::SpvOptions opt;
        opt.generateDebugInfo = false;
        opt.disableOptimizer = true;
        opt.validate = false;
        glslang::GlslangToSpv(*p.getIntermediate(stages[s]), spv[s], &opt);
        if (spv[s].empty()) { set_log(prog, "spirv generation failed"); return false; }
    }
    return true;
}

/* ---- SPIR-V disk cache ----------------------------------------------------
 * glslang is the slow part of a link (a few to tens of ms a program, and the
 * renderer builds its texture-filter variants and the effects' chains as they
 * are first used: hitches the first time). Its output is kept on disk, keyed
 * by the two sources and the rules, under <exe>/cache/sgl_spirv (or
 * PSX_SGL_SHADER_CACHE=<dir>; =0 off). The SPIR-V is placed and reflected
 * after it as usual, so a hit gives the same program as a compile. */
const uint32_t kSpvCacheVersion = 1;   /* bump with anything that changes compile_program's output */

std::string spv_cache_dir() {
    static std::string dir;
    static bool done = false;
    if (done) return dir;
    done = true;
    const char *e = getenv("PSX_SGL_SHADER_CACHE");
    if (e && e[0] == '0' && e[1] == 0) return dir;
    if (e && *e) {
        dir = e;
    } else {
        const char *base = SDL_GetBasePath();
        if (!base) return dir;
        dir = std::string(base) + "cache/sgl_spirv";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) dir.clear();
    return dir;
}
uint64_t fnv1a(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
std::string spv_cache_path(const char *vs_src, const char *fs_src, bool gl_rules) {
    const std::string dir = spv_cache_dir();
    if (dir.empty()) return dir;
    uint64_t h = 1469598103934665603ull;
    const uint32_t tag[2] = {kSpvCacheVersion, gl_rules ? 1u : 0u};
    h = fnv1a(h, tag, sizeof tag);
    h = fnv1a(h, vs_src, strlen(vs_src) + 1);
    h = fnv1a(h, fs_src, strlen(fs_src) + 1);
    h = fnv1a(h, kVsPostamble, strlen(kVsPostamble));
    char name[40];
    snprintf(name, sizeof name, "/%016llx.spv", (unsigned long long)h);
    return dir + name;
}
bool spv_cache_load(const std::string &path, std::vector<uint32_t> spv[2]) {
    if (path.empty()) return false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t hd[4] = {0, 0, 0, 0};
    bool ok = fread(hd, sizeof hd, 1, f) == 1 && hd[0] == 0x434C4753u /* SGLC */ && hd[1] == kSpvCacheVersion &&
              hd[2] > 0 && hd[3] > 0 && hd[2] < (1u << 22) && hd[3] < (1u << 22);
    for (int s = 0; ok && s < 2; s++) {
        spv[s].resize(hd[2 + s]);
        ok = fread(spv[s].data(), 4, spv[s].size(), f) == spv[s].size() && spv[s][0] == 0x07230203u;
    }
    fclose(f);
    if (!ok) { spv[0].clear(); spv[1].clear(); }
    return ok;
}
void spv_cache_store(const std::string &path, const std::vector<uint32_t> spv[2]) {
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t hd[4] = {0x434C4753u, kSpvCacheVersion, (uint32_t)spv[0].size(), (uint32_t)spv[1].size()};
    bool ok = fwrite(hd, sizeof hd, 1, f) == 1;
    for (int s = 0; ok && s < 2; s++) ok = fwrite(spv[s].data(), 4, spv[s].size(), f) == spv[s].size();
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
}

/* ---- SPIR-V decoration patching ---------------------------------------- */
enum { OpName = 5, OpDecorate = 71, OpFunction = 54, OpTypeVoid = 19 };
enum { DecLocation = 30, DecIndex = 32, DecBinding = 33, DecDescriptorSet = 34 };

/* Set (or add) `OpDecorate id dec value`. */
void spv_set_decoration(std::vector<uint32_t> &w, uint32_t id, uint32_t dec, uint32_t value) {
    size_t i = 5, first_after_annotations = 0;
    while (i < w.size()) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (n == 0) break;
        if (op == OpDecorate && n >= 4 && w[i + 1] == id && w[i + 2] == dec) { w[i + 3] = value; return; }
        if (op == OpDecorate || op == 72 /* MemberDecorate */ || op == 73 || op == 74 || op == 75 ||
            op == 332 /* DecorateId */ || op == 5632 || op == 5633)
            first_after_annotations = i + n;
        if (op == OpTypeVoid || op == OpFunction || (op >= 19 && op <= 39)) {
            if (!first_after_annotations) first_after_annotations = i;
            break;
        }
        i += n;
    }
    if (!first_after_annotations) first_after_annotations = i;
    const uint32_t ins[4] = {(4u << 16) | OpDecorate, id, dec, value};
    w.insert(w.begin() + (ptrdiff_t)first_after_annotations, ins, ins + 4);
}
void spv_remove_decoration(std::vector<uint32_t> &w, uint32_t id, uint32_t dec) {
    size_t i = 5;
    while (i < w.size()) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (n == 0) break;
        if (op == OpDecorate && n >= 3 && w[i + 1] == id && w[i + 2] == dec) {
            w.erase(w.begin() + (ptrdiff_t)i, w.begin() + (ptrdiff_t)(i + n));
            continue;
        }
        i += n;
    }
}

GLenum gl_type_of(const spirv_cross::SPIRType &t) {
    using BT = spirv_cross::SPIRType::BaseType;
    const int v = (int)t.vecsize;
    if (t.basetype == BT::SampledImage || t.basetype == BT::Image) {
        const spirv_cross::SPIRType::ImageType &im = t.image;
        (void)im;
        return 0x8B5E; /* refined by the caller */
    }
    switch (t.basetype) {
    case BT::Float:
        if (t.columns == 4) return 0x8B5C;          /* MAT4 */
        if (t.columns == 3) return 0x8B5B;
        if (t.columns == 2) return 0x8B5A;
        return v == 1 ? GL_FLOAT : v == 2 ? 0x8B50 : v == 3 ? 0x8B51 : 0x8B52;
    case BT::Int: return v == 1 ? GL_INT : v == 2 ? 0x8B53 : v == 3 ? 0x8B54 : 0x8B55;
    case BT::UInt: return v == 1 ? GL_UNSIGNED_INT : v == 2 ? 0x8DC6 : v == 3 ? 0x8DC7 : 0x8DC8;
    case BT::Boolean: return v == 1 ? 0x8B56 : v == 2 ? 0x8B57 : v == 3 ? 0x8B58 : 0x8B59;
    default: return GL_FLOAT;
    }
}

SglUniform *uniform_get(std::vector<SglUniform> &us, const std::string &name) {
    for (SglUniform &u : us)
        if (name == u.name) return &u;
    SglUniform u;
    memset(&u, 0, sizeof(u));
    snprintf(u.name, sizeof(u.name), "%s", name.c_str());
    u.count = 1;
    u.off[0] = u.off[1] = -1;
    u.binding[0] = u.binding[1] = -1;
    us.push_back(u);
    return &us.back();
}

/* GL matches varyings by name; glslang's automatic locations follow each
 * stage's declaration order. Number them by name in both stages: the vertex
 * shader's outputs in their order, the fragment inputs it lacks after them. */
void match_varyings(std::vector<uint32_t> spv[2]) {
    std::vector<std::pair<std::string, uint32_t>> names;   /* name -> location */
    uint32_t next = 0;
    auto slots = [](const spirv_cross::Compiler &c, const spirv_cross::Resource &r) {
        const spirv_cross::SPIRType &t = c.get_type(r.type_id);
        uint32_t n = t.columns ? t.columns : 1;
        for (uint32_t a : t.array) n *= a ? a : 1;
        return n;
    };
    auto loc_of = [&](const std::string &name, uint32_t n) {
        for (auto &p : names) if (p.first == name) return p.second;
        names.push_back({name, next});
        next += n;
        return names.back().second;
    };
    {
        spirv_cross::Compiler c(spv[0]);
        spirv_cross::ShaderResources r = c.get_shader_resources();
        std::vector<std::pair<uint32_t, uint32_t>> set;   /* id, location */
        for (const spirv_cross::Resource &res : r.stage_outputs)
            set.push_back({(uint32_t)res.id, loc_of(res.name, slots(c, res))});
        for (auto &p : set) spv_set_decoration(spv[0], p.first, DecLocation, p.second);
    }
    {
        spirv_cross::Compiler c(spv[1]);
        spirv_cross::ShaderResources r = c.get_shader_resources();
        std::vector<std::pair<uint32_t, uint32_t>> set;
        for (const spirv_cross::Resource &res : r.stage_inputs)
            set.push_back({(uint32_t)res.id, loc_of(res.name, slots(c, res))});
        for (auto &p : set) spv_set_decoration(spv[1], p.first, DecLocation, p.second);
    }
}

/* Put the stage's resources where SDL_GPU wants them and read the catalog. */
bool place_and_reflect(SglObj *prog, std::vector<uint32_t> spv[2], bool gl_rules,
                       std::vector<SglUniform> &us) {
    if (gl_rules) match_varyings(spv);
    for (int s = 0; s < 2; s++) {
        const uint32_t sampler_set = s == 0 ? 0 : 2, block_set = s == 0 ? 1 : 3;
        {
            spirv_cross::Compiler c(spv[s]);
            spirv_cross::ShaderResources r = c.get_shader_resources();
            if (gl_rules) {
                /* Samplers: this stage's own 0..n-1, in their mapped order. */
                std::vector<std::pair<uint32_t, uint32_t>> order;   /* (binding, id) */
                for (const spirv_cross::Resource &res : r.sampled_images)
                    order.push_back({c.get_decoration(res.id, spv::DecorationBinding), (uint32_t)res.id});
                std::sort(order.begin(), order.end());
                for (size_t i = 0; i < order.size(); i++) {
                    spv_set_decoration(spv[s], order[i].second, DecDescriptorSet, sampler_set);
                    spv_set_decoration(spv[s], order[i].second, DecBinding, (uint32_t)i);
                }
                for (const spirv_cross::Resource &res : r.uniform_buffers) {
                    spv_set_decoration(spv[s], res.id, DecDescriptorSet, block_set);
                    spv_set_decoration(spv[s], res.id, DecBinding, 0);
                }
                if (s == 1) {
                    /* glBindFragDataLocation(Indexed): the named output's
                     * location. Index 1 (dual-source) has no attachment here:
                     * location 1, unwritten (sgl_caps().dual_source_blend 0). */
                    for (const spirv_cross::Resource &res : r.stage_outputs) {
                        for (int b = 0; b < prog->nfrag_bind; b++) {
                            if (res.name != prog->frag_bind[b].name) continue;
                            uint32_t loc = prog->frag_bind[b].color;
                            if (prog->frag_bind[b].index == 1) loc = 1;
                            spv_set_decoration(spv[s], res.id, DecLocation, loc);
                            spv_remove_decoration(spv[s], res.id, DecIndex);
                        }
                    }
                }
            }
        }
        spirv_cross::Compiler c(spv[s]);
        spirv_cross::ShaderResources r = c.get_shader_resources();
        prog->nsamplers[s] = 0;
        for (const spirv_cross::Resource &res : r.sampled_images) {
            const uint32_t set = c.get_decoration(res.id, spv::DecorationDescriptorSet);
            const uint32_t bind = c.get_decoration(res.id, spv::DecorationBinding);
            if (set != sampler_set || bind >= SGL_MAX_SAMPLERS) {
                set_log(prog, "sampler outside the SDL_GPU layout");
                return false;
            }
            const spirv_cross::SPIRType &t = c.get_type(res.base_type_id);
            const spirv_cross::SPIRType &st = c.get_type(t.image.type);
            const bool is_uint = st.basetype == spirv_cross::SPIRType::UInt;
            const bool is_int = st.basetype == spirv_cross::SPIRType::Int;
            SglUniform *u = uniform_get(us, res.name);
            u->sampler = 1;
            u->gltype = is_uint ? 0x8DD2 : is_int ? 0x8DCA : 0x8B5E;
            u->binding[s] = (int)bind;
            const spirv_cross::SPIRType &vt = c.get_type(res.type_id);
            if (!vt.array.empty() && vt.array[0] > (uint32_t)u->count) u->count = (int)vt.array[0];
            for (int e = 0; e < u->count && (int)bind + e < SGL_MAX_SAMPLERS; e++)
                prog->sampler_uint[s][bind + e] = is_uint;
            if ((int)bind + u->count > prog->nsamplers[s]) prog->nsamplers[s] = (int)bind + u->count;
        }
        prog->block_size[s] = 0;
        for (const spirv_cross::Resource &res : r.uniform_buffers) {
            const spirv_cross::SPIRType &bt = c.get_type(res.base_type_id);
            prog->block_size[s] = (int)c.get_declared_struct_size(bt);
            for (uint32_t m = 0; m < bt.member_types.size(); m++) {
                const spirv_cross::SPIRType &mt = c.get_type(bt.member_types[m]);
                std::string name = c.get_member_name(res.base_type_id, m);
                if (name.empty()) continue;
                SglUniform *u = uniform_get(us, name);
                u->gltype = gl_type_of(mt);
                u->off[s] = (int)c.type_struct_member_offset(bt, m);
                if (!mt.array.empty()) {
                    u->stride[s] = (int)c.type_struct_member_array_stride(bt, m);
                    if ((int)mt.array[0] > u->count) u->count = (int)mt.array[0];
                } else {
                    u->stride[s] = 16;
                }
            }
            break;   /* one block per stage */
        }
        if (s == 0) {
            prog->vs_inputs = 0;
            for (const spirv_cross::Resource &res : r.stage_inputs) {
                const uint32_t loc = c.get_decoration(res.id, spv::DecorationLocation);
                if (loc < SGL_MAX_ATTRIBS) {
                    prog->vs_inputs |= 1u << loc;
                    prog->vs_input_comps[loc] = (uint8_t)c.get_type(res.type_id).vecsize;
                    snprintf(prog->vs_input_name[loc], sizeof(prog->vs_input_name[loc]), "%s",
                             c.get_name(res.id).c_str());
                }
            }
        }
    }
    return true;
}

bool make_shaders(SglObj *prog, std::vector<uint32_t> spv[2]) {
    const uint64_t t0 = SDL_GetTicksNS();
    struct Tm { uint64_t t0; ~Tm() { g_sgl_time_ns[SGL_T_SHADER] += SDL_GetTicksNS() - t0; } } tm{t0};
    for (int s = 0; s < 2; s++) {
        SDL_GPUShaderCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.code_size = spv[s].size() * 4;
        ci.code = (const Uint8 *)spv[s].data();
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.stage = s == 0 ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        ci.num_samplers = (Uint32)prog->nsamplers[s];
        ci.num_uniform_buffers = prog->block_size[s] > 0 ? 1 : 0;
        prog->gs[s] = SDL_CreateGPUShader(g_sgl.dev, &ci);
        if (!prog->gs[s]) {
            set_log(prog, std::string("SDL_CreateGPUShader: ") + SDL_GetError());
            return false;
        }
    }
    return true;
}

int link_common(SglObj *prog, const char *vs_src, const char *fs_src, bool gl_rules) {
    std::vector<uint32_t> spv[2];
    std::vector<SglUniform> us;
    try {
        const std::string cpath = spv_cache_path(vs_src, fs_src, gl_rules);
        if (spv_cache_load(cpath, spv)) {
            g_sgl_spv_hits++;
        } else {
            const uint64_t t0 = SDL_GetTicksNS();
            const bool ok = compile_program(prog, vs_src, fs_src, gl_rules, spv);
            g_sgl_time_ns[SGL_T_GLSL] += SDL_GetTicksNS() - t0;
            if (!ok) return 0;
            g_sgl_spv_misses++;
            spv_cache_store(cpath, spv);
        }
        if (!place_and_reflect(prog, spv, gl_rules, us)) return 0;
    } catch (const std::exception &e) {
        set_log(prog, std::string("reflection: ") + e.what());
        return 0;
    }
    if (!make_shaders(prog, spv)) return 0;
    /* Locations: one per uniform, arrays take count of them. */
    int loc = 0;
    for (SglUniform &u : us) { u.loc = loc; loc += u.count; }
    prog->nu = (int)us.size();
    prog->nlocs = loc;
    prog->u = (SglUniform *)calloc(us.size() ? us.size() : 1, sizeof(SglUniform));
    if (!prog->u) return 0;
    if (!us.empty()) memcpy(prog->u, us.data(), us.size() * sizeof(SglUniform));
    free(prog->loc_u);
    prog->loc_u = (uint16_t *)calloc(loc ? (size_t)loc : 1, sizeof(uint16_t));
    if (!prog->loc_u) return 0;
    for (size_t i = 0; i < us.size(); i++)
        for (int e = 0; e < us[i].count; e++) prog->loc_u[us[i].loc + e] = (uint16_t)i;
    for (int s = 0; s < 2; s++) {
        free(prog->block[s]);
        prog->block[s] = prog->block_size[s] > 0 ? (uint8_t *)calloc(1, (size_t)prog->block_size[s]) : nullptr;
        prog->block_dirty[s] = 1;
        prog->block_stream[s] = UINT64_MAX;
        for (int b = 0; b < SGL_MAX_SAMPLERS; b++) prog->sampler_unit[s][b] = 0;
    }
    return 1;
}

}  // namespace

extern "C" int sgl_shader_link(SglObj *prog, const char *vs_src, const char *fs_src) {
    return link_common(prog, vs_src, fs_src, true);
}
extern "C" int sgl_shader_link_internal(SglObj *prog, const char *vs_src, const char *fs_src) {
    return link_common(prog, vs_src, fs_src, false);
}
extern "C" void sgl_shader_shutdown(void) {
    /* glslang keeps its process state for a later context. */
}
