/*
 * phantom.h - Phantom Arcade launcher, integrated into the Groovy core.
 *
 * The launcher is what the core shows when no client is streaming. It replaces the
 * bouncing-logo screensaver with a game list, talks to the Phantom Arcade host daemon
 * over UDP, and asks it to start an emulator. When the host's video arrives, the core
 * switches to it through the path it already had (groovy.cpp setInit -> logo off), so
 * there is no core reload, no second .rbf and no launcher script anywhere.
 *
 * Call order from groovy.cpp:
 *   phantom_init()        once, from groovy_start()
 *   phantom_idle_enter()  wherever the logo used to be switched on  (no client)
 *   phantom_idle_leave()  wherever the logo used to be switched off (client blitting)
 *   phantom_idle_poll()   once per groovy_poll(), cheap when nothing changed
 */
#ifndef PHANTOM_H
#define PHANTOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the core puts on screen when no client is streaming. Stored in phantom.ini and
 * set from the OSD's Phantom Arcade page, which is the single control for this.
 *
 * LOGO additionally respects the existing Server > Screensaver option, so a user who
 * never opens the Phantom page and had the screensaver switched off keeps exactly the
 * behaviour they had. LAUNCHER does not: the launcher is the reason this core exists,
 * and a blank screen with no way to discover the setting is a bad default. */
#define PHANTOM_IDLE_LAUNCHER 0
#define PHANTOM_IDLE_LOGO     1
#define PHANTOM_IDLE_OFF      2

void phantom_init(void);
void phantom_stop(void);

/* Idle-screen ownership. phantom_idle_enter() is a no-op (returning 0) when the user
 * has chosen the logo instead, which is how groovy.cpp decides whether to run its
 * original loadLogo() path. */
int  phantom_idle_enter(void);
void phantom_idle_leave(void);
void phantom_idle_poll(void);
int  phantom_owns_screen(void);

/* Input. Both return 1 when the launcher consumed the event, in which case the caller
 * must not also forward it to the host - otherwise navigating the menu types into
 * whatever the host has open. */
int phantom_joystick(unsigned char joy, uint32_t bitmask);
int phantom_keyboard(uint16_t key, int press);

/* Exit hotkey, live while a stream is running: hold Start+Select for ~1.2s to tell the
 * host to kill the emulator. Returns 1 once, on the frame the combo completes. */
int phantom_exit_hotkey(unsigned char joy, uint32_t bitmask);

/* OSD page (menu.cpp). */
int         phantom_get_idle_mode(void);
void        phantom_set_idle_mode(int mode);
const char *phantom_host_text(void);
const char *phantom_state_text(void);
int         phantom_title_count(void);
void        phantom_request_refresh(void);
void        phantom_send_kill(void);
void        phantom_return_to_launcher(void);
int         phantom_get_exit_hotkey(void);
void        phantom_set_exit_hotkey(int on);
void        phantom_save_config(void);

#ifdef __cplusplus
}
#endif

#endif
