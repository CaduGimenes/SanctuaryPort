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
 * PS4 installs them; run.py may give several such folders (one per title ID, ';' on Windows and
 * ':' elsewhere, as os.pathsep), and their labels are listed once each (GitHub #21: the
 * Necromancer under another region's title ID). Diablo III's packs are entitlement-only (license +
 * config, no data), so listing them is enough; they are reported installed, as shadPS4 does.
 *
 * A manifest can declare the same labels without any folder (addons.json beside the executable,
 * or SANCTUARY_ADDONS/--addons): {"format":"bbport-addons/1","addons":{"<title>":["<label>"]}}.
 * Declared labels are matched by AppContent service title and deduplicated against folders. */
#define ADDON_LABEL_SIZE 17
#define MAX_ADDONS 256
#define MAX_DECLARED 256
#define MANIFEST_LIMIT (64*1024)
typedef struct { char label[ADDON_LABEL_SIZE]; uint8_t pad[3]; uint32_t status; } AddcontInfo;
_Static_assert(sizeof(AddcontInfo)==24,"OrbisAppContentAddcontInfo layout");
#define ADDCONT_INSTALLED 4
#ifdef _WIN32
#define ADDCONT_SEPARATOR ';'
#else
#define ADDCONT_SEPARATOR ':'
#endif
static char addon_labels[MAX_ADDONS][ADDON_LABEL_SIZE];
static uint32_t addon_count, addons_scanned;
static char service_titles[8][16];
void runtime_content_set_service(uint32_t service,const char *title_id) {
    if (service>7 || initialized || !title_id || strlen(title_id)>=sizeof(service_titles[0]) ||
        strspn(title_id,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=strlen(title_id)) {
        fputs("ERROR: invalid or late AppContent service configuration\n",stderr); exit(1);
    }
    strcpy(service_titles[service],title_id);
}
static int matches_service(uint32_t service,const char *title) {
    /* An unconfigured service 0 keeps the old folder-only behaviour; other slots are empty. */
    if (service>7 || (service && !service_titles[service][0])) return 0;
    return !service_titles[service][0] || !strcmp(title,service_titles[service]);
}
typedef struct { char title[16]; char label[ADDON_LABEL_SIZE]; } DeclaredAddon;
static DeclaredAddon declared[MAX_DECLARED];
static unsigned declared_count;
static int manifest_loaded;
static void scan_addon_dir(const char *dir) {
    DIR *d=*dir ? opendir(dir) : NULL;
    if (!d) { printf("Runtime: AppContent add-on folder not readable: %s\n",dir); return; }
    for (struct dirent *e; (e=readdir(d)) && addon_count<MAX_ADDONS;) {
        if (e->d_name[0]=='.') continue;
        if (strlen(e->d_name)!=16) {
            printf("Runtime: AppContent skipped %s in %s (an add-on folder is its 16-character label)\n",e->d_name,dir);
            continue;
        }
        int known=0;
        for (uint32_t i=0; i<addon_count && !known; ++i) known=!memcmp(addon_labels[i],e->d_name,16);
        if (known) continue;
        memcpy(addon_labels[addon_count],e->d_name,16); addon_labels[addon_count][16]=0;
        printf("Runtime: AppContent add-on %s\n",addon_labels[addon_count]);
        ++addon_count;
    }
    closedir(d);
}
static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned=1;
    const char *list=getenv("SANCTUARY_ADDCONT_DIR");
    if (!list) return;
    char dir[4096];
    for (const char *p=list; *p;) {
        const char *end=strchr(p,ADDCONT_SEPARATOR);
        size_t n=end ? (size_t)(end-p) : strlen(p);
        if (n && n<sizeof(dir)) { memcpy(dir,p,n); dir[n]=0; scan_addon_dir(dir); }
        if (!end) break;
        p=end+1;
    }
}
static int title_ok(const char *text) {
    const size_t n=strlen(text);
    return n && n<sizeof(declared[0].title) &&
        strspn(text,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")==n;
}
static int label_ok(const char *text) {
    const size_t n=strlen(text);
    return n && n<ADDON_LABEL_SIZE && text[0]!='.' &&
        strspn(text,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==n;
}
static int declared_add(const char *title,const char *label) {
    if (!title_ok(title) || !label_ok(label)) return -1;
    for (unsigned i=0;i<declared_count;++i)
        if (!strcmp(declared[i].title,title) && !strcmp(declared[i].label,label)) return 0;
    if (declared_count>=MAX_DECLARED) return -1;
    snprintf(declared[declared_count].title,sizeof(declared[0].title),"%s",title);
    snprintf(declared[declared_count].label,sizeof(declared[0].label),"%s",label);
    ++declared_count;
    return 0;
}
/* Strict reader for the manifest subset: objects, arrays and strings (escapes, no \u). */
typedef struct { const char *p,*end; } ManifestText;
static void mf_ws(ManifestText *t) {
    while (t->p<t->end && (*t->p==' '||*t->p=='\t'||*t->p=='\r'||*t->p=='\n')) ++t->p;
}
static int mf_char(ManifestText *t,char want) {
    mf_ws(t);
    if (t->p>=t->end || *t->p!=want) return -1;
    ++t->p;
    return 0;
}
static int mf_string(ManifestText *t,char *out,size_t size) {
    mf_ws(t);
    if (t->p>=t->end || *t->p!='"') return -1;
    ++t->p;
    size_t n=0;
    while (t->p<t->end && *t->p!='"') {
        unsigned char ch=(unsigned char)*t->p++;
        if (ch=='\\') {
            if (t->p>=t->end) return -1;
            ch=(unsigned char)*t->p++;
            if (ch=='n') ch='\n';
            else if (ch=='t') ch='\t';
            else if (ch=='r') ch='\r';
            else if (ch!='"' && ch!='\\' && ch!='/') return -1;
        }
        if (ch<0x20 || n+1>=size) return -1;
        out[n++]=(char)ch;
    }
    if (t->p>=t->end) return -1;
    ++t->p; out[n]=0;
    return 0;
}
static int mf_labels(ManifestText *t,const char *title) {
    char label[ADDON_LABEL_SIZE];
    if (mf_char(t,'[')) return -1;
    mf_ws(t);
    if (t->p<t->end && *t->p==']') { ++t->p; return 0; }
    for (;;) {
        if (mf_string(t,label,sizeof(label)) || declared_add(title,label)) return -1;
        mf_ws(t);
        if (t->p>=t->end) return -1;
        if (*t->p==',') { ++t->p; continue; }
        if (*t->p==']') { ++t->p; return 0; }
        return -1;
    }
}
static int mf_titles(ManifestText *t) {
    char title[sizeof(declared[0].title)];
    if (mf_char(t,'{')) return -1;
    mf_ws(t);
    if (t->p<t->end && *t->p=='}') { ++t->p; return 0; }
    for (;;) {
        if (mf_string(t,title,sizeof(title)) || mf_char(t,':') || !title_ok(title) ||
            mf_labels(t,title)) return -1;
        mf_ws(t);
        if (t->p>=t->end) return -1;
        if (*t->p==',') { ++t->p; continue; }
        if (*t->p=='}') { ++t->p; return 0; }
        return -1;
    }
}
static int manifest_parse(const char *text,size_t size) {
    ManifestText t={text,text+size};
    char key[16], format[32], note[256];
    int have_format=0, have_addons=0, have_note=0;
    if (mf_char(&t,'{')) return -1;
    mf_ws(&t);
    if (t.p<t.end && *t.p=='}') return -1;   /* format and addons are mandatory */
    for (;;) {
        if (mf_string(&t,key,sizeof(key)) || mf_char(&t,':')) return -1;
        if (!strcmp(key,"format")) {
            if (have_format++ || mf_string(&t,format,sizeof(format)) ||
                strcmp(format,"bbport-addons/1")) return -1;
        } else if (!strcmp(key,"addons")) {
            if (have_addons++ || mf_titles(&t)) return -1;
        } else if (!strcmp(key,"note")) {
            if (have_note++ || mf_string(&t,note,sizeof(note))) return -1;
        } else return -1;
        mf_ws(&t);
        if (t.p>=t.end) return -1;
        if (*t.p==',') { ++t.p; continue; }
        if (*t.p=='}') { ++t.p; break; }
        return -1;
    }
    mf_ws(&t);
    return t.p!=t.end || !have_format || !have_addons ? -1 : 0;
}
int runtime_content_addons(const char *path) {
    if (!path || !*path || initialized || manifest_loaded) return -1;
    FILE *file=fopen(path,"rb");
    if (!file) return 0;   /* no manifest: add-on folders only */
    char *text=malloc(MANIFEST_LIMIT+1);
    if (!text) { fclose(file); return -1; }
    const size_t size=fread(text,1,MANIFEST_LIMIT+1,file);
    const int bad=ferror(file) || size>MANIFEST_LIMIT;
    fclose(file);
    int result=-1;
    if (!bad && !manifest_parse(text,size)) { manifest_loaded=1; result=1; }
    free(text);
    if (result<0) {
        declared_count=0;
        fprintf(stderr,"ERROR: malformed add-on manifest: %s\n",path);
        return -1;
    }
    printf("Runtime: add-on manifest %s declares %u entitlements\n",path,declared_count);
    return result;
}
static int declared_installed(uint32_t service,const char *label) {
    for (unsigned i=0;i<declared_count;++i)
        if (!strcmp(declared[i].label,label) && matches_service(service,declared[i].title)) return 1;
    return 0;
}
static int folder_installed(const char *label) {
    scan_addons();
    for (uint32_t i=0;i<addon_count;++i) if (!strcmp(addon_labels[i],label)) return 1;
    return 0;
}
/* Folders (service 0) plus the manifest's labels for the scanned service, each once. */
static uint32_t collect_addon_labels(uint32_t service,char (*out)[ADDON_LABEL_SIZE],uint32_t capacity) {
    uint32_t count=0;
    int truncated=0;
    if (service==0) {
        scan_addons();
        for (uint32_t i=0;i<addon_count;++i) {
            if (count>=capacity) { truncated=1; break; }
            memcpy(out[count],addon_labels[i],ADDON_LABEL_SIZE);
            ++count;
        }
    }
    for (unsigned i=0;i<declared_count;++i) {
        if (!matches_service(service,declared[i].title)) continue;
        if (count>=capacity) { truncated=1; break; }
        int seen=0;
        for (uint32_t k=0;k<count && !seen;++k) seen=!strcmp(out[k],declared[i].label);
        if (seen) continue;
        memcpy(out[count],declared[i].label,ADDON_LABEL_SIZE);
        ++count;
    }
    if (truncated) fprintf(stderr,"Runtime: AppContent inventory truncated at %u entries\n",capacity);
    return count;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service>7 || ((!capacity || !list) && !hits)) return PARAMETER;
    char found[MAX_ADDONS][ADDON_LABEL_SIZE];
    const uint32_t total=collect_addon_labels(service,found,MAX_ADDONS);
    uint32_t n=0;
    if (list) for (; n<capacity && n<total; ++n) {
        AddcontInfo *info=(AddcontInfo *)list+n;
        memset(info,0,sizeof(*info));
        memcpy(info->label,found[n],ADDON_LABEL_SIZE);
        info->status=ADDCONT_INSTALLED;
    }
    if (hits) *hits=list ? n : total;
    printf("Runtime: AppContent inventory service %u: %u installed entries\n",service,total);
    ++lists; return 0;
}
#define ADDCONT_NOT_FOUND ((int32_t)0x80D90005)
#define ADDCONT_NOT_MOUNTED ((int32_t)0x80D90007)
/* sceAppContentGetAddcontInfo: one installed label, from a folder or the manifest. */
static ABI int32_t addon_info(uint32_t service,const void *label,void *info) {
    require_initialized();
    if (service>7 || !label || !info) return PARAMETER;
    char name[ADDON_LABEL_SIZE]={0};
    memcpy(name,label,ADDON_LABEL_SIZE-1);
    if (!declared_installed(service,name) && !folder_installed(name)) return ADDCONT_NOT_FOUND;
    AddcontInfo out;
    memset(&out,0,sizeof(out));
    memcpy(out.label,name,strlen(name));
    out.status=ADDCONT_INSTALLED;
    memcpy(info,&out,sizeof(out));
    return 0;
}
/* Entitlement-only packs have no data to mount. A manifest-only label reports its missing folder. */
static ABI int32_t addon_mount(uint32_t service,const void *label,char *mount_point) {
    (void)mount_point;
    if (service>7 || !label) return PARAMETER;
    char name[ADDON_LABEL_SIZE]={0};
    memcpy(name,label,ADDON_LABEL_SIZE-1);
    if (declared_installed(service,name) && !folder_installed(name)) {
        printf("Runtime: add-on '%s' is declared without a data folder\n",name);
        return ADDCONT_NOT_FOUND;
    }
    return ADDCONT_NOT_MOUNTED;
}
static ABI int32_t addon_unmount(const char *mount_point) { (void)mount_point; return 0; }
/* Add-ons the game can list once AppContent is initialized (for the system's entitlement event). */
unsigned runtime_content_addons_ready(void) {
    if (!initialized) return 0;
    char found[MAX_ADDONS][ADDON_LABEL_SIZE];
    return (unsigned)collect_addon_labels(0,found,MAX_ADDONS);
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_arg;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u, declared entitlements=%u\n",refs,initialized,queries,lists,declared_count);
}
