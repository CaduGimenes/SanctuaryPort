/* /app0 as the update folder over the game folder (SANCTUARY_APP0_LOWER, GitHub #22): files the
 * update lacks come from the game, the update's copy wins, and directory listings hold both. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void put(const char *root,const char *relative,const char *text) {
    char path[1024]; snprintf(path,sizeof(path),"%s/%s",root,relative);
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void expect(const char *guest,const char *text) {
    int fd=(int)do_open(guest,0,0); assert(fd>=3);
    char content[32]={0}; size_t n=strlen(text);
    assert(do_read(fd,content,sizeof(content)-1)==(int64_t)n && !strcmp(content,text));
    GuestStat info; assert(!do_stat(guest,&info) && info.size==(int64_t)n);
    assert(!do_close(fd));
}
/* The names of a guest directory's listing, each once. */
static int listed(const char *guest,const char *name) {
    int dir=(int)do_open(guest,0x20000,0); assert(dir>=3);
    char entries[4096]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int seen=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2); assert(length);
        if (!strcmp(entries+p+8,name)) ++seen;
        p+=length;
    }
    assert(!do_close(dir));
    return seen;
}
int main(void) {
    char root[]="/tmp/sanctuary-app0-layers-XXXXXX";
    assert(mkdtemp(root));
    char game[512],update[512],user[512],path[1024];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(update,sizeof(update),"%s/update",root);
    snprintf(user,sizeof(user),"%s/user",root);
    assert(!mkdir(game,0755) && !mkdir(update,0755));
    snprintf(path,sizeof(path),"%s/CPKs",game); assert(!mkdir(path,0755));
    snprintf(path,sizeof(path),"%s/CPKs",update); assert(!mkdir(path,0755));
    snprintf(path,sizeof(path),"%s/Only",game); assert(!mkdir(path,0755));
    put(game,"CPKs/base.cpk","base");
    put(game,"CPKs/both.cpk","old");
    put(update,"CPKs/both.cpk","patched");
    put(update,"CPKs/new.cpk","added");
    put(game,"Only/deep.txt","deep");
    setenv("SANCTUARY_APP0_LOWER",game,1);
    runtime_file_configure(update,user);
    expect("/app0/CPKs/base.cpk","base");      /* only in the game */
    expect("/app0/CPKs/both.cpk","patched");   /* the update wins */
    expect("/app0/CPKs/new.cpk","added");      /* only in the update */
    expect("/app0/Only/deep.txt","deep");      /* a folder only the game has */
    expect("/hostapp/CPKs/base.cpk","base");
    expect("CPKs/base.cpk","base");
    GuestStat info; assert(do_stat("/app0/CPKs/missing.cpk",&info)==-ENOENT);
    assert(do_open("/app0/CPKs/base.cpk",2,0)==-EROFS);
    assert(listed("/app0/CPKs","base.cpk")==1 && listed("/app0/CPKs","both.cpk")==1 &&
           listed("/app0/CPKs","new.cpk")==1);
    assert(listed("/app0","Only")==1 && listed("/app0","CPKs")==1);
    assert(listed("/app0/Only","deep.txt")==1);
    /* Saves are not layered. */
    assert(do_stat("/data/base.cpk",&info)==-ENOENT);
    char cmd[1100]; snprintf(cmd,sizeof(cmd),"rm -rf '%s'",root); assert(!system(cmd));
    puts("App0 layers: update over game, lower-only files and folders, merged listings PASS");
}
