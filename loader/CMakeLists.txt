cmake_minimum_required(VERSION 3.22)
project(grannymod C)

# Lua 5.4 is vendored by the CI workflow into loader/vendor/lua before configure,
# so mod scripts get a real interpreter with no runtime downloads on device.
set(LUA_SRC
    vendor/lua/lapi.c vendor/lua/lauxlib.c vendor/lua/lbaselib.c vendor/lua/lcode.c
    vendor/lua/lcorolib.c vendor/lua/lctype.c vendor/lua/ldblib.c vendor/lua/ldebug.c
    vendor/lua/ldo.c vendor/lua/ldump.c vendor/lua/lfunc.c vendor/lua/lgc.c
    vendor/lua/linit.c vendor/lua/liolib.c vendor/lua/llex.c vendor/lua/lmathlib.c
    vendor/lua/lmem.c vendor/lua/loadlib.c vendor/lua/lobject.c vendor/lua/lopcodes.c
    vendor/lua/lparser.c vendor/lua/lstate.c vendor/lua/lstring.c vendor/lua/lstrlib.c
    vendor/lua/ltable.c vendor/lua/ltablib.c vendor/lua/ltm.c vendor/lua/lundump.c
    vendor/lua/lutf8lib.c vendor/lua/lvm.c vendor/lua/lzio.c)

if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/vendor/lua/lapi.c)
  set(SRC src/main.c src/reflect.c src/hooks.c src/menu.c src/mods.c src/scripts.c ${LUA_SRC})
else()
  message(WARNING "vendor/lua missing: building without script hooks")
  set(SRC src/main.c src/reflect.c src/hooks.c src/menu.c src/mods.c)
endif()

add_library(grannymod SHARED ${SRC})
target_include_directories(grannymod PRIVATE src vendor/lua)
target_compile_definitions(grannymod PRIVATE LUA_USE_POSIX)
target_compile_options(grannymod PRIVATE -O2 -fvisibility=hidden -Wall)
# libil2cpp is dlopen'd at runtime, never linked, so the APK keeps loading
# order under the game's control.
target_link_libraries(grannymod log dl)
