#!/usr/bin/env python3
"""Guard sdl_gpu (Vulkan) as the default renderer across runtime and generators.

The games ship on sdl_gpu, the GL renderer on SDL3's GPU API. A missing [video]
block or stale generator must not silently fall back to OpenGL or to the slow
SDL/software renderer.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def require_regex(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.S):
        raise AssertionError(message)


def main() -> int:
    config = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
    cli = (ROOT / "recompiler/src/main_cli.cpp").read_text(encoding="utf-8")
    toml = (ROOT / "recompiler/src/main_toml.cpp").read_text(encoding="utf-8")

    require(config, "inline constexpr int VIDEO_RENDERER_SDL_GPU = 3;",
            "sdl_gpu renderer ID is not centrally named")
    require(config,
            "inline constexpr int DEFAULT_VIDEO_RENDERER = VIDEO_RENDERER_SDL_GPU;",
            "runtime config default renderer is not sdl_gpu")
    require(config, "video_renderer = DEFAULT_VIDEO_RENDERER;",
            "RuntimeConfig no longer uses the central default renderer")
    require(main_cpp,
            "g_video_renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;",
            "runtime boot default no longer uses the central default renderer")

    generated_renderer = r'\[video\]\\n"\s*"renderer = \\"sdl_gpu\\"\\n'
    require_regex(cli, generated_renderer,
                  "psxrecomp CLI-generated game.toml does not ask for sdl_gpu")
    require(toml, 'toml += "[video]\\n";',
            "psxrecomp-toml output is missing a [video] block")
    require(toml, 'toml += "renderer = \\"sdl_gpu\\"\\n";',
            "psxrecomp-toml output does not ask for sdl_gpu")
    probe = (ROOT / "tools/new_project_layout/probe_disc.py").read_text(encoding="utf-8")
    require(probe, """'renderer = "sdl_gpu"',""",
            "new-project layout game.toml does not ask for sdl_gpu")

    print("PASS: sdl_gpu is the default renderer and generated TOMLs say so")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
