# Include from a private Linux compatibility-worker build. No guest or engine
# binaries are downloaded or installed by this build.
function(add_canvas_linux_runtime root)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_X11 OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    set(SDL_ALSA OFF CACHE BOOL "" FORCE)
    set(SDL_PULSEAUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    # SDL's libc symbol checks must link against the target libc.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    add_subdirectory("${root}/lib/sdl2" sdl EXCLUDE_FROM_ALL)
    add_subdirectory("${root}/lib/zlib" zlib EXCLUDE_FROM_ALL)

    file(GLOB_RECURSE runtime CONFIGURE_DEPENDS "${root}/source/*.cpp" "${root}/lib/asmjit/asmjit/*.cpp")
    list(FILTER runtime EXCLUDE REGEX "/(test|ui|opengl|vulkan|armv7|wasm|emscripten)/|/networkutils[.]cpp$")
    file(GLOB platform CONFIGURE_DEPENDS "${root}/platform/linux/*.cpp" "${root}/platform/sdl/*.cpp")
    list(FILTER platform EXCLUDE REGEX "/(platformOpenGL|knativescreenGL|kvulkanSDL)[.]cpp$")
    file(GLOB libraries CONFIGURE_DEPENDS "${root}/lib/softfloat/source/*.c"
        "${root}/lib/softfloat/source/8086-SSE/*.c" "${root}/lib/pugixml/src/*.cpp")
    add_executable(CanvasBoxedWine ${runtime} ${platform} ${libraries}
        "${root}/lib/zlib/contrib/minizip/ioapi.c"
        "${root}/lib/zlib/contrib/minizip/unzip.c"
        "${root}/lib/zlib/contrib/minizip/zip.c"
        "${root}/lib/zlib/contrib/minizip/mztools.c"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/HeadlessLinux.cpp")
    target_compile_features(CanvasBoxedWine PRIVATE cxx_std_20)
    target_compile_options(CanvasBoxedWine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=date-time>)
    target_compile_definitions(CanvasBoxedWine PRIVATE BOXEDWINE_64 BOXEDWINE_JIT
        BOXEDWINE_JIT_ARMV8 BOXEDWINE_POSIX BOXEDWINE_MULTI_THREADED
        BOXEDWINE_MEM_CACHE BOXEDWINE_HOST_EXCEPTIONS BOXEDWINE_HAS_SETJMP BOXEDWINE_DISABLE_UI
        BOXEDWINE_ZLIB ASMJIT_STATIC SIMDE_NO_NATIVE SDL2=1)
    target_include_directories(CanvasBoxedWine PRIVATE "${root}/include"
        "${root}/lib/asmjit" "${root}/lib/simde" "${root}/lib/pugixml/src"
        "${root}/lib/softfloat/source" "${root}/lib/softfloat/source/include"
        "${root}/lib/softfloat/source/8086-SSE" "${root}/lib/zlib"
        "${root}/lib/zlib/contrib/minizip" "${CMAKE_CURRENT_BINARY_DIR}/zlib")
    target_link_libraries(CanvasBoxedWine PRIVATE SDL2-static zlibstatic dl pthread m)
endfunction()
