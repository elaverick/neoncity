cmake_minimum_required(VERSION 3.21)
project(NeonCity C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- Vulkan headers + volk (meta-loader: no SDK import library needed) -------
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.304 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vulkan_headers)

set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
# SOURCE_SUBDIR points at a non-existent directory so MakeAvailable only
# downloads volk; we compile volk.c ourselves against the fetched headers.
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.304 GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(volk)
add_library(volk STATIC ${volk_SOURCE_DIR}/volk.c)
target_include_directories(volk PUBLIC ${volk_SOURCE_DIR})
target_link_libraries(volk PUBLIC Vulkan::Headers)
if(WIN32)
  target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# --- GLFW ---------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# --- stb (image loading for PBR textures; header-only, so download only) ------
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(stb)

# --- Shader compiler: use the Vulkan SDK's if present, otherwise build glslang -
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin)
if(GLSLC)
  message(STATUS "Using glslc: ${GLSLC}")
  set(SHADER_CMD ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num)
  set(SHADER_DEP "")
else()
  find_program(GLSLANG_VALIDATOR glslangValidator HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin)
  if(GLSLANG_VALIDATOR)
    message(STATUS "Using glslangValidator: ${GLSLANG_VALIDATOR}")
    set(SHADER_CMD ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 -x)
    set(SHADER_DEP "")
  else()
    message(STATUS "No Vulkan SDK shader compiler found - building glslang from source")
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
      GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
      GIT_TAG 15.1.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glslang)
    set(SHADER_CMD $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.3 -x)
    set(SHADER_DEP glslang-standalone)
  endif()
endif()

# Shaders are compiled to comma-separated SPIR-V word lists and #included into
# the executable, so the game is a single self-contained binary.
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})
set(SHADERS city.vert city.frag fullscreen.vert sky.frag composite.frag bloom_down.comp bloom_up.comp
            cloud_noise.comp clouds.comp cloud_composite.frag ssr.comp ssr_apply.frag shelter.vert shelter.frag)
file(GLOB SHADER_INCLUDES ${SHADER_SRC_DIR}/*.glsl)
set(SPV_OUTPUTS "")
foreach(S ${SHADERS})
  set(OUT ${SHADER_OUT_DIR}/${S}.inc)
  add_custom_command(OUTPUT ${OUT}
    COMMAND ${SHADER_CMD} -I${SHADER_SRC_DIR} -o ${OUT} ${SHADER_SRC_DIR}/${S}
    DEPENDS ${SHADER_SRC_DIR}/${S} ${SHADER_INCLUDES} ${SHADER_DEP}
    COMMENT "Compiling shader ${S}" VERBATIM)
  list(APPEND SPV_OUTPUTS ${OUT})
endforeach()
add_custom_target(shaders DEPENDS ${SPV_OUTPUTS})

# --- Tests: generator determinism ------------------------------------------------
enable_testing()
add_executable(city_test tests/city_test.c src/city.c src/litter.c src/plan.c src/building.c src/interior.c src/arcology.c src/thread.c)
target_include_directories(city_test PRIVATE src)
# Top-down map of the generated plan (city_map out.bmp [x z] [size] [m/px]).
add_executable(city_map tests/city_map.c src/city.c src/litter.c src/plan.c src/building.c src/interior.c src/arcology.c)
target_include_directories(city_map PRIVATE src)
# Building generator lab: archetype mix, retries, rejections; fails on invalid buildings.
add_executable(building_lab tests/building_lab.c src/city.c src/litter.c src/plan.c src/building.c src/interior.c src/arcology.c)
target_include_directories(building_lab PRIVATE src)
# Office lobby interiors: pass rate, attempts, rejections; optional plan renders
# (interior_lab [size_m] [renders] [out_dir] [verbose_count] [x z]); arcologies with
# plan and section renders (interior_lab arcology [size_m] [renders] [out_dir] [verbose_count] [x z]).
add_executable(interior_lab tests/interior_lab.c src/city.c src/litter.c src/plan.c src/building.c src/interior.c src/arcology.c)
target_include_directories(interior_lab PRIVATE src ${stb_SOURCE_DIR})
foreach(T city_test city_map building_lab interior_lab)
  if(MSVC)
    target_compile_options(${T} PRIVATE /W3 /wd4996)
  else()
    target_link_libraries(${T} PRIVATE m)
    if(UNIX)
      target_link_libraries(${T} PRIVATE pthread)
    endif()
  endif()
endforeach()
add_test(NAME city_determinism COMMAND city_test)
add_test(NAME building_rules COMMAND building_lab 2000)
add_test(NAME interior_rules COMMAND interior_lab 1500)
# Arcology interiors over a wide area (only mega plots are generated, so it is quick).
add_test(NAME arcology_rules COMMAND interior_lab arcology 12000)

# --- Tools: PBR texture packer --------------------------------------------------
# Converts an ambientCG-style download into assets/textures/<name>/ (see README).
add_executable(pbr_pack tools/pbr_pack.c)
target_include_directories(pbr_pack PRIVATE ${stb_SOURCE_DIR})
if(MSVC)
  target_compile_options(pbr_pack PRIVATE /W3 /wd4996)
else()
  target_link_libraries(pbr_pack PRIVATE m)
endif()
# Crops a single round object (the tactile stud) out of its texture set.
add_executable(stud_pack tools/stud_pack.c)
target_include_directories(stud_pack PRIVATE ${stb_SOURCE_DIR})
if(MSVC)
  target_compile_options(stud_pack PRIVATE /W3 /wd4996)
else()
  target_link_libraries(stud_pack PRIVATE m)
endif()

# --- Game ---------------------------------------------------------------------
add_executable(NeonCity
  src/main.c src/vkcore.c src/renderer.c src/textures.c src/city.c src/litter.c src/plan.c src/building.c src/interior.c src/arcology.c src/chunks.c src/player.c src/thread.c)
add_dependencies(NeonCity shaders)
target_include_directories(NeonCity PRIVATE src ${SHADER_OUT_DIR} ${stb_SOURCE_DIR})
# Authored textures are loaded at run time from assets/ beside the executable.
add_custom_command(TARGET NeonCity POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:NeonCity>/assets
  VERBATIM)
target_link_libraries(NeonCity PRIVATE volk glfw)
if(MSVC)
  target_compile_options(NeonCity PRIVATE /W3 /wd4100 /wd4996 /fp:fast)
  set_property(TARGET NeonCity PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET volk PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET glfw PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(NeonCity PRIVATE -Wall -Wno-unused-parameter -Wno-missing-braces)
  target_link_libraries(NeonCity PRIVATE m)
  if(UNIX)
    target_link_libraries(NeonCity PRIVATE pthread dl)
  endif()
endif()
