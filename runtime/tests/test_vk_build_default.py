#!/usr/bin/env python3
from pathlib import Path
import re


root = Path(__file__).parents[2]
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text()
config_loader = (root / "recompiler" / "src" / "config_loader.h").read_text()

option = re.search(
    r'option\(PSX_ENABLE_VULKAN\s+"[^"]*"\s+(ON|OFF)\)', runtime_cmake
)
assert option, "PSX_ENABLE_VULKAN option not found"
assert option.group(1) == "ON", "Vulkan backend is not compiled by default"
# The raw Vulkan backend is built, but the default renderer is sdl_gpu, not it.
assert "DEFAULT_VIDEO_RENDERER = VIDEO_RENDERER_SDL_GPU" in config_loader, (
    "the runtime renderer default is not sdl_gpu"
)

print("Vulkan build/default-renderer policy test passed")
