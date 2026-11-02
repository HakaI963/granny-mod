/* Reflection helpers over the exported il2cpp API. */
#include "gm.h"
#include <string.h>

static gmi_t *A(void) { return (gmi_t *)gm_api(); }

Il2CppClass *gm_class(const char *assembly, const char *ns, const char *name) {
    gmi_t *a = A();
    if (!gm_ready())
        return NULL;
    for (size_t i = 0; i < a->nasms; i++) {
        Il2CppAssembly *asm_ = a->asms[i];
        const Il2CppImage *img = a->assembly_get_image(asm_);
        if (!img)
            continue;
        Il2CppClass *c = a->class_from_name(img, ns, name);
        if (c)
            return c;
        /* the caller may name the assembly, but scanning is cheap and safer
         * than trusting an image name that may differ between builds */
        (void)assembly;
    }
    return NULL;
}

static Il2CppFieldInfo *find_field(Il2CppClass *c, const char *name) {
    gmi_t *a = A();
    void *iter = NULL;
    Il2CppFieldInfo *f;
    while ((f = a->class_get_fields(c, &iter)))
        if (a->field_get_name(f) && strcmp(a->field_get_name(f), name) == 0)
            return f;
    /* Unity serialises private fields as "<Name>k__BackingField" */
    char backing[192];
    snprintf(backing, sizeof backing, "<%s>k__BackingField", name);
    iter = NULL;
    while ((f = a->class_get_fields(c, &iter)))
        if (a->field_get_name(f) && strcmp(a->field_get_name(f), backing) == 0)
            return f;
    return NULL;
}

bool gm_set_float(Il2CppObject *obj, const char *field, float v) {
    gmi_t *a = A();
    Il2CppClass *c = a->object_get_class(obj);
    if (!c)
        return false;
    Il2CppFieldInfo *f = find_field(c, field);
    if (!f)
        return false;
    a->field_set_value(obj, f, &v);
    return true;
}

bool gm_get_float(Il2CppObject *obj, const char *field, float *out) {
    gmi_t *a = A();
    Il2CppClass *c = a->object_get_class(obj);
    if (!c)
        return false;
    Il2CppFieldInfo *f = find_field(c, field);
    if (!f)
        return false;
    a->field_get_value(obj, f, out);
    return true;
}

bool gm_set_static_float(Il2CppClass *c, const char *field, float v) {
    gmi_t *a = A();
    Il2CppFieldInfo *f = find_field(c, field);
    if (!f)
        return false;
    a->field_static_set_value(f, &v);
    return true;
}

Il2CppObject *gm_call0(Il2CppObject *obj, const char *ns, const char *cls, const char *method) {
    gmi_t *a = A();
    Il2CppClass *c = gm_class(NULL, ns, cls);
    if (!c)
        return NULL;
    /* argc 0 covers the getters we need (Camera.main, GameObject.Find is
     * static with one string arg and is handled separately) */
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, method, 0);
    if (!m)
        return NULL;
    Il2CppObject *exc = NULL;
    return a->runtime_invoke(m, obj, NULL, &exc);
}

/* Instance call with one object argument (e.g. GameObject.Find("/Name")). */
Il2CppObject *gm_call1_obj(Il2CppObject *obj, const char *ns, const char *cls, const char *method,
                           void *arg) {
    gmi_t *a = A();
    Il2CppClass *c = gm_class(NULL, ns, cls);
    if (!c)
        return NULL;
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, method, 1);
    if (!m)
        return NULL;
    void *params[1] = {arg};
    Il2CppObject *exc = NULL;
    return a->runtime_invoke(m, obj, params, &exc);
}

Il2CppObject *gm_find_object(const char *path) {
    gmi_t *a = A();
    Il2CppObject *s = a->string_new(path);
    return gm_call1_obj(NULL, "UnityEngine", "GameObject", "Find", s);
}
