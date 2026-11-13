#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

static int capture;
static int vibration_percent=100;
static uint16_t rumble_low, rumble_high;
int sanctuarygpu_overlay_captures_input(void) { return capture; }
int sanctuarygpu_vibration_percent(void) { return vibration_percent; }

static bool SDLCALL record_rumble(void *userdata, uint16_t low, uint16_t high) {
    (void)userdata;
    rumble_low=low;
    rumble_high=high;
    return true;
}
void runtime_user_events(void) {}
int sanctuarygpu_stick_deadzone_percent(void) { return 0; }
void sanctuarygpu_request_screenshot(void) {}
int sanctuarygpu_mouse_prompt_distance(void) { return 0; }
void sanctuarygpu_mouse_pause_key(void) {}
int sanctuarygpu_mouse_aim_style(void) { return 0; }
int sanctuarygpu_mouse_aim_sensitivity(void) { return 100; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}
static PadData read_pad(void) {
    PadData d;
    assert(pad_read_state(1,&d)==0);
    return d;
}
/* One finger in the middle of the left (480) or right (1440) half of the touchpad. */
static int touch_at(const PadData *d, int x) {
    return d->touch_count==1 && d->touches[0].x==x && d->touches[0].y==471;
}
static void pump(void) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }
/* A press on a further gamepad decides after the mirror window (250 ms): seen now, judged later. */
static unsigned users_after_press(void) {
    pump(); runtime_pad_users();
    usleep(270000);
    pump(); return runtime_pad_users();
}

int main(void) {
    const char *tmp=getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char path[1024];
    snprintf(path,sizeof(path),"%s/sanctuary-pad-test-XXXXXX",tmp);
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("SANCTUARY_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);

    /* A click without a touch surface (Back, Tab, Backspace, the pad file) plays like a finger:
     * the touch comes first, the button CLICK_LEAD_US later, and the touch stays CLICK_TAIL_US
     * after the release. Games may act on the release of a click with the finger still down
     * and ignore a touch that appears and vanishes with the button. */
    inject(path,"cross l3 touchpad_left");
    PadData data=read_pad();
    assert((data.buttons & (BTN_CROSS|BTN_L3))==(BTN_CROSS|BTN_L3));
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    inject(path,"");
    data=read_pad();
    assert(data.buttons==0 && touch_at(&data,480)); /* released: the finger lingers */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* A tap shorter than the lead still clicks, for CLICK_HOLD_US. */
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* L1 + the right click is a left click (photo mode on a pad without a touchpad). The side
     * is chosen when the click starts and kept until it ends. */
    inject(path,"l1 touchpad_right");
    data=read_pad();
    assert((data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* The settings menu takes the input and ends a click; the source starts a new one after. */
    inject(path,"touchpad_right");
    usleep(60000);
    data=read_pad();
    assert(touch_at(&data,1440));
    capture=1;
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    capture=0;
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="sanctuary test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=record_rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    /* A real touchpad: the fingers and the click go to the game as they are. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    pump();
    data=read_pad();
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    const uint8_t motors[]={255,128};
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==65535 && rumble_high==32896);
    vibration_percent=50;
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==32767 && rumble_high==16448);
    vibration_percent=0;
    assert(pad_vibration(1,motors)==0);
    assert(rumble_low==0 && rumble_high==0);
    vibration_percent=100;
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    data=read_pad();
    assert(data.touch_count==0 && data.buttons==0);
    capture=0;
    /* The click without a finger is played as a left click. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Back (Share, View) is the right click: the pause menu. With L1 it is the left click. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert((data.buttons & BTN_L1) && !(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & (BTN_L1|BTN_TOUCHPAD))==(BTN_L1|BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Local co-op: a second gamepad logs user 2 in; its pad reads only that gamepad. */
    assert(runtime_pad_users()==0);
    desc.name="sanctuary test controller 2";
    SDL_JoystickID id2=SDL_AttachVirtualJoystick(&desc);
    assert(id2!=0);
    SDL_Joystick *joystick2=SDL_OpenJoystick(id2);
    assert(joystick2);
    pump();
    /* #20: a further gamepad joins only when a button is pressed on it. Moving sticks (a motion
     * sensor device, a drifting stick) does not, nor a button player 1 holds too (a mirror). */
    assert(runtime_pad_users()==0);
    assert(SDL_SetJoystickVirtualAxis(joystick2,SDL_GAMEPAD_AXIS_LEFTX,20000));
    pump();
    assert(runtime_pad_users()==0);
    assert(SDL_SetJoystickVirtualAxis(joystick2,SDL_GAMEPAD_AXIS_LEFTX,0));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(users_after_press()==0);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,false));
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_EAST,false));
    pump(); runtime_pad_users();
    /* A copy that is a little ahead of player 1's pad (Steam Input, #20 on 0.5.0): the copy's
     * press first, player 1's 60 ms later, still player 1's. */
    usleep(300000);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_WEST,true));
    pump(); assert(runtime_pad_users()==0);
    usleep(60000);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_WEST,true));
    assert(users_after_press()==0);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_WEST,false));
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_WEST,false));
    pump(); runtime_pad_users();
    usleep(300000);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_START,true));
    pump();
    assert(runtime_pad_users()==0);  /* not before the window */
    assert(users_after_press()==(1u<<2));
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_START,false));
    pump();
    assert(runtime_pad_users()==(1u<<2));  /* it keeps its seat */
    assert(pad_open(2,0,0,NULL)==2 && pad_open(2,0,0,NULL)==ERR_ALREADY_OPENED);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualAxis(joystick2,SDL_GAMEPAD_AXIS_LEFTX,-32768));
    pump();
    PadData second;
    assert(pad_read_state(2,&second)==0);
    assert(second.connected && (second.buttons & BTN_CROSS) && second.left_x==0);
    data=read_pad();
    assert(!(data.buttons & BTN_CROSS) && data.left_x==128);
    assert(SDL_SetJoystickVirtualButton(joystick2,SDL_GAMEPAD_BUTTON_SOUTH,false));
    /* Users 3 and 4: one more gamepad each, in the order they connect; each reads its own. */
    desc.name="sanctuary test controller 3";
    SDL_JoystickID id3=SDL_AttachVirtualJoystick(&desc);
    desc.name="sanctuary test controller 4";
    SDL_JoystickID id4=SDL_AttachVirtualJoystick(&desc);
    assert(id3 && id4);
    SDL_Joystick *joystick3=SDL_OpenJoystick(id3), *joystick4=SDL_OpenJoystick(id4);
    assert(joystick3 && joystick4);
    pump();
    assert(runtime_pad_users()==(1u<<2));
    /* Seats in the order the buttons are pressed: controller 3 first, then controller 4. */
    assert(SDL_SetJoystickVirtualButton(joystick3,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(users_after_press()==((1u<<2)|(1u<<3)));
    assert(SDL_SetJoystickVirtualButton(joystick3,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(SDL_SetJoystickVirtualButton(joystick4,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(users_after_press()==((1u<<2)|(1u<<3)|(1u<<4)));
    assert(SDL_SetJoystickVirtualButton(joystick4,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(pad_open(3,0,0,NULL)==3 && pad_open(4,0,0,NULL)==4 && pad_open(5,0,0,NULL)==ERR_INVALID_ARG);
    assert(SDL_SetJoystickVirtualButton(joystick4,SDL_GAMEPAD_BUTTON_NORTH,true));
    pump();
    PadData third, fourth;
    assert(pad_read_state(3,&third)==0 && pad_read_state(4,&fourth)==0);
    assert(third.connected && !(third.buttons & BTN_TRIANGLE));
    assert(fourth.connected && (fourth.buttons & BTN_TRIANGLE));
    assert(SDL_SetJoystickVirtualButton(joystick4,SDL_GAMEPAD_BUTTON_NORTH,false));
    SDL_CloseJoystick(joystick3);
    assert(SDL_DetachVirtualJoystick(id3));
    pump();
    assert(runtime_pad_users()==((1u<<2)|(1u<<4)));  /* player 4 keeps its gamepad */
    SDL_CloseJoystick(joystick4);
    assert(SDL_DetachVirtualJoystick(id4));
    pump();
    assert(pad_close(3)==0 && pad_close(4)==0);
    SDL_CloseJoystick(joystick2);
    assert(SDL_DetachVirtualJoystick(id2));
    pump();
    assert(pad_read_state(2,&second)==0 && !second.connected && second.buttons==0);
    assert(runtime_pad_users()==0);
    assert(pad_close(2)==0 && pad_read_state(2,&second)==ERR_INVALID_HANDLE);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, staged left/right clicks, L1 chord, SDL touch coordinates, overlay capture, player 2");
}
