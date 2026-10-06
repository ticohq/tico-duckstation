# From PCSX2: On macOS, Mono.framework contains an ancient version of libpng. We don't want that.
# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
if(APPLE)
  set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
  set(CMAKE_FIND_FRAMEWORK NEVER)
endif()

# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NINTENDO_SWITCH)
  # The libraries portlibs lacks (SoundTouch, SPIRV-Cross) are built
  # and installed into TICO_DEPS_DIR by build_duckstation_nro.sh; no SDL, the
  # Switch has its own input and audio.
  list(APPEND CMAKE_FIND_ROOT_PATH "${TICO_DEPS_DIR}")
  list(APPEND CMAKE_PREFIX_PATH "${TICO_DEPS_DIR}")
else()
  find_package(SDL2 2.30.6 REQUIRED)
endif()
find_package(Zstd 1.5.6 REQUIRED)
find_package(WebP REQUIRED) # v1.4.0, spews an error on Linux because no pkg-config.
find_package(ZLIB REQUIRED) # 1.3, but Mac currently doesn't use it.
find_package(PNG 1.6.40 REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype 2.13.2 REQUIRED) # 2.13.3, but flatpak is still on 2.13.2.
if(NINTENDO_SWITCH)
  # SoundTouch and its C wrapper (the API DuckStation calls), compiled into
  # static libraries by build_duckstation_nro.sh: its own build only makes the
  # wrapper as a shared library
  add_library(SoundTouch::SoundTouch STATIC IMPORTED)
  set_target_properties(SoundTouch::SoundTouch PROPERTIES
    IMPORTED_LOCATION "${TICO_DEPS_DIR}/lib/libSoundTouch.a"
    INTERFACE_INCLUDE_DIRECTORIES "${TICO_DEPS_DIR}/include")
  add_library(SoundTouch::SoundTouchDLL STATIC IMPORTED)
  set_target_properties(SoundTouch::SoundTouchDLL PROPERTIES
    IMPORTED_LOCATION "${TICO_DEPS_DIR}/lib/libSoundTouchDLL.a"
    INTERFACE_INCLUDE_DIRECTORIES "${TICO_DEPS_DIR}/include"
    INTERFACE_LINK_LIBRARIES SoundTouch::SoundTouch)
else()
  find_package(SoundTouch 2.3.3 REQUIRED)
  # no SVG fonts (lunasvg), cpuinfo backend or Discord for Horizon
  find_package(lunasvg 2.4.1 REQUIRED)
  find_package(cpuinfo REQUIRED)
  find_package(DiscordRPC 3.4.0 REQUIRED)
endif()

if(NOT WIN32)
  find_package(CURL REQUIRED)
endif()

if(ENABLE_X11)
  find_package(X11 REQUIRED)
  if (NOT X11_Xrandr_FOUND)
    message(FATAL_ERROR "XRandR extension is required")
  endif()
endif()

if(ENABLE_WAYLAND)
  find_package(ECM REQUIRED NO_MODULE)
  list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
  find_package(Wayland REQUIRED Egl)
endif()

if(ENABLE_VULKAN AND NOT NINTENDO_SWITCH)
  # on the Switch, glslang compiles the shaders and SPIRV-Cross is linked statically
  find_package(Shaderc REQUIRED)
  find_package(spirv_cross_c_shared REQUIRED)

  if(LINUX)
    # We need to add the rpath for shaderc to the executable.
    get_target_property(SHADERC_LIBRARY Shaderc::shaderc_shared IMPORTED_LOCATION)
    get_filename_component(SHADERC_LIBRARY_DIRECTORY ${SHADERC_LIBRARY} DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH ${SHADERC_LIBRARY_DIRECTORY})
    get_target_property(SPIRV_CROSS_LIBRARY spirv-cross-c-shared IMPORTED_LOCATION)
    get_filename_component(SPIRV_CROSS_LIBRARY_DIRECTORY ${SPIRV_CROSS_LIBRARY} DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH ${SPIRV_CROSS_LIBRARY_DIRECTORY})
  endif()
endif()

if(LINUX)
  find_package(UDEV REQUIRED)
endif()

if(NOT WIN32 AND NOT APPLE AND NOT NINTENDO_SWITCH)
  find_package(Libbacktrace REQUIRED)
endif()

if(NOT ANDROID AND NOT WIN32 AND NOT NINTENDO_SWITCH)
  find_package(FFMPEG COMPONENTS avcodec avformat avutil swresample swscale)
  if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found, using bundled headers.")
  endif()
endif()
if(NOT ANDROID AND NOT FFMPEG_FOUND)
  set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/ffmpeg/include")
endif()

if(APPLE)
  set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})
endif()
