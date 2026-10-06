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
    ../tico/TicoChainload.cpp
    ../tico/TicoChainload.h
    ../tico/TicoConfig.h
    ../tico/TicoDuckBridge.cpp
    ../tico/TicoDuckBridge.h
    ../tico/TicoLogger.h
    ../tico/TicoMain.h
    ../tico/TicoOverlay.cpp
    ../tico/TicoOverlay.h
  )
  target_link_libraries(duckstation-nogui PUBLIC "${NX_LIBRARIES}")
  target_link_libraries(duckstation-nogui PRIVATE rapidjson)

  # The renderers' drivers, linked statically (build_duckstation_nro.sh):
  # deko3D's shader compiler, glslang for Vulkan's shaders, Mesa's NVK for
  # Vulkan, and Mesa's EGL and desktop GL (nvc0, or Zink on NVK) for OpenGL.
  if(TICO_DEPS_DIR)
    target_link_libraries(duckstation-nogui PRIVATE
      "${TICO_GLSLANG_BUILD}/glslang/libglslang.a"
      "${TICO_GLSLANG_BUILD}/glslang/libglslang-default-resource-limits.a"
      "${TICO_DEPS_DIR}/lib/libuam.a"
      "-Wl,--start-group"
      "${TICO_NVK_ARCHIVE}"
      -l:libGL.a -l:libEGL.a -l:libglapi.a
      -l:libmesa_util_c11.a -l:libblake3.a -l:libmesa_util.a -l:libmesa_util_simd.a -l:libxmlconfig.a
      ${TICO_NVK_DEPS}
      "-Wl,--end-group"
    )
    # Zink reaches NVK through its ICD entry points
    target_link_options(duckstation-nogui PRIVATE
      "-Wl,-u,vk_icdGetInstanceProcAddr" "-Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion")
  endif()

  nx_generate_nacp (tico-duckstation.nacp
    NAME    "Tico DuckStation"
    AUTHOR  "ticoverse.com"
    VERSION "1.0.0"
  )

  # Create folder early otherwise the CMake script complains
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/romfs)
  file(REMOVE ${CMAKE_BINARY_DIR}/romfs/LICENSE)
  file(COPY ${CMAKE_SOURCE_DIR}/src/tico/romfs/assets DESTINATION ${CMAKE_BINARY_DIR}/romfs)
  file(COPY ${CMAKE_SOURCE_DIR}/src/tico/romfs/fonts DESTINATION ${CMAKE_BINARY_DIR}/romfs)

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
