/*
 * Monetisation neutralisation.
 *
 * The base APK ships AppLovin + LevelPlay + Unity IAP + SafeDK. Both a dex side
 * (4941 SDK class references) and a managed side exist. Doing this on the Java
 * side means editing thousands of smali call sites, so we take the single
 * chokepoint instead: the managed classes that decide whether ads run.
 *
 * Names below were read from the shipped global-metadata.dat
 * (Assembly-CSharp.dll, type range 3892..4776):
 *
 *   AppLovinAdsManager (3919, 13 methods)   FetchAds, ShowBanner, ShowAds
 *   LevelPlayManager   (3938, 12 methods)
 *   FetchAds           (3928, 16 methods)
 *   NoMoreAds          (3942, 9 methods)
 *   GameOverAds        (3929, 3 methods)
 *   PlayVideoAds       (3944, 2 methods)
 *   CheckingGDPR       (3920, 9 methods)
 *   AdManager          (3916, 1 method)
 *
 * Strategy: no-op the *show/load* methods by swapping their methodPointer to a
 * stub, and set the "purchased/ads-off" flags the game reads. Because we patch
 * method pointers rather than code bytes, an unhooked call site is harmless.
 */
#include "gm.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* every neutralised managed method lands here; returning NULL matches an
 * object-returning signature and is harmless for void ones */
Il2CppObject *gm_stub_return(void) { return NULL; }

/* returns true if the method was found and neutralised */
static bool void_method(const char *cls, const char *method, int argc) {
    gmi_t *a = A2();
    Il2CppClass *c = gm_class(NULL, "", cls);
    if (!c) {
        GM_LOGW("monetisation: class %s absent", cls);
        return false;
    }
    Il2CppMethodInfo *m = a->class_get_method_from_name(c, method, argc);
    if (!m) {
        GM_LOGW("monetisation: %s.%s/%d absent", cls, method, argc);
        return false;
    }
    void **slot = (void **)m;
    if (*slot == NULL || *slot == (void *)gm_stub_return)
        return false; /* already neutralised, or a body-less declaration */
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(ps - 1);
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0)
        return false;
    *slot = (void *)gm_stub_return;
    mprotect((void *)page, (size_t)ps, PROT_READ);
    GM_LOGI("monetisation: neutralised %s.%s", cls, method);
    return true;
}

void gm_monetisation_disable(void) {
    /* ad entry points */
    void_method("AppLovinAdsManager", "ShowAds", 0);
    void_method("AppLovinAdsManager", "ShowBanner", 1);
    void_method("AppLovinAdsManager", "FetchAds", 0);
    void_method("FetchAds", "FetchAds", 0);
    void_method("ShowAds", "Start", 0);
    void_method("ShowBanner", "Start", 0);
    void_method("LevelPlayManager", "LoadAd", 1);
    void_method("LevelPlayManager", "ShowAd", 1);
    void_method("PlayVideoAds", "PlayAd", 0);
    void_method("GameOverAds", "ShowAd", 1);
    void_method("AdManager", "ShowAds", 0);
    void_method("LoadVideos", "LoadVid", 0);
    void_method("CheckingGDPR", "Start", 0);

    /* rewarded-ad gates that grant extra days / removed ads */
    void_method("GameOverAds", "HandleGameOverAd", 0);

    /* unlock IAP: the game gates ad removal on a purchased flag it reads from
     * PlayerPrefs / its own bool fields */
    gmi_t *a = A2();
    static const char *unlock[] = {"NoMoreAds", "CheckingGDPRoption", NULL};
    for (int i = 0; unlock[i]; i++) {
        Il2CppClass *c = gm_class(NULL, "", unlock[i]);
        if (!c)
            continue;
        void *it = NULL;
        Il2CppFieldInfo *f;
        while ((f = a->class_get_fields(c, &it))) {
            const char *fn = a->field_get_name(f);
            if (!fn)
                continue;
            if (!strcmp(fn, "haveAdsRemoved") || !strcmp(fn, "isPurchased") ||
                !strcmp(fn, "adsRemoved") || !strcmp(fn, "gotIt")) {
                static const bool yes = 1;
                a->field_static_set_value(f, (void *)&yes);
                GM_LOGI("monetisation: unlocked %s.%s", unlock[i], fn);
            }
        }
    }
}
