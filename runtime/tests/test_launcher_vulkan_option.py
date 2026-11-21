#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text(encoding="utf-8")
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

assert "runtime/" + "launcher" not in runtime_cmake
assert "Rml" + "Ui" not in runtime_cmake
assert '#include "launcher.h"' not in main

assert "bool                  video_offer_vulkan = false;" in config_h
assert "bool vulkan_offered = false;" in config_h
assert 'video.contains("offer_vulkan")' in config_cpp
assert 'rt.video_offer_vulkan = toml::find<bool>(video, "offer_vulkan");' in config_cpp
assert "bool vulkan_offered = false;" in config_cpp
assert "loaded.vulkan_offered = vulkan_offered;" in config_cpp

assert '"Software"' in main
assert '"OpenGL"' in main
assert '"Vulkan (Recommended)"' in main
assert '"Vulkan (direct)"' in main
assert "gi->renderer_labels = kPsxRendererLabels;" in main
assert "gi->num_renderers = vulkan_offered_b ? 4 : 3;" in main
assert "id < 0 || id > 3 || (id == 2 && !raw_vulkan_offered)" in main
# Launcher rows are not renderer IDs (2 and 3 trade places): every crossing
# goes through renderer_row_swap, never a straight copy.
for straight in ("g_video_renderer = ls.renderer;", "us.renderer = ls.renderer;",
                 "v.renderer = ls.renderer;", "ls.renderer = v.renderer;",
                 "ls.renderer = g_video_renderer;"):
    assert straight not in main, straight
assert main.count("renderer_row_swap(ls.renderer)") == 4
assert "settings requested Vulkan, but this game does not" in main

print("Launcher Vulkan-option test passed")
