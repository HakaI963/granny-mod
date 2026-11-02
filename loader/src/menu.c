/*
 * In-game menu: the Mods button, the mod browser, import and delete.
 *
 * The Mods entry is a *clone of the real PlayButton* rather than a hand-placed
 * rectangle. Cloning inherits the RectTransform's sizeDelta (160x30) and
 * localScale (0.69,1.57) exactly, so size and padding match by construction;
 * we then move it one pitch below Play. That pitch, 67.2, is measured from the
 * shipped scene: Play y=76.8, MoreGames y=-86.4, Quit y=-153.6, all at x=178.5,
 * so Mods goes at x=178.5, y=9.6.
 *
 * Cloning uses UnityEngine.Object.Internal_CloneSingle, not
 * Object.Instantiate<T>. Instantiate is a *generic* method, and invoking the
 * generic definition through il2cpp_runtime_invoke is undefined - it needs an
 * instantiated MethodInfo. Internal_CloneSingle is the non-generic icall the
 * public API itself calls, takes one argument and returns Object, so it can be
 * invoked safely from native code. Both names were confirmed present in the
 * shipped global-metadata.dat before this was written.
 *
 * Clicks cannot use UnityEvent: il2cpp_delegate_create is not exported by this
 * build, so no managed listener can be attached. hooks.c detours
 * UnityEngine.UI.Button.OnPointerClick and dispatches on the clicked object's
 * name. gm_button_click returns true when it consumed the click - required for
 * the Mods clone, which would otherwise inherit PlayButton's handler and start
 * a level.
 */
#include "gm.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern int gm_mod_count(void);
extern const char *gm_mod_name(int);
extern bool gm_mod_enabled(int);
extern void gm_mod_toggle(int);

static Il2CppObject *s_play;
static Il2CppObject *s_mods_go;
static Il2CppObject *s_rows[24];
static int s_nrows;
static bool s_browser_open;

/* Measured from the shipped menu scene. */
#define MENU_X 178.5f
#define MODS_Y 9.6f
#define ROW_PITCH 20.0f

/* ---- reflection helpers used only by the menu --------------------------- */

/* Defined below; used by gm_menu_install. */
static void log_screen(void);
static bool read_anchored(Il2CppObject *go, float *x, float *y);
static void log_pos(Il2CppObject *go, const char *tag);
static bool add_text_label(Il2CppObject *parentGo, const char *want);

static Il2CppObject *invoke0(Il2CppMethodInfo *m, Il2CppObject *self) {
    gmi_t *a = A2();
    Il2CppObject *exc = NULL;
    return a->runtime_invoke(m, self, NULL, &exc);
}

static Il2CppObject *invoke1(Il2CppMethodInfo *m, Il2CppObject *self, void *arg) {
    gmi_t *a = A2();
    void *params[1] = {arg};
    Il2CppObject *exc = NULL;
    return a->runtime_invoke(m, self, params, &exc);
}

static Il2CppMethodInfo *method(const char *ns, const char *cls, const char *name, int argc) {
    gmi_t *a = A2();
    Il2CppClass *c = gm_class(NULL, ns, cls);
    if (!c)
        return NULL;
    return a->class_get_method_from_name(c, name, argc);
}

/* Read a managed string back into ASCII. Il2CppString is [header][length][chars]. */
static bool jstring_to_ascii(Il2CppObject *s, char *out, size_t cap) {
    if (!s)
        return false;
    /* Il2CppString = Il2CppObject header (klass pointer + monitor pointer),
     * then int32 length, then UTF-16 code units. Il2CppObject itself is opaque
     * here, so the two words are spelled out rather than embedded. */
    struct il2cpp_string { void *klass; void *monitor; int32_t length; };
    struct il2cpp_string *st = (struct il2cpp_string *)s;
    if (st->length < 0)
        return false;
    int n = st->length < (int)cap - 1 ? st->length : (int)cap - 1;
    /*
     * The first code unit sits at offset 20 (klass 8 + monitor 8 + length 4).
     * sizeof(struct il2cpp_string) is 24, because the compiler pads the struct to
     * an 8-byte alignment - using it dropped the first two characters and made
     * "PlayButton" read back as "ayButton", which would also have broken the
     * click dispatch that compares this string to "ModsButton".
     */
    const uint16_t *chars =
        (const uint16_t *)(void *)((char *)st + 2 * sizeof(void *) + sizeof(int32_t));
    for (int i = 0; i < n; i++)
        out[i] = (char)chars[i];
    out[n] = 0;
    return true;
}

static Il2CppObject *go_of(Il2CppObject *component) {
    Il2CppMethodInfo *m = method("UnityEngine", "Component", "get_gameObject", 0);
    return m ? invoke0(m, component) : NULL;
}

static bool name_of(Il2CppObject *obj, char *out, size_t cap) {
    gmi_t *a = A2();
    if (!obj)
        return false;
    Il2CppClass *c = a->object_get_class(obj);
    if (!c)
        return false;
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, "get_name", 0);
    if (!m)
        return false;
    return jstring_to_ascii(invoke0(m, obj), out, cap);
}

static Il2CppObject *transform_of(Il2CppObject *go) {
    gmi_t *a = A2();
    Il2CppClass *c = a->object_get_class(go);
    if (!c)
        return NULL;
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, "get_transform", 0);
    return m ? invoke0(m, go) : NULL;
}

static void move_anchored(Il2CppObject *transform, float x, float y) {
    gmi_t *a = A2();
    Il2CppClass *rt = gm_class(NULL, "UnityEngine", "RectTransform");
    if (!rt)
        return;
    Il2CppMethodInfo *m = a->class_get_method_from_name(rt, "set_anchoredPosition", 1);
    if (!m)
        return;
    /* Vector2 = two floats = 8 bytes, passed by reference in a params slot */
    struct { float x, y; } v = {x, y};
    void *params[1] = {&v};
    Il2CppObject *exc = NULL;
    a->runtime_invoke(m, transform, params, &exc);
}

/* ---- hierarchy dump ---------------------------------------------------- */

static Il2CppObject *tr_method(const char *name, int argc) {
    return (Il2CppObject *)method("UnityEngine", "Transform", name, argc);
}

static void dump_walk(Il2CppObject *t, int depth, int *budget) {
    if (!t || *budget <= 0 || depth > 4)
        return;
    gmi_t *a = A2();
    Il2CppObject *kid = NULL;
    Il2CppMethodInfo *gc = (Il2CppMethodInfo *)tr_method("GetChild", 1);
    Il2CppMethodInfo *cn = (Il2CppMethodInfo *)tr_method("get_name", 0);
    char nm[128] = {0};
    if (cn) {
        Il2CppObject *so = invoke0(cn, t);
        jstring_to_ascii(so, nm, sizeof nm);
    }
    (*budget)--;
    char pad[24] = {0};
    for (int i = 0; i < depth && i < 10; i++)
        strcat(pad, "  ");
    GM_LOGI("ui %s%s", pad, nm[0] ? nm : "?");
    if (!gc)
        return;
    for (int i = 0; i < 24 && *budget > 0; i++) {
        int idx = i;
        void *p[1] = {&idx};
        Il2CppObject *ex = NULL;
        kid = a->runtime_invoke(gc, t, p, &ex);
        if (!kid)
            break;
        dump_walk(kid, depth + 1, budget);
    }
}

static void dump_ui(void) {
    if (!s_play)
        return;
    Il2CppObject *t = transform_of(s_play);
    if (!t)
        return;
    Il2CppMethodInfo *parent = (Il2CppMethodInfo *)tr_method("get_parent", 0);
    Il2CppObject *p = parent ? invoke0(parent, t) : NULL;
    Il2CppObject *root = t;
    for (int i = 0; i < 3 && p; i++) {
        Il2CppObject *q = parent ? invoke0(parent, p) : NULL;
        if (!q)
            break;
        p = q;
    }
    int budget = 40;
    GM_LOGI("ui tree (button's ancestors, depth<=4):");
    dump_walk(p ? p : root, 0, &budget);
}

/* ---- cloning ----------------------------------------------------------- */

static Il2CppObject *clone_object(Il2CppObject *original) {
    gmi_t *a = A2();
    Il2CppClass *objectc = gm_class(NULL, "UnityEngine", "Object");
    if (!objectc) {
        GM_LOGE("UnityEngine.Object missing");
        return NULL;
    }
    static const char *icalls[] = {"Internal_CloneSingle", "Instantiate", NULL};
    for (int i = 0; icalls[i]; i++) {
        Il2CppMethodInfo *m = a->class_get_method_from_name(objectc, icalls[i], 1);
        if (!m) {
            GM_LOGW("Object.%s/1 not available", icalls[i]);
            continue;
        }
        Il2CppObject *exc = NULL;
        void *params[1] = {original};
        Il2CppObject *r = a->runtime_invoke(m, NULL, params, &exc);
        if (exc) {
            GM_LOGW("Object.%s threw", icalls[i]);
            continue;
        }
        if (r) {
            GM_LOGI("clone via Object.%s", icalls[i]);
            return r;
        }
    }
    return NULL;
}

/* ---- label ------------------------------------------------------------- */

/* The button's own text, if it has one. Granny's menu labels are word-art
 * images, so a TMP/Text child may not exist; trying is cheap and the result
 * tells us what the real hierarchy is. */
static bool label_clone(Il2CppObject *go, const char *want) {
    gmi_t *a = A2();
    static const char *types[] = {"TMPro.TextMeshProUGUI, Unity.TextMeshPro",
                                  "UnityEngine.UI.Text, UnityEngine.UI", NULL};
    Il2CppMethodInfo *gettype = method("System", "Type", "GetType", 1);
    Il2CppMethodInfo *getcomp = method("UnityEngine", "Component", "GetComponent", 1);
    if (!gettype || !getcomp) {
        GM_LOGW("no Type.GetType/Component.GetComponent(Type): cannot label the button");
        return false;
    }
    Il2CppObject *t = transform_of(go);
    if (!t)
        return false;
    Il2CppMethodInfo *child = method("UnityEngine", "Transform", "GetChild", 1);
    /*
     * get_childCount returns an int, which il2cpp_runtime_invoke hands back as
     * a *boxed* value for value types; casting that pointer to an int would be
     * meaningless. Walking GetChild(i) until it returns null is both correct and
     * needs no unboxing.
     */
    for (int i = 0; i < 12; i++) {
        if (!child)
            break;
        int idx = i;
        void *p[1] = {&idx};
        Il2CppObject *exc = NULL;
        Il2CppObject *kid = a->runtime_invoke(child, t, p, &exc);
        if (!kid)
            break;
        Il2CppObject *kidgo = go_of(kid);
        char cn[128] = {0};
        name_of(kidgo, cn, sizeof cn);
        GM_LOGI("  child[%d] = '%s'", i, cn);
        for (int k = 0; types[k]; k++) {
            Il2CppObject *tn = invoke1(gettype, NULL, a->string_new(types[k]));
            if (!tn)
                continue;
            Il2CppObject *comp = invoke1(getcomp, kidgo, tn);
            if (!comp)
                continue;
            Il2CppMethodInfo *settext = NULL;
            Il2CppClass *cc = a->object_get_class(comp);
            if (cc)
                settext = a->class_get_method_from_name(cc, "set_text", 1);
            if (!settext)
                continue;
            Il2CppObject *s = a->string_new(want);
            void *tp[1] = {s};
            Il2CppObject *e2 = NULL;
            a->runtime_invoke(settext, comp, tp, &e2);
            GM_LOGI("labelled button '%s' via %s", want, types[k]);
            return true;
        }
    }
    return false;
}

/* ---- boxed values ------------------------------------------------------ */

/* il2cpp_runtime_invoke boxes value-type results, so Vector3 and Int32 come back
 * as an ordinary Il2CppObject whose payload starts after the 16-byte header
 * (klass pointer + monitor pointer). */
static int gm_boxed_int(Il2CppObject *o) {
    if (!o)
        return 0;
    int32_t v;
    memcpy(&v, (char *)o + 2 * sizeof(void *), sizeof v);
    return v;
}

static void log_screen(void) {
    gmi_t *a = A2();
    Il2CppClass *scr = gm_class(NULL, "UnityEngine", "Screen");
    if (!scr)
        return;
    Il2CppMethodInfo *w = a->class_get_method_from_name(scr, "get_width", 0);
    Il2CppMethodInfo *h = a->class_get_method_from_name(scr, "get_height", 0);
    if (!w || !h)
        return;
    GM_LOGI("Unity screen is %dx%d", gm_boxed_int(invoke0(w, NULL)), gm_boxed_int(invoke0(h, NULL)));
}

/* Position of a GameObject's transform in Unity world/screen units, so a tap can
 * be aimed at the button we just created. */
static void log_pos(Il2CppObject *go, const char *tag) {
    gmi_t *a = A2();
    Il2CppObject *t = transform_of(go);
    if (!t)
        return;
    Il2CppClass *tc = gm_class(NULL, "UnityEngine", "Transform");
    if (!tc)
        return;
    Il2CppMethodInfo *gp = a->class_get_method_from_name(tc, "get_position", 0);
    if (!gp)
        return;
    Il2CppObject *box = invoke0(gp, t);
    float x = 0, y = 0;
    if (box && gm_get_float(box, "x", &x) && gm_get_float(box, "y", &y))
        GM_LOGI("%s position = (%.1f, %.1f)", tag, x, y);
    else
        GM_LOGW("%s: could not read the boxed Vector3", tag);
}

/* Read a RectTransform's anchoredPosition (boxed Vector2 = two floats after the
 * 16-byte object header). */
static bool read_anchored(Il2CppObject *go, float *x, float *y) {
    gmi_t *a = A2();
    Il2CppObject *t = transform_of(go);
    if (!t)
        return false;
    Il2CppClass *rt = gm_class(NULL, "UnityEngine", "RectTransform");
    if (!rt)
        return false;
    Il2CppMethodInfo *ga = a->class_get_method_from_name(rt, "get_anchoredPosition", 0);
    if (!ga)
        return false;
    Il2CppObject *box = invoke0(ga, t);
    if (!box)
        return false;
    return gm_get_float(box, "x", x) && gm_get_float(box, "y", y);
}

/* ---- label from a real text element ----------------------------------- */

/*
 * The tree dump proved every menu button is a leaf: PlayButton/MoreGamesButton/
 * QuitButton are word-art images with no text child, so there is no label to
 * edit. 'Build Number' is a text element in the same container, so the Mods
 * button clones THAT: the copy carries the game's own font, material and
 * metrics, and only the string changes. That renders "MODS" in Granny's own
 * typography rather than inventing UI.
 */
static bool add_text_label(Il2CppObject *parentGo, const char *want) {
    gmi_t *a = A2();
    Il2CppObject *src = gm_find_object("Build Number");
    if (!src) {
        GM_LOGW("no 'Build Number' text element to clone for the label");
        return false;
    }
    Il2CppObject *lbl = clone_object(src);
    if (!lbl)
        return false;

    Il2CppMethodInfo *setname = method("UnityEngine", "GameObject", "set_name", 1);
    if (setname)
        invoke1(setname, lbl, a->string_new("ModsLabel"));

    Il2CppObject *lt = transform_of(lbl);
    Il2CppObject *pt = transform_of(parentGo);
    Il2CppMethodInfo *setparent = method("UnityEngine", "Transform", "SetParent", 1);
    if (setparent && lt && pt)
        invoke1(setparent, lt, pt);
    Il2CppMethodInfo *last = method("UnityEngine", "Transform", "SetAsLastSibling", 0);
    if (last && lt)
        invoke0(last, lt);
    if (lt)
        move_anchored(lt, 0.0f, 0.0f);

    /* The text component is on this object itself, not on a child. */
    Il2CppMethodInfo *gc = method("UnityEngine", "GameObject", "GetComponent", 1);
    Il2CppMethodInfo *gt = method("System", "Type", "GetType", 1);
    if (!gc || !gt) {
        GM_LOGW("Type.GetType/GetComponent unavailable; cannot set the label text");
        return false;
    }
    static const char *types[] = { "TMPro.TextMeshProUGUI, Unity.TextMeshPro",
                                   "UnityEngine.UI.Text, UnityEngine.UI", NULL };
    for (int k = 0; types[k]; k++) {
        Il2CppObject *tn = invoke1(gt, NULL, a->string_new(types[k]));
        if (!tn)
            continue;
        Il2CppObject *comp = invoke1(gc, lbl, tn);
        if (!comp)
            continue;
        Il2CppClass *cc = a->object_get_class(comp);
        Il2CppMethodInfo *st = cc ? a->class_get_method_from_name(cc, "set_text", 1) : NULL;
        if (!st)
            continue;
        invoke1(st, comp, a->string_new(want));
        GM_LOGI("labelled the Mods button '%s' via %s", want, types[k]);
        return true;
    }
    GM_LOGW("cloned a label but found no text component on it");
    return false;
}

/* ---- public ------------------------------------------------------------ */

static unsigned s_attempts;

bool gm_menu_install(void) {
    gmi_t *a = A2();
    if (!s_play) {
        if (++s_attempts % 30 != 1)
            return false; /* GameObject.Find is a managed call: ~2 per second is plenty */
        Il2CppObject *go = gm_find_object("PlayButton");
        if (!go)
            go = gm_find_object("Canvas/MenuButtons/PlayButton");
        if (!go) {
            /* Rate-limited so the log says whether the pump ticks at all and
             * whether the menu object is actually named PlayButton. */
            GM_LOGI("menu install: PlayButton not found yet (attempt %u)", s_attempts);
            return false;
        }
        s_play = go;
        char n[64];
        if (name_of(s_play, n, sizeof n))
            GM_LOGI("found the real menu button '%s' to clone", n);
    }

    Il2CppObject *mods = clone_object(s_play);
    if (!mods) {
        GM_LOGE("cloning PlayButton failed");
        return false;
    }
    s_mods_go = mods;

    Il2CppMethodInfo *setname = method("UnityEngine", "GameObject", "set_name", 1);
    if (setname)
        invoke1(setname, mods, a->string_new("ModsButton"));

    /* Live geometry: read the real buttons' anchored positions and derive the
     * row pitch, instead of trusting offline scene numbers. */
    float px = 0, py = 0, mx = 0, my = 0, qx = 0, qy = 0;
    read_anchored(s_play, &px, &py);
    Il2CppObject *more = gm_find_object("MoreGamesButton");
    Il2CppObject *quit = gm_find_object("QuitButton");
    if (more)
        read_anchored(more, &mx, &my);
    if (quit)
        read_anchored(quit, &qx, &qy);
    float pitch = 67.2f;
    if (more && quit && fabsf(my - qy) > 1.0f)
        pitch = fabsf(my - qy);
    GM_LOGI("live geometry: play=(%.1f,%.1f) more=(%.1f,%.1f) quit=(%.1f,%.1f) pitch=%.1f", px, py,
            mx, my, qx, qy, pitch);

    Il2CppObject *t = transform_of(mods);
    if (t)
        move_anchored(t, px, py - pitch);

    dump_ui();
    if (!add_text_label(mods, "Mods") && !label_clone(mods, "Mods"))
        GM_LOGW("ModsButton has no label; it will show the inherited PLAY art");

    log_screen();
    log_pos(mods, "ModsButton");
    log_pos(s_play, "PlayButton");
    float ax = 0, ay = 0;
    read_anchored(mods, &ax, &ay);
    GM_LOGI("ModsButton anchoredPosition is now (%.1f, %.1f)", ax, ay);

    GM_LOGI("Mods button cloned from PlayButton at (%.1f, %.1f)", MENU_X, MODS_Y);
    return true;
}

static void rows_clear(void) {
    Il2CppMethodInfo *destroy = method("UnityEngine", "Object", "Destroy", 1);
    for (int i = 0; i < s_nrows; i++) {
        if (s_rows[i] && destroy)
            invoke1(destroy, NULL, s_rows[i]);
        s_rows[i] = NULL;
    }
    s_nrows = 0;
}

static void rows_build(void) {
    rows_clear();
    if (!s_play)
        return;
    int n = gm_mod_count();
    for (int i = 0; i < n && i < 24; i++) {
        Il2CppObject *row = clone_object(s_play);
        if (!row)
            break;
        char buf[160];
        snprintf(buf, sizeof buf, "ModsRow|%s|%s", gm_mod_enabled(i) ? "on" : "off",
                 gm_mod_name(i) ? gm_mod_name(i) : "?");
        Il2CppMethodInfo *setname = method("UnityEngine", "GameObject", "set_name", 1);
        if (setname)
            invoke1(setname, row, A2()->string_new(buf));
        label_clone(row, gm_mod_enabled(i) ? gm_mod_name(i) : gm_mod_name(i));
        Il2CppObject *t = transform_of(row);
        if (t)
            move_anchored(t, MENU_X, MODS_Y - (float)(i + 1) * ROW_PITCH);
        s_rows[s_nrows++] = row;
    }
    GM_LOGI("mods browser shows %d mod(s)", n);
}

/* Called from the Button.OnPointerClick detour. Returns true when the click was
 * ours and the game's own handler must not run. */
bool gm_button_click(void *button, void *pointer_data) {
    (void)pointer_data;
    gmi_t *a = A2();
    if (!gm_ready() || !button)
        return false;
    Il2CppClass *bc = a->object_get_class((Il2CppObject *)button);
    if (!bc)
        return false;
    Il2CppMethodInfo *mg = a->class_get_method_from_name(bc, "get_gameObject", 0);
    if (!mg)
        return false;
    Il2CppObject *go = invoke0(mg, (Il2CppObject *)button);
    char name[128] = {0};
    if (!name_of(go, name, sizeof name))
        return false;

    if (!strcmp(name, "ModsButton")) {
        s_browser_open = !s_browser_open;
        if (s_browser_open)
            rows_build();
        else
            rows_clear();
        GM_LOGI("mods browser %s (%d mods found)", s_browser_open ? "open" : "closed",
                gm_mod_count());
        return true;
    }
    if (strncmp(name, "ModsRow|", 8) == 0) {
        for (int i = 0; i < s_nrows; i++) {
            if (s_rows[i] == go) {
                gm_mod_toggle(i);
                rows_build();
                break;
            }
        }
        return true;
    }
    return false;
}
