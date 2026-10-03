/*
 * phantom_ui.h - the Phantom Arcade launcher's view and navigation state.
 *
 * This half of the launcher is deliberately free of every dependency the rest of the
 * core carries: no SPI, no sockets, no MiSTer SDK, no libc beyond string.h/stdint.h.
 * It is a pure function of a ph_ui struct onto a BGR888 canvas. That is what lets
 * tools/phantom_preview.c build it with the host compiler and dump real frames to
 * PPM, so the layout can be looked at without a DE10-Nano in the loop - the same
 * reason Groovy keeps its NLC codec testable off-target.
 *
 * Everything that talks to hardware or the network lives in phantom.cpp.
 */
#ifndef PHANTOM_UI_H
#define PHANTOM_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The launcher's canvas is the core's 480i mode: 720x480 progressive framebuffer
 * scanned out interlaced (CMD_SWITCHRES interlace=2). See phantom.cpp for the
 * modeline and why it is that one. */
#define PH_CANVAS_W 720
#define PH_CANVAS_H 480

#define PH_MAX_GAMES 512
#define PH_ID_LEN    64
#define PH_TITLE_LEN 96
#define PH_SYS_LEN   32

/* One entry of the host's library. Everything here comes from the host daemon's
 * catalog; nothing is invented locally. A field the host did not send stays empty
 * and the renderer omits that row rather than substituting a plausible-looking
 * default - a launcher that displays timings it guessed is worse than one that
 * shows none. */
typedef struct
{
	char     id[PH_ID_LEN];       /* launch key, sent back verbatim as LAUNCH:<id> */
	char     title[PH_TITLE_LEN]; /* display name */
	char     system[PH_SYS_LEN];  /* emulator key: groovymame/flycast/pcsx2/dolphin/model2 */
	char     sysname[PH_SYS_LEN]; /* hardware name for display: "Capcom CPS-3" */
	char     mode[PH_SYS_LEN];    /* "240p @ 59.6Hz", host-supplied, may be empty */
	uint32_t hsync_hz;            /* 15734, 0 = unknown */
	uint32_t vsync_chz;           /* centi-Hz: 5994 = 59.94Hz, 0 = unknown */
} ph_game;

/* Header link indicator. SEARCHING is the pre-discovery state and must not be shown
 * as an error: a cold boot legitimately sits there for a second or two. */
typedef enum
{
	PH_HOST_SEARCHING = 0,
	PH_HOST_ONLINE,
	PH_HOST_OFFLINE
} ph_host_state;

typedef enum
{
	PH_VIEW_LIST = 0,  /* browsing */
	PH_VIEW_BUSY       /* a LAUNCH has gone out; waiting for the host's first frame */
} ph_view;

/* Category tabs. These filter on ph_game.system, so they are a property of the
 * launcher, not of the host's catalog: a host that serves only MAME still shows all
 * the tabs, with the empty ones reporting zero titles rather than vanishing (a tab
 * strip that changes width as the library loads is disorienting on a CRT). */
#define PH_TAB_ALL   0
#define PH_TAB_MAME  1
#define PH_TAB_NAOMI 2
#define PH_TAB_PS2   3
#define PH_TAB_CUBE  4
#define PH_TAB_COUNT 5

typedef struct
{
	/* canvas */
	uint8_t *fb;      /* BGR888, PH_CANVAS_W * PH_CANVAS_H * 3 bytes */
	int      w, h;

	/* catalog */
	ph_game games[PH_MAX_GAMES];
	int     count;

	/* navigation. sel indexes the *filtered* list, not games[]. */
	int tab;
	int sel;
	int scroll;

	/* host link, for the header */
	ph_host_state host_state;
	char          host_ip[64];
	int           host_port;

	/* transient view state */
	ph_view view;
	char    busy_title[PH_TITLE_LEN];
	char    status[96];

	/* animation. tick_ms is monotonic milliseconds, supplied by the caller so this
	 * module needs no clock of its own (and the preview harness can fake it). */
	uint32_t tick_ms;

	/* marquee scroll for the selected row's over-long title */
	uint32_t marquee_t0;
	int      marquee_px;

	int dirty; /* set by any state change; phantom.cpp clears it after presenting */
} ph_ui;

void ph_ui_init(ph_ui *u, uint8_t *fb, int w, int h);

/* Draw the whole canvas. Cheap enough to do unconditionally (720x480 fills in well
 * under a millisecond on the DE10-Nano's A9) so there is no partial-redraw path to
 * get wrong. */
void ph_ui_render(ph_ui *u);

/* Navigation. Each returns 1 when something changed and a redraw is owed. */
int ph_ui_move(ph_ui *u, int dy);
int ph_ui_tab(ph_ui *u, int dx);

/* Advance animation only. Returns 1 if the frame would differ from the last one. */
int ph_ui_animate(ph_ui *u, uint32_t tick_ms);

/* Fill out[] with indices into games[] that pass the current tab's filter.
 * Returns how many were written. */
int ph_ui_filtered(const ph_ui *u, int *out, int max);

/* The highlighted game, or NULL when the filtered list is empty. */
const ph_game *ph_ui_selected(const ph_ui *u);

/* Keep sel/scroll in range after the catalog has been replaced under us, trying to
 * stay on the same game id where it still exists. */
void ph_ui_reclamp(ph_ui *u, const char *keep_id);

#ifdef __cplusplus
}
#endif

#endif
