/* Player profiles (GitHub #93): the first list from existing saves, seats from the launcher's
 * settings, and the list a joining player chooses from. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_profiles.c"
#include <assert.h>
#include <unistd.h>

static char user_dir[256];
const char *runtime_file_user_dir(void) { return user_dir; }
static unsigned seated;
static int unseated, busy, captured;
unsigned runtime_pad_seated(void) { return seated; }
uint32_t runtime_pad_seat_device(int user) { return 70u+(uint32_t)user; }
void runtime_pad_unseat(int user) { unseated=user; seated&=~(1u<<user); }
int runtime_dialog_busy(void) { return busy; }
int sanctuarygpu_overlay_captures_input(void) { return captured; }
/* The overlay: what was shown, and what the player answers. */
static int window=1, choice_open, choice_answer=-1, choice_count, ended, text_open, text_answer;
static uint32_t choice_owner, text_owner;
static char choice_title[64], choice_items[8][32], text_start[64], text_answer_text[64];
int sanctuarygpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) {
    (void)images; (void)focus;
    if (!window) return 0;
    snprintf(choice_title,sizeof choice_title,"%s",title);
    for (int i=0;i<count && i<8;++i) snprintf(choice_items[i],sizeof choice_items[i],"%s",items[i]);
    choice_count=count; choice_open=1; choice_answer=-1; choice_owner=0;
    return 1;
}
int sanctuarygpu_choice_poll(void) { return choice_answer; }
void sanctuarygpu_choice_set_owner(uint32_t id) { choice_owner=id; }
void sanctuarygpu_choice_end(void) { ended=1; choice_open=0; }
int sanctuarygpu_text_input_begin(const char *text, const char *prompt) {
    (void)prompt;
    if (!window) return 0;
    snprintf(text_start,sizeof text_start,"%s",text);
    text_open=1; text_answer=0; text_owner=0;
    return 1;
}
int sanctuarygpu_text_input_poll(char *out, uint64_t size) { snprintf(out,size,"%s",text_answer_text); return text_answer; }
void sanctuarygpu_text_input_set_owner(uint32_t id) { text_owner=id; }

static void reset(void) {
    loaded=0; profile_count=0; configured=0; pad_count=0;
    memset(seats,0,sizeof seats); memset(&prompt,0,sizeof prompt);
}
/* The launcher's controllers: "profile<TAB>guid<TAB>serial<TAB>path" lines; NULL: no launcher. */
static void pads_env(const char *lines) {
    if (lines) setenv("SANCTUARY_PAD_PROFILES",lines,1); else unsetenv("SANCTUARY_PAD_PROFILES");
}
static void make(const char *relative) {
    char path[512]; snprintf(path,sizeof path,"%s/%s",user_dir,relative);
    assert(!mkdir(path,0755) || errno==EEXIST);
}
static void read_list(char *out, size_t size) {
    char path[512]; snprintf(path,sizeof path,"%s/profiles.txt",user_dir);
    FILE *f=fopen(path,"rb"); assert(f);
    size_t n=fread(out,1,size-1,f); out[n]=0; fclose(f);
}
static const char *dir(int user) { static char id[RUNTIME_PROFILE_ID_SIZE]; runtime_profile_dir(user,id); return id; }
static const char *name(int user) { static char text[RUNTIME_PROFILE_NAME_SIZE]; runtime_profile_name(user,text); return text; }

int main(void) {
    char text[64], list[512];
    /* Names: 16 bytes at most, cut between characters, no tabs or line breaks (#23). */
    assert(runtime_profile_clean_name("  Ação Bárbaro Supremo",text)==15 && !strcmp(text,"Ação Bárbaro"));
    assert(runtime_profile_clean_name("Deckard\tCain\n",text) && !strcmp(text,"DeckardCain"));
    assert(runtime_profile_clean_name("   ",text)==0 && !text[0]);

    snprintf(user_dir,sizeof user_dir,"out/profiles-test-XXXXXX");
    assert(mkdtemp(user_dir));
    make("savedata"); make("savedata/1"); make("savedata/3"); make("savedata/12"); make("savedata/backup");

    /* No list yet: it is made from the saves there were, profile 1 named as before (#23). No
     * seat settings (run.sh, scripted runs): seat n is folder n, as before profiles. */
    setenv("SANCTUARY_PLAYER_NAME","Stavros",1);
    pads_env(NULL);
    reset();
    assert(!strcmp(dir(1),"1") && !strcmp(dir(2),"2") && !strcmp(dir(4),"4"));
    read_list(list,sizeof list);
    assert(!strcmp(list,"1\tStavros\n3\tPlayer 3\n12\tPlayer 12\n"));
    assert(!strcmp(name(1),"Stavros") && !strcmp(name(2),"Player 2") && !strcmp(name(3),"Player 3"));
    assert(runtime_profile_seat_ready(2));

    /* The launcher's list is read as it is; names changed there show in the game (#86). */
    char path[512]; snprintf(path,sizeof path,"%s/profiles.txt",user_dir);
    FILE *f=fopen(path,"wb"); assert(f);
    fputs("1\tStavros\n3\tLeah\r\n12\tTyrael\nbad line\nx\tNot an id\n",f); fclose(f);
    /* Controllers from the launcher, each with its profile, whatever seat it takes: by serial
     * (its device path may change), by path when it has no serial, by model when one controller
     * of that model alone is listed. "ask" lines give none. */
    pads_env("3\tguidA\tSER1\t/dev/a\n12\tguidB\t\t/dev/hidraw4\nask\tguidC\t\t\nbad\n");
    reset();
    assert(!runtime_profile_seat_ready(1) && !runtime_profile_seat_ready(4)); /* every seat waits for a press */
    runtime_profile_device(1,"SER1","/dev/other","guidA");  /* player 2's heroes, solo or first */
    assert(runtime_profile_seat_ready(1) && !strcmp(dir(1),"3") && !strcmp(name(1),"Leah"));
    runtime_profile_device(4,NULL,"/dev/hidraw4","guidB");
    assert(!strcmp(dir(4),"12") && !strcmp(name(4),"Tyrael"));
    runtime_profile_device(3,"SER9",NULL,"guidA");          /* another controller of that model */
    runtime_profile_device(2,NULL,NULL,"guidC");            /* its profile: ask */
    assert(!runtime_profile_seat_ready(2) && !runtime_profile_seat_ready(3));
    assert(!strcmp(dir(2),"2")); /* a waiting seat: the game does not know it yet */
    assert(profile_count==3 && pad_count==2); /* the "ask" line gives no profile */

    /* Player 2 joins: once no other list is up, they choose from the profiles not seated. */
    seated=1u<<2; busy=1;
    runtime_profile_prompts();
    assert(!choice_open);
    busy=0; captured=1;
    runtime_profile_prompts();
    assert(!choice_open);
    captured=0;
    runtime_profile_prompts();
    assert(choice_open && !strcmp(choice_title,"Player 2: choose a profile") && choice_owner==72);
    assert(choice_count==2 && !strcmp(choice_items[0],"Stavros") && !strcmp(choice_items[1],"New profile"));
    runtime_profile_prompts();
    assert(!runtime_profile_seat_ready(2));
    choice_answer=0;
    runtime_profile_prompts();
    assert(runtime_profile_seat_ready(2) && !strcmp(dir(2),"1") && !strcmp(name(2),"Stavros"));

    /* Their gamepad leaves: the seat asks the next player who takes it. */
    runtime_profile_seat_left(2);
    assert(!runtime_profile_seat_ready(2));
    /* A new profile: named on the keyboard (Cross accepts "Player 2" from a gamepad). Its folder
     * number is above every profile and every save folder. */
    runtime_profile_prompts();
    assert(choice_open && choice_count==2);
    choice_answer=1;
    runtime_profile_prompts();
    assert(text_open && !strcmp(text_start,"Player 2") && text_owner==72);
    snprintf(text_answer_text,sizeof text_answer_text,"  Deckard\tCain");
    runtime_profile_prompts();
    assert(!runtime_profile_seat_ready(2));
    text_answer=1;
    runtime_profile_prompts();
    assert(runtime_profile_seat_ready(2) && !strcmp(dir(2),"13") && !strcmp(name(2),"DeckardCain"));
    read_list(list,sizeof list);
    assert(!strcmp(list,"1\tStavros\n3\tLeah\n12\tTyrael\n13\tDeckardCain\n"));

    /* Player 3 cancels: their gamepad leaves the seat. */
    seated|=1u<<3; choice_open=0;
    runtime_profile_prompts();
    assert(choice_open && !strcmp(choice_title,"Player 3: choose a profile"));
    assert(choice_count==2 && !strcmp(choice_items[0],"Stavros")); /* 3, 12, 13 are seated */
    choice_answer=-2;
    runtime_profile_prompts();
    assert(unseated==3 && !runtime_profile_seat_ready(3));
    /* Player 3's gamepad disconnects while the list is up: it closes. */
    seated|=1u<<3; choice_open=0;
    runtime_profile_prompts();
    assert(choice_open);
    seated&=~(1u<<3);
    runtime_profile_prompts();
    assert(ended && !choice_open && prompt.state==IDLE);
    /* A game dialog takes the overlay: the list comes back after it. */
    seated|=1u<<3;
    runtime_profile_prompts();
    assert(prompt.state==CHOOSING);
    runtime_profile_yield();
    assert(prompt.state==IDLE);
    /* No window to ask in: the first free profile. */
    window=0;
    runtime_profile_prompts();
    assert(runtime_profile_seat_ready(3) && !strcmp(dir(3),"1"));

    /* Seat 1 waits for the first button press (runtime_pad.c). Asked before that, it answers
     * with the first free profile without taking it. The keyboard has a profile too; a profile
     * already seated is not taken twice: the seat asks. */
    pads_env("1\tkeyboard\t\t\n");
    reset(); window=1; seated=0; choice_open=0;
    runtime_profile_device(2,NULL,NULL,"keyboard");
    assert(!strcmp(dir(2),"1"));
    assert(!runtime_profile_seat_ready(1) && !strcmp(dir(1),"3") && !strcmp(name(1),"Player"));
    runtime_profile_device(1,NULL,NULL,"keyboard");
    assert(!runtime_profile_seat_ready(1));
    runtime_profile_prompts();
    assert(!choice_open); /* nobody pressed yet */
    seated=1u<<1;
    runtime_profile_prompts();
    assert(choice_open && !strcmp(choice_title,"Player 1: choose a profile") && choice_owner==71);
    assert(choice_count==4 && !strcmp(choice_items[0],"Leah") && !strcmp(choice_items[3],"New profile"));
    choice_answer=-2; /* cancelled: seat 1 waits for a press again */
    runtime_profile_prompts();
    assert(unseated==1 && !runtime_profile_seat_ready(1));
    seated=1u<<1; choice_open=0;
    runtime_profile_prompts();
    assert(choice_open);
    choice_answer=1;
    runtime_profile_prompts();
    assert(runtime_profile_seat_ready(1) && !strcmp(dir(1),"12") && !strcmp(name(1),"Tyrael"));

    /* One profile only: a player who would be asked takes it; a second one (it is seated) gets
     * the list, to make a new profile. */
    snprintf(path,sizeof path,"%s/profiles.txt",user_dir);
    f=fopen(path,"wb"); assert(f); fputs("1\tStavros\n",f); fclose(f);
    pads_env("");
    reset(); window=1; seated=1u<<1; choice_open=0;
    runtime_profile_prompts();
    assert(!choice_open && runtime_profile_seat_ready(1) && !strcmp(name(1),"Stavros"));
    seated=(1u<<1)|(1u<<2);
    runtime_profile_prompts();
    assert(choice_open && choice_count==1 && !strcmp(choice_items[0],"New profile"));

    char command[600]; snprintf(command,sizeof command,"rm -rf '%s'",user_dir);
    assert(!system(command));
    puts("profiles test: ok");
    return 0;
}
