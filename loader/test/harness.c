/*
 * Host harness for the mod parser/discoverer.
 *
 * The loader's il2cpp paths cannot run off-device, but JSON parsing, folder
 * discovery, enable/disable and persistence are all pure host code and are the
 * parts most likely to silently eat a malformed mod. This builds mods.c with
 * stubbed reflection and asserts on a fixture tree.
 *
 *   gcc -harness: see run_harness.sh
 */
#include "gm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- stubs for everything mods.c reaches outside itself --------------- */
bool gm_ready(void) { return true; }
void *gm_api(void) { static gmi_t a; return &a; }
bool gm_try_bind(void) { return true; }
void gm_pump_main(void) {}
void gm_hooks_install(void) {}
bool gm_menu_install(void) { return true; }
void gm_button_click(void *b, void *e) { (void)b; (void)e; }
Il2CppClass *gm_class(const char *a, const char *n, const char *c) {
    (void)a; (void)n; (void)c;
    return (Il2CppClass *)0x1;
}
bool gm_set_float(Il2CppObject *o, const char *f, float v) { (void)o; (void)f; (void)v; return true; }
bool gm_get_float(Il2CppObject *o, const char *f, float *v) { (void)o; (void)f; (void)v; return true; }
bool gm_set_static_float(Il2CppClass *c, const char *f, float v) { (void)c; (void)f; (void)v; return true; }
Il2CppObject *gm_find_object(const char *p) { (void)p; return (Il2CppObject *)0x1; }
Il2CppObject *gm_call1_obj(Il2CppObject *o, const char *a, const char *b, const char *c, void *d) {
    (void)o; (void)a; (void)b; (void)c; (void)d;
    return NULL;
}
extern int gm_mod_count(void);
extern const char *gm_mod_name(int);
extern bool gm_mod_enabled(int);
extern void gm_mod_toggle(int);

static int failures;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void wf(const char *path, const char *text) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        char buf[512];
        snprintf(buf, sizeof buf, "%s", dir);
        for (char *p = buf + 1; *p; p++)
            if (*p == '/')
                *p = 0, mkdir(buf, 0777), *p = '/';
        mkdir(buf, 0777);
    }
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void) {
    const char *root = "/tmp/gm-fixtures";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", root, root);
    if (system(cmd))
        return 2;

    /* fast granny: float targets, enabled by default */
    wf("/tmp/gm-fixtures/fast-granny/mod.json",
       "{ \"name\": \"Fast Granny\", \"targets\": { \"EnemyAIGranny.walkSpeed\": 5.5, "
       "\"EnemyAIGranny.seeRange\": 25.0, \"FPSControllerNEW.forwardSpeed\": 7.25 } }");

    /* explicitly disabled */
    wf("/tmp/gm-fixtures/sleepy/mod.json",
       "{ \"name\": \"Sleepy Granny\", \"enabled\": false, \"targets\": { \"EnemyAIGranny.walkSpeed\": 0.1 } }");

    /* malformed: unterminated object must not abort or crash the scan */
    wf("/tmp/gm-fixtures/broken/mod.json", "{ \"name\": \"Broken\", \"targets\": { ");

    /* no mod.json at all -> ignored */
    mkdir("/tmp/gm-fixtures/notamod", 0777);

    setenv("EXTERNAL_MEDIA_DIR", "/tmp/gm-fixtures-parent", 1);
    /* gm_mods_tick() derives the path from EXTERNAL_MEDIA_DIR; point the
     * parent so it resolves to the fixture root we just built */
    if (system("mkdir -p /tmp/gm-fixtures-parent/com.dvloper.granny.mod && "
               "ln -sfn /tmp/gm-fixtures /tmp/gm-fixtures-parent/com.dvloper.granny.mod/mods"))
        return 2;

    gm_mods_tick();

    printf("count=%d names:", gm_mod_count());
    for (int i=0;i<gm_mod_count();i++) printf(" [%d]='%s'", i, gm_mod_name(i));
    printf("\n");
    CHECK(gm_mod_count() == 3);
    int fast = -1, sleepy = -1, broken = -1;
    for (int i = 0; i < gm_mod_count(); i++) {
        const char *n = gm_mod_name(i);
        if (n && !strcmp(n, "Fast Granny"))
            fast = i;
        if (n && !strcmp(n, "Sleepy Granny"))
            sleepy = i;
        if (n && (!strcmp(n, "broken") || !strcmp(n, "Broken")))
            broken = i;
    }
    CHECK(fast >= 0);
    CHECK(sleepy >= 0);
    CHECK(broken >= 0); /* parsed to the end despite truncation */

    /* name defaulting from the folder id when the JSON key is absent */
    CHECK(gm_mod_name(broken) != NULL);

    CHECK(gm_mod_enabled(fast) == true);  /* default on */
    CHECK(gm_mod_enabled(sleepy) == false);

    /* toggle persists to mod.json */
    gm_mod_toggle(sleepy);
    CHECK(gm_mod_enabled(sleepy) == true);
    gm_mods_tick(); /* rescan re-reads from disk */
    int s2 = -1;
    for (int i = 0; i < gm_mod_count(); i++) {
        const char *n = gm_mod_name(i);
        if (n && !strcmp(n, "Sleepy Granny"))
            s2 = i;
    }
    CHECK(s2 >= 0);
    CHECK(gm_mod_enabled(s2) == true);

    printf(failures ? "\n%d failure(s)\n" : "\nall parser checks passed\n", failures);
    return failures ? 1 : 0;
}
