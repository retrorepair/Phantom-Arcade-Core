/*
 * phantom_art.h - the picture behind the menu, fetched from the host a game at a time.
 *
 * The host (host/art_http.h) keeps a picture - a title screen or a gameplay screenshot - for as many games as it
 * has been able to find one for, and hands it over HTTP as 720*480*3 bytes of B,G,R, ready to draw. This is the
 * cabinet's half: it is told which row of the list the cursor is on, waits for the cursor to settle, asks, and
 * hands back the picture when it arrives.
 *
 * Like the rest of the launcher it runs on the one HPS thread that also services the video stream, so nothing here
 * may wait: the connection is non-blocking and every call does what can be done without waiting and returns. A
 * megabyte arrives over many calls.
 *
 * What a row is: the same three things a launch quotes - the library version, the list, the position - because the
 * cabinet holds a window of the list, not every id. The caller builds the query ("v=..&l=..&i=..&k=title") and a key
 * that names the same row, and this module never looks inside either.
 *
 * What it says back (pa_status):
 *   PA_NOTHING  nothing wanted, or nothing yet
 *   PA_READY    the picture for the wanted row is in pa_picture()
 *   PA_NONE     the host has no picture for it and is not looking for one
 *   PA_PENDING  the host is fetching it; this module is asking again
 * A picture that arrived for an earlier row stays in pa_picture() until a later one replaces it, so the menu does
 * not flash back to its plain background between two rows that both have pictures.
 */
#ifndef PHANTOM_ART_H
#define PHANTOM_ART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PA_W      720
#define PA_H      480
#define PA_BYTES  (PA_W * PA_H * 3)

/* How long the cursor must stay on a row before it is asked for, so a held button crossing a library asks for nothing. */
#define PA_DEBOUNCE_MS 250

typedef enum
{
	PA_NOTHING = 0,
	PA_READY,
	PA_NONE,
	PA_PENDING
} pa_status;

typedef struct pa_art
{
	/* the host */
	char     host[64];
	int      port;

	/* the row that is wanted */
	int      want;
	char     want_key[96];
	char     want_query[160];
	uint32_t want_since;

	/* the transfer */
	int      state;
	int      fd;
	uint32_t state_t0;
	uint32_t retry_at;
	int      tries;         /* times the host has said "fetching" for this row */
	int      fails;         /* times the network has let this row down */
	char     req[256];
	int      req_len, req_sent;
	char     head[1024];
	int      head_len;
	int      http_status;
	long     content_len;
	long     body_got;
	uint8_t *body;          /* the picture as it arrives */

	/* the answer */
	pa_status status;
	uint8_t *pic;           /* the last picture that arrived */
	char     pic_key[96];   /* the row it was for */
	int      stale;         /* the host said the library changed: the caller should refetch its pages */
	uint32_t pics;          /* how many pictures have arrived, for the caller to notice a new one */
} pa_art;

void pa_init(pa_art *a);
void pa_free(pa_art *a);

/* Where the host's HTTP server is. A change drops whatever was in flight and the wanted row. */
void pa_set_host(pa_art *a, const char *ip, int port);

/* The row under the cursor, or nothing (key NULL: art is off, or the list is not on screen). Cheap to call every pass.
 * A new key restarts the wait; the same key changes nothing. */
void pa_want(pa_art *a, const char *key, const char *query, uint32_t now);

/* Do what can be done without waiting. Returns 1 when pa_status() or pa_picture() changed. */
int pa_tick(pa_art *a, uint32_t now);

pa_status      pa_get_status(const pa_art *a);
const uint8_t *pa_picture(const pa_art *a);      /* PA_BYTES of B,G,R, or NULL before the first picture */
const char    *pa_picture_key(const pa_art *a);  /* the key the picture in pa_picture() was for */

/* Set after the host answered 409 (the library changed); clears it. */
int pa_take_stale(pa_art *a);

#ifdef __cplusplus
}
#endif

#endif
