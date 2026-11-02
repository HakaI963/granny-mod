/*
 * Managed method-pointer detours.
 *
 * il2cpp invokes managed code through MethodInfo.methodPointer, the first field
 * of the Il2CppMethodInfo the reflection API hands back. Swapping that single
 * word redirects every caller in the program without touching a byte of code,
 * which is what makes this survivable across Granny builds: no absolute code
 * addresses are baked in.
 *
 * No delegate creation is possible here - il2cpp_delegate_create is NOT exported
 * by this build - which is why the Mods button cannot be wired through
 * UnityEvent and clicks are caught by detouring UnityEngine.UI.Button's own
 * handler instead.
 *
 * Before overwriting a slot we require the existing value to lie inside
 * libil2cpp.so's image (gm_il2cpp_contains). A methodPointer outside the module
 * means our layout assumption is wrong for this runtime, and writing to it
 * would crash the game at the first call; refusing and logging is the safe
 * outcome.
 */
#include "gm.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef Il2CppObject *(*fn_inst0)(Il2CppObject *);
typedef Il2CppObject *(*fn_inst1)(Il2CppObject *, void *, Il2CppObject **);

static void *s_orig_update;
static void *s_orig_click;

static Il2CppObject *det_update(Il2CppObject *self) {
    gm_pump_main();
    fn_inst0 o = (fn_inst0)s_orig_update;
    return o ? o(self) : NULL;
}

static Il2CppObject *det_click(Il2CppObject *self, void *evt, Il2CppObject **exc) {
    if (gm_button_click(self, evt))
        return NULL; /* consumed: never reach PlayButton's real handler */
    fn_inst1 o = (fn_inst1)s_orig_click;
    return o ? o(self, evt, exc) : NULL;
}

static bool patch_method(Il2CppClass *c, const char *mname, int argc, void *detour, void **out_orig) {
    gmi_t *a = A2();
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, mname, argc);
    if (!m) {
        GM_LOGW("no %s.%s/%d", a->class_get_name(c), mname, argc);
        return false;
    }
    void **slot = (void **)m;
    void *cur = *slot;
    if (cur == detour) {
        GM_LOGW("%s.%s already hooked; leaving it alone", a->class_get_name(c), mname);
        return false;
    }
    if (!cur) {
        GM_LOGW("%s.%s has a null methodPointer (abstract or body-less)", a->class_get_name(c), mname);
        return false;
    }
    if (!gm_il2cpp_contains(cur)) {
        GM_LOGE("REFUSING to patch %s.%s: current pointer %p is outside libil2cpp.so - "
                "methodPointer is not the first field in this runtime",
                a->class_get_name(c), mname, cur);
        return false;
    }
    if (!gm_make_writable(slot)) {
        GM_LOGE("could not make the page holding %s.%s writable", a->class_get_name(c), mname);
        return false;
    }
    *out_orig = cur;
    *slot = detour;
    gm_restore_prot(slot);
    GM_LOGI("hooked %s.%s/%d (body was %p)", a->class_get_name(c), mname, argc, cur);
    return true;
}

/*
 * The pump needs a MonoBehaviour whose Update runs in *every* scene, menu
 * included. StartNewDay is Granny's persistent day manager, but if a build
 * moves it, betting the whole mod layer on one class name would silently stop
 * mods from ticking, so each candidate is tried in turn and the winner is
 * logged.
 */
/* Namespace + class, in preference order.
 *
 * EventSystem.Update is first on purpose: UnityEngine.EventSystems.EventSystem
 * ticks every frame in ANY scene that has UI, which includes the main menu.
 * StartNewDay is Granny's own day manager and was the original choice, but it
 * is not guaranteed to run on the menu screen, and a pump that only ticks during
 * gameplay cannot install a menu button. */
static const struct { const char *ns; const char *cls; } k_pump_candidates[] = {
    { "UnityEngine.EventSystems", "EventSystem" },
    { "UnityEngine.UI", "Button" },
    { "", "StartNewDay" },
    { "", "FPSControllerNEW" },
    { "", "MenuButtons" },
};

void gm_hooks_install(void) {
    bool pumped = false;
    for (unsigned i = 0; i < sizeof k_pump_candidates / sizeof *k_pump_candidates && !pumped; i++) {
        Il2CppClass *c = gm_class(NULL, k_pump_candidates[i].ns, k_pump_candidates[i].cls);
        if (!c) {
            GM_LOGW("pump candidate %s.%s not present", k_pump_candidates[i].ns,
                    k_pump_candidates[i].cls);
            continue;
        }
        if (patch_method(c, "Update", 0, (void *)det_update, &s_orig_update)) {
            GM_LOGI("pump armed on %s.%s.Update", k_pump_candidates[i].ns,
                    k_pump_candidates[i].cls);
            pumped = true;
        }
    }
    if (!pumped)
        GM_LOGE("no pump class available: mods will not tick and the menu will not appear");

    /* UnityEngine.UI.Button lives in the UnityEngine.UI assembly with a
     * namespace; the game resolves it the same way, so try both spellings. */
    Il2CppClass *btn = gm_class(NULL, "UnityEngine.UI", "Button");
    if (!btn)
        btn = gm_class(NULL, "", "Button");
    if (btn)
        patch_method(btn, "OnPointerClick", 1, (void *)det_click, &s_orig_click);
    else
        GM_LOGE("UnityEngine.UI.Button missing: menu clicks unavailable");
}
