/*
 * Lua 5.4 scripting bridge.
 *
 * Every mod folder may ship .lua files under scripts/, executed on the Unity main thread
 * in a sandboxed order. The exposed surface is the reflection API, so a script
 * can read and write any field the loader can resolve and call the same
 * entry points the C API uses.
 *
 * Not exposed: file system writes, os.execute, package.loadlib, network.
 */
#include "gm.h"
#include <string.h>

#if __has_include(<lua.h>)
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#define GM_HAS_LUA 1
#else
#define GM_HAS_LUA 0
#endif

#include <dirent.h>
#include <sys/stat.h>

#if GM_HAS_LUA
static lua_State *L;

extern int gm_mod_count(void);
extern const char *gm_mod_name(int);
extern bool gm_mod_enabled(int);

/* --- gameplay surface -------------------------------------------------- */

static int l_setf(lua_State *l) {
    const char *cls = luaL_checkstring(l, 1);
    const char *field = luaL_checkstring(l, 2);
    float v = (float)luaL_checknumber(l, 3);
    Il2CppObject *root = NULL;
    if (!strcmp(cls, "EnemyAIGranny"))
        root = gm_find_object("Granny");
    else if (!strcmp(cls, "FPSControllerNEW"))
        root = gm_find_object("Player");
    else if (!strcmp(cls, "Main Camera") || !strcmp(cls, "StartNewDay"))
        root = gm_find_object("Main Camera");
    if (!root)
        return luaL_error(l, "no live object for %s", cls);
    if (!gm_set_float(root, field, v))
        return luaL_error(l, "no field %s.%s", cls, field);
    lua_pushboolean(l, 1);
    return 1;
}

static int l_getf(lua_State *l) {
    const char *cls = luaL_checkstring(l, 1);
    const char *field = luaL_checkstring(l, 2);
    Il2CppObject *root = NULL;
    if (!strcmp(cls, "EnemyAIGranny"))
        root = gm_find_object("Granny");
    else if (!strcmp(cls, "FPSControllerNEW"))
        root = gm_find_object("Player");
    if (!root)
        return luaL_error(l, "no live object for %s", cls);
    float v = 0;
    if (!gm_get_float(root, field, &v))
        return luaL_error(l, "no field %s.%s", cls, field);
    lua_pushnumber(l, v);
    return 1;
}

static int l_find(lua_State *l) {
    Il2CppObject *o = gm_find_object(luaL_checkstring(l, 1));
    lua_pushboolean(l, o != NULL);
    return 1;
}

static int l_log(lua_State *l) {
    GM_LOGI("lua: %s", luaL_checkstring(l, 1));
    return 0;
}

static int l_modcount(lua_State *l) {
    lua_pushinteger(l, gm_mod_count());
    return 1;
}

static const luaL_Reg k_api[] = {
    {"setField", l_setf},
    {"getField", l_getf},
    {"find", l_find},
    {"log", l_log},
    {"modCount", l_modcount},
    {NULL, NULL},
};

static int run_file(const char *path) {
    int r = luaL_dofile(L, path);
    if (r != 0)
        GM_LOGE("lua %s: %s", path, lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_pop(L, 1);
    return r;
}
#endif /* GM_HAS_LUA */

void gm_scripts_init(const char *moddir) {
#if GM_HAS_LUA
    if (L)
        return;
    L = luaL_newstate();
    if (!L)
        return;
    /* base + math + string + table only: no io, os, package or debug, so a
     * mod cannot touch the filesystem or spawn anything */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    lua_newtable(L);
    for (const luaL_Reg *r = k_api; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "granny");
    GM_LOGI("lua %s ready", LUA_VERSION_RELEASE);
#else
    (void)moddir;
#endif
}

void gm_scripts_run_mod(const char *dir) {
#if GM_HAS_LUA
    if (!L)
        gm_scripts_init(dir);
    char path[GM_MAX_PATH];
    snprintf(path, sizeof path, "%s/init.lua", dir);
    struct stat st;
    if (stat(path, &st) == 0)
        run_file(path);
    snprintf(path, sizeof path, "%s/scripts", dir);
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".lua"))
            continue;
        char f[GM_MAX_PATH];
        snprintf(f, sizeof f, "%s/%s", path, e->d_name);
        run_file(f);
    }
    closedir(d);
#else
    (void)dir;
#endif
}
