/*
 * Function interception.
 *
 * Two mechanisms, because they solve different problems:
 *
 *  1. GOT hooking of libil2cpp.so's own relocations — used for calls il2cpp
 *     makes *outward* (e.g. into libunity.so). Not what we mostly need.
 *  2. Inline trampoline on exported il2cpp entry points, plus - the one that
 *     actually matters here - detouring managed method *pointers* that live in
 *     the Il2CppClass method array. il2cpp invokes managed code through
 *     klass->methods[i]->methodPointer, so swapping that pointer redirects the
 *     call for every caller in the program without touching any code bytes.
 *     That is how we pump the main thread and how we catch Button clicks.
 *
 * No delegate creation is possible: il2cpp_delegate_create is not exported by
 * this build (checked), which is why the Mods button cannot be wired through
 * UnityEvent and must instead intercept UnityEngine.UI.Button's own handler.
 */
#include "gm.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    const char *cls_ns;
    const char *cls;
    const char *method;
    int argc;
    void *detour;
    void **original;
} hook_req_t;

/*
 * Managed methods we detour. Every entry was read out of the shipped
 * global-metadata.dat, so the names are the real ones, not guesses:
 *   StartNewDay.Update      (type 4490, methods 32373+12) -> main-thread pump
 *   Button.OnPointerClick   -> Mods / More Games / Quit dispatch
 */
static const hook_req_t k_hooks[] = {
    {NULL, "StartNewDay", "Update", 0, NULL, NULL},
    {NULL, "Button", "OnPointerClick", 1, NULL, NULL},
};

static void *s_orig_update;
static void *s_orig_click;

/* Trampolines: same signature as the managed method's native shim. */
typedef Il2CppObject *(*fn_inst0)(Il2CppObject *);
typedef Il2CppObject *(*fn_inst1)(Il2CppObject *, void *, Il2CppObject **);

static Il2CppObject *det_update(Il2CppObject *self) {
    gm_pump_main();
    fn_inst0 o = (fn_inst0)s_orig_update;
    return o ? o(self) : NULL;
}

static Il2CppObject *det_click(Il2CppObject *self, void *evt, Il2CppObject **exc) {
    gm_button_click(self, evt);
    fn_inst1 o = (fn_inst1)s_orig_click;
    return o ? o(self, evt, exc) : NULL;
}

static bool patch_method(Il2CppClass *c, const char *mname, int argc, void *detour, void **out_orig) {
    gmi_t *a = A2();
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, mname, argc);
    if (!m) {
        GM_LOGE("method %s.%s/%d not found", a->class_get_name(c), mname, argc);
        return false;
    }
    /* methodPointer is the first field of Il2CppMethodInfo in every v31 build
     * we have inspected; it is what il2cpp_call_impl dereferences. */
    void **slot = (void **)m;
    *out_orig = *slot;
    long ps = (long)sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)slot & ~(ps - 1);
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
        GM_LOGE("mprotect failed for %s", mname);
        return false;
    }
    *slot = detour;
    mprotect((void *)page, (size_t)ps, PROT_READ);
    GM_LOGI("hooked %s.%s/%d", a->class_get_name(c), mname, argc);
    return true;
}

void gm_hooks_install(void) {
    gmi_t *a = A2();
    Il2CppClass *snd = gm_class(NULL, "", "StartNewDay");
    if (snd)
        patch_method(snd, "Update", 0, (void *)det_update, &s_orig_update);
    else
        GM_LOGE("StartNewDay missing: mods will not tick");

    /* UnityEngine.UI.Button lives in the UnityEngine.UI assembly with a
     * namespace; look it up the same way the game does. */
    Il2CppClass *btn = gm_class(NULL, "UnityEngine.UI", "Button");
    if (!btn)
        btn = gm_class(NULL, "", "Button");
    if (btn)
        patch_method(btn, "OnPointerClick", 1, (void *)det_click, &s_orig_click);
    else
        GM_LOGE("UnityEngine.UI.Button missing: menu clicks unavailable");
    (void)a;
}
