include_guard(GLOBAL)

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/psx_dependency_archive.cmake")

# The shader compilers of the sgl layer ([video] renderer = "sdl_gpu",
# docs/SDL_GPU_RENDERER_PLAN.md): the GL renderer's GLSL 330 (its own programs
# and the .pfx chains, loaded and swapped live) is compiled to SPIR-V at run
# time by glslang, and SPIR-V to HLSL (Direct3D 12) / MSL (Metal) by
# SPIRV-Cross. Both are built here as static libraries from pinned archives
# (third_party/deps.manifest; vendored copies are used when present), so the
# game never needs the network to compile a shader.
#   glslang     BSD-3-Clause / MIT / Apache-2.0 (KhronosGroup/glslang)
#   SPIRV-Cross Apache-2.0                     (KhronosGroup/SPIRV-Cross)

set(_psx_sgl_ts "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(_psx_sgl_ts DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# glslang: the GLSL front end + SPIR-V back end only (no optimizer, no HLSL
# input, no binaries, no tests, no install).
set(ENABLE_OPT OFF CACHE BOOL "glslang: no spirv-opt (sgl)" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "glslang: no HLSL input (sgl)" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "glslang: no binaries (sgl)" FORCE)
set(ENABLE_GLSLANG_JS OFF CACHE BOOL "glslang: no JS (sgl)" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "glslang: no tests (sgl)" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "glslang: no install (sgl)" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "glslang: no External/ (sgl)" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "glslang: no PCH (sgl)" FORCE)

psxrecomp_dependency_source_dir(psx_glslang ENV PSX_GLSLANG_SOURCE_DIR
    OUT _psx_glslang_src)
psxrecomp_dependency_archive(psx_glslang SOURCE_DIR "${_psx_glslang_src}"
    OUT_URL _psx_glslang_url OUT_HASH _psx_glslang_hash)
FetchContent_Declare(psx_glslang
    URL "${_psx_glslang_url}"
    URL_HASH "${_psx_glslang_hash}"
    ${_psx_sgl_ts})

# SPIRV-Cross: the GLSL/HLSL/MSL back ends as static libraries only.
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)

psxrecomp_dependency_source_dir(psx_spirv_cross ENV PSX_SPIRV_CROSS_SOURCE_DIR
    OUT _psx_spvc_src)
psxrecomp_dependency_archive(psx_spirv_cross SOURCE_DIR "${_psx_spvc_src}"
    OUT_URL _psx_spvc_url OUT_HASH _psx_spvc_hash)
FetchContent_Declare(psx_spirv_cross
    URL "${_psx_spvc_url}"
    URL_HASH "${_psx_spvc_hash}"
    ${_psx_sgl_ts})

# Neither project's warnings are ours; keep them out of the runtime's build log.
set(_psx_sgl_saved_c_flags "${CMAKE_C_FLAGS}")
set(_psx_sgl_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
FetchContent_MakeAvailable(psx_glslang psx_spirv_cross)
set(CMAKE_C_FLAGS "${_psx_sgl_saved_c_flags}")
set(CMAKE_CXX_FLAGS "${_psx_sgl_saved_cxx_flags}")

# One interface target the runtime links: the compilers and their headers.
if(NOT TARGET psx_sgl_shadercomp)
    add_library(psx_sgl_shadercomp INTERFACE)
    target_link_libraries(psx_sgl_shadercomp INTERFACE
        glslang glslang-default-resource-limits
        spirv-cross-hlsl spirv-cross-msl spirv-cross-glsl spirv-cross-core)
    target_include_directories(psx_sgl_shadercomp INTERFACE
        "${psx_glslang_SOURCE_DIR}"
        "${psx_spirv_cross_SOURCE_DIR}")
endif()

unset(_psx_glslang_src)
unset(_psx_glslang_url)
unset(_psx_glslang_hash)
unset(_psx_spvc_src)
unset(_psx_spvc_url)
unset(_psx_spvc_hash)
unset(_psx_sgl_ts)
