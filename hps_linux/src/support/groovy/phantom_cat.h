/*
 * phantom_cat.h - the library as the cabinet sees it: a window onto the host's list, not a copy of it.
 *
 * The launcher used to receive the whole catalog in one datagram and keep every game in an array. That
 * stops working somewhere around a few hundred titles (one datagram, one fixed array) and could never work
 * for a library of hundreds of thousands. So the host keeps the library, sorted, one list per system plus one
 * for everything, and this fetches only what the screen is showing:
 *
 *   CATINFO              the library's version, size, and each system's name and count
 *   LETTERS:<ver>:<list> 28 numbers: where each letter begins, so jumping to "S" is a lookup
 *   LIST:<ver>:<list>:<offset>:<count>
 *                        a page of rows
 *
 * Pages are fixed-size and aligned, and a small least-recently-used cache holds the ones around the cursor,
 * plus a few ahead in the direction of travel so that holding the stick down does not outrun the network.
 * A row that has not arrived yet reads as NULL and the renderer draws a placeholder - which, on a LAN, is
 * for a few milliseconds at most.
 *
 * Every request quotes the library's version. If the host has rescanned in the meantime it answers STALE,
 * this forgets what it holds, and the next CATINFO starts it again from the new list; the cursor stays where
 * it was, clamped to the new length.
 *
 * Pure C with no sockets: phantom.cpp sends what pc_next_request produces and hands every reply datagram to
 * pc_on_reply. That is what lets the test build this against the daemon's own reply code (host/game_index.h).
 */
#ifndef PHANTOM_CAT_H
#define PHANTOM_CAT_H

#include <stdint.h>

#include "phantom_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_PAGE      32     /* rows per page; the host's replies are built to this */
#define PC_PAGES     24     /* pages held: room for the screen, the read-ahead, and a few recent tabs */
#define PC_MAX_SYS   16
#define PC_PENDING    4     /* requests in flight at once */
#define PC_RETRY_MS 400     /* a request unanswered this long is sent again */
#define PC_AHEAD      2     /* pages read ahead in the direction of travel */

typedef struct
{
	char key[PH_SYS_LEN];
	char name[PH_SYS_LEN];
	int  count;
} pc_sys;

typedef struct
{
	int      valid;
	int      list;
	int      page;
	int      n;          /* rows filled, from the start of the page */
	uint32_t used;       /* least-recently-used stamp */
	ph_game  rows[PC_PAGE];
} pc_page;

typedef struct
{
	int      active;
	int      list;
	int      offset;
	uint32_t sent;
} pc_pend;

typedef struct pc_cat
{
	int      have;       /* CATINFO received for `ver` */
	uint32_t ver;
	int      total;
	int      scanning;   /* the host is still looking for games */
	int      nsys;
	pc_sys   sys[PC_MAX_SYS];

	/* index 0 = every game, 1 + s = system s */
	int      letters_have[PC_MAX_SYS + 1];
	int      letters[PC_MAX_SYS + 1][28];
	int      letters_asked[PC_MAX_SYS + 1];
	uint32_t letters_sent[PC_MAX_SYS + 1];

	pc_page  pages[PC_PAGES];
	pc_pend  pend[PC_PENDING];
	uint32_t tick;

	int      want_info;  /* the host said STALE: ask for CATINFO again now */
} pc_cat;

/* What a reply turned out to be. */
enum
{
	PC_NONE = 0,        /* not ours, or nothing to act on */
	PC_INFO,            /* CATINFO, same version as before (the scanning flag may have changed) */
	PC_INFO_NEW,        /* CATINFO, a different library: everything held was dropped */
	PC_PAGE_IN,         /* rows arrived */
	PC_LETTERS_IN,      /* a letter table arrived */
	PC_STALE            /* the host's library is not the one this was asking about */
};

void pc_init(pc_cat *c);

/* Forget the library entirely (the host went away, or a different one answered). */
void pc_reset(pc_cat *c);

/* Handle one reply datagram, NUL-terminated and modified in place. Returns a PC_* event. */
int pc_on_reply(pc_cat *c, char *buf, int len);

/* The list a tab shows: 0 for every game, 1 + s for system s, -1 when the host has no system for the tab. */
int         pc_list_for_tab(const pc_cat *c, int tab);
const char *pc_list_key(const pc_cat *c, int list);   /* "*" or the system key */
int         pc_list_count(const pc_cat *c, int list);

/* The next request worth sending, written to out. `first`..`last` are the rows on screen, `dir` the way the
 * cursor has been moving (-1, 0, 1). Returns 1 with a request to send, or 0 when nothing more is needed
 * right now; call it until it returns 0. */
int pc_next_request(pc_cat *c, uint32_t now_ms, int list, int first, int last, int dir, char *out, int outsz);

/* The renderer's view of this library. */
ph_source pc_source(pc_cat *c);

/* The host's order for titles - letters after everything else, case-insensitive, with the letters' groups
 * contiguous - for the older catalog path, which sorts on this side. Must agree with host/game_index.h. */
int pc_title_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif
