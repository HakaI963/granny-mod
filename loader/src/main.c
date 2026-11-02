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
#include <sys/prctl.h>
#include <sys/mman.h>
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

/* ---- GOT interception -------------------------------------------------
 *
 * libunity/libmain import dlsym (R_AARCH64_JUMP_SLOT), and they resolve every
 * il2cpp entry point through it. Patching their dlsym GOT slot lets us wrap
 * il2cpp_init itself, which gives one deterministic fact: the runtime is
 * initialised. That matters because calling il2cpp_domain_get() *before*
 * il2cpp_init() faults inside il2cpp - observed as
 *   #02 libil2cpp.so+0x1382700  (body of the il2cpp_domain_get thunk)
 *   #03 libgrannymod.so gm_try_bind
 *   #04 libgrannymod.so gm_bind_thread
 * i.e. a SIGSEGV we caused by probing too early, not a game bug.
 */
static atomic_bool g_init_done;

typedef void *(*dlsym_t)(void *, const char *);
typedef void *(*il2cpp_init_t)(const char *);
static dlsym_t real_dlsym;
static il2cpp_init_t real_il2cpp_init;

static void *hook_il2cpp_init(const char *domain_name) {
    void *r = real_il2cpp_init ? real_il2cpp_init(domain_name) : NULL;
    atomic_store(&g_init_done, true);
    GM_LOGI("il2cpp_init returned (domain=%p, name=%s)", r, domain_name ? domain_name : "?");
    /* We are on the Unity main thread here, and only metadata lookups plus a
     * pointer swap follow, so binding now is both safe and early. */
    if (!gm_try_bind())
        GM_LOGW("bind deferred: assemblies not visible yet at il2cpp_init return");
    return r;
}

static void *hook_dlsym(void *h, const char *name) {
    void *r = real_dlsym ? real_dlsym(h, name) : NULL;
    if (name && r && !real_il2cpp_init && !strcmp(name, "il2cpp_init")) {
        real_il2cpp_init = (il2cpp_init_t)r;
        GM_LOGI("intercepted dlsym(il2cpp_init) at %p", r);
        return (void *)hook_il2cpp_init;
    }
    return r;
}

/* One GOT slot we are looking for in one loaded library. */
typedef struct {
    const char *leaf;
    const char *sym;
    void *newval;
    void **old;
    bool hit;
} got_req_t;

/*
 * Rewrite a JUMP_SLOT/GLOB_DAT relocation in an already-loaded shared object.
 * Only the dynamic table is needed, which is present in the mapped image, so
 * this works on stripped shipping libraries.
 */
static int got_patch_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    got_req_t *r = (got_req_t *)data;
    const char *nm = info->dlpi_name;
    if (!nm)
        return 0;
    const char *slash = strrchr(nm, '/');
    nm = slash ? slash + 1 : nm;
    if (strcmp(nm, r->leaf) != 0)
        return 0;

    uintptr_t bias = (uintptr_t)info->dlpi_addr;
    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(bias + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    if (!dyn)
        return 1;

    const char *strtab = NULL;
    const Elf64_Sym *symtab = NULL;
    const Elf64_Rela *pltrel = NULL, *dynrel = NULL;
    size_t pltrelsz = 0, dynrelsz = 0;
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:  strtab  = (const char *)(uintptr_t)d->d_un.d_ptr; break;
        case DT_SYMTAB:  symtab  = (const Elf64_Sym *)(uintptr_t)d->d_un.d_ptr; break;
        case DT_JMPREL:  pltrel  = (const Elf64_Rela *)(uintptr_t)d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = (size_t)d->d_un.d_val; break;
        case DT_RELA:    dynrel  = (const Elf64_Rela *)(uintptr_t)d->d_un.d_ptr; break;
        case DT_RELASZ:  dynrelsz = (size_t)d->d_un.d_val; break;
        default: break;
        }
    }
    if (!strtab || !symtab)
        return 1;
    /*
     * Do NOT bias these. In a loaded ET_DYN image the dynamic linker has
     * already relocated every DT_* pointer entry, so DT_STRTAB / DT_SYMTAB /
     * DT_JMPREL / DT_RELA hold absolute addresses. Only an r_offset *inside* a
     * relocation record is still file-relative. Adding the bias a second time
     * gave a wild pointer and SIGSEGV at 0xb637c10a2b8 in gm-bind.
     */

    const Elf64_Rela *rels[2] = {pltrel, dynrel};
    size_t reln[2] = {pltrelsz, dynrelsz};
    for (int pass = 0; pass < 2; pass++) {
        if (!rels[pass])
            continue;
        const Elf64_Rela *rel = rels[pass]; /* already absolute */
        size_t n = reln[pass];
        for (size_t i = 0; (i + 1) * sizeof(Elf64_Rela) <= n; i++) {
            const Elf64_Rela *e = &rel[i];
            unsigned type = (unsigned)ELF64_R_TYPE(e->r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT)
                continue;
            uint32_t si = (uint32_t)(e->r_info >> 32);
            const char *rn = strtab + symtab[si].st_name;
            if (strcmp(rn, r->sym) != 0)
                continue;
            /* r_offset is the one value here that is still a vaddr */
            void **slot = (void **)(bias + e->r_offset);
            if ((uintptr_t)slot < bias || (uintptr_t)slot - bias > 0x10000000) {
                GM_LOGE("implausible GOT offset 0x%lx for %s in %s", (unsigned long)e->r_offset,
                        r->sym, r->leaf);
                continue;
            }
            long ps = sysconf(_SC_PAGESIZE);
            uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(ps - 1);
            if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
                GM_LOGE("mprotect failed for %s GOT", r->leaf);
                continue;
            }
            void *was = *slot;
            *slot = r->newval;
            if (r->old && !*r->old)
                *r->old = was;
            r->hit = true;
            GM_LOGI("patched %s GOT for %s at %p (was %p)", r->leaf, r->sym, (void *)slot, was);
        }
    }
    return 1; /* the leaf matched; no other copy of it exists */
}

static bool gm_got_patch(const char *leaf, const char *sym, void *newval, void **old) {
    got_req_t req;
    req.leaf = leaf;
    req.sym = sym;
    req.newval = newval;
    req.old = old;
    req.hit = false;
    dl_iterate_phdr(got_patch_cb, &req);
    return req.hit;
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
    prctl(PR_SET_NAME, "gm-bind", 0, 0, 0);
    /*
     * Step 1: get the dlsym interception installed while libunity/libmain are
     * still being mapped, so il2cpp_init is wrapped before it is ever called.
     * Step 2: only touch il2cpp once that has actually happened. Probing
     * earlier is what produced the SIGSEGV inside il2cpp_domain_get.
     */
    bool hooked = false;
    for (int i = 0; i < 3000 && !atomic_load(&g_init_done); i++) {
        if (!hooked) {
            /* The saved original *is* the real dlsym, so no second lookup is
             * needed (and dlsym(RTLD_DEFAULT,"dlsym") would just find our own
             * libdl entry, not the resolved slot). */
            bool a = gm_got_patch("libunity.so", "dlsym", (void *)hook_dlsym, (void **)&real_dlsym);
            bool b = gm_got_patch("libmain.so", "dlsym", (void *)hook_dlsym, (void **)&real_dlsym);
            if (a || b) {
                hooked = true;
                GM_LOGI("dlsym intercepted in %s%s", a ? "libunity" : "", b ? " libmain" : "");
            }
        }
        usleep(5 * 1000);
    }
    if (!atomic_load(&g_init_done)) {
        GM_LOGE("il2cpp_init was never intercepted; not probing (probing early crashes the game)");
        return NULL;
    }
    for (int i = 0; i < 400; i++) {
        if (gm_try_bind())
            return NULL;
        usleep(50 * 1000);
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

/* Binding is now reachable from two places - the bind thread and
 * hook_il2cpp_init on the Unity main thread - so it must be single-shot. Without
 * this, the second entrant would run patch_method again, read the detour that
 * the first pass installed as the "original" body, and every subsequent call
 * would recurse into itself until the stack died. */
static atomic_flag g_binding = ATOMIC_FLAG_INIT;

bool gm_try_bind(void) {
    if (atomic_load(&g_ready))
        return true;
    if (atomic_flag_test_and_set(&g_binding)) {
        /* another thread is mid-bind; do not interleave */
        return false;
    }
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
        h = dlopen("libil2cpp.so", RTLD_NOW); /* already a DT_NEEDED of the app */
    if (!h) {
        atomic_flag_clear(&g_binding);
        return false;
    }

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
            atomic_flag_clear(&g_binding);
            return false;
        }

    if (!gm_resolve_domain()) {
        atomic_flag_clear(&g_binding);
        return false;
    }

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
