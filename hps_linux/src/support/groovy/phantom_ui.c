/*
 * phantom_ui.c - Phantom Arcade launcher rendering and navigation.
 *
 * Draws onto the core's own 480i framebuffer (720x480 BGR888). See phantom_ui.h for
 * why this file has no hardware or network dependencies.
 *
 * Two constraints shape every number below, both of them properties of the display
 * and neither negotiable:
 *
 *   480i interlace. The framebuffer is progressive but the core scans it out as two
 *   fields. Anything one pixel tall lands in exactly one field and therefore blinks
 *   at 30Hz. So: text is never drawn below scale 2 (a 2px stroke lands in both
 *   fields), rules are 2px or 4px, and nothing relies on an odd-height feature.
 *   This is the single biggest difference from Phantom Arcade's framebuffer frontend,
 *   which drew 1px-stroke scale-1 text because /dev/fb0 on the menu core is 480p.
 *
 *   Arcade CRT overscan. Real cabinet monitors lose the edges, so content keeps a
 *   PH_SAFE_X / PH_SAFE_Y margin and nothing load-bearing touches it. The core's own
 *   OSD "CRT H/V offset" options remain the fine adjustment.
 *
 * Colours are written as 0xRRGGBB and swapped to the framebuffer's B,G,R byte order
 * by put_px(), so the constants here read the same way they do in Phantom Arcade's
 * original palette. Pure white is avoided deliberately: tools/gen_logo.py already
 * found that Y=100% strobes on these monitors and settled on a warm cream instead.
 */

#include <string.h>
#include <stdio.h>

#include "phantom_ui.h"
#include "phantom_font.h"

/* ---- palette (0xRRGGBB) ------------------------------------------------------ */

#define C_BG          0x0B0C12
#define C_HEAD_A      0x1F172C
#define C_HEAD_B      0x121422
#define C_TABBAR      0x11121B
#define C_CARD        0x1B1D2A
#define C_BORDER      0x2E3246
#define C_BORDER_HI   0x474E6B
#define C_ROW_A       0x1E202E
#define C_ROW_B       0x171924
#define C_ROW_SEL     0x3A2E12
#define C_AMBER       0xF0A020
#define C_AMBER_HI    0xFBCF6A
#define C_AMBER_DK    0x78350F
#define C_CYAN        0x2CC0D8
#define C_GREEN       0x18C070
#define C_RED         0xE04444
#define C_YELLOW      0xE0B020
#define C_TEXT        0xDCE2EC
#define C_TEXT_DIM    0x9AA6BC
#define C_TEXT_MUTE   0x6B7690
#define C_INACTIVE    0x252838
#define C_PANEL_DK    0x141620

/* ---- layout ------------------------------------------------------------------ */

#define PH_SAFE_X   16
#define PH_SAFE_Y   8

/* Sized for 720x480. The list gets everything left over: 15 rows of 90 columns, where
 * the first version managed 11 rows of 45 on the same screen. */
#define HEAD_Y      0
#define HEAD_H      44
#define TAB_Y       48
#define TAB_H       28
#define LIST_Y      80
#define LIST_H      338
#define LIST_HDR_H  24
#define ROW_H       20
#define DETAIL_Y    422
#define DETAIL_H    24
#define FOOT_Y      450
#define FOOT_H      30

#define GLYPH_W 8
#define GLYPH_H 12

/* The font declares 12 rows but inks only rows 2..9 - rows 0, 1, 10 and 11 are blank in
 * all 95 glyphs (',QV_gjpqy' are the ones that reach row 9). Drawing the declared cell
 * spent a third of every row's height on nothing, which is what actually limited the
 * list to eleven entries on a 480-line screen. Only the inked band is drawn, and the
 * row pitch is set from that. */
#define FONT_TOP 2
#define FONT_BOT 9
#define FONT_INK (FONT_BOT - FONT_TOP + 1)

/* marquee timing: hold, scroll, hold, snap back */
#define MQ_HOLD_MS   1800
#define MQ_STEP_MS   40

/* ---- primitives -------------------------------------------------------------- */

static inline void put_px(ph_ui *u, int x, int y, uint32_t rgb)
{
	if (x < 0 || y < 0 || x >= u->w || y >= u->h) return;
	uint8_t *p = u->fb + ((size_t)y * u->w + x) * 3;
	p[0] = (uint8_t)(rgb & 0xff);         /* B */
	p[1] = (uint8_t)((rgb >> 8) & 0xff);  /* G */
	p[2] = (uint8_t)((rgb >> 16) & 0xff); /* R */
}

static void rect(ph_ui *u, int x, int y, int w, int h, uint32_t rgb)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > u->w) w = u->w - x;
	if (y + h > u->h) h = u->h - y;
	if (w <= 0 || h <= 0) return;

	/* Build one row then replicate it; the inner loop is a byte copy rather than a
	 * per-pixel channel split, which matters because this fills ~1MB a frame. */
	uint8_t *first = u->fb + ((size_t)y * u->w + x) * 3;
	for (int i = 0; i < w; i++)
	{
		first[i * 3 + 0] = (uint8_t)(rgb & 0xff);
		first[i * 3 + 1] = (uint8_t)((rgb >> 8) & 0xff);
		first[i * 3 + 2] = (uint8_t)((rgb >> 16) & 0xff);
	}
	for (int j = 1; j < h; j++)
	{
		memcpy(u->fb + ((size_t)(y + j) * u->w + x) * 3, first, (size_t)w * 3);
	}
}

static void vgrad(ph_ui *u, int x, int y, int w, int h, uint32_t c1, uint32_t c2)
{
	if (h <= 0) return;
	int r1 = (c1 >> 16) & 0xff, g1 = (c1 >> 8) & 0xff, b1 = c1 & 0xff;
	int r2 = (c2 >> 16) & 0xff, g2 = (c2 >> 8) & 0xff, b2 = c2 & 0xff;
	for (int j = 0; j < h; j++)
	{
		int r = r1 + (r2 - r1) * j / h;
		int g = g1 + (g2 - g1) * j / h;
		int b = b1 + (b2 - b1) * j / h;
		rect(u, x, y + j, w, 1, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
	}
}

/* A 2px border, not 1px: a single-pixel box edge is a one-field feature and crawls. */
static void box(ph_ui *u, int x, int y, int w, int h, uint32_t bg, uint32_t border, uint32_t hi)
{
	rect(u, x, y, w, h, bg);
	rect(u, x, y, w, 2, border);
	rect(u, x, y + h - 2, w, 2, border);
	rect(u, x, y, 2, h, border);
	rect(u, x + w - 2, y, 2, h, border);
	if (hi)
	{
		rect(u, x + 2, y + 2, w - 4, 2, hi);
	}
}

/* Horizontal and vertical scale are separate on purpose, and this is the whole reason
 * the launcher gets any value out of 480i.
 *
 * Interlace constrains the vertical axis only: a 1px-tall stroke lands in one field and
 * blinks at 30Hz, so vertical scale is never below 2. Nothing of the sort applies
 * horizontally - the beam resolves every pixel of a line in both fields alike. Drawing
 * body text at 2x in both directions therefore threw away half the horizontal
 * resolution for no benefit at all, and left game titles truncated at 45 characters on
 * a screen that can carry 90.
 *
 * So the chrome (wordmark, tabs, button legends) stays square-scaled and chunky, while
 * the data - game titles, system names, the detail line - is drawn narrow: 1x across,
 * 2x down. Full horizontal detail, still two scanlines per stroke.
 */
static void glyph_s(ph_ui *u, int x, int y, char c, uint32_t rgb, int sx, int sy)
{
	int idx = (unsigned char)c - 32;
	if (idx < 0 || idx >= 95) idx = 31; /* '?' */
	const uint8_t *g = font8x12[idx];

	for (int r = FONT_TOP; r <= FONT_BOT; r++)
	{
		uint8_t row = g[r];
		if (!row) continue;
		for (int b = 0; b < GLYPH_W; b++)
		{
			if (!((row >> (7 - b)) & 1)) continue;
			rect(u, x + b * sx, y + (r - FONT_TOP) * sy, sx, sy, rgb);
		}
	}
}

static void glyph(ph_ui *u, int x, int y, char c, uint32_t rgb, int s)
{
	glyph_s(u, x, y, c, rgb, s, s);
}

/* Narrow text: 1x across, 2x down. 8 px wide, 16 px tall, two scanlines per stroke. */
#define PH_NARROW_W GLYPH_W
#define PH_NARROW_H (FONT_INK * 2)

static void text_n(ph_ui *u, int x, int y, const char *s, uint32_t rgb)
{
	if (!s) return;
	for (; *s; s++)
	{
		glyph_s(u, x, y, *s, rgb, 1, 2);
		x += PH_NARROW_W;
	}
}

static void text_n_right(ph_ui *u, int xr, int y, const char *s, uint32_t rgb)
{
	if (!s) return;
	text_n(u, xr - (int)strlen(s) * PH_NARROW_W, y, s, rgb);
}

static void text_n_clip(ph_ui *u, int x, int y, const char *s, uint32_t rgb, int maxw)
{
	int maxc = maxw / PH_NARROW_W;
	if (maxc <= 0 || !s) return;

	int len = (int)strlen(s);
	if (len <= maxc) { text_n(u, x, y, s, rgb); return; }

	char buf[PH_TITLE_LEN + 8];
	int keep = maxc - 3;
	if (keep < 1) keep = 1;
	if (keep > (int)sizeof(buf) - 4) keep = (int)sizeof(buf) - 4;
	memcpy(buf, s, (size_t)keep);
	buf[keep] = 0;
	strcat(buf, "...");
	text_n(u, x, y, buf, rgb);
}

static void text_n_scroll(ph_ui *u, int x, int y, const char *s, uint32_t rgb, uint32_t sh,
                          int winw, int off)
{
	int i = 0;
	for (const char *p = s; *p; p++, i++)
	{
		int gx = x - off + i * PH_NARROW_W;
		if (gx + PH_NARROW_W <= x) continue;
		if (gx >= x + winw) break;
		if (gx >= x && gx + PH_NARROW_W <= x + winw)
		{
			if (sh) glyph_s(u, gx + 1, y + 2, *p, sh, 1, 2);
			glyph_s(u, gx, y, *p, rgb, 1, 2);
		}
	}
}

static void text(ph_ui *u, int x, int y, const char *s, uint32_t rgb, int scale)
{
	if (!s) return;
	for (; *s; s++)
	{
		glyph(u, x, y, *s, rgb, scale);
		x += GLYPH_W * scale;
	}
}

/* Drop-shadowed text. The shadow is what keeps amber legible against the amber-tinted
 * selection bar; without it the selected row's title loses its edges on a CRT. */
static void text_sh(ph_ui *u, int x, int y, const char *s, uint32_t rgb, uint32_t sh, int scale)
{
	if (!s) return;
	if (sh) text(u, x + scale, y + scale, s, sh, scale);
	text(u, x, y, s, rgb, scale);
}

/* Arrow glyphs. The font is ASCII 32..126 only, so the stick and cursor indicators
 * are drawn rather than typed: a '^'/'v' pair reads as punctuation at this size, and
 * reaching below 32 in font8x12 would just print '?'. Each triangle is built from
 * 2px-tall bands for the same both-fields reason as everything else here. */
typedef enum { PH_ARROW_UP, PH_ARROW_DOWN, PH_ARROW_LEFT, PH_ARROW_RIGHT } ph_arrow;

static void arrow(ph_ui *u, int x, int y, int size, ph_arrow dir, uint32_t rgb)
{
	int half = size / 2;
	for (int i = 0; i < half; i++)
	{
		int span = (i + 1) * 2;      /* widens by 2 per 2px band */
		int band = 2;
		switch (dir)
		{
		case PH_ARROW_UP:
			rect(u, x + half - i - 1, y + i * band, span, band, rgb);
			break;
		case PH_ARROW_DOWN:
			rect(u, x + half - i - 1, y + size - (i + 1) * band, span, band, rgb);
			break;
		case PH_ARROW_LEFT:
			rect(u, x + i * band, y + half - i - 1, band, span, rgb);
			break;
		case PH_ARROW_RIGHT:
			rect(u, x + size - (i + 1) * band, y + half - i - 1, band, span, rgb);
			break;
		}
	}
}

/* ---- catalog filtering ------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
		if (ca != cb) return 0;
	}
	return *a == 0 && *b == 0;
}

static int tab_match(int tab, const char *sys)
{
	switch (tab)
	{
	case PH_TAB_ALL:
		return 1;
	case PH_TAB_MAME:
		return ieq(sys, "groovymame") || ieq(sys, "mame") || ieq(sys, "arcade");
	case PH_TAB_NAOMI:
		return ieq(sys, "flycast") || ieq(sys, "naomi") || ieq(sys, "dreamcast");
	case PH_TAB_PS2:
		return ieq(sys, "pcsx2") || ieq(sys, "ps2");
	case PH_TAB_CUBE:
		return ieq(sys, "dolphin") || ieq(sys, "gamecube") || ieq(sys, "gc") || ieq(sys, "wii");
	default:
		return 0;
	}
}

int ph_ui_filtered(const ph_ui *u, int *out, int max)
{
	int n = 0;
	for (int i = 0; i < u->count && n < max; i++)
	{
		if (tab_match(u->tab, u->games[i].system)) out[n++] = i;
	}
	return n;
}

const ph_game *ph_ui_selected(const ph_ui *u)
{
	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);
	if (n <= 0 || u->sel < 0 || u->sel >= n) return 0;
	return &u->games[idx[u->sel]];
}

static int visible_rows(void)
{
	return (LIST_H - LIST_HDR_H - 4) / ROW_H;
}

/* Pixel budget the selected row's title gets. Shared by the renderer and the marquee
 * so the two cannot disagree about when a title overflows - if they do, a title
 * scrolls that does not need to, or sits clipped and still. */
static int title_window(const ph_ui *u, const ph_game *g, int n, int *out_sys_x, int *out_sys_w)
{
	int w = u->w - 2 * PH_SAFE_X;
	int bar_w = (n > visible_rows()) ? 14 : 0;

	/* The system name is capped before the title is measured, so one absurdly long
	 * hardware string cannot squeeze every title down to an ellipsis. */
	const char *sys = g->sysname[0] ? g->sysname : g->system;
	int sys_cap = (w - 24 - bar_w) * 2 / 5;
	int sys_w = (int)strlen(sys) * PH_NARROW_W;
	if (sys_w > sys_cap) sys_w = sys_cap;

	int tx = PH_SAFE_X + 30;
	int sys_x = PH_SAFE_X + w - 12 - bar_w - sys_w;
	int tw = sys_x - tx - 16;
	if (tw < 32) tw = 32;

	if (out_sys_x) *out_sys_x = sys_x;
	if (out_sys_w) *out_sys_w = sys_w;
	return tw;
}

static void fix_scroll(ph_ui *u, int n)
{
	int rows = visible_rows();
	if (u->sel < 0) u->sel = 0;
	if (u->sel >= n) u->sel = n ? n - 1 : 0;
	if (u->sel < u->scroll) u->scroll = u->sel;
	if (u->sel >= u->scroll + rows) u->scroll = u->sel - rows + 1;
	if (u->scroll > n - rows) u->scroll = n - rows;
	if (u->scroll < 0) u->scroll = 0;
}

/* ---- public state transitions ------------------------------------------------ */

void ph_ui_init(ph_ui *u, uint8_t *fb, int w, int h)
{
	memset(u, 0, sizeof(*u));
	u->fb = fb;
	u->w = w;
	u->h = h;
	u->host_port = 0;
	u->host_state = PH_HOST_SEARCHING;
	u->view = PH_VIEW_LIST;
	u->dirty = 1;
}

int ph_ui_move(ph_ui *u, int dy)
{
	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);
	if (n <= 0) return 0;

	int before = u->sel;
	u->sel += dy;
	/* Wrap. On a cabinet with no keyboard, wrapping is the difference between
	 * reaching the last title and holding the stick for six seconds. */
	if (u->sel < 0) u->sel = n - 1;
	if (u->sel >= n) u->sel = 0;
	fix_scroll(u, n);

	if (u->sel != before)
	{
		u->marquee_t0 = u->tick_ms;
		u->marquee_px = 0;
		u->dirty = 1;
		return 1;
	}
	return 0;
}

int ph_ui_tab(ph_ui *u, int dx)
{
	int before = u->tab;
	u->tab += dx;
	if (u->tab < 0) u->tab = PH_TAB_COUNT - 1;
	if (u->tab >= PH_TAB_COUNT) u->tab = 0;
	if (u->tab == before) return 0;

	u->sel = 0;
	u->scroll = 0;
	u->marquee_t0 = u->tick_ms;
	u->marquee_px = 0;
	u->dirty = 1;
	return 1;
}

void ph_ui_reclamp(ph_ui *u, const char *keep_id)
{
	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);

	if (keep_id && *keep_id)
	{
		for (int i = 0; i < n; i++)
		{
			if (strcmp(u->games[idx[i]].id, keep_id) == 0)
			{
				u->sel = i;
				fix_scroll(u, n);
				u->dirty = 1;
				return;
			}
		}
	}
	fix_scroll(u, n);
	u->dirty = 1;
}

int ph_ui_animate(ph_ui *u, uint32_t tick_ms)
{
	u->tick_ms = tick_ms;

	/* Only the marquee and the busy spinner animate, so only they can dirty a frame.
	 * Everything else is event-driven, which keeps the idle launcher from rewriting
	 * a megabyte of DDR sixty times a second for nothing. */
	if (u->view == PH_VIEW_BUSY) return 1;

	const ph_game *g = ph_ui_selected(u);
	if (!g) return 0;

	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);
	int winw = title_window(u, g, n, 0, 0);
	int full = (int)strlen(g->title) * PH_NARROW_W;
	if (full <= winw)
	{
		if (u->marquee_px)
		{
			u->marquee_px = 0;
			return 1;
		}
		return 0;
	}

	uint32_t over = full - winw;
	uint32_t dt = tick_ms - u->marquee_t0;
	uint32_t travel = over * MQ_STEP_MS;
	uint32_t cycle = MQ_HOLD_MS + travel + MQ_HOLD_MS;
	uint32_t ph = cycle ? (dt % cycle) : 0;

	int want;
	if (ph < MQ_HOLD_MS)                       want = 0;
	else if (ph < MQ_HOLD_MS + travel)         want = (int)((ph - MQ_HOLD_MS) / MQ_STEP_MS);
	else                                       want = (int)over;
	if (want > (int)over) want = (int)over;

	if (want != u->marquee_px)
	{
		u->marquee_px = want;
		return 1;
	}
	return 0;
}

/* ---- rendering --------------------------------------------------------------- */

static int ntw(const char *s)
{
	return s ? (int)strlen(s) * PH_NARROW_W : 0;
}

static void draw_header(ph_ui *u)
{
	vgrad(u, 0, HEAD_Y, u->w, HEAD_H - 4, C_HEAD_A, C_HEAD_B);
	rect(u, 0, HEAD_Y + HEAD_H - 4, u->w, 4, C_AMBER);

	/* Wordmark stays square-scaled and chunky - it is the one thing meant to read from
	 * across a room, and it is short enough that the width costs nothing. */
	text_sh(u, PH_SAFE_X, HEAD_Y + 6, "PHANTOM ARCADE", C_AMBER, C_AMBER_DK, 3);

	const char *label;
	uint32_t led;
	switch (u->host_state)
	{
	case PH_HOST_ONLINE:  label = "HOST ONLINE";   led = C_GREEN;  break;
	case PH_HOST_OFFLINE: label = "NO HOST";       led = C_RED;    break;
	default:              label = "SEARCHING...";  led = C_YELLOW; break;
	}

	char addr[96];
	if (u->host_state == PH_HOST_ONLINE && u->host_ip[0])
		snprintf(addr, sizeof(addr), "%s:%d", u->host_ip, u->host_port);
	else
		addr[0] = 0;

	int xr = u->w - PH_SAFE_X;
	text_n_right(u, xr, HEAD_Y + 4, label, C_TEXT_DIM);
	if (addr[0]) text_n_right(u, xr, HEAD_Y + 22, addr, C_TEXT_MUTE);

	rect(u, xr - ntw(label) - 16, HEAD_Y + 8, 8, 8, led);
}

static void draw_tabs(ph_ui *u)
{
	static const char *tabs[PH_TAB_COUNT] = { "ALL", "GROOVYMAME", "NAOMI", "PS2", "GAMECUBE" };

	rect(u, 0, TAB_Y, u->w, TAB_H, C_TABBAR);

	int x = PH_SAFE_X;
	for (int t = 0; t < PH_TAB_COUNT; t++)
	{
		int tw = ntw(tabs[t]) + 16;
		if (t == u->tab)
		{
			box(u, x, TAB_Y + 1, tw, TAB_H - 4, C_AMBER, C_AMBER_HI, 0);
			text_n(u, x + 8, TAB_Y + 6, tabs[t], 0x1A1200);
		}
		else
		{
			box(u, x, TAB_Y + 1, tw, TAB_H - 4, C_INACTIVE, C_BORDER, 0);
			text_n(u, x + 8, TAB_Y + 6, tabs[t], C_TEXT_MUTE);
		}
		x += tw + 8;
	}

	/* Position within the filtered list, parked at the right end of the tab strip. */
	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);
	char cnt[32];
	snprintf(cnt, sizeof(cnt), "%d/%d", n ? u->sel + 1 : 0, n);
	if (u->w - PH_SAFE_X - ntw(cnt) > x + 8)
	{
		text_n_right(u, u->w - PH_SAFE_X, TAB_Y + 6, cnt, C_TEXT_MUTE);
	}

	rect(u, 0, TAB_Y + TAB_H, u->w, 2, C_BORDER);
}

/* The empty states. Each one names the reason and the fix; none of them pretends
 * there is a library when there is not. */
static void draw_empty(ph_ui *u, int x, int y, int w, int any_games)
{
	int cx = x + 20;
	int cy = y + 32;

	if (any_games)
	{
		text_n(u, cx, cy, "No titles in this category.", C_AMBER);
		text_n(u, cx, cy + 26, "Press LEFT / RIGHT to change platform.", C_TEXT_DIM);
		return;
	}

	switch (u->host_state)
	{
	case PH_HOST_SEARCHING:
		text_n(u, cx, cy, "Looking for a Phantom Arcade host...", C_AMBER);
		text_n(u, cx, cy + 26, "Broadcasting on the local network.", C_TEXT_DIM);
		break;
	case PH_HOST_ONLINE:
		text_n(u, cx, cy, "Host online, library is empty.", C_AMBER);
		text_n(u, cx, cy + 26, "Run Auto-Scan ROMs in the PC manager to publish a catalog.", C_TEXT_DIM);
		break;
	default:
		text_n(u, cx, cy, "No host found.", C_RED);
		text_n(u, cx, cy + 26, "Start Phantom Arcade Manager on the PC,", C_TEXT_DIM);
		text_n(u, cx, cy + 48, "then press REFRESH to search again.", C_TEXT_DIM);
		if (u->host_ip[0])
		{
			char s[96];
			snprintf(s, sizeof(s), "Configured host: %s:%d", u->host_ip, u->host_port);
			text_n(u, cx, cy + 76, s, C_TEXT_MUTE);
		}
		break;
	}
	(void)w;
}

static void draw_list(ph_ui *u)
{
	int x = PH_SAFE_X;
	int w = u->w - 2 * PH_SAFE_X;

	box(u, x, LIST_Y, w, LIST_H, C_CARD, C_BORDER, C_BORDER_HI);

	/* column header */
	rect(u, x + 2, LIST_Y + 2, w - 4, LIST_HDR_H - 2, C_PANEL_DK);
	text_n(u, x + 12, LIST_Y + 5, "GAME TITLE", C_TEXT_MUTE);
	text_n_right(u, x + w - 12, LIST_Y + 5, "SYSTEM", C_TEXT_MUTE);
	rect(u, x + 2, LIST_Y + LIST_HDR_H, w - 4, 2, C_BORDER);

	int idx[PH_MAX_GAMES];
	int n = ph_ui_filtered(u, idx, PH_MAX_GAMES);

	if (n == 0)
	{
		draw_empty(u, x, LIST_Y + LIST_HDR_H, w, u->count > 0);
		return;
	}

	int rows = visible_rows();
	int inner_y = LIST_Y + LIST_HDR_H + 4;
	int bar_w = (n > rows) ? 14 : 0;

	for (int r = 0; r < rows; r++)
	{
		int vi = r + u->scroll;
		if (vi >= n) break;

		const ph_game *g = &u->games[idx[vi]];
		int ry = inner_y + r * ROW_H;
		int sel = (vi == u->sel);

		/* The system column is right-aligned and variable width, so the title's
		 * budget has to be measured against this row's actual system string. A
		 * fixed split ran "Capcom CPS-3" straight through the end of "Street
		 * Fighter III: 3rd Strike". */
		const char *sys = g->sysname[0] ? g->sysname : g->system;
		int sys_x, sys_w;
		int tx = x + 30;
		int title_w = title_window(u, g, n, &sys_x, &sys_w);

		if (sel)
		{
			rect(u, x + 2, ry, w - 4, ROW_H, C_ROW_SEL);
			rect(u, x + 2, ry, 6, ROW_H, C_AMBER);
			arrow(u, x + 12, ry + 4, 12, PH_ARROW_RIGHT, C_AMBER);
		}
		else
		{
			rect(u, x + 2, ry, w - 4, ROW_H, (vi & 1) ? C_ROW_B : C_ROW_A);
		}

		uint32_t tc = sel ? C_AMBER_HI : C_TEXT;

		/* The selected row scrolls instead of clipping, so a long title is readable
		 * in full without leaving the list. Unselected rows clip: fifteen marquees
		 * at once would be unreadable. */
		if (sel)
		{
			text_n_scroll(u, tx, ry + 2, g->title, tc, 0x000000, title_w, u->marquee_px);
		}
		else
		{
			text_n_clip(u, tx, ry + 2, g->title, tc, title_w);
		}

		text_n_clip(u, sys_x, ry + 2, sys, sel ? C_AMBER : C_TEXT_MUTE, sys_w);
	}

	/* Scrollbar. Only drawn when it means something, and 6px wide so it is visible
	 * on an interlaced display. */
	if (bar_w)
	{
		int track_y = inner_y;
		int track_h = rows * ROW_H;
		int bar_h = track_h * rows / n;
		if (bar_h < 12) bar_h = 12;
		int bar_y = track_y + (track_h - bar_h) * u->scroll / (n - rows);
		int bx = x + w - 4 - bar_w + 4;
		rect(u, bx, track_y, 6, track_h, C_PANEL_DK);
		rect(u, bx, bar_y, 6, bar_h, C_BORDER_HI);
	}
}

static void draw_detail(ph_ui *u)
{
	int x = PH_SAFE_X;
	int w = u->w - 2 * PH_SAFE_X;

	box(u, x, DETAIL_Y, w, DETAIL_H, C_PANEL_DK, C_BORDER, 0);

	/* A transient message takes the whole bar. Without this the only place status text
	 * appeared was the busy overlay, so a launch that timed out dropped silently back
	 * to the list and looked like the button had done nothing. phantom.cpp clears
	 * status after a few seconds. */
	if (u->status[0] && u->view != PH_VIEW_BUSY)
	{
		text_n_clip(u, x + 12, DETAIL_Y + 4, u->status, C_AMBER, w - 24);
		return;
	}

	const ph_game *g = ph_ui_selected(u);
	if (!g)
	{
		text_n(u, x + 12, DETAIL_Y + 4, "--", C_TEXT_MUTE);
		return;
	}

	/* Left: year and manufacturer, which is what somebody standing at a cabinet
	 * actually wants to know. This used to print the launch id - useful exactly once,
	 * while the catalog plumbing was being debugged, and noise for ever after. The id
	 * is still visible in the host's log if a set ever needs chasing down.
	 *
	 * Falls back to the system name rather than inventing anything: a host that sends
	 * no year is a host that does not know it. */
	char left[160];
	if (g->year[0] && g->maker[0])      snprintf(left, sizeof(left), "%s   %s", g->year, g->maker);
	else if (g->year[0])                snprintf(left, sizeof(left), "%s", g->year);
	else if (g->maker[0])               snprintf(left, sizeof(left), "%s", g->maker);
	else                                snprintf(left, sizeof(left), "%s", g->sysname[0] ? g->sysname : g->system);
	text_n_clip(u, x + 12, DETAIL_Y + 4, left, C_TEXT_DIM, w - 24 - 40 * PH_NARROW_W);

	/* Right: the timing the host reported. Omitted entirely when unknown rather than
	 * filled in with a stock 15.734kHz that may be wrong for this title. */
	char right[128];
	if (g->hsync_hz && g->vsync_chz)
	{
		snprintf(right, sizeof(right), "%u.%02u kHz / %u.%02u Hz",
		         g->hsync_hz / 1000, (g->hsync_hz % 1000) / 10,
		         g->vsync_chz / 100, g->vsync_chz % 100);
	}
	else if (g->mode[0])
	{
		snprintf(right, sizeof(right), "%s", g->mode);
	}
	else
	{
		snprintf(right, sizeof(right), "timing from host at launch");
	}
	text_n_right(u, x + w - 12, DETAIL_Y + 4, right, C_CYAN);
}

static void draw_footer(ph_ui *u)
{
	rect(u, 0, FOOT_Y, u->w, FOOT_H, C_PANEL_DK);
	rect(u, 0, FOOT_Y, u->w, 2, C_BORDER);

	int y = FOOT_Y + 7;
	int x = PH_SAFE_X;

	arrow(u, x, y + 1, 14, PH_ARROW_UP, C_AMBER);
	arrow(u, x + 18, y + 1, 14, PH_ARROW_DOWN, C_AMBER);
	x += 38;
	text_n(u, x, y, "SELECT", C_TEXT_DIM);
	x += ntw("SELECT") + 22;

	arrow(u, x, y + 1, 14, PH_ARROW_LEFT, C_CYAN);
	arrow(u, x + 18, y + 1, 14, PH_ARROW_RIGHT, C_CYAN);
	x += 38;
	text_n(u, x, y, "PLATFORM", C_TEXT_DIM);
	x += ntw("PLATFORM") + 22;

	rect(u, x, y + 3, 12, 12, C_RED);
	x += 18;
	text_n(u, x, y, "LAUNCH", C_TEXT);
	x += ntw("LAUNCH") + 22;

	rect(u, x, y + 3, 12, 12, C_YELLOW);
	x += 18;
	text_n(u, x, y, "REFRESH", C_TEXT_DIM);
}

/* The busy overlay. There is deliberately no percentage and no countdown: the core
 * cannot know how long the host will take to start an emulator, and Phantom Arcade's
 * original fixed delay is exactly the guess this integration removes. The launcher
 * simply waits, and the first arriving frame tears this screen down (groovy.cpp's
 * setInit path), so the transition is driven by the real event. */
static void draw_busy(ph_ui *u)
{
	int bw = 560;
	int bh = 150;
	int bx = (u->w - bw) / 2;
	int by = (u->h - bh) / 2;

	box(u, bx, by, bw, bh, 0x161826, C_AMBER, C_AMBER_DK);

	text(u, bx + 20, by + 16, "STARTING STREAM", C_AMBER, 2);
	text_n_clip(u, bx + 20, by + 56, u->busy_title, C_TEXT, bw - 40);
	text_n_clip(u, bx + 20, by + 80, u->status, C_CYAN, bw - 40);

	/* A static rule, not a progress bar. There used to be an animated sweep here, which
	 * meant redrawing and recopying the whole canvas every tick for the entire time the
	 * host was starting an emulator - a megabyte of uncached writes into the DDR region
	 * the arriving video is about to use. The launcher now stops animating once a launch
	 * is out (phantom.cpp), and a bar frozen part-way across would read as stuck, so
	 * there is nothing here that pretends to measure progress. */
	rect(u, bx + 20, by + 114, bw - 40, 4, C_AMBER_DK);
}

void ph_ui_render(ph_ui *u)
{
	if (!u->fb) return;

	rect(u, 0, 0, u->w, u->h, C_BG);

	draw_header(u);
	draw_tabs(u);
	draw_list(u);
	draw_detail(u);
	draw_footer(u);

	if (u->view == PH_VIEW_BUSY) draw_busy(u);

	u->dirty = 0;
}
