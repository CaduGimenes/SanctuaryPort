/* Local co-op with real gamepads and no SANCTUARY_PAD_FILE (how players run it): player 2's
 * sticks come from their own gamepad (GitHub #20: they were held at 0, up-left). */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

int sanctuarygpu_overlay_captures_input(void) { return 0; }
int sanctuarygpu_vibration_percent(void) { return 100; }
void runtime_user_events(void) {}
/* Profiles (runtime_profiles.c): every seat has one unless a test says otherwise. */
static unsigned not_ready, seats_left;
int runtime_profile_seat_ready(int user) { return !(not_ready & (1u<<user)); }
void runtime_profile_seat_left(int user) { seats_left|=1u<<user; }
static int devices_seen;
void runtime_profile_device(int user, const char *serial, const char *path, const char *guid) { (void)user; (void)serial; (void)path; (void)guid; ++devices_seen; }
int sanctuarygpu_stick_deadzone_percent(void) { return 0; }
void sanctuarygpu_request_screenshot(void) {}
int sanctuarygpu_mouse_prompt_distance(void) { return 0; }
void sanctuarygpu_mouse_pause_key(void) {}
int sanctuarygpu_mouse_aim_style(void) { return 0; }
int sanctuarygpu_mouse_aim_sensitivity(void) { return 100; }
int sanctuarygpu_wheel_turn_speed(void) { return 120; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void pump(void) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }
static SDL_Joystick *attach(SDL_VirtualJoystickDesc *desc, const char *name) {
    desc->name=name;
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    return joystick;
}

int main(void) {
#ifdef _WIN32
    _putenv_s("SANCTUARY_PAD_FILE","");
#else
    unsetenv("SANCTUARY_PAD_FILE");
#endif
    setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    SDL_Joystick *first=attach(&desc,"sanctuary co-op controller 1");
    pump();
    PadData d;
    assert(pad_read_state(1,&d)==0); /* player 1 takes the first gamepad */
    SDL_Joystick *second=attach(&desc,"sanctuary co-op controller 2");
    pump();
    runtime_pad_users();

    /* Player 2 joins with a press on their own gamepad. */
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_START,true));
    pump(); runtime_pad_users();
    usleep(270000);
    pump();
    assert(runtime_pad_users()==(1u<<2));
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_START,false));
    assert(pad_open(2,0,0,NULL)==2);

    /* Sticks at rest: centred, not pushed up-left. */
    pump();
    assert(pad_read_state(2,&d)==0);
    assert(d.connected && d.left_x==128 && d.left_y==128 && d.right_x==128 && d.right_y==128);
    /* Player 2's stick moves player 2 only. */
    assert(SDL_SetJoystickVirtualAxis(second,SDL_GAMEPAD_AXIS_LEFTX,32767));
    assert(SDL_SetJoystickVirtualAxis(second,SDL_GAMEPAD_AXIS_RIGHTY,-32768));
    pump();
    assert(pad_read_state(2,&d)==0);
    assert(d.left_x==255 && d.left_y==128 && d.right_y==0);
    assert(pad_read_state(1,&d)==0);
    assert(d.left_x==128 && d.right_y==128);

    assert(SDL_SetJoystickVirtualAxis(second,SDL_GAMEPAD_AXIS_LEFTX,0));
    assert(SDL_SetJoystickVirtualAxis(second,SDL_GAMEPAD_AXIS_RIGHTY,0));
    pump();

    /* Player 1's controller drops (Bluetooth): player 2's must not drive player 1 too. */
    const SDL_JoystickID first_id=SDL_GetJoystickID(first);
    SDL_CloseJoystick(first);
    assert(SDL_DetachVirtualJoystick(first_id));
    pump();
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_SOUTH,true));
    pump();
    assert(pad_read_state(1,&d)==0);
    assert(!(d.buttons & BTN_CROSS));          /* player 1: keyboard only, not player 2's pad */
    assert(pad_read_state(2,&d)==0);
    assert(d.buttons & BTN_CROSS);             /* player 2 keeps its own */
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_SOUTH,false));
    pump();

    /* It comes back: player 1 again, not a new player 3. */
    SDL_Joystick *back=attach(&desc,"sanctuary co-op controller 1 again");
    pump();
    assert(pad_read_state(1,&d)==0);
    assert(SDL_SetJoystickVirtualButton(back,SDL_GAMEPAD_BUTTON_SOUTH,true));
    pump(); runtime_pad_users();
    usleep(270000);
    pump();
    assert(pad_read_state(1,&d)==0);
    assert(d.buttons & BTN_CROSS);
    assert(runtime_pad_users()==(1u<<2));     /* still players 1 and 2 only */
    assert(pad_read_state(2,&d)==0);
    assert(!(d.buttons & BTN_CROSS));

    /* #93: while player 2 chooses a profile the seat is taken but not logged in. */
    not_ready=1u<<2;
    assert(runtime_pad_seated()==(1u<<2) && runtime_pad_users()==0);
    assert(runtime_pad_seat_device(2)==SDL_GetJoystickID(second) && runtime_pad_seat_device(3)==0);
    /* Cancelled with Circle: the gamepad leaves the seat; the Circle still held is no new press. */
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_EAST,true));
    pump();
    runtime_pad_unseat(2);
    assert(seats_left==(1u<<2));
    pump(); runtime_pad_seated();
    usleep(270000);
    pump();
    assert(runtime_pad_seated()==0 && runtime_pad_users()==0);
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_EAST,false));
    pump(); runtime_pad_seated();
    /* Its next press takes the seat again; with a profile it logs in. */
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_START,true));
    pump(); runtime_pad_seated();
    usleep(270000);
    pump();
    assert(runtime_pad_seated()==(1u<<2) && runtime_pad_users()==0);
    not_ready=0;
    assert(runtime_pad_users()==(1u<<2));
    assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_START,false));

    /* Seat 1 that asks: player 1 cancelled its list. Its gamepad is free again, the button it
     * still holds is no press, and the game hears nothing from player 1 meanwhile. */
    not_ready=1u<<1;
    runtime_pad_unseat(1);
    pump();
    assert(pad_read_state(1,&d)==0 && !d.buttons);
    assert(runtime_pad_seated()==(1u<<2) && runtime_pad_seat_device(1)==0);
    assert(SDL_SetJoystickVirtualButton(back,SDL_GAMEPAD_BUTTON_SOUTH,false));
    pump();
    assert(pad_read_state(1,&d)==0);
    /* Its next press takes seat 1 (player 2's gamepad stays player 2's). */
    assert(SDL_SetJoystickVirtualButton(back,SDL_GAMEPAD_BUTTON_START,true));
    pump();
    assert(pad_read_state(1,&d)==0 && !d.buttons);  /* choosing: the game hears nothing */
    assert(runtime_pad_seated()==((1u<<1)|(1u<<2)) && runtime_pad_users()==(1u<<2));
    assert(runtime_pad_seat_device(1)==SDL_GetJoystickID(back));
    /* Chosen: player 1 plays; the held press stays hidden until released. */
    not_ready=0;
    assert(runtime_pad_seated()==(1u<<2));
    assert(pad_read_state(1,&d)==0 && !d.buttons);
    assert(SDL_SetJoystickVirtualButton(back,SDL_GAMEPAD_BUTTON_START,false));
    pump();
    assert(pad_read_state(1,&d)==0);
    assert(SDL_SetJoystickVirtualButton(back,SDL_GAMEPAD_BUTTON_SOUTH,true));
    pump();
    assert(pad_read_state(1,&d)==0 && (d.buttons & BTN_CROSS));

    SDL_Quit();
    puts("pad co-op test: ok");
    return 0;
}
