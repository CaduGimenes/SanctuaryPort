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

    (void)first;
    SDL_Quit();
    puts("pad co-op test: ok");
    return 0;
}
