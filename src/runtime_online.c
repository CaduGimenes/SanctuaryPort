/* Online play (SANCTUARY_ONLINE=1 or BB_ONLINE=1): the bbnet20 module answers the game's network
 * and PSN imports, and the C runtime only loads it and forwards the image lifecycle calls.
 * Offline, or when the module is missing, every import stays with the runtime's offline answers. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <dlfcn.h>
#endif

#define BBNET_ABI_VERSION 2
typedef struct BbNetHost {
    uint32_t abi;
    const char *app0, *serial, *title, *app_ver;
    int32_t (*thread_spawn)(void **, void *(ABI *)(void *), void *, uint64_t, const char *);
    int32_t (*thread_join)(void *, void **);
    void (*thread_exit)(void *);
    int (*memory_is_mapped)(uintptr_t, uint64_t);
} BbNetHost;
typedef int (*BbNetInitFn)(const BbNetHost *);
typedef uintptr_t (*BbNetResolveFn)(const char *);
typedef void (*BbNetImageFn)(uint8_t *, uint64_t);
typedef void (*BbNetReadyFn)(uintptr_t, uint64_t);
typedef void (*BbNetStartFn)(void);

static char game_app0[PATH_MAX], game_serial[16], game_title[128], game_version[16];

void runtime_online_configure(const char *app0, const char *serial, const char *title,
                              const char *version) {
    snprintf(game_app0, sizeof(game_app0), "%s", app0 ? app0 : "");
    snprintf(game_serial, sizeof(game_serial), "%s", serial ? serial : "");
    snprintf(game_title, sizeof(game_title), "%s", title ? title : "");
    snprintf(game_version, sizeof(game_version), "%s", version ? version : "");
}

int runtime_online_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("SANCTUARY_ONLINE");
        if (!value) value = getenv("BB_ONLINE");
        enabled = value && value[0] == '1';
    }
    return enabled;
}

/* Local answers that must stay in the C runtime even online (trophies, store and profile dialogs). */
static int local_symbol(const char *symbol) {
    return symbol && (!strncmp(symbol, "sceNpTrophy", 11) ||
                      !strncmp(symbol, "sceNpCommerce", 13) ||
                      !strncmp(symbol, "sceNpProfileDialog", 18));
}

int runtime_online_symbol(const char *name) {
    if (!runtime_online_enabled()) return 0;
    const char *symbol = runtime_symbol(name);
    /* A name the import table does not list may still be one of the online module's 58 additions;
     * let it look. Known local modules stay here. */
    return !local_symbol(symbol);
}

static void *module_handle;
static BbNetResolveFn module_resolve_fn;
static BbNetImageFn image_mapped_fn, image_patched_fn;
static BbNetReadyFn image_ready_fn;
static BbNetStartFn start_fn;

static void *load_module(void) {
#ifdef _WIN32
    HMODULE module = NULL;
#else
    void *module = NULL;
#endif
    char path[PATH_MAX];
    const char *forced = getenv("SANCTUARY_NET_MODULE");
    if (!forced) forced = getenv("BB_NET_MODULE");
    if (forced && forced[0]) {
        snprintf(path, sizeof(path), "%s", forced);
    } else {
        char exe[PATH_MAX - 32];
#ifdef _WIN32
        const DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
        if (!n || n >= sizeof(exe)) return NULL;
#else
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return NULL;
        exe[n] = 0;
#endif
        char *slash = strrchr(exe, '/');
#ifdef _WIN32
        char *back = strrchr(exe, '\\');
        if (!slash || (back && back > slash)) slash = back;
#endif
        if (!slash) return NULL;
        slash[1] = 0;
#ifdef _WIN32
        snprintf(path, sizeof(path), "%sonline\\sanctuarynet.dll", exe);
#else
        snprintf(path, sizeof(path), "%sonline/libsanctuarynet.so", exe);
#endif
    }
    /* The module's copied shadPS4 paths read BB_GPU_USER_DIR; the port configures
     * SANCTUARY_GPU_USER_DIR (run.py). Give it the same writable cache/log folder. */
    const char *gpu_user = getenv("SANCTUARY_GPU_USER_DIR");
    if (gpu_user && *gpu_user && !getenv("BB_GPU_USER_DIR")) {
#ifdef _WIN32
        _putenv_s("BB_GPU_USER_DIR", gpu_user);
#else
        setenv("BB_GPU_USER_DIR", gpu_user, 1);
#endif
    }
#ifdef _WIN32
    module = LoadLibraryA(path);
#else
    module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!module) {
#ifdef _WIN32
        printf("Online: module not available (%s): offline play\n", path);
#else
        printf("Online: module not available (%s): %s\n", path, dlerror());
#endif
        return NULL;
    }
    const BbNetInitFn init =
#ifdef _WIN32
        (BbNetInitFn)GetProcAddress((HMODULE)module, "bbnet_init");
#else
        (BbNetInitFn)dlsym(module, "bbnet_init");
#endif
    module_resolve_fn =
#ifdef _WIN32
        (BbNetResolveFn)GetProcAddress((HMODULE)module, "bbnet_resolve");
#else
        (BbNetResolveFn)dlsym(module, "bbnet_resolve");
#endif
#ifdef _WIN32
    image_mapped_fn = (BbNetImageFn)GetProcAddress((HMODULE)module, "bbnet_image_mapped");
    image_patched_fn = (BbNetImageFn)GetProcAddress((HMODULE)module, "bbnet_image_patched");
    image_ready_fn = (BbNetReadyFn)GetProcAddress((HMODULE)module, "bbnet_image_ready");
    start_fn = (BbNetStartFn)GetProcAddress((HMODULE)module, "bbnet_start");
#else
    image_mapped_fn = (BbNetImageFn)dlsym(module, "bbnet_image_mapped");
    image_patched_fn = (BbNetImageFn)dlsym(module, "bbnet_image_patched");
    image_ready_fn = (BbNetReadyFn)dlsym(module, "bbnet_image_ready");
    start_fn = (BbNetStartFn)dlsym(module, "bbnet_start");
#endif
    if (!init || !module_resolve_fn) {
        printf("Online: %s did not start: offline play\n", path);
#ifndef _WIN32
        dlclose(module);
#else
        FreeLibrary((HMODULE)module);
#endif
        return NULL;
    }
    const BbNetHost host = {
        .abi = BBNET_ABI_VERSION,
        .app0 = game_app0,
        .serial = game_serial,
        .title = game_title,
        .app_ver = game_version,
        .thread_spawn = runtime_thread_spawn,
        .thread_join = runtime_thread_join_spawned,
        .thread_exit = runtime_thread_exit,
        .memory_is_mapped = runtime_memory_is_mapped,
    };
    if (init(&host) != 0) {
        printf("Online: %s refused the runtime ABI: offline play\n", path);
        return NULL;
    }
    module_handle = module;
    printf("Online: module %s\n", path);
    return module;
}

uintptr_t runtime_online_resolve(const char *name) {
    if (!runtime_online_symbol(name)) return 0;
    static int tried;
    if (!module_handle && !tried) { tried = 1; load_module(); }
    return module_resolve_fn ? module_resolve_fn(name) : 0;
}

void runtime_online_image_mapped(uint8_t *image, uint64_t size) {
    if (image_mapped_fn) image_mapped_fn(image, size);
}
void runtime_online_image_patched(uint8_t *image, uint64_t size) {
    if (image_patched_fn) image_patched_fn(image, size);
}
void runtime_online_image_ready(uintptr_t base, uint64_t size) {
    if (image_ready_fn) image_ready_fn(base, size);
}
void runtime_online_start(void) {
    if (start_fn) start_fn();
}
