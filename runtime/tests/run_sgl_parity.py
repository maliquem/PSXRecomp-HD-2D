"""Run a GL renderer fixture on the driver's OpenGL and on sgl (SDL_GPU).

The fixtures (test_gl_*.c) include gpu_gl_renderer.c and check what it draws
against the software renderer, word for word. Built once with sgl compiled in
(PSX_HAVE_SGL) and run twice: PSX_GL_IMPL=opengl, then PSX_GL_IMPL=sgl. Both
runs must pass; on sgl that is the GL renderer drawn by Vulkan / Direct3D 12.

    python -I run_sgl_parity.py --build-dir K:/src/bof4-gpu/build-gpu --out <dir>

--build-dir is a configured game build whose _deps hold glslang and
SPIRV-Cross already built (any target linking the runtime builds them).
Light: hidden 128x128 window, a few hundred draws per run."""
import argparse
import os
import pathlib
import re
import subprocess
import sys


def find_one(root, pattern):
    hits = sorted(pathlib.Path(root).rglob(pattern))
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--toolchain", default=os.environ.get("PSXRECOMP_TOOLCHAIN_DIR", r"C:\retcomm-toolchain"))
    ap.add_argument("--fixture", default="test_gl_readback_region.c")
    ap.add_argument("--scales", default="1,4")
    ap.add_argument("--impls", default="opengl,sgl")
    a = ap.parse_args()
    fw = pathlib.Path(__file__).resolve().parents[2]
    tc = pathlib.Path(a.toolchain)
    bin_ = tc / "bin"
    build = pathlib.Path(a.build_dir).resolve()
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    deps = build / "_deps"
    glslang_src = deps / "psx_glslang-src"
    spvc_src = deps / "psx_spirv_cross-src"
    libs = []
    for pat in ("libglslang.a", "libglslang-default-resource-limits.a",
                "libspirv-cross-hlsl.a", "libspirv-cross-msl.a", "libspirv-cross-glsl.a",
                "libspirv-cross-core.a"):
        p = find_one(deps, pat)
        if not p:
            print("missing", pat, "under", deps, "- build the runtime target first")
            return 2
        libs.append(p)
    sdl_inc = tc / "deps" / "include"
    sdl_lib = tc / "deps" / "lib" / "libSDL3.a"
    env = os.environ.copy()
    env["PATH"] = str(bin_) + os.pathsep + env.get("PATH", "")
    inc = ["-I", str(fw / "runtime/include"), "-I", str(fw / "runtime/src"),
           "-I", str(fw / "runtime/tests"), "-I", str(sdl_inc)]
    defs = ["-DPSX_SDL3=1", "-DPSX_NO_DEBUG_TOOLS=1", "-DPSX_HAVE_SGL=1"]
    src = fw / "runtime/src"
    # The GL fixtures open an SDL_WINDOW_OPENGL window: a copy of the fixture
    # asks the window sgl needs instead when PSX_GL_IMPL=sgl.
    fixture = fw / "runtime/tests" / a.fixture
    text = fixture.read_text(encoding="utf-8")
    if "SDL_WINDOW_OPENGL" in text and "impl_window_flags" not in text and "sgl_window_flags" not in text:
        text = text.replace("SDL_WINDOW_OPENGL", "PSX_TEST_WINDOW_FLAGS")
        text = ("#define PSX_TEST_WINDOW_FLAGS (gl_api_impl() == 1 ? "
                "(Uint32)gl_renderer_impl_window_flags() : SDL_WINDOW_OPENGL)\n" + text)
        fixture = out / ("fixture_" + a.fixture)
        fixture.write_text(text, encoding="utf-8")
    c_sources = [("probe", fixture), ("sw", src / "gpu_sw_renderer.c"),
                 ("rp", src / "render_pass_plan.c"), ("xr", src / "psx_openxr.c"),
                 ("rth", src / "render_thread.c"), ("pth", src / "present_thread.c"),
                 ("fg", src / "frame_gen.c"), ("hov", src / "host_overlay.c"),
                 ("al", src / "area_lights.c"),
                 ("sgl_gl", src / "sgl/sgl_gl.c"), ("sgl_gpu", src / "sgl/sgl_gpu.c")]
    cxx_sources = [("sgl_shader", src / "sgl/sgl_shader.cpp")]
    log = []

    def run(cmd, **kw):
        cmd = [str(c) for c in cmd]
        r = subprocess.run(cmd, cwd=out, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", env=kw.get("env", env))
        log.append((cmd, r.returncode, r.stdout, r.stderr))
        return r

    objs = []
    for name, path in c_sources:
        if not path.exists():
            continue
        o = out / (name + ".o")
        r = run([bin_ / "clang.exe", "-std=gnu11", "-O1", "-flto", "-w", *defs, *inc, "-c", path, "-o", o])
        if r.returncode:
            print(r.stderr[-4000:])
            return 2
        objs.append(o)
    for name, path in cxx_sources:
        o = out / (name + ".o")
        r = run([bin_ / "clang++.exe", "-std=c++17", "-O1", "-flto", "-w", *defs, *inc,
                 "-I", glslang_src, "-I", spvc_src, "-c", path, "-o", o])
        if r.returncode:
            print(r.stderr[-4000:])
            return 2
        objs.append(o)
    sys_libs = ["m", "kernel32", "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
                "version", "uuid", "advapi32", "setupapi", "shell32", "dinput8", "opengl32"]
    exe = out / "probe.exe"
    # Full LTO: the fixtures stub only what their paths reach; LTO drops the rest
    # (as the GL fixtures' own runners do).
    link = [bin_ / "clang++.exe", "-O1", "-flto", "-fuse-ld=lld", "-Wl,--error-limit=0", *objs,
            sdl_lib, *libs, *["-l" + l for l in sys_libs], "-static", "-o", exe]
    r = run(link)
    if r.returncode:
        # The renderer grows faster than the fixtures' stubs: stub what is
        # still missing (0 / zeroed storage), typed by the runtime's own objects.
        missing = sorted(set(re.findall(r"undefined symbol: (\S+)", r.stderr)))
        if not missing:
            print(r.stderr[-4000:])
            return 2
        kinds = {}
        rt_objs = list((build / "CMakeFiles").rglob("psxrecomp/runtime/src/**/*.obj"))
        nm = subprocess.run([str(bin_ / "llvm-nm.exe"), "--defined-only", *map(str, rt_objs)],
                            capture_output=True, text=True, errors="replace").stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] in missing:
                kinds.setdefault(parts[2], parts[1])
        lines = ["/* run_sgl_parity.py: stubs for what the fixture does not provide. */"]
        for sym in missing:
            k = kinds.get(sym, "T")
            if k in "DdBbRrCc":
                lines.append(f"__attribute__((aligned(16))) unsigned char {sym}[4096];")
            else:
                lines.append(f"long long {sym}(void) {{ return 0; }}")
        stub = out / "auto_stubs.c"
        stub.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"auto-stubbed {len(missing)} symbols ({stub})")
        so = out / "auto_stubs.o"
        if run([bin_ / "clang.exe", "-O1", "-w", "-c", stub, "-o", so]).returncode:
            return 2
        link.insert(link.index(sdl_lib), so)
        r = run(link)
        if r.returncode:
            print(r.stderr[-4000:])
            return 2
    results = {}
    for impl in a.impls.split(","):
        for scale in a.scales.split(","):
            e = env.copy()
            e["PSX_GL_IMPL"] = impl
            r = run([exe, scale], env=e)
            lines = r.stdout.strip().splitlines()
            m = re.fullmatch(r"checks=(\d+) failures=(\d+)", lines[-1]) if lines else None
            fails = [l for l in r.stderr.splitlines() if l.startswith("FAIL ")]
            ok = r.returncode == 0 and m and int(m[2]) == 0
            results[(impl, scale)] = ok
            head = next((l for l in lines if l.startswith("driver=")), "")
            print(f"{impl:7s} scale {scale}: {'PASS' if ok else 'FAIL'} "
                  f"({m[0] if m else 'no summary'}, exit {r.returncode}) {head}")
            for f in fails[:12]:
                print("   ", f)
            for l in r.stderr.splitlines():
                if "words differ" in l or "sgl:" in l:
                    print("   ", l)
            (out / f"run_{impl}_{scale}.txt").write_text(r.stdout + "\n--- stderr ---\n" + r.stderr,
                                                         encoding="utf-8")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
