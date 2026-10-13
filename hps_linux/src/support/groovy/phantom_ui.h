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

/* The resident list: what the older catalog path fills, and what the tests and the preview tool
 * use. A library bigger than this is not held here at all - see ph_source. */
#define PH_MAX_GAMES 512
#define PH_ID_LEN    64
#define PH_TITLE_LEN 128
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
	char     mode[PH_SYS_LEN];    /* "320x240 @ 59.64Hz TATE", host-supplied, may be empty */
	char     year[8];             /* "1996", from MAME; empty when the host did not send it */
	char     maker[48];           /* "Raizing / Eighting" */
	uint32_t hsync_hz;            /* 15734, 0 = unknown */
	uint32_t vsync_chz;           /* centi-Hz: 5994 = 59.94Hz, 0 = unknown */
} ph_game;

/* Where the list's rows come from, when they are not all held here.
 *
 * A library of hundreds of thousands of titles cannot live in an array: the host keeps it, sorted, and the
 * cabinet fetches the page it is looking at (phantom_cat.c). The renderer only ever asks for the rows it is
 * about to draw, by position in the current tab's list, so it does not care which kind of source it has.
 *
 * With none set, rows come from games[]/count below, filtered by tab - which is how it has always worked. */
typedef struct ph_source
{
	/* Titles in the tab. Negative: nothing known yet. */
	int            (*count)(void *ctx, int tab);
	/* Row `index` of the tab, or NULL when it has not arrived yet (it is drawn as a placeholder). */
	const ph_game *(*row)(void *ctx, int tab, int index);
	/* Index of the first title filed under letter k of the tab (0 = anything not a letter, 1..26 = A..Z), k = 27
	 * giving the count. Negative: not known yet. */
	int            (*letter_start)(void *ctx, int tab, int k);
	void           *ctx;
} ph_source;

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
	PH_VIEW_BUSY,      /* a LAUNCH has gone out; waiting for the host's first frame */
	PH_VIEW_ADJUST     /* the screen-adjust page: set how much of each edge the display hides */
} ph_view;

/* How many pixels of each edge of the 720x480 canvas the display may hide. A TV's overscan
 * is not symmetric - one that loses 20 lines at the top can lose 45 at the bottom - so each
 * edge is its own number. Everything the launcher draws that has to be read lives inside the
 * rectangle these leave; only backgrounds run out to the edge. A CRT that shows the whole
 * raster wants all four at zero. Values are even: an odd margin would put an edge on one field
 * of the interlaced scan only. */
typedef struct
{
	int l, t, r, b;
} ph_safe;

/* Broadcast "title safe" is 90% of the picture: 5% either side, 36px across and 24px down. */
#define PH_SAFE_DEF_L 36
#define PH_SAFE_DEF_T 24
#define PH_SAFE_DEF_R 36
#define PH_SAFE_DEF_B 24
#define PH_SAFE_MAX_X 120
#define PH_SAFE_MAX_Y 100

/* Category tabs. These filter on ph_game.system, so they are a property of the
 * launcher, not of the host's catalog: a host that serves only MAME still shows all
 * the tabs, with the empty ones reporting zero titles rather than vanishing (a tab
 * strip that changes width as the library loads is disorienting on a CRT). */
/* One tab per emulator the host can drive. These mirror the table in the PC manager
 * (host/PhantomArcadeManager.cpp g_emus), which is the set of GroovyNLC-capable
 * emulators plus GroovyMAME. */
#define PH_TAB_ALL    0
#define PH_TAB_MAME   1
#define PH_TAB_FBNEO  2
#define PH_TAB_DC     3
#define PH_TAB_PS2    4
#define PH_TAB_PS3    5
#define PH_TAB_XBOX   6
#define PH_TAB_GCWII  7
#define PH_TAB_PC     8
#define PH_TAB_RA     9
#define PH_TAB_COUNT  10

typedef struct
{
	/* canvas */
	uint8_t *fb;      /* BGR888, PH_CANVAS_W * PH_CANVAS_H * 3 bytes */
	int      w, h;

	/* catalog */
	ph_game games[PH_MAX_GAMES];
	int     count;
	ph_source src;        /* when src.count is set, rows come from here and games[]/count are not used */
	int     lib_loading;  /* the host is online but has not sent its library's size yet */
	int     lib_scanning; /* the host is still looking for games; what is listed may not be everything */

	/* navigation. sel indexes the *filtered* list, not games[]. */
	int tab;
	int sel;
	int scroll;

	/* display overscan; see ph_safe */
	ph_safe safe;
	ph_safe safe_saved;   /* what to go back to if the adjust page is cancelled */
	int     adj_edge;     /* 0 top, 1 bottom, 2 left, 3 right: the edge the adjust page is moving */

	/* host link, for the header */
	ph_host_state host_state;
	char          host_ip[64];
	int           host_port;

	/* transient view state */
	ph_view  view;
	char     busy_title[PH_TITLE_LEN];
	uint32_t busy_t0;    /* tick_ms when the launch went out, for the elapsed counter */
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

/* Move by a screenful. Clamps at the ends rather than wrapping: a page jump that wraps
 * lands somewhere unrelated, where a single step wrapping is easy to follow. */
int ph_ui_page(ph_ui *u, int dir);

/* Move by `rows` titles (negative: back), clamped at both ends. This is how a held Left/Right crosses a
 * library that is far too long to page through a screen at a time. */
int ph_ui_seek(ph_ui *u, int rows);

/* Jump to the first title of the next (dir > 0) or the current/previous (dir < 0) letter.
 * Titles that do not start with a letter are one group, ahead of "A". Does nothing until the letter table
 * is known. */
int ph_ui_letter(ph_ui *u, int dir);

/* Titles in tab `tab`, from whichever source the list has. */
int ph_ui_tab_count(const ph_ui *u, int tab);

/* Does a catalog entry of this system belong on this tab? (The tab strip is the launcher's; a host just
 * names its systems.) */
int ph_tab_matches(int tab, const char *sys);

/* Rows the list shows at once with the current margins. */
int ph_ui_rows(const ph_ui *u);

/* Safe area. set clamps to the allowed range and rounds each value down to even. */
void ph_ui_set_safe(ph_ui *u, int l, int t, int r, int b);
void ph_ui_adjust_begin(ph_ui *u);
int  ph_ui_adjust_edge(ph_ui *u, int dir);          /* choose the edge to move */
int  ph_ui_adjust_move(ph_ui *u, int delta);        /* move it: + is further in from the edge */
void ph_ui_adjust_reset(ph_ui *u);                  /* back to the defaults */
void ph_ui_adjust_end(ph_ui *u, int keep);          /* keep=0 restores what it was on entry */

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
