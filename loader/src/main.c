/*
 * GrannyMod loader.
 *
 * Injected via System.loadLibrary("grannymod") from
 * com.safedk.android.SafeDKApplication.<clinit>, which is the first app code
 * the process runs, so it executes before Unity boots and before libil2cpp
 * registers its image.
 *
 * libil2cpp.so exports the real reflection API (verified: 241 il2cpp_* symbols
 * including class_from_name, runtime_invoke, field_{get,set}_value and
 * field_static_set_value), so mods are driven through that rather than through
 * hard-coded offsets. That keeps the loader working across Granny builds as
 * long as the managed names are stable.
 *
 * Threading model: il2cpp is not safe to touch from an arbitrary thread, so
 * everything managed happens on the Unity main thread, pumped from a hook on
 * a MonoBehaviour that Unity calls every frame (see pump_hook).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <android/log.h>

#include "gm.h"

#define TAG "GrannyMod"

/* Set once the main-thread pump has resolved the il2cpp API. */
static gmi_t g;
static atomic_bool g_ready;
static atomic_bool g_ui_done;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pending work queued from other threads (Java/file watcher) onto main. */
typedef struct job {
    void (*fn)(void *arg);
    void *arg;
    struct job *next;
} job_t;
static job_t *g_jobs;

void gm_post(void (*fn)(void *), void *arg) {
    job_t *j = calloc(1, sizeof *j);
    if (!j)
        return;
    j->fn = fn;
    j->arg = arg;
    pthread_mutex_lock(&g_lock);
    j->next = g_jobs;
    g_jobs = j;
    pthread_mutex_unlock(&g_lock);
}

static void gm_drain(void) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        job_t *j = g_jobs;
        if (j)
            g_jobs = j->next;
        pthread_mutex_unlock(&g_lock);
        if (!j)
            return;
        j->fn(j->arg);
        free(j);
    }
}

bool gm_ready(void) {
    return atomic_load(&g_ready);
}

void *gm_api(void) {
    return &g;
}

/*
 * il2cpp binds lazily: a class must be static-constructed before its static
 * fields are meaningful, and UnityEngine.Object cannot be touched before the
 * runtime is up. Callers that run from <clinit> therefore park here until the
 * pump proves the domain is live.
 */
static bool gm_resolve_domain(void) {
    if (g.domain)
        return true;
    g.domain = g.domain_get();
    if (!g.domain)
        return false;
    g.asms = g.domain_get_assemblies(g.domain, &g.nasms);
    return g.asms != NULL;
}

/*
 * Range of libil2cpp.so's image in memory. Managed method bodies live inside
 * it, so any MethodInfo.methodPointer we are about to overwrite must fall in
 * this range - that is what proves the offset assumption is right before we
 * patch, rather than after the game dies in a signal handler.
 */
static uintptr_t s_il2cpp_base;
static size_t s_il2cpp_span;

struct phdr_walk {
    uintptr_t base;
    size_t span;
};

/*
 * dl_iterate_phdr is available since API 21. dlinfo/RTLD_DL_LINKMAP are not
 * usable here: Bionic does not declare them the way glibc does, so a host
 * compile alone will not catch that (it did not).
 */
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (!info->dlpi_name)
        return 0;
    const char *slash = strrchr(info->dlpi_name, '/');
    const char *leaf = slash ? slash + 1 : info->dlpi_name;
    if (strcmp(leaf, "libil2cpp.so") != 0)
        return 0;
    struct phdr_walk *w = (struct phdr_walk *)data;
    w->base = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        /* explicit Elf64_ types: ElfW() is a glibc-ism, and this module is
         * built for arm64-v8a only */
        const Elf64_Phdr *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            size_t end = ph->p_vaddr + ph->p_memsz;
            if (end > w->span)
                w->span = end;
        }
    }
    return 1; /* stop walking */
}

static void gm_map_il2cpp(void) {
    struct phdr_walk w = {0, 0};
    dl_iterate_phdr(phdr_cb, &w);
    if (!w.base || !w.span) {
        GM_LOGE("libil2cpp.so not found in the program header list");
        return;
    }
    s_il2cpp_base = w.base;
    s_il2cpp_span = w.span;
    GM_LOGI("libil2cpp.so mapped at 0x%lx, span 0x%zx", (unsigned long)w.base, w.span);
}

bool gm_il2cpp_contains(const void *p) {
    if (!s_il2cpp_span)
        return false;
    uintptr_t v = (uintptr_t)p;
    return v >= s_il2cpp_base && v - s_il2cpp_base < s_il2cpp_span;
}

/*
 * Binding cannot be driven from the pump: the pump only exists once a hook is
 * installed, and installing a hook needs the binding - so driving bind from the
 * pump is a deadlock (that is why a previous build logged "loader resident" and
 * then nothing forever). A monitor thread polls until the domain is live.
 * Only metadata lookups happen here; every managed *invocation* stays on the
 * Unity main thread inside gm_pump_main().
 */
static void *gm_bind_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 900; i++) {
        if (gm_try_bind())
            return NULL;
        usleep(100 * 1000);
    }
    GM_LOGE("bind gave up: il2cpp domain never became available");
    return NULL;
}

static void __attribute__((constructor)) gm_ctor(void) {
    atomic_store(&g_ready, false);
    GM_LOGI("loader resident, monitor thread starting");
    pthread_t th;
    if (pthread_create(&th, NULL, gm_bind_thread, NULL) == 0)
        pthread_detach(th);
    else
        GM_LOGE("could not start bind thread; falling back to pump-driven bind");
}

bool gm_try_bind(void) {
    if (atomic_load(&g_ready))
        return true;
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
        h = dlopen("libil2cpp.so", RTLD_NOW); /* already a DT_NEEDED of the app */
    if (!h)
        return false;

    if (!g.domain_get)
        g.domain_get = dlsym(h, "il2cpp_domain_get");
    if (!g.domain_get_assemblies)
        g.domain_get_assemblies = dlsym(h, "il2cpp_domain_get_assemblies");
    if (!g.class_from_name)
        g.class_from_name = dlsym(h, "il2cpp_class_from_name");
    if (!g.assembly_get_image)
        g.assembly_get_image = dlsym(h, "il2cpp_assembly_get_image");
    if (!g.image_get_class_count)
        g.image_get_class_count = dlsym(h, "il2cpp_image_get_class_count");
    if (!g.image_get_class)
        g.image_get_class = dlsym(h, "il2cpp_image_get_class");
    if (!g.class_get_method_from_name)
        g.class_get_method_from_name = dlsym(h, "il2cpp_class_get_method_from_name");
    if (!g.runtime_invoke)
        g.runtime_invoke = dlsym(h, "il2cpp_runtime_invoke");
    if (!g.field_set_value)
        g.field_set_value = dlsym(h, "il2cpp_field_set_value");
    if (!g.field_get_value)
        g.field_get_value = dlsym(h, "il2cpp_field_get_value");
    if (!g.field_static_set_value)
        g.field_static_set_value = dlsym(h, "il2cpp_field_static_set_value");
    if (!g.class_get_fields)
        g.class_get_fields = dlsym(h, "il2cpp_class_get_fields");
    if (!g.class_get_name)
        g.class_get_name = dlsym(h, "il2cpp_class_get_name");
    if (!g.class_get_namespace)
        g.class_get_namespace = dlsym(h, "il2cpp_class_get_namespace");
    if (!g.field_get_name)
        g.field_get_name = dlsym(h, "il2cpp_field_get_name");
    if (!g.method_get_name)
        g.method_get_name = dlsym(h, "il2cpp_method_get_name");
    if (!g.object_get_class)
        g.object_get_class = dlsym(h, "il2cpp_object_get_class");
    if (!g.string_new)
        g.string_new = dlsym(h, "il2cpp_string_new");
    if (!g.class_from_type)
        g.class_from_type = dlsym(h, "il2cpp_class_from_type");
    if (!g.field_get_type)
        g.field_get_type = dlsym(h, "il2cpp_field_get_type");
    if (!g.type_get_type)
        g.type_get_type = dlsym(h, "il2cpp_type_get_type");

    const void *need[] = {g.domain_get, g.domain_get_assemblies, g.class_from_name,
                          g.assembly_get_image,  g.runtime_invoke,      g.field_set_value,
                          g.field_static_set_value};
    for (unsigned i = 0; i < sizeof need / sizeof *need; i++)
        if (!need[i]) {
            GM_LOGE("missing il2cpp export #%u", i);
            return false;
        }

    if (!gm_resolve_domain())
        return false;

    gm_map_il2cpp();

    /*
     * g_ready must be set *before* the hook/monetisation passes: gm_class()
     * refuses to resolve anything until the runtime is declared ready, and both
     * of those passes resolve classes. Setting it afterwards made every lookup
     * return NULL, so no hook was ever installed.
     */
    atomic_store(&g_ready, true);
    GM_LOGI("bound: %zu assemblies, main thread pump arming", g.nasms);

    gm_hooks_install();
    /* disable ads/IAP after hooks so the very first ad request is already gone */
    gm_monetisation_disable();
    return true;
}

/* Called from the hooked MonoBehaviour.Update on the Unity main thread. */
void gm_pump_main(void) {
    if (!atomic_load(&g_ready) && !gm_try_bind())
        return;
    gm_drain();
    gm_mods_tick();
    if (!atomic_load(&g_ui_done)) {
        if (gm_menu_install())
            atomic_store(&g_ui_done, true);
    }
}
