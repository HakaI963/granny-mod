/*
 * Mod discovery and application.
 *
 * A mod is a directory (or .gmod zip extracted next to it) under
 *
 *   /sdcard/Android/media/<package>/mods/<mod-name>/mod.json
 *
 * Android/media is deliberate: it is readable and writable by a file manager
 * without any runtime permission on API 30+, unlike Android/data.
 *
 * mod.json drives three kinds of change, all resolved by *name* through the
 * exported il2cpp reflection API:
 *
 *   targets  - managed fields to write, e.g. "EnemyAIGranny.walkSpeed"
 *   spawns   - instantiate/add/remove scene objects by their real names
 *   scripts  - .lua files handed to the embedded interpreter (scripts.c)
 *
 * The field names come from the shipped global-metadata.dat; see docs/API.md
 * for the full list recovered from EnemyAIGranny (type 4020), FPSControllerNEW
 * (4079), InventoryController (4366) and StartNewDay (4490).
 */
#include "gm.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define GM_MAX_MODS 64
#define GM_MAX_FIELDS 192

typedef struct {
    char cls[96];
    char field[96];
    float value;
    bool is_static;
} field_write_t;

typedef struct {
    char id[128];
    char name[128];
    char dir[GM_MAX_PATH];
    bool enabled;
    bool deleted;
    int nfields;
    field_write_t fields[GM_MAX_FIELDS];
} mod_t;

static mod_t g_mods[GM_MAX_MODS];
static int g_nmods;
static char g_mods_path[GM_MAX_PATH];
static uint64_t g_scan_next;
/* test hook: set to 0 to force the next tick to rescan */
int gm_mods_scan_forcereset = 1;

const char *gm_mods_dir(void) {
    return g_mods_path;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool read_file(const char *p, char *buf, size_t cap) {
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return true;
}

/* ---- minimal JSON reader ----------------------------------------------
 * Enough for the fixed mod schema: objects, strings, numbers, booleans and
 * one nesting level of arrays. Written inline so the loader has no third-party
 * dependency in the APK. */
static const char *j_skip(const char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}
static const char *j_str(const char *s, char *out, size_t cap) {
    s = j_skip(s);
    if (*s != '"')
        return NULL;
    s++;
    size_t i = 0;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1])
            s++;
        if (i + 1 < cap)
            out[i++] = *s;
        s++;
    }
    out[i] = 0;
    return *s == '"' ? s + 1 : NULL;
}
static const char *j_val(const char *s, double *num, char *str, size_t cap, bool *boolean) {
    s = j_skip(s);
    if (*s == '"')
        return j_str(s, str, cap);
    if (!strncmp(s, "true", 4)) {
        *boolean = true;
        return s + 4;
    }
    if (!strncmp(s, "false", 5)) {
        *boolean = false;
        return s + 5;
    }
    char *end;
    *num = strtod(s, &end);
    return end == s ? NULL : end;
}

/* Parse "targets": { "Class.field": value, ... } */
static bool parse_targets(const char *s, mod_t *m) {
    s = strstr(s, "\"targets\"");
    if (!s)
        return false;
    /* step over the key itself before looking for the colon - doing j_skip on
     * the quote left *s == '"', so every targets block parsed as empty and mods
     * loaded with 0 fields */
    s = j_skip(s + 9);
    if (*s != ':')
        return false;
    s = j_skip(s + 1);
    if (*s != '{')
        return false;
    s++;
    while (*s && *s != '}') {
        char key[256] = {0};
        s = j_str(s, key, sizeof key);
        if (!s)
            return false;
        s = j_skip(s);
        if (*s != ':')
            return false;
        double num = 0;
        char sval[128] = {0};
        bool boolean = false;
        s = j_val(s + 1, &num, sval, sizeof sval, &boolean);
        if (!s)
            return false;

        if (sval[0]) {
            /* "s:TEXT" sets a string field; a bare number sets a float */
            GM_LOGW("mod %s: string targets not supported yet (%s)", m->id, key);
        } else if (m->nfields < GM_MAX_FIELDS) {
            char *dot = strrchr(key, '.');
            field_write_t *fw = &m->fields[m->nfields++];
            if (dot) {
                size_t n = (size_t)(dot - key);
                if (n >= sizeof fw->cls)
                    n = sizeof fw->cls - 1;
                memcpy(fw->cls, key, n);
                fw->cls[n] = 0;
                snprintf(fw->field, sizeof fw->field, "%s", dot + 1);
            } else {
                snprintf(fw->cls, sizeof fw->cls, "%s", "StartNewDay");
                snprintf(fw->field, sizeof fw->field, "%.95s", key);
            }
            fw->value = (float)num;
            fw->is_static = false;
        }
        s = j_skip(s);
        if (*s == ',')
            s++;
    }
    return true;
}

static void load_mod(const char *dir, const char *id) {
    if (g_nmods >= GM_MAX_MODS)
        return;
    char path[GM_MAX_PATH], txt[64 * 1024];
    snprintf(path, sizeof path, "%.1000s/mod.json", dir);
    if (!read_file(path, txt, sizeof txt))
        return;

    mod_t *m = &g_mods[g_nmods];
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    snprintf(m->id, sizeof m->id, "%s", id);

    const char *p = strstr(txt, "\"name\"");
    if (p) {
        p = j_skip(p + 6);
        if (*p == ':')
            j_str(p + 1, m->name, sizeof m->name);
    }
    if (!m->name[0])
        snprintf(m->name, sizeof m->name, "%s", id);

    p = strstr(txt, "\"enabled\"");
    bool boolean = false;
    double num = 0;
    char tmp[8] = {0};
    if (p) {
        p = j_skip(p + 9);
        if (*p == ':')
            j_val(p + 1, &num, tmp, sizeof tmp, &boolean);
    } else {
        boolean = true; /* new mods default to on */
    }
    m->enabled = boolean;

    if (strstr(txt, "\"targets\""))
        parse_targets(txt, m);

    g_nmods++;
    GM_LOGI("mod '%s' (%d fields, %s)", m->name, m->nfields, m->enabled ? "on" : "off");
}

static void scan(void) {
    g_nmods = 0;
    DIR *d = opendir(g_mods_path);
    if (!d) {
        mkdir(g_mods_path, 0777);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char sub[GM_MAX_PATH];
        snprintf(sub, sizeof sub, "%.780s/%.200s", g_mods_path, e->d_name);
        if (is_dir(sub))
            load_mod(sub, e->d_name);
    }
    closedir(d);
}

/* Apply one mod's field writes by name, against the live objects.
 *
 * The scene object that owns each managed class is known from the shipped
 * scenes, so a target like "EnemyAIGranny.walkSpeed" resolves to: find the
 * GameObject called Granny, ask it for the EnemyAIGranny component, then write
 * the field on that component. Writing onto the GameObject itself would corrupt
 * memory, so every step is checked and a missing class or component is reported
 * rather than guessed at.
 *
 * Every il2cpp entry point is NULL-checked: the host harness runs this code with
 * an unbound API table, and calling through a NULL pointer is exactly what made
 * the harness segfault as soon as mod.json targets parsed correctly.
 */
struct cls_root { const char *cls; const char *root; };
static const struct cls_root k_roots[] = {
    { "EnemyAIGranny", "Granny" },
    { "FPSControllerNEW", "Player" },
    { "StartNewDay", "Main Camera" },
    { "InventoryController", "Player" },
    { NULL, NULL },
};

static const char *root_for(const char *cls) {
    for (int i = 0; k_roots[i].cls; i++)
        if (!strcmp(k_roots[i].cls, cls))
            return k_roots[i].root;
    return NULL;
}

static void apply_mod(const mod_t *m) {
    gmi_t *a = A2();
    if (!a || !a->class_from_name || !a->runtime_invoke)
        return; /* API not bound yet: nothing can be resolved safely */
    for (int i = 0; i < m->nfields; i++) {
        const field_write_t *fw = &m->fields[i];
        Il2CppClass *c = gm_class(NULL, "", fw->cls);
        if (!c) {
            static int warned[GM_MAX_FIELDS];
            if (!warned[i]) {
                warned[i] = 1;
                GM_LOGW("%s: no managed class '%s'", m->id, fw->cls);
            }
            continue;
        }
        if (fw->is_static) {
            gm_set_static_float(c, fw->field, fw->value);
            continue;
        }
        const char *root = root_for(fw->cls);
        if (!root) {
            continue;
        }
        Il2CppObject *go = gm_find_object(root);
        if (!go)
            continue; /* scene not loaded yet */

        /* GameObject.GetComponent(Type) with an assembly-qualified name. */
        Il2CppClass *goc = a->object_get_class ? a->object_get_class(go) : NULL;
        Il2CppMethodInfo *gc = goc && a->class_get_method_from_name
            ? a->class_get_method_from_name(goc, "GetComponent", 1) : NULL;
        Il2CppClass *typec = gm_class(NULL, "System", "Type");
        Il2CppMethodInfo *tgtype = typec && a->class_get_method_from_name
            ? a->class_get_method_from_name(typec, "GetType", 1) : NULL;
        if (!gc || !tgtype || !a->string_new)
            return;

        char aq[256];
        snprintf(aq, sizeof aq, "%.200s, Assembly-CSharp", fw->cls);
        Il2CppObject *ex = NULL;
        void *pa[1];
        pa[0] = a->string_new(aq);
        Il2CppObject *tn = a->runtime_invoke(tgtype, NULL, pa, &ex);
        if (!tn || ex)
            continue;
        void *pb[1] = {tn};
        ex = NULL;
        Il2CppObject *comp = a->runtime_invoke(gc, go, pb, &ex);
        if (!comp || ex)
            continue;
        if (!gm_set_float(comp, fw->field, fw->value)) {
            static int warnedf[GM_MAX_FIELDS];
            if (!warnedf[i]) {
                warnedf[i] = 1;
                GM_LOGW("%s: no float field '%s' on %s", m->id, fw->field, fw->cls);
            }
        }
    }
}

static void apply_all(void) {
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].enabled)
            apply_mod(&g_mods[i]);
}

void gm_mods_tick(void) {
    if (!gm_ready())
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec;
    if (gm_mods_scan_forcereset == 0 && now < g_scan_next)
        return;
    g_scan_next = now + 3; /* rescan the mods folder every 3s so drops land */

    if (!g_mods_path[0]) {
        /* Explicit override: used by the host harness (which cannot read
         * /proc/self/cmdline to mean anything) and handy for debugging on device. */
        const char *force = getenv("GM_MODS_DIR");
        if (force && *force) {
            snprintf(g_mods_path, sizeof g_mods_path, "%.1000s", force);
            GM_LOGI("mods directory forced to %s", g_mods_path);
            mkdir(g_mods_path, 0777);
            scan();
            apply_all();
            return;
        }
        /*
         * The package must be discovered, not baked. This build installs as
         * com.dvloper.granny.mod, but the clone engine renames it again
         * (com.altaris.apps.*), and Android only lets a process read its OWN
         * /sdcard/Android/media/<pkg> directory - a hard-coded name would look
         * in a directory the app cannot see and silently find no mods.
         * /proc/self/cmdline holds the process name, which for a normal (non
         * :subprocess) app process is exactly the package id.
         */
        char pkg[256] = {0};
        FILE *cf = fopen("/proc/self/cmdline", "re");
        if (cf) {
            size_t n = fread(pkg, 1, sizeof pkg - 1, cf);
            fclose(cf);
            /* cmdline is NUL separated; the first argument is the process name */
            for (size_t i = 0; i < n; i++)
                if (pkg[i] == 0) { pkg[i] = 0; break; }
        }
        char *colon = strchr(pkg, ':');
        if (colon)
            *colon = 0; /* :subprocess processes still own the same media dir */
        if (!pkg[0])
            snprintf(pkg, sizeof pkg, "%s", "com.dvloper.granny.mod");

        const char *ext = getenv("EXTERNAL_STORAGE");
        const char *base = ext && *ext ? ext : "/storage/emulated/0";
        snprintf(g_mods_path, sizeof g_mods_path, "%s/Android/media/%s/mods", base, pkg);
        GM_LOGI("mods directory = %s (package %s)", g_mods_path, pkg);
        mkdir(g_mods_path, 0777);
    }
    scan();
    apply_all();
}

int gm_mod_field_count(int i) {
    return (i >= 0 && i < g_nmods) ? g_mods[i].nfields : -1;
}

int gm_mod_count(void) {
    return g_nmods;
}
const char *gm_mod_name(int i) {
    return (i >= 0 && i < g_nmods) ? g_mods[i].name : NULL;
}
bool gm_mod_enabled(int i) {
    return (i >= 0 && i < g_nmods) ? g_mods[i].enabled : false;
}
void gm_mod_toggle(int i) {
    if (i >= 0 && i < g_nmods) {
        g_mods[i].enabled = !g_mods[i].enabled;
        /* persist so the choice survives a restart */
        char path[GM_MAX_PATH], txt[64 * 1024];
        snprintf(path, sizeof path, "%.1000s/mod.json", g_mods[i].dir);
        if (read_file(path, txt, sizeof txt)) {
            char *p = strstr(txt, "\"enabled\"");
            if (p) {
                char *colon = strchr(p, ':');
                char *val = colon ? colon + 1 : NULL;
                if (val) {
                    while (*val && (isspace((unsigned char)*val)))
                        val++;
                    const char *want = g_mods[i].enabled ? "true" : "false";
                    size_t wl = strlen(want);
                    memcpy(val, want, wl);
                    /* clear any leftover characters from the old literal */
                    for (char *q = val + wl; *q && *q != ',' && *q != '}' && !isspace((unsigned char)*q); q++)
                        *q = ' ';
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        fwrite(txt, 1, strlen(txt), f);
                        fclose(f);
                    }
                }
            }
        }
    }
}
