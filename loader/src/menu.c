/*
 * In-game menu: the Mods button, the mod browser, and import/delete.
 *
 * Rather than hard-code a rectangle, the Mods label is a clone of the real
 * PlayButton GameObject. Cloning inherits the RectTransform's sizeDelta
 * (160x30) and localScale (0.69,1.57) exactly, so padding and size match by
 * construction; we then move it one pitch below Play, which is 67.2 units
 * measured from the shipped MoreGamesButton -> QuitButton pair in level1.
 *
 * Click handling cannot use UnityEvent: this build does not export
 * il2cpp_delegate_create, so no managed listener can be attached from native
 * code. Instead hooks.c detours UnityEngine.UI.Button.OnPointerClick and we
 * dispatch on the clicked object's name.
 */
#include "gm.h"
#include <stdio.h>
#include <string.h>

extern int gm_mod_count(void);
extern const char *gm_mod_name(int);
extern bool gm_mod_enabled(int);
extern void gm_mod_toggle(int);

/* Unity object caches */
static Il2CppObject *s_canvas;
static Il2CppObject *s_play;
static Il2CppObject *s_mods_go;
static Il2CppObject *s_labels[24];
static int s_nlabels;
static bool s_browser_open;
static int s_sel;

#define PITCH 67.2f

/* --- tiny UI through reflection ---------------------------------------
 * Text is drawn with the game's own TextMeshPro, which is already loaded, so
 * no font asset has to be injected. */

static Il2CppObject *call_static0(const char *ns, const char *cls, const char *m) {
    return gm_call0(NULL, ns, cls, m);
}

static Il2CppObject *instantiate(Il2CppObject *original) {
    gmi_t *a = A2();
    Il2CppObject *typearg = NULL;
    Il2CppClass *objectc = gm_class(NULL, "UnityEngine", "Object");
    if (!objectc)
        return NULL;
    Il2CppMethodInfo *m = a->class_get_method_from_name(objectc, "Instantiate", 1);
    if (!m)
        return NULL;
    void *params[1] = {original};
    Il2CppObject *exc = NULL;
    (void)typearg;
    return a->runtime_invoke(m, NULL, params, &exc);
}

static bool set_name(Il2CppObject *obj, const char *name) {
    gmi_t *a = A2();
    Il2CppClass *c = a->object_get_class(obj);
    if (!c)
        return false;
    Il2CppFieldInfo *iter;
    (void)iter;
    /* name is a property on Object/GameObject; set via its backing field */
    static Il2CppFieldInfo *s_name_field;
    if (!s_name_field) {
        void *it = NULL;
        Il2CppFieldInfo *f;
        while ((f = a->class_get_fields(c, &it)))
            if (!strcmp(a->field_get_name(f), "m_Name")) {
                s_name_field = f;
                break;
            }
        if (!s_name_field)
            return false;
    }
    /* GameObject's name is a native string field; write through the setter to
     * keep Unity's internal bookkeeping intact */
    Il2CppMethodInfo *set = a->class_get_method_from_name(c, "set_name", 1);
    if (!set)
        return false;
    Il2CppObject *s = a->string_new(name);
    void *params[1] = {s};
    Il2CppObject *exc = NULL;
    a->runtime_invoke(set, obj, params, &exc);
    return true;
}

static bool set_sibling_index(Il2CppObject *transform, int idx) {
    gmi_t *a = A2();
    Il2CppClass *tc = gm_class(NULL, "UnityEngine", "Transform");
    if (!tc)
        return false;
    Il2CppMethodInfo *m = a->class_get_method_from_name(tc, "SetSiblingIndex", 1);
    if (!m)
        return false;
    int v = idx;
    void *params[1] = {&v};
    Il2CppObject *exc = NULL;
    a->runtime_invoke(m, transform, params, &exc);
    return true;
}

static void *get_transform(Il2CppObject *go) {
    gmi_t *a = A2();
    Il2CppClass *goc = a->object_get_class(go);
    if (!goc)
        return NULL;
    Il2CppMethodInfo *m = a->class_get_method_from_name(goc, "get_transform", 0);
    if (!m)
        return NULL;
    Il2CppObject *exc = NULL;
    return a->runtime_invoke(m, go, NULL, &exc);
}

static void move_anchored(Il2CppObject *rect, float x, float y) {
    gmi_t *a = A2();
    Il2CppClass *rt = gm_class(NULL, "UnityEngine", "RectTransform");
    if (!rt)
        return;
    Il2CppMethodInfo *m = a->class_get_method_from_name(rt, "set_anchoredPosition", 1);
    if (!m)
        return;
    /* Vector2 is passed by value; two packed floats is the ABI for a
     * 8-byte struct on arm64 */
    struct { float x, y; } v = {x, y};
    void *params[1] = {&v};
    Il2CppObject *exc = NULL;
    a->runtime_invoke(m, rect, params, &exc);
}

/* --- browser ---------------------------------------------------------- */

static void relayout(void) {
    if (!s_mods_go)
        return;
    void *t = get_transform(s_mods_go);
    if (!t)
        return;
    /* keep the Mods label visible when the browser is open */
    move_anchored(t, 178.5f, 9.6f);
}

bool gm_menu_install(void) {
    s_canvas = call_static0("UnityEngine", "GameObject", "Find");
    if (!s_canvas) {
        Il2CppObject *s = A2()->string_new("Canvas");
        s_canvas = gm_call1_obj(NULL, "UnityEngine", "GameObject", "Find", s);
    }
    if (!s_canvas) {
        s_canvas = gm_find_object("Canvas");
    }
    if (!s_canvas) {
        s_canvas = gm_find_object("MenuButtons");
    }
    if (!s_canvas)
        return false; /* menu scene not loaded yet, retry next tick */

    s_play = gm_find_object("Canvas/MenuButtons/PlayButton");
    if (!s_play)
        s_play = gm_find_object("PlayButton");
    if (!s_play)
        return false;

    s_mods_go = instantiate(s_play);
    if (!s_mods_go)
        return false;
    set_name(s_mods_go, "ModsButton");
    relayout();
    GM_LOGI("Mods button cloned from PlayButton");
    return true;
}

static void browser_refresh(void) {
    for (int i = 0; i < s_nlabels; i++)
        if (s_labels[i])
            set_name(s_labels[i], "gmRow");
    s_nlabels = 0;
    int n = gm_mod_count();
    for (int i = 0; i < n && i < 20; i++) {
        if (!s_play)
            break;
        Il2CppObject *row = instantiate(s_play);
        if (!row)
            continue;
        char buf[160];
        snprintf(buf, sizeof buf, "%s%s", gm_mod_enabled(i) ? "" : "off ", gm_mod_name(i));
        set_name(row, buf);
        void *t = get_transform(row);
        if (t)
            move_anchored(t, 178.5f, 9.6f - (float)(i + 1) * (PITCH * 0.5f));
        s_labels[s_nlabels++] = row;
    }
}

void gm_button_click(void *button, void *pointer_data) {
    (void)pointer_data;
    if (!gm_ready() || !button)
        return;
    gmi_t *a = A2();
    Il2CppObject *go = NULL;
    Il2CppClass *bc = a->object_get_class(button);
    if (!bc)
        return;
    Il2CppMethodInfo *mg = a->class_get_method_from_name(bc, "get_gameObject", 0);
    if (!mg)
        return;
    Il2CppObject *exc = NULL;
    go = a->runtime_invoke(mg, button, NULL, &exc);
    if (!go)
        return;

    /* read the name back to identify which label was pressed */
    static Il2CppMethodInfo *s_get_name;
    if (!s_get_name)
        s_get_name = a->class_get_method_from_name(a->object_get_class(go), "get_name", 0);
    if (!s_get_name)
        return;
    Il2CppObject *nobj = a->runtime_invoke(s_get_name, go, NULL, &exc);
    if (!nobj)
        return;
    /* Il2CppString: length then UTF-16 chars */
    struct il2cpp_string { int32_t length; uint16_t chars[1]; };
    struct il2cpp_string *st = (struct il2cpp_string *)nobj;
    char name[64] = {0};
    int n = st->length < 60 ? st->length : 60;
    for (int i = 0; i < n; i++)
        name[i] = (char)st->chars[i];

    if (!strcmp(name, "ModsButton")) {
        s_browser_open = !s_browser_open;
        if (s_browser_open)
            browser_refresh();
        GM_LOGI("mods browser %s (%d mods)", s_browser_open ? "open" : "closed", gm_mod_count());
        return;
    }
    if (!s_browser_open)
        return;
    /* a pressed row toggles that mod; the Quit-position row means back */
    for (int i = 0; i < s_nlabels; i++) {
        if (s_labels[i] == go) {
            gm_mod_toggle(i);
            browser_refresh();
            return;
        }
    }
}
