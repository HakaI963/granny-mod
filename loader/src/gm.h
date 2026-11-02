/* GrannyMod loader — shared declarations. */
#ifndef GM_H
#define GM_H

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <android/log.h>

#define GM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GrannyMod", __VA_ARGS__)
#define GM_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "GrannyMod", __VA_ARGS__)
#define GM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GrannyMod", __VA_ARGS__)

/* Opaque il2cpp handles; we only ever pass them back to il2cpp. */
typedef struct Il2CppObject Il2CppObject;
typedef struct Il2CppClass Il2CppClass;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage Il2CppImage;
typedef struct Il2CppMethodInfo Il2CppMethodInfo;
typedef struct Il2CppFieldInfo Il2CppFieldInfo;
typedef struct Il2CppType Il2CppType;
typedef struct Il2CppDomain Il2CppDomain;

/* Resolved il2cpp entry points. */
typedef struct {
    Il2CppDomain *(*domain_get)(void);
    Il2CppAssembly **(*domain_get_assemblies)(Il2CppDomain *, size_t *);
    Il2CppClass *(*class_from_name)(const Il2CppImage *, const char *ns, const char *name);
    const Il2CppImage *(*assembly_get_image)(Il2CppAssembly *);
    uint32_t (*image_get_class_count)(const Il2CppImage *);
    Il2CppClass *(*image_get_class)(const Il2CppImage *, uint32_t);
    Il2CppMethodInfo *(*class_get_method_from_name)(Il2CppClass *, const char *, int);
    Il2CppObject *(*runtime_invoke)(const Il2CppMethodInfo *, Il2CppObject *, void **, Il2CppObject **);
    void (*field_set_value)(Il2CppObject *, const Il2CppFieldInfo *, void *);
    void (*field_get_value)(Il2CppObject *, const Il2CppFieldInfo *, void *);
    void (*field_static_set_value)(const Il2CppFieldInfo *, void *);
    Il2CppFieldInfo *(*class_get_fields)(Il2CppClass *, void **);
    const char *(*class_get_name)(Il2CppClass *);
    const char *(*class_get_namespace)(Il2CppClass *);
    const char *(*field_get_name)(Il2CppFieldInfo *);
    const char *(*method_get_name)(Il2CppMethodInfo *);
    Il2CppClass *(*object_get_class)(Il2CppObject *);
    Il2CppObject *(*string_new)(const char *);
    Il2CppClass *(*class_from_type)(const Il2CppType *);
    const Il2CppType *(*field_get_type)(Il2CppFieldInfo *);
    int32_t (*type_get_type)(const Il2CppType *);

    /* cached */
    Il2CppDomain *domain;
    Il2CppAssembly **asms;
    size_t nasms;
} gmi_t;

/* main.c */
bool gm_ready(void);
void *gm_api(void);
bool gm_try_bind(void);
/* true when p falls inside libil2cpp.so's mapped image */
bool gm_il2cpp_contains(const void *p);
void gm_pump_main(void);
void gm_post(void (*)(void *), void *);

/* every translation unit reaches the resolved API the same way */
#define A2() ((gmi_t *)gm_api())

/* hooks.c — detours on managed method pointers */
void gm_hooks_install(void);

/* menu.c — the Mods button and in-app mod browser */
bool gm_menu_install(void);
/* returns true when the click belonged to a Mods widget and the game's own
 * Button handler must not run (a Mods clone would otherwise start a level) */
bool gm_button_click(void *button, void *pointer_data);

/* mods.c — mod discovery, enable/disable, application */
void gm_mods_tick(void);

/* reflection helpers (reflect.c) */
Il2CppClass *gm_class(const char *assembly, const char *ns, const char *name);
bool gm_set_float(Il2CppObject *obj, const char *field, float v);
bool gm_get_float(Il2CppObject *obj, const char *field, float *out);
bool gm_set_static_float(Il2CppClass *c, const char *field, float v);
Il2CppObject *gm_call0(Il2CppObject *obj, const char *cls_ns, const char *cls, const char *method);
Il2CppObject *gm_call1_obj(Il2CppObject *obj, const char *ns, const char *cls, const char *method,
                           void *arg);
Il2CppObject *gm_find_object(const char *path);

#define GM_MAX_PATH 1024
const char *gm_mods_dir(void);

#endif /* GM_H */

/* monetise.c — ad/IAP neutralisation via managed method-pointer swaps */
void gm_monetisation_disable(void);
Il2CppObject *gm_stub_return(void);
