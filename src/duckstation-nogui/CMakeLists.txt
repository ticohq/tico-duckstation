add_executable(duckstation-nogui
  nogui_host.cpp
  nogui_host.h
  nogui_platform.h
)

target_precompile_headers(duckstation-nogui PRIVATE "pch.h")
target_link_libraries(duckstation-nogui PRIVATE core util common imgui scmversion)

add_core_resources(duckstation-nogui)

if(NINTENDO_SWITCH)
  enable_language(ASM)

  target_sources(duckstation-nogui PRIVATE
    switch_nogui_platform.cpp
    switch_nogui_platform.h
    switch_asm.S
    ../tico/TicoDuckBridge.h
  )
  # the tico overlay (quick menu, settings, states, RetroAchievements toasts)
  set(TICO_FRONTEND "${CMAKE_SOURCE_DIR}/tico/frontend")
  target_sources(duckstation-nogui PRIVATE
    ${TICO_FRONTEND}/DuckCheats.cpp
    ${TICO_FRONTEND}/DuckDiscs.cpp
    ${TICO_FRONTEND}/DuckLibrary.cpp
    ${TICO_FRONTEND}/DuckShaders.cpp
    ${TICO_FRONTEND}/DuckSlangChain.cpp
    ${TICO_FRONTEND}/TicoChainload.cpp
    ${TICO_FRONTEND}/TicoDuckBridge.cpp
    ${TICO_FRONTEND}/TicoSlang.cpp
    ${TICO_FRONTEND}/UsbStorage.cpp
    ${TICO_FRONTEND}/overlay/imgui_overlay.cpp
    ${TICO_FRONTEND}/overlay/overlay_ui.cpp
    ${TICO_FRONTEND}/overlay/ra_alerts.cpp
    ${TICO_FRONTEND}/overlay/tico_config.cpp
    ${TICO_FRONTEND}/overlay/translation_manager.cpp
  )
  target_include_directories(duckstation-nogui PRIVATE
    "${TICO_FRONTEND}" "${TICO_FRONTEND}/deps/usbhsfs/include")
  # RetroArch slang presets (TicoSlang): glslang compiles them for their
  # reflection, SPIRV-Reflect reads it
  set(TICO_SPIRV_REFLECT "${CMAKE_SOURCE_DIR}/tico/deps/SPIRV-Reflect")
  target_sources(duckstation-nogui PRIVATE "${TICO_SPIRV_REFLECT}/spirv_reflect.c")
  set_source_files_properties("${TICO_SPIRV_REFLECT}/spirv_reflect.c" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(duckstation-nogui PRIVATE
    "${TICO_SPIRV_REFLECT}" "${TICO_GLSLANG_SOURCE}" "${TICO_GLSLANG_BUILD}/include")
  # USB drives: FAT/exFAT, and NTFS read only
  target_link_libraries(duckstation-nogui PRIVATE
    "${TICO_FRONTEND}/deps/usbhsfs/lib/libusbhsfs.a" "${TICO_FRONTEND}/deps/usbhsfs/lib/libusbntfs.a")
  # libsodium: TicoSecure opens the session tico seals for each launch
  target_link_libraries(duckstation-nogui PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib/libsodium.a")
  target_include_directories(duckstation-nogui PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")
  target_link_libraries(duckstation-nogui PUBLIC "${NX_LIBRARIES}")

  # The renderers' drivers, linked statically (build_duckstation_nro.sh):
  # deko3D's shader compiler, glslang for Vulkan's shaders, and Mesa's NVK for
  # Vulkan.
  if(TICO_DEPS_DIR)
    target_link_libraries(duckstation-nogui PRIVATE
      "${TICO_GLSLANG_BUILD}/glslang/libglslang.a"
      "${TICO_GLSLANG_BUILD}/glslang/libglslang-default-resource-limits.a"
      "${TICO_DEPS_DIR}/lib/libuam.a"
      # SPIR-V to GLSL, for deko3D's post-processing and slang shaders
      "${TICO_DEPS_DIR}/lib/libspirv-cross-c.a" "${TICO_DEPS_DIR}/lib/libspirv-cross-glsl.a"
      "${TICO_DEPS_DIR}/lib/libspirv-cross-util.a" "${TICO_DEPS_DIR}/lib/libspirv-cross-core.a"
      "-Wl,--start-group"
      "${TICO_NVK_ARCHIVE}"
      -l:libmesa_util_c11.a -l:libblake3.a -l:libmesa_util.a -l:libmesa_util_simd.a -l:libxmlconfig.a
      ${TICO_NVK_DEPS}
      "-Wl,--end-group"
    )
  endif()

  nx_generate_nacp (tico-duckstation.nacp
    NAME    "Tico DuckStation"
    AUTHOR  "ticoverse.com"
    VERSION "3.0.0"
  )

  # Create folder early otherwise the CMake script complains
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/romfs)
  file(REMOVE ${CMAKE_BINARY_DIR}/romfs/LICENSE)
  # the overlay's pictures and fonts, its strings, and the settings it lists
  # (the module's settings.json, which tico reads too)
  file(COPY ${TICO_FRONTEND}/romfs/assets DESTINATION ${CMAKE_BINARY_DIR}/romfs)
  file(COPY ${TICO_FRONTEND}/romfs/fonts DESTINATION ${CMAKE_BINARY_DIR}/romfs)
  file(COPY ${TICO_FRONTEND}/romfs/shaders DESTINATION ${CMAKE_BINARY_DIR}/romfs)
  file(COPY ${CMAKE_SOURCE_DIR}/tico/lang DESTINATION ${CMAKE_BINARY_DIR}/romfs)
  file(COPY ${CMAKE_SOURCE_DIR}/tico/module/settings.json DESTINATION ${CMAKE_BINARY_DIR}/romfs/module)

  if (CMAKE_BUILD_TYPE STREQUAL Debug)
    nx_create_nro(duckstation-nogui
      OUTPUT ${CMAKE_BINARY_DIR}/tico-duckstation.nro
      NACP  tico-duckstation.nacp
      ICON  "${CMAKE_SOURCE_DIR}/extras/icons/icon-switch.jpg"
    )
  else()
    nx_create_nro(duckstation-nogui
      OUTPUT ${CMAKE_BINARY_DIR}/tico-duckstation.nro
      NACP  tico-duckstation.nacp
      ICON  "${CMAKE_SOURCE_DIR}/extras/icons/icon-switch.jpg"
      ROMFS ${CMAKE_BINARY_DIR}/romfs
    )
  endif()
endif()
