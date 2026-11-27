/* Entitlements declared in addons.json, without add-on folders (d3pc's manifest). */
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
typedef int32_t (ABI *Mount)(uint32_t,const void *,char *);
typedef int32_t (ABI *Info)(uint32_t,const void *,void *);
#define GET(t,n) ((t)runtime_content_resolve(n))
static void write_text(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");
    assert(file);
    assert(fputs(text,file)>=0);
    assert(fclose(file)==0);
}
static void remove_tree(void) {
    remove("out/addons-manifest-test.json");
    rmdir("out/addons-manifest-dir/CUSA00242/NECROMANCER00003");
    rmdir("out/addons-manifest-dir/CUSA00242/D3PRIMEEVIL00000");
    rmdir("out/addons-manifest-dir/CUSA00242");
    rmdir("out/addons-manifest-dir");
}
int main(void) {
    remove_tree();
    /* A missing manifest keeps folder-only discovery. */
    assert(runtime_content_addons("out/addons-manifest-absent.json")==0);
    /* A malformed manifest is rejected and declares nothing. */
    write_text("out/addons-manifest-test.json",
        "{\"format\":\"bbport-addons/1\",\"addons\":{\"CUSA00242\":[\"bad label\"]}}");
    assert(runtime_content_addons("out/addons-manifest-test.json")==-1);
    /* Valid manifest: duplicate labels and another title are accepted; load happens once. */
    write_text("out/addons-manifest-test.json",
        "{\"format\":\"bbport-addons/1\",\"note\":\"test\",\"addons\":{"
        "\"CUSA00242\":[\"NECROMANCER00003\",\"D3CELEBRATIONDLC\",\"NECROMANCER00003\"],"
        "\"CUSA99999\":[\"OTHERPACK0000000\"]}}");
    assert(runtime_content_addons("out/addons-manifest-test.json")==1);
    assert(runtime_content_addons("out/addons-manifest-test.json")==-1);
    runtime_content_set_service(0,"CUSA00242");
    /* One folder label, one of which is also declared: the union is listed once each. */
    assert(mkdir("out/addons-manifest-dir",0755)==0);
    assert(mkdir("out/addons-manifest-dir/CUSA00242",0755)==0);
    assert(mkdir("out/addons-manifest-dir/CUSA00242/NECROMANCER00003",0755)==0);
    assert(mkdir("out/addons-manifest-dir/CUSA00242/D3PRIMEEVIL00000",0755)==0);
    assert(setenv("SANCTUARY_ADDCONT_DIR","out/addons-manifest-dir/CUSA00242",1)==0);

    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    assert(GET(Module,"g8cM39EUZ6o#libSceSysmodule")(0xb4)==0);
    unsigned char initial[32]={0}, boot[40];
    assert(GET(Init,"R9lA82OraNs#libSceAppContent")(initial,boot)==0);
    List list=GET(List,"xnd8BJzAxmk#libSceAppContent");
    Mount mount=GET(Mount,"VANhIWcqYak#libSceAppContent");
    Info info=GET(Info,"m47juOmH0VE#libSceAppContent");
    assert(list && mount && info);

    struct Entry { char label[17]; char pad[3]; uint32_t status; };
    struct { struct Entry entries[4]; int32_t canary; } out;
    memset(&out,0x5a,sizeof(out));
    uint32_t hits=0;
    assert(list(0,out.entries,4,&hits)==0 && hits==3);
    const char *expected[3]={"NECROMANCER00003","D3CELEBRATIONDLC","D3PRIMEEVIL00000"};
    for (uint32_t i=0;i<3;++i) {
        assert(out.entries[i].status==4);
        int found=0;
        for (uint32_t j=0;j<3;++j) found|=!strcmp(out.entries[i].label,expected[j]);
        assert(found);
        for (uint32_t j=0;j<i;++j) assert(strcmp(out.entries[j].label,out.entries[i].label));
    }
    assert(out.entries[3].status==0x5a5a5a5a); /* unused slots are never written */
    assert(out.canary==0x5a5a5a5a);
    assert(list(1,out.entries,4,&hits)==0 && hits==0);
    assert(list(0,NULL,0,&hits)==0 && hits==3);
    assert(runtime_content_addons_ready()==3);
    struct { struct Entry entry; int32_t canary; } one;
    memset(&one,0x6b,sizeof(one));
    assert(info(0,"D3CELEBRATIONDLC",&one.entry)==0);
    assert(one.entry.status==4 && !strcmp(one.entry.label,"D3CELEBRATIONDLC"));
    assert(one.canary==0x6b6b6b6b);
    assert((uint32_t)info(0,"UNKNOWN000000000",&one.entry)==0x80d90005);
    char point[40]={0};
    /* Declared-only labels have no data folder to mount; a folder label takes the usual path. */
    assert((uint32_t)mount(0,"D3CELEBRATIONDLC",point)==0x80d90005);
    assert((uint32_t)mount(0,"NECROMANCER00003",point)==0x80d90007);

    remove_tree();
    puts("PASS: add-on manifest, folder dedup and declared-only mount");
    return 0;
}
