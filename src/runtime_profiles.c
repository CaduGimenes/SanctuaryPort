/* Player profiles for local co-op (GitHub #93). A profile is a named save folder,
 * <user>/savedata/<id>/; the list is <user>/profiles.txt, "id<TAB>name" per line, which the
 * launcher edits too. The game's users 1..4 are seats, taken in the order controllers press a
 * button. SANCTUARY_PAD_PROFILES (the launcher's Game > Players) gives each controller it knows
 * a profile: one line per controller, "profile<TAB>guid<TAB>serial<TAB>path" (profile "ask":
 * none). A controller that takes a seat plays its own profile, whatever the seat; one without
 * (or whose profile another seat holds) gets a list to choose from, and its seat logs in once
 * a profile is chosen. The keyboard's line has the guid "keyboard". Without the variable
 * (run.sh, scripted runs, tests), seat n is folder n, as before profiles.
 * Seat 1 is logged in from the start: no controller is player 1 until one presses a button
 * (runtime_pad.c), and the game hears nothing from player 1 until it has a profile (Diablo III
 * reads no save before its "Press any button").
 * Folder ids are numbers so that the saves of users 1..4 are profiles 1..4 without moving. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/sanctuarygpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define SEATS 4
#define MAX_PROFILES 64
#define ID_SIZE 16
#define NAME_SIZE RUNTIME_PROFILE_NAME_SIZE

typedef struct { char id[ID_SIZE]; char name[NAME_SIZE]; } Profile;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Profile profiles[MAX_PROFILES];
static int profile_count, loaded, configured;
/* The controllers the launcher gave a profile (SANCTUARY_PAD_PROFILES). */
#define MAX_PADS 32
static struct { char profile[ID_SIZE], guid[40], serial[128], path[260]; } pads[MAX_PADS];
static int pad_count;
/* seats[n]: id "" while an "ask" seat waits for its player's choice. */
static struct { int ask; char id[ID_SIZE]; } seats[SEATS+1];

/* A name as a PS4 user name: at most 16 bytes of UTF-8 cut at a character boundary, without
 * tabs, line breaks or surrounding spaces (the list is one line per profile). */
size_t runtime_profile_clean_name(const char *in, char *out) {
    size_t n=0;
    if (in) while (*in==' ') ++in;
    for (const unsigned char *p=(const unsigned char *)(in ? in : ""); *p;) {
        size_t len=*p<0x80 ? 1 : (*p&0xE0)==0xC0 ? 2 : (*p&0xF0)==0xE0 ? 3 : 4;
        size_t have=0;
        while (have<len && p[have]) ++have;
        if (have<len || n+len>NAME_SIZE-1) break;
        if (len==1 && *p<0x20) { ++p; continue; }
        memcpy(out+n,p,len); n+=len; p+=len;
    }
    while (n>0 && out[n-1]==' ') --n;
    out[n]=0;
    return n;
}
static void copy_id(char *out, const char *id) {
    const size_t n=strnlen(id,ID_SIZE-1);
    memcpy(out,id,n); out[n]=0;
}
static int valid_id(const char *id) {
    size_t n=strlen(id);
    if (!n || n>=ID_SIZE) return 0;
    for (size_t i=0;i<n;++i) if (id[i]<'0' || id[i]>'9') return 0;
    return 1;
}
static Profile *find(const char *id) {
    for (int i=0;i<profile_count;++i) if (!strcmp(profiles[i].id,id)) return &profiles[i];
    return NULL;
}
static void list_path(char *out, size_t size) { snprintf(out,size,"%s/profiles.txt",runtime_file_user_dir()); }
static int save_list(void) {
    char path[600], temp[620];
    list_path(path,sizeof(path));
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    for (int i=0;i<profile_count;++i) fprintf(f,"%s\t%s\n",profiles[i].id,profiles[i].name);
    if (fclose(f)) return -1;
#ifdef _WIN32
    remove(path); /* the CRT's rename does not replace */
#endif
    if (rename(temp,path)) { printf("Runtime: profiles: cannot write %s (%s)\n",path,strerror(errno)); return -1; }
    return 0;
}
static void add(const char *id, const char *name) {
    if (profile_count>=MAX_PROFILES || find(id)) return;
    Profile *p=&profiles[profile_count++];
    copy_id(p->id,id);
    if (!runtime_profile_clean_name(name,p->name)) snprintf(p->name,sizeof(p->name),"Player %.8s",id);
}
static int by_number(const void *a, const void *b) { return atoi((const char *)a)-atoi((const char *)b); }
/* The first list, from what existed before profiles: profile 1 named as the launcher's old
 * Player name, and each other numbered save folder (local co-op players 2..4). */
static void first_list(void) {
    add("1",getenv("SANCTUARY_PLAYER_NAME") && *getenv("SANCTUARY_PLAYER_NAME") ? getenv("SANCTUARY_PLAYER_NAME") : "Player");
    char path[600], ids[MAX_PROFILES][ID_SIZE]; int n=0;
    snprintf(path,sizeof(path),"%s/savedata",runtime_file_user_dir());
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d)) && n<MAX_PROFILES;)
        if (valid_id(e->d_name) && strcmp(e->d_name,"1")) copy_id(ids[n++],e->d_name);
    if (d) closedir(d);
    qsort(ids,(size_t)n,ID_SIZE,by_number);
    for (int i=0;i<n;++i) { char name[NAME_SIZE]; snprintf(name,sizeof(name),"Player %.8s",ids[i]); add(ids[i],name); }
    if (!save_list()) printf("Runtime: profiles: created the list with %d profile%s\n",profile_count,profile_count==1 ? "" : "s");
}
static int seated_elsewhere(const char *id, int seat) {
    for (int s=1;s<=SEATS;++s) if (s!=seat && !strcmp(seats[s].id,id)) return 1;
    return 0;
}
/* Called under lock. */
static void load(void) {
    if (loaded) return;
    loaded=1;
    char path[600], line[256];
    list_path(path,sizeof(path));
    FILE *f=fopen(path,"rb");
    if (f) {
        while (fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            char *tab=strchr(line,'\t');
            if (!tab) continue;
            *tab=0;
            if (valid_id(line)) add(line,tab+1);
        }
        fclose(f);
    } else first_list();
    const char *list=getenv("SANCTUARY_PAD_PROFILES");
    configured=list!=NULL;
    for (const char *line=list; line && *line && pad_count<MAX_PADS;) {
        const size_t n=strcspn(line,"\n");
        char row[600]; snprintf(row,sizeof(row),"%.*s",(int)(n<sizeof(row)-1 ? n : sizeof(row)-1),line);
        line+=n; if (*line) ++line;
        char *field[4]={row,NULL,NULL,NULL};
        for (int i=1;i<4 && field[i-1];++i) { char *tab=strchr(field[i-1],'\t'); if (tab) { *tab=0; field[i]=tab+1; } }
        if (!field[1] || !valid_id(field[0])) continue; /* "ask", or not a line */
        copy_id(pads[pad_count].profile,field[0]);
        snprintf(pads[pad_count].guid,sizeof(pads[0].guid),"%s",field[1]);
        snprintf(pads[pad_count].serial,sizeof(pads[0].serial),"%s",field[2] ? field[2] : "");
        snprintf(pads[pad_count].path,sizeof(pads[0].path),"%s",field[3] ? field[3] : "");
        ++pad_count;
    }
    for (int s=1;s<=SEATS;++s) {
        if (!configured) snprintf(seats[s].id,ID_SIZE,"%d",s);
        else seats[s].ask=1;
    }
    if (configured) printf("Runtime: profiles: %d controller%s with a profile; each seat takes its controller's, or asks\n",pad_count,pad_count==1 ? "" : "s");
    else puts("Runtime: profiles: seat n plays save folder n (no SANCTUARY_PAD_PROFILES)");
}
/* The profile the launcher gave this controller: by serial, else by device path when either
 * has no serial, else by model (guid) when one controller of that model alone is listed.
 * Called under lock. */
static const char *pad_profile(const char *serial, const char *path, const char *guid) {
    serial=serial ? serial : ""; path=path ? path : ""; guid=guid ? guid : "";
    for (int i=0;i<pad_count && *serial;++i) if (!strcmp(pads[i].serial,serial)) return pads[i].profile;
    for (int i=0;i<pad_count && *path;++i)
        if (!strcmp(pads[i].path,path) && (!*pads[i].serial || !*serial)) return pads[i].profile;
    int found=-1, same=0;
    for (int i=0;i<pad_count && *guid;++i) if (!strcmp(pads[i].guid,guid)) { found=i; ++same; }
    return same==1 && (!*serial || !*pads[found].serial) ? pads[found].profile : NULL;
}
/* A controller took seat `user` (its first press): its own profile, when it has one that no
 * other seat holds; otherwise the seat asks. */
void runtime_profile_device(int user, const char *serial, const char *path, const char *guid) {
    pthread_mutex_lock(&lock);
    load();
    if (configured && user>=1 && user<=SEATS && seats[user].ask && !seats[user].id[0]) {
        const char *id=pad_profile(serial,path,guid);
        if (id && !seated_elsewhere(id,user)) {
            copy_id(seats[user].id,id);
            printf("Runtime: profiles: seat %d plays as profile %s (its controller's)\n",user,id);
        } else printf("Runtime: profiles: seat %d asks (%s)\n",user,id ? "its controller's profile is in another seat" : "a controller with no profile");
    }
    pthread_mutex_unlock(&lock);
}
/* Seat 1 is logged in from the start: asked for before its player chose, it answers with the
 * first profile no other seat names, without taking it (the choice still comes). Under lock. */
static const char *seat_one_guess(void) {
    for (int i=0;i<profile_count;++i) if (!seated_elsewhere(profiles[i].id,1)) return profiles[i].id;
    return "1";
}

/* The save folder of seat `user` (1..4): a profile id; the seat's number while it waits for a
 * choice (the game does not know the user then) and for other ids. */
void runtime_profile_dir(int user, char out[RUNTIME_PROFILE_ID_SIZE]) {
    pthread_mutex_lock(&lock);
    load();
    if (user>=1 && user<=SEATS && seats[user].id[0]) snprintf(out,RUNTIME_PROFILE_ID_SIZE,"%s",seats[user].id);
    else if (user==1) copy_id(out,seat_one_guess());
    else snprintf(out,RUNTIME_PROFILE_ID_SIZE,"%d",user);
    pthread_mutex_unlock(&lock);
}
/* The name the game shows for seat `user`: its profile's; "Player"/"Player N" without one. */
void runtime_profile_name(int user, char out[RUNTIME_PROFILE_NAME_SIZE]) {
    pthread_mutex_lock(&lock);
    load();
    const Profile *p=user>=1 && user<=SEATS && seats[user].id[0] ? find(seats[user].id) : NULL;
    if (p) snprintf(out,RUNTIME_PROFILE_NAME_SIZE,"%s",p->name);
    else if (user==1) snprintf(out,RUNTIME_PROFILE_NAME_SIZE,"Player");
    else snprintf(out,RUNTIME_PROFILE_NAME_SIZE,"Player %d",user);
    pthread_mutex_unlock(&lock);
}
/* 1 when seat `user` has its profile (the game may learn of the login). */
int runtime_profile_seat_ready(int user) {
    pthread_mutex_lock(&lock);
    load();
    const int ready=user>=1 && user<=SEATS && seats[user].id[0];
    pthread_mutex_unlock(&lock);
    return ready;
}
/* The seat's gamepad left: an "ask" seat asks again for the next player who takes it. */
void runtime_profile_seat_left(int user) {
    pthread_mutex_lock(&lock);
    if (user>=1 && user<=SEATS && seats[user].ask && seats[user].id[0]) {
        printf("Runtime: profiles: seat %d left profile %s\n",user,seats[user].id);
        seats[user].id[0]=0;
    }
    pthread_mutex_unlock(&lock);
}
/* A new profile named `name` (cleaned; empty: "Player <id>"): its id in out, 0 on success.
 * Ids are numbers above every listed profile and every save folder, so a new profile never
 * takes over a folder left behind. Called under lock. */
static int create(const char *name, char out[ID_SIZE]) {
    if (profile_count>=MAX_PROFILES) return -1;
    long next=1;
    for (int i=0;i<profile_count;++i) { long v=atol(profiles[i].id); if (v>=next) next=v+1; }
    char path[600];
    snprintf(path,sizeof(path),"%s/savedata",runtime_file_user_dir());
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) if (valid_id(e->d_name)) { long v=atol(e->d_name); if (v>=next) next=v+1; }
    if (d) closedir(d);
    if (next>999999999) return -1;
    snprintf(out,ID_SIZE,"%d",(int)next);
    add(out,name);
    if (save_list()) { --profile_count; return -1; }
    printf("Runtime: profiles: created profile %s (%s)\n",out,profiles[profile_count-1].name);
    return 0;
}
int runtime_profile_create(const char *name, char out[RUNTIME_PROFILE_ID_SIZE]) {
    pthread_mutex_lock(&lock);
    load();
    const int r=create(name,out);
    pthread_mutex_unlock(&lock);
    return r;
}

/* ---- The list a joining player chooses from (seats that ask; seat 1 on its first press) ----
 * Driven from runtime_user_events on the game thread. It opens when no other list, text box
 * or menu is up, and takes input from the joining gamepad (and the keyboard) only, so the
 * players already in keep playing. A game dialog that opens meanwhile takes the overlay
 * (runtime_profile_yield); the list comes back once it closes. */
enum { IDLE, CHOOSING, NAMING };
static struct {
    int state, seat, count;          /* count: profiles listed before "New profile" */
    char ids[MAX_PROFILES][ID_SIZE];
} prompt;
static const char *const NEW_PROFILE="New profile";

static void resolve(int seat, const char *id) {
    pthread_mutex_lock(&lock);
    if (!seated_elsewhere(id,seat)) {
        copy_id(seats[seat].id,id);
        printf("Runtime: profiles: seat %d plays as profile %s\n",seat,id);
    }
    pthread_mutex_unlock(&lock);
}
static void open_list(int seat) {
    const char *items[MAX_PROFILES+1];
    static char names[MAX_PROFILES][NAME_SIZE];
    int n=0;
    pthread_mutex_lock(&lock);
    for (int i=0;i<profile_count && n<MAX_PROFILES;++i) {
        if (seated_elsewhere(profiles[i].id,0)) continue; /* seat 0: none, so every seated one */
        copy_id(prompt.ids[n],profiles[i].id);
        snprintf(names[n],NAME_SIZE,"%s",profiles[i].name);
        items[n]=names[n]; ++n;
    }
    pthread_mutex_unlock(&lock);
    items[n]=NEW_PROFILE;
    char title[64]; snprintf(title,sizeof(title),"Player %d: choose a profile",seat);
    prompt.seat=seat; prompt.count=n;
    /* SANCTUARY_PROFILE_CHOICE (development, scripted runs): answers the list without showing
     * it, as SANCTUARY_IME_TEXT does the name box: an item's number, or "new:<name>". */
    const char *scripted=getenv("SANCTUARY_PROFILE_CHOICE");
    if (scripted && *scripted) {
        char id[ID_SIZE];
        printf("Runtime: profiles: seat %d answered by SANCTUARY_PROFILE_CHOICE=%s\n",seat,scripted);
        if (!strncmp(scripted,"new:",4)) { if (!runtime_profile_create(scripted+4,id)) resolve(seat,id); }
        else if (atoi(scripted)>=0 && atoi(scripted)<n) resolve(seat,prompt.ids[atoi(scripted)]);
        return;
    }
    if (!sanctuarygpu_choice_begin(title,items,NULL,n+1,0)) {
        /* No window to ask in: the first free profile, else a new one. */
        char id[ID_SIZE];
        if (n) resolve(seat,prompt.ids[0]);
        else if (!runtime_profile_create("",id)) resolve(seat,id);
        return;
    }
    sanctuarygpu_choice_set_owner(runtime_pad_seat_device(seat));
    printf("Runtime: profiles: seat %d chooses a profile (%d listed)\n",seat,n);
    prompt.state=CHOOSING;
}
static void open_naming(int seat) {
    char start[NAME_SIZE], title[64];
    snprintf(start,sizeof(start),"Player %d",seat);
    snprintf(title,sizeof(title),"Player %d: new profile name",seat);
    if (!sanctuarygpu_text_input_begin(start,title)) {
        char id[ID_SIZE];
        if (!runtime_profile_create(start,id)) resolve(seat,id);
        prompt.state=IDLE;
        return;
    }
    sanctuarygpu_text_input_set_owner(runtime_pad_seat_device(seat));
    prompt.state=NAMING;
}
void runtime_profile_prompts(void) {
    if (prompt.state==IDLE) {
        const unsigned seated=runtime_pad_seated();
        if (sanctuarygpu_overlay_captures_input() || runtime_dialog_busy()) return;
        for (int s=1;s<=SEATS;++s)
            if ((seated & (1u<<s)) && !runtime_profile_seat_ready(s)) { open_list(s); return; }
        return;
    }
    const int seat=prompt.seat;
    const int gone=!(runtime_pad_seated() & (1u<<seat));
    if (prompt.state==CHOOSING) {
        const int choice=sanctuarygpu_choice_poll();
        if (gone && choice==-1) { sanctuarygpu_choice_end(); prompt.state=IDLE; return; }
        if (choice==-1) return;
        prompt.state=IDLE;
        if (gone) return;
        if (choice<0) {
            printf("Runtime: profiles: seat %d cancelled; its gamepad waits for a button again\n",seat);
            runtime_pad_unseat(seat);
        } else if (choice<prompt.count) resolve(seat,prompt.ids[choice]);
        else open_naming(seat);
        return;
    }
    char text[256];
    const int state=sanctuarygpu_text_input_poll(text,sizeof(text));
    if (!state) return;
    prompt.state=IDLE;
    if (state!=1 || gone) return; /* cancelled: the list again */
    char name[NAME_SIZE], id[ID_SIZE];
    if (!runtime_profile_clean_name(text,name)) snprintf(name,sizeof(name),"Player %d",seat);
    if (!runtime_profile_create(name,id)) resolve(seat,id);
}
/* A game dialog takes the overlay's list or text box: the prompt opens again later. */
void runtime_profile_yield(void) {
    if (prompt.state!=IDLE) printf("Runtime: profiles: a game dialog opened; seat %d is asked again after it\n",prompt.seat);
    prompt.state=IDLE;
}
