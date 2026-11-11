/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation. */
#include <dirent.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* Internal variant (WipEout's Fios2 loads 0xd7): same tracking; the module's start result is 0. */
static ABI int32_t module_load_internal_arg(uint32_t id, int32_t argc, const void *argv, uint64_t flags, int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    int32_t status=module_load((uint16_t)id);
    if (!status && result) *result=0;
    return status;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* Add-ons: SANCTUARY_ADDCONT_DIR holds one folder per entitlement label (16 characters), as the
 * PS4 installs them. Diablo III's packs are entitlement-only (license + config, no data), so
 * listing them is enough; they are reported installed, as shadPS4 does. */
typedef struct { char label[17]; uint8_t pad[3]; uint32_t status; } AddcontInfo;
_Static_assert(sizeof(AddcontInfo)==24,"OrbisAppContentAddcontInfo layout");
#define ADDCONT_INSTALLED 4
static char addon_labels[32][17];
static uint32_t addon_count, addons_scanned;
/* SANCTUARY_ADDCONT_ALL=1: the known add-on labels are always reported installed, without any
 * addcont folders. Diablo III's add-ons are entitlement-only (license + config, no data), so the
 * game only reads the ownership bit; PS4 sends the add-on list from the installed licenses. */
static const char *const always_addons[] = {
    "NECROMANCER00003", /* the Necromancer DLC */
    /* The cosmetic packs are entitlement-only too; append their labels here when known. */
};
static void add_addon(const char *label) {
    for (uint32_t i = 0; i < addon_count; ++i)
        if (!memcmp(addon_labels[i], label, 17)) return; /* already listed */
    if (addon_count >= 32) return;
    memcpy(addon_labels[addon_count], label, 16);
    addon_labels[addon_count][16] = 0;
    printf("Runtime: AppContent add-on %s\n", addon_labels[addon_count]);
    ++addon_count;
}
static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned=1;
    const char *dir=getenv("SANCTUARY_ADDCONT_DIR");
    if (dir && *dir) {
        DIR *d=opendir(dir);
        for (struct dirent *e; d && (e=readdir(d)) && addon_count<32;) {
            if (strlen(e->d_name)!=16) continue;
            add_addon(e->d_name);
        }
        if (d) closedir(d);
    }
    if (getenv("SANCTUARY_ADDCONT_ALL"))
        for (uint32_t i = 0; i < sizeof(always_addons)/sizeof(*always_addons); ++i)
            add_addon(always_addons[i]);
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* Other service labels (Diablo III 1.47 asks for label 1 too) have no add-ons here. */
    if (service) { if (hits) *hits=0; ++lists; return 0; }
    scan_addons();
    uint32_t n=0;
    if (list) for (; n<capacity && n<addon_count; ++n) {
        AddcontInfo *info=(AddcontInfo *)list+n;
        memset(info,0,sizeof(*info));
        memcpy(info->label,addon_labels[n],17);
        info->status=ADDCONT_INSTALLED;
    }
    if (hits) *hits=list ? n : addon_count;
    ++lists; return 0;
}
/* Entitlement-only packs have nothing to mount. */
#define ADDCONT_NOT_MOUNTED ((int32_t)0x80D90007)
static ABI int32_t addon_mount(uint32_t service,const void *label,char *mount_point) {
    (void)service; (void)label; (void)mount_point; return ADDCONT_NOT_MOUNTED;
}
static ABI int32_t addon_unmount(const char *mount_point) { (void)mount_point; return 0; }
/* Add-ons the game can list once AppContent is initialized (for the system's entitlement event). */
unsigned runtime_content_addons_ready(void) {
    if (!initialized) return 0;
    scan_addons();
    return addon_count;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_arg;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
