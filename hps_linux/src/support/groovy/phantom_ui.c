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
 *   Overscan. Real cabinet monitors lose the edges, and a TV can lose far more, and
 *   unevenly - one that hides 20 lines at the top can hide 45 at the bottom. So content
 *   stays inside a per-edge safe area (ph_safe) that is set from the launcher's own
 *   screen-adjust page, and only backgrounds run out to the edge. The core's OSD "CRT H/V
 *   offset" options remain the fine adjustment for a CRT.
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

/* Sized for 720x480. The heights below are fixed; where each band sits is not - it follows
 * the safe area. The list takes whatever is left between the tab bar and the detail line:
 * 15 rows of 90 columns with no margins, where the first version managed 11 rows of 45. */
#define HEAD_H      44
#define TAB_H       28
#define LIST_HDR_H  24
#define ROW_H       20
#define DETAIL_H    24
#define FOOT_H      30

typedef struct
{
	int l, r;          /* left and right safe edges, in pixels from each side */
	int head_y;        /* top of the wordmark band's content */
	int tab_y;
	int list_y, list_h;
	int detail_y;
	int foot_y;
} ph_layout;

static void layout(const ph_ui *u, ph_layout *L)
{
	L->l = u->safe.l;
	L->r = u->safe.r;
	L->head_y = u->safe.t;
	L->tab_y = L->head_y + 48;
	L->foot_y = u->h - u->safe.b - FOOT_H;
	L->detail_y = L->foot_y - 4 - DETAIL_H;
	L->list_y = L->tab_y + TAB_H + 4;
	L->list_h = L->detail_y - 4 - L->list_y;
	if (L->list_h < LIST_HDR_H + 4 + ROW_H) L->list_h = LIST_HDR_H + 4 + ROW_H;
}

#define LAYOUT ph_layout L; layout(u, &L)

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

/* Draw something solid whatever the current opacity is: the things that must read as solid (the amber rules, the
 * selected tab, the cursor, the button badges) so a picture behind the menu cannot wash them out. */
#define OPAQUE(u, stmt) do { int _sa = (u)->alpha; (u)->alpha = 0; stmt; (u)->alpha = _sa; } while (0)

static void rect(ph_ui *u, int x, int y, int w, int h, uint32_t rgb)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > u->w) w = u->w - x;
	if (y + h > u->h) h = u->h - y;
	if (w <= 0 || h <= 0) return;

	if (u->alpha)
	{
		/* Translucent: blend with what is on the canvas already, which is the picture and whatever panels went
		 * down before this one. out = under * (1 - a) + colour * a, per channel. */
		const unsigned a = (unsigned)u->alpha, ia = 256u - a;
		const unsigned cb = (rgb & 0xff) * a, cg = ((rgb >> 8) & 0xff) * a, cr = ((rgb >> 16) & 0xff) * a;
		for (int j = 0; j < h; j++)
		{
			uint8_t *p = u->fb + ((size_t)(y + j) * u->w + x) * 3;
			for (int i = 0; i < w; i++, p += 3)
			{
				p[0] = (uint8_t)((p[0] * ia + cb) >> 8);
				p[1] = (uint8_t)((p[1] * ia + cg) >> 8);
				p[2] = (uint8_t)((p[2] * ia + cr) >> 8);
			}
		}
		return;
	}

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

void ph_ui_dim(uint8_t *dst, const uint8_t *src, size_t n, int keep_percent)
{
	if (keep_percent < 0) keep_percent = 0;
	if (keep_percent > 100) keep_percent = 100;
	const unsigned k = (unsigned)keep_percent * 256u / 100u;
	for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)((src[i] * k) >> 8);
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

	const int sa = u->alpha;   /* text is what has to be read: never translucent */
	u->alpha = 0;
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
	u->alpha = sa;
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
	case PH_TAB_FBNEO:
		return ieq(sys, "fbneo") || ieq(sys, "fightcade");
	case PH_TAB_DC:
		return ieq(sys, "flycast") || ieq(sys, "naomi") || ieq(sys, "dreamcast");
	case PH_TAB_PS2:
		return ieq(sys, "pcsx2") || ieq(sys, "ps2");
	case PH_TAB_PS3:
		return ieq(sys, "rpcs3") || ieq(sys, "ps3");
	case PH_TAB_XBOX:
		return ieq(sys, "xemu") || ieq(sys, "xbox");
	case PH_TAB_GCWII:
		return ieq(sys, "dolphin") || ieq(sys, "gamecube") || ieq(sys, "wii");
	case PH_TAB_PC:
		return ieq(sys, "86box") || ieq(sys, "pc") || ieq(sys, "dos");
	case PH_TAB_RA:
		return ieq(sys, "retroarch");
	default:
		return 0;
	}
}

int ph_tab_matches(int tab, const char *sys)
{
	return tab_match(tab, sys);
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

/* The letter a title files under: A-Z whatever its case, everything else as one group (0). */
static int letter_key(const char *t)
{
	char c = t ? t[0] : 0;
	if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
	else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
	else c = 0;
	return c;
}

/* The resident list, as a source. These scan games[] for each question, which is fine for the few hundred rows
 * it is limited to and keeps the tests and the preview tool exactly as they were. */
static int res_count(const ph_ui *u, int tab)
{
	int n = 0;
	for (int i = 0; i < u->count; i++)
		if (tab_match(tab, u->games[i].system)) n++;
	return n;
}

static const ph_game *res_row(const ph_ui *u, int tab, int index)
{
	if (index < 0) return 0;
	for (int i = 0; i < u->count; i++)
		if (tab_match(tab, u->games[i].system) && index-- == 0) return &u->games[i];
	return 0;
}

/* First index of letter k in a list that is in title order: how many titles file under an earlier letter. */
static int res_letter(const ph_ui *u, int tab, int k)
{
	int n = 0, before = 0;
	for (int i = 0; i < u->count; i++)
	{
		if (!tab_match(tab, u->games[i].system)) continue;
		n++;
		if (letter_key(u->games[i].title) < k) before++;
	}
	return k >= 27 ? n : before;
}

static int src_count(const ph_ui *u, int tab)
{
	int n = u->src.count ? u->src.count(u->src.ctx, tab) : res_count(u, tab);
	return n < 0 ? 0 : n;
}

static const ph_game *src_row(const ph_ui *u, int tab, int index)
{
	return u->src.row ? u->src.row(u->src.ctx, tab, index) : res_row(u, tab, index);
}

static int src_letter(const ph_ui *u, int tab, int k)
{
	return u->src.letter_start ? u->src.letter_start(u->src.ctx, tab, k) : res_letter(u, tab, k);
}

int ph_ui_tab_count(const ph_ui *u, int tab)
{
	return src_count(u, tab);
}

const ph_game *ph_ui_selected(const ph_ui *u)
{
	int n = src_count(u, u->tab);
	if (n <= 0 || u->sel < 0 || u->sel >= n) return 0;
	return src_row(u, u->tab, u->sel);
}

static int visible_rows(const ph_ui *u)
{
	LAYOUT;
	return (L.list_h - LIST_HDR_H - 4) / ROW_H;
}

/* Pixel budget the selected row's title gets. Shared by the renderer and the marquee
 * so the two cannot disagree about when a title overflows - if they do, a title
 * scrolls that does not need to, or sits clipped and still. */
static int title_window(const ph_ui *u, const ph_game *g, int n, int *out_sys_x, int *out_sys_w)
{
	LAYOUT;
	int w = u->w - L.l - L.r;
	int bar_w = (n > visible_rows(u)) ? 14 : 0;

	/* The system name is capped before the title is measured, so one absurdly long
	 * hardware string cannot squeeze every title down to an ellipsis. */
	const char *sys = g->sysname[0] ? g->sysname : g->system;
	int sys_cap = (w - 24 - bar_w) * 2 / 5;
	int sys_w = (int)strlen(sys) * PH_NARROW_W;
	if (sys_w > sys_cap) sys_w = sys_cap;

	int tx = L.l + 30;
	int sys_x = L.l + w - 12 - bar_w - sys_w;
	int tw = sys_x - tx - 16;
	if (tw < 32) tw = 32;

	if (out_sys_x) *out_sys_x = sys_x;
	if (out_sys_w) *out_sys_w = sys_w;
	return tw;
}

static void fix_scroll(ph_ui *u, int n)
{
	int rows = visible_rows(u);
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
	u->safe.l = PH_SAFE_DEF_L;
	u->safe.t = PH_SAFE_DEF_T;
	u->safe.r = PH_SAFE_DEF_R;
	u->safe.b = PH_SAFE_DEF_B;
	u->safe_saved = u->safe;
	u->dirty = 1;
}

int ph_ui_move(ph_ui *u, int dy)
{
	int n = src_count(u, u->tab);
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
	int n = src_count(u, u->tab);

	/* Staying on the same game by id is only possible when every row is at hand. A source that fetches
	 * pages keeps the position instead, clamped to whatever the list now holds. */
	if (keep_id && *keep_id && !u->src.row)
	{
		for (int i = 0; i < n; i++)
		{
			const ph_game *g = res_row(u, u->tab, i);
			if (g && strcmp(g->id, keep_id) == 0)
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

int ph_ui_rows(const ph_ui *u)
{
	return visible_rows(u);
}

int ph_ui_seek(ph_ui *u, int rows)
{
	int n = src_count(u, u->tab);
	if (n <= 0 || rows == 0) return 0;

	int before = u->sel;
	long to = (long)u->sel + rows;
	if (to < 0) to = 0;
	if (to > n - 1) to = n - 1;
	u->sel = (int)to;
	fix_scroll(u, n);

	if (u->sel == before) return 0;
	u->marquee_t0 = u->tick_ms;
	u->marquee_px = 0;
	u->dirty = 1;
	return 1;
}

int ph_ui_page(ph_ui *u, int dir)
{
	if (dir == 0) return 0;
	return ph_ui_seek(u, dir * visible_rows(u));
}

int ph_ui_letter(ph_ui *u, int dir)
{
	int n = src_count(u, u->tab);
	if (n <= 0 || dir == 0) return 0;

	/* Where each letter's titles begin. Not known yet (a host-backed list fetches the table the first time a
	 * tab is shown) means the press has nothing to act on. */
	int s[28];
	for (int k = 0; k < 28; k++)
	{
		s[k] = (k == 27) ? n : src_letter(u, u->tab, k);
		if (s[k] < 0) return 0;
	}

	/* The letter the selection is in: the last one that has titles and begins at or before it. */
	int cur = 0;
	for (int k = 0; k < 27; k++)
		if (s[k] < s[k + 1] && s[k] <= u->sel) cur = k;

	int to = u->sel;
	if (dir > 0)
	{
		int k = cur + 1;
		while (k < 27 && s[k] >= s[k + 1]) k++;           /* skip letters with no titles */
		if (k < 27)
		{
			to = s[k];
		}
		else
		{
			k = 0;                                         /* past the last letter wraps to the first */
			while (k < 27 && s[k] >= s[k + 1]) k++;
			to = (k < 27) ? s[k] : 0;
		}
	}
	else
	{
		if (u->sel > s[cur])
		{
			to = s[cur];                                   /* first press goes to the top of this letter */
		}
		else
		{
			int k = cur - 1;
			while (k >= 0 && s[k] >= s[k + 1]) k--;
			if (k >= 0)
			{
				to = s[k];                                 /* then to the top of the one before */
			}
			else
			{
				k = 26;                                    /* before the first letter wraps to the last */
				while (k >= 0 && s[k] >= s[k + 1]) k--;
				to = (k >= 0) ? s[k] : 0;
			}
		}
	}

	if (to == u->sel) return 0;
	u->sel = to;
	fix_scroll(u, n);
	u->marquee_t0 = u->tick_ms;
	u->marquee_px = 0;
	u->dirty = 1;
	return 1;
}

static int clamp_even(int v, int max)
{
	if (v < 0) v = 0;
	if (v > max) v = max;
	return v & ~1;
}

void ph_ui_set_safe(ph_ui *u, int l, int t, int r, int b)
{
	u->safe.l = clamp_even(l, PH_SAFE_MAX_X);
	u->safe.t = clamp_even(t, PH_SAFE_MAX_Y);
	u->safe.r = clamp_even(r, PH_SAFE_MAX_X);
	u->safe.b = clamp_even(b, PH_SAFE_MAX_Y);

	/* The list shows more or fewer rows now, so the scroll position may no longer be valid. */
	fix_scroll(u, src_count(u, u->tab));
	u->dirty = 1;
}

void ph_ui_adjust_begin(ph_ui *u)
{
	u->safe_saved = u->safe;
	u->adj_edge = 1;       /* the bottom is the edge a TV most often hides too much of */
	u->adj_return = (u->view == PH_VIEW_SETTINGS) ? PH_VIEW_SETTINGS : PH_VIEW_LIST;
	u->view = PH_VIEW_ADJUST;
	u->dirty = 1;
}

int ph_ui_adjust_edge(ph_ui *u, int dir)
{
	u->adj_edge = (u->adj_edge + (dir < 0 ? 3 : 1)) & 3;
	u->dirty = 1;
	return 1;
}

int ph_ui_adjust_move(ph_ui *u, int delta)
{
	int *p;
	int max;
	switch (u->adj_edge)
	{
	case 0:  p = &u->safe.t; max = PH_SAFE_MAX_Y; break;
	case 1:  p = &u->safe.b; max = PH_SAFE_MAX_Y; break;
	case 2:  p = &u->safe.l; max = PH_SAFE_MAX_X; break;
	default: p = &u->safe.r; max = PH_SAFE_MAX_X; break;
	}
	int v = clamp_even(*p + delta, max);
	if (v == *p) return 0;
	*p = v;
	u->dirty = 1;
	return 1;
}

void ph_ui_adjust_reset(ph_ui *u)
{
	u->safe.l = PH_SAFE_DEF_L;
	u->safe.t = PH_SAFE_DEF_T;
	u->safe.r = PH_SAFE_DEF_R;
	u->safe.b = PH_SAFE_DEF_B;
	u->dirty = 1;
}

void ph_ui_adjust_end(ph_ui *u, int keep)
{
	if (!keep) u->safe = u->safe_saved;
	u->view = (u->adj_return == PH_VIEW_SETTINGS) ? PH_VIEW_SETTINGS : PH_VIEW_LIST;
	ph_ui_set_safe(u, u->safe.l, u->safe.t, u->safe.r, u->safe.b);   /* re-clamp the scroll */
}

/* ---- the settings menu ------------------------------------------------------------ */

static int set_selectable(const ph_ui *u, int i)
{
	return i >= 0 && i < u->set_count && u->set_rows[i].kind != PH_SET_INFO;
}

void ph_ui_settings_begin(ph_ui *u)
{
	u->view = PH_VIEW_SETTINGS;
	u->set_sel = 0;
	for (int i = 0; i < u->set_count; i++)
		if (set_selectable(u, i)) { u->set_sel = i; break; }
	u->dirty = 1;
}

void ph_ui_settings_end(ph_ui *u)
{
	u->view = PH_VIEW_LIST;
	u->dirty = 1;
}

int ph_ui_settings_move(ph_ui *u, int dy)
{
	if (u->set_count <= 0 || !dy) return 0;
	const int step = dy < 0 ? -1 : 1;
	int i = u->set_sel;
	for (int n = 0; n < u->set_count; n++)
	{
		i = (i + step + u->set_count) % u->set_count;
		if (set_selectable(u, i))
		{
			u->set_sel = i;
			u->dirty = 1;
			return 1;
		}
	}
	return 0;
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

	int n = src_count(u, u->tab);
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
	LAYOUT;
	/* The band itself runs to the top of the screen so there is no stripe of bare
	 * background above it; only the text is held inside the margin. */
	vgrad(u, 0, 0, u->w, L.head_y + HEAD_H - 4, C_HEAD_A, C_HEAD_B);
	OPAQUE(u, rect(u, 0, L.head_y + HEAD_H - 4, u->w, 4, C_AMBER));

	/* Wordmark stays square-scaled and chunky - it is the one thing meant to read from
	 * across a room, and it is short enough that the width costs nothing. */
	text_sh(u, L.l, L.head_y + 6, "PHANTOM ARCADE", C_AMBER, C_AMBER_DK, 3);

	const char *label;
	uint32_t led;
	switch (u->host_state)
	{
	case PH_HOST_ONLINE:
		label = u->lib_scanning ? "HOST SCANNING" : "HOST ONLINE";
		led = u->lib_scanning ? C_YELLOW : C_GREEN;
		break;
	case PH_HOST_OFFLINE: label = "NO HOST";       led = C_RED;    break;
	default:              label = "SEARCHING...";  led = C_YELLOW; break;
	}

	char addr[96];
	if (u->host_state == PH_HOST_ONLINE && u->host_ip[0])
		snprintf(addr, sizeof(addr), "%s:%d", u->host_ip, u->host_port);
	else
		addr[0] = 0;

	int xr = u->w - L.r;
	text_n_right(u, xr, L.head_y + 4, label, C_TEXT_DIM);
	if (addr[0]) text_n_right(u, xr, L.head_y + 22, addr, C_TEXT_MUTE);

	OPAQUE(u, rect(u, xr - ntw(label) - 16, L.head_y + 8, 8, 8, led));
}

/* Width of tab t's box, padding included. */
static int tab_width(const char *label)
{
	return ntw(label) + 16;
}

static void draw_tabs(ph_ui *u)
{
	LAYOUT;
	static const char *tabs[PH_TAB_COUNT] =
		{ "ALL", "MAME", "FBNEO", "DREAMCAST", "PS2", "PS3", "XBOX",
		  "GC/WII", "PC", "RETROARCH" };

	/* Gap between tab boxes. Each box already carries 8px of padding inside it. */
	enum { TAB_GAP = 4 };
	/* Drawn where tabs run off that side, so a strip that is scrolled does not look
	 * like a strip that is simply short. */
	static const char *MORE_L = "<";
	static const char *MORE_R = ">";

	rect(u, 0, L.tab_y, u->w, TAB_H, C_TABBAR);

	/* Position within the filtered list, parked at the right end of the strip. Measured
	 * first because the tabs get whatever width it does not want. */
	int n = src_count(u, u->tab);
	char cnt[32];
	snprintf(cnt, sizeof(cnt), "%d/%d", n ? u->sel + 1 : 0, n);

	const int cnt_w = ntw(cnt) + 12;
	const int strip_l = L.l;
	int strip_r = u->w - L.r - cnt_w;
	if (strip_r < strip_l) strip_r = strip_l;

	/* The strip scrolls rather than being limited to what fits: the tab count follows
	 * the emulators the host can drive, so sooner or later it will not fit whatever the
	 * labels are shortened to. Anchor on the selected tab and walk outwards.
	 *
	 * Recomputed every frame from u->tab alone, so there is no scroll position to keep
	 * in step with the selection - moving along the strip can never leave the highlight
	 * off-screen, and the same tab always draws in the same place. */
	int first = u->tab, last = u->tab;
	int used = tab_width(tabs[u->tab]);

	/* Grow right first so moving forward reveals what is coming, then left with
	 * whatever is left over. */
	while (last + 1 < PH_TAB_COUNT &&
	       used + TAB_GAP + tab_width(tabs[last + 1]) <= strip_r - strip_l)
	{
		used += TAB_GAP + tab_width(tabs[last + 1]);
		last++;
	}
	while (first > 0 &&
	       used + TAB_GAP + tab_width(tabs[first - 1]) <= strip_r - strip_l)
	{
		used += TAB_GAP + tab_width(tabs[first - 1]);
		first--;
	}

	int x = strip_l;
	if (first > 0)
	{
		text_n(u, x, L.tab_y + 6, MORE_L, C_TEXT_MUTE);
		x += ntw(MORE_L) + TAB_GAP;
	}

	for (int t = first; t <= last; t++)
	{
		int tw = tab_width(tabs[t]);
		if (t == u->tab)
		{
			OPAQUE(u, box(u, x, L.tab_y + 1, tw, TAB_H - 4, C_AMBER, C_AMBER_HI, 0));
			text_n(u, x + 8, L.tab_y + 6, tabs[t], 0x1A1200);
		}
		else
		{
			box(u, x, L.tab_y + 1, tw, TAB_H - 4, C_INACTIVE, C_BORDER, 0);
			text_n(u, x + 8, L.tab_y + 6, tabs[t], C_TEXT_MUTE);
		}
		x += tw + TAB_GAP;
	}

	if (last < PH_TAB_COUNT - 1 && x + ntw(MORE_R) <= strip_r)
		text_n(u, x, L.tab_y + 6, MORE_R, C_TEXT_MUTE);

	text_n_right(u, u->w - L.r, L.tab_y + 6, cnt, C_TEXT_MUTE);

	rect(u, 0, L.tab_y + TAB_H, u->w, 2, C_BORDER);
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
		text_n(u, cx, cy + 26, "Press L / R to change platform.", C_TEXT_DIM);
		return;
	}

	if (u->host_state == PH_HOST_ONLINE && u->lib_scanning)
	{
		text_n(u, cx, cy, "The host is scanning your ROM folders...", C_AMBER);
		text_n(u, cx, cy + 26, "Games appear here as they are found.", C_TEXT_DIM);
		return;
	}
	if (u->host_state == PH_HOST_ONLINE && u->lib_loading)
	{
		text_n(u, cx, cy, "Loading the library...", C_AMBER);
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
		text_n(u, cx, cy + 48, "then press Y (REFRESH) to search again.", C_TEXT_DIM);
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

static void draw_list_body(ph_ui *u)
{
	LAYOUT;
	int x = L.l;
	int w = u->w - L.l - L.r;

	box(u, x, L.list_y, w, L.list_h, C_CARD, C_BORDER, C_BORDER_HI);

	/* column header */
	rect(u, x + 2, L.list_y + 2, w - 4, LIST_HDR_H - 2, C_PANEL_DK);
	text_n(u, x + 12, L.list_y + 5, "GAME TITLE", C_TEXT_MUTE);
	text_n_right(u, x + w - 12, L.list_y + 5, "SYSTEM", C_TEXT_MUTE);
	rect(u, x + 2, L.list_y + LIST_HDR_H, w - 4, 2, C_BORDER);

	int n = src_count(u, u->tab);

	if (n == 0)
	{
		draw_empty(u, x, L.list_y + LIST_HDR_H, w, src_count(u, PH_TAB_ALL) > 0);
		return;
	}

	int rows = visible_rows(u);
	int inner_y = L.list_y + LIST_HDR_H + 4;
	int bar_w = (n > rows) ? 14 : 0;

	for (int r = 0; r < rows; r++)
	{
		int vi = r + u->scroll;
		if (vi >= n) break;

		/* NULL: the page holding this row is still on its way from the host. */
		const ph_game *g = src_row(u, u->tab, vi);
		int ry = inner_y + r * ROW_H;
		int sel = (vi == u->sel);
		int tx = x + 30;

		if (sel)
		{
			{
				const int sa = u->alpha;
				if (sa) u->alpha = sa + 80 > 255 ? 255 : sa + 80;   /* the cursor row must stand out of a see-through list */
				rect(u, x + 2, ry, w - 4, ROW_H, C_ROW_SEL);
				u->alpha = sa;
			}
			OPAQUE(u, rect(u, x + 2, ry, 6, ROW_H, C_AMBER));
			OPAQUE(u, arrow(u, x + 12, ry + 4, 12, PH_ARROW_RIGHT, C_AMBER));
		}
		else
		{
			rect(u, x + 2, ry, w - 4, ROW_H, (vi & 1) ? C_ROW_B : C_ROW_A);
		}

		if (!g)
		{
			/* A bar where the title will be, a different length each row so it reads as text on its way and not
			 * as a ruled line. Drawn rather than left blank because blank looks like an empty entry. */
			rect(u, tx, ry + 6, 96 + (vi * 53) % 168, 8, sel ? C_AMBER_DK : C_BORDER);
			continue;
		}

		/* The system column is right-aligned and variable width, so the title's
		 * budget has to be measured against this row's actual system string. A
		 * fixed split ran "Capcom CPS-3" straight through the end of "Street
		 * Fighter III: 3rd Strike". */
		const char *sys = g->sysname[0] ? g->sysname : g->system;
		int sys_x, sys_w;
		int title_w = title_window(u, g, n, &sys_x, &sys_w);

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

/* The list is drawn at its own opacity: it is the biggest thing on screen and the one a picture is most worth having
 * behind, so it is the most see-through. Everything in it that is text stays solid regardless. */
static void draw_list(ph_ui *u)
{
	const int sa = u->alpha;
	if (sa) u->alpha = u->bg_list_alpha > 0 ? u->bg_list_alpha : PH_BG_LIST_ALPHA;
	draw_list_body(u);
	u->alpha = sa;
}

static void draw_detail(ph_ui *u)
{
	LAYOUT;
	int x = L.l;
	int w = u->w - L.l - L.r;

	box(u, x, L.detail_y, w, DETAIL_H, C_PANEL_DK, C_BORDER, 0);

	/* A transient message takes the whole bar. Without this the only place status text
	 * appeared was the busy overlay, so a launch that timed out dropped silently back
	 * to the list and looked like the button had done nothing. phantom.cpp clears
	 * status after a few seconds. */
	if (u->status[0] && u->view != PH_VIEW_BUSY)
	{
		text_n_clip(u, x + 12, L.detail_y + 4, u->status, C_AMBER, w - 24);
		return;
	}

	const ph_game *g = ph_ui_selected(u);
	if (!g)
	{
		const int waiting = u->host_state == PH_HOST_ONLINE && src_count(u, u->tab) > 0;
		text_n(u, x + 12, L.detail_y + 4, waiting ? "Loading..." : "--", C_TEXT_MUTE);
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
	text_n_clip(u, x + 12, L.detail_y + 4, left, C_TEXT_DIM, w - 24 - 40 * PH_NARROW_W);

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
	text_n_right(u, x + w - 12, L.detail_y + 4, right, C_CYAN);
}

/* ---- button legend ----------------------------------------------------------- */

/* The four face buttons in MiSTer's SNES-style colours - A red, B yellow, X blue, Y green -
 * so the key at the bottom matches the pad in the user's hands. The core names the pad's ten
 * positions A, B, X, Y, L, R, Select, Start, L2, R2 in that order (input.cpp), and phantom.cpp
 * reads exactly those. */
#define C_BTN_A 0xE04444
#define C_BTN_B 0xE0B020
#define C_BTN_X 0x4A86E8
#define C_BTN_Y 0x18C070

static int isqrt_i(int v)
{
	int r = 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

/* A filled disc, built from 2px-tall bands like everything else here. */
static void disc(ph_ui *u, int x, int y, int d, uint32_t rgb)
{
	int R = d / 2;
	const int sa = u->alpha;   /* the button colours are the point of the legend: solid */
	u->alpha = 0;
	for (int j = 0; j < d; j += 2)
	{
		int dy = (j + 1) - R;
		int half = isqrt_i(R * R - dy * dy);
		rect(u, x + R - half, y + j, half * 2, 2, rgb);
	}
	u->alpha = sa;
}

/* A face button: coloured disc with its letter. 18px so the narrow glyph (8 wide, 16 inked
 * rows) sits inside it. */
static void badge(ph_ui *u, int x, int y, uint32_t rgb, char ch)
{
	disc(u, x, y, 18, rgb);
	glyph_s(u, x + 5, y + 1, ch, 0x101010, 1, 2);
}

/* A shoulder button or Select: a labelled key cap. Returns its width. */
static int keycap(ph_ui *u, int x, int y, const char *label)
{
	int w = ntw(label) + 12;
	rect(u, x, y, w, 18, C_BORDER_HI);
	rect(u, x + 2, y + 2, w - 4, 14, C_INACTIVE);
	text_n(u, x + 6, y + 1, label, C_TEXT);
	return w;
}

/* The D-pad as a plus, with the two arms the item uses lit. */
static void dpad(ph_ui *u, int x, int y, int vertical)
{
	rect(u, x, y + 6, 20, 8, C_BORDER_HI);
	rect(u, x + 6, y, 8, 20, C_BORDER_HI);
	if (vertical)
	{
		rect(u, x + 6, y, 8, 8, C_AMBER);
		rect(u, x + 6, y + 12, 8, 8, C_AMBER);
	}
	else
	{
		rect(u, x, y + 6, 8, 8, C_AMBER);
		rect(u, x + 12, y + 6, 8, 8, C_AMBER);
	}
}

enum { LG_MOVE, LG_PAGE, LG_SYSTEM, LG_LETTER, LG_LAUNCH, LG_REFRESH, LG_SCREEN, LG_COUNT };

/* Which items go first when the margins leave too little width. */
static const int lg_drop_order[LG_COUNT] =
	{ LG_SCREEN, LG_LETTER, LG_PAGE, LG_REFRESH, LG_SYSTEM, LG_MOVE, LG_LAUNCH };

static int lg_width(int it)
{
	switch (it)
	{
	case LG_MOVE:    return 20 + 6 + ntw("MOVE");
	case LG_PAGE:    return 20 + 6 + ntw("PAGE");
	case LG_SYSTEM:  return (ntw("L") + 12) + 4 + (ntw("R") + 12) + 6 + ntw("SYSTEM");
	case LG_LETTER:  return (ntw("L2") + 12) + 4 + (ntw("R2") + 12) + 6 + ntw("A-Z");
	case LG_LAUNCH:  return 18 + 6 + ntw("LAUNCH");
	case LG_REFRESH: return 18 + 6 + ntw("REFRESH");
	default:         return (ntw("SEL") + 12) + 6 + ntw("MENU");
	}
}

static int lg_draw(ph_ui *u, int it, int x, int y)
{
	switch (it)
	{
	case LG_MOVE:
		dpad(u, x, y, 1);
		text_n(u, x + 26, y + 1, "MOVE", C_TEXT_DIM);
		break;
	case LG_PAGE:
		dpad(u, x, y, 0);
		text_n(u, x + 26, y + 1, "PAGE", C_TEXT_DIM);
		break;
	case LG_SYSTEM:
	{
		int a = keycap(u, x, y, "L");
		int b = keycap(u, x + a + 4, y, "R");
		text_n(u, x + a + 4 + b + 6, y + 1, "SYSTEM", C_TEXT_DIM);
		break;
	}
	case LG_LETTER:
	{
		int a = keycap(u, x, y, "L2");
		int b = keycap(u, x + a + 4, y, "R2");
		text_n(u, x + a + 4 + b + 6, y + 1, "A-Z", C_TEXT_DIM);
		break;
	}
	case LG_LAUNCH:
		badge(u, x, y, C_BTN_A, 'A');
		text_n(u, x + 24, y + 1, "LAUNCH", C_TEXT);
		break;
	case LG_REFRESH:
		badge(u, x, y, C_BTN_Y, 'Y');
		text_n(u, x + 24, y + 1, "REFRESH", C_TEXT_DIM);
		break;
	default:
	{
		int a = keycap(u, x, y, "SEL");
		text_n(u, x + a + 6, y + 1, "MENU", C_TEXT_DIM);
		break;
	}
	}
	return lg_width(it);
}

static void draw_footer(ph_ui *u)
{
	LAYOUT;

	/* The panel runs to the bottom edge of the screen; the key inside it stays above the margin. */
	rect(u, 0, L.foot_y, u->w, u->h - L.foot_y, C_PANEL_DK);
	rect(u, 0, L.foot_y, u->w, 2, C_BORDER);

	enum { GAP = 14 };
	int avail = u->w - L.l - L.r;
	int shown[LG_COUNT];
	int total = 0, count = 0;
	for (int i = 0; i < LG_COUNT; i++)
	{
		shown[i] = 1;
		total += lg_width(i);
		count++;
	}
	total += GAP * (count - 1);

	/* Too narrow for the whole key: drop the least important items until it fits. */
	for (int k = 0; k < LG_COUNT && total > avail; k++)
	{
		int it = lg_drop_order[k];
		shown[it] = 0;
		total -= lg_width(it) + GAP;
	}

	int y = L.foot_y + 6;
	int x = L.l;
	for (int i = 0; i < LG_COUNT; i++)
	{
		if (!shown[i]) continue;
		x += lg_draw(u, i, x, y) + GAP;
	}
}

/* ---- the screen-adjust page --------------------------------------------------- */

static void frame(ph_ui *u, int x, int y, int w, int h, int th, uint32_t rgb)
{
	rect(u, x, y, w, th, rgb);
	rect(u, x, y + h - th, w, th, rgb);
	rect(u, x, y, th, h, rgb);
	rect(u, x + w - th, y, th, h, rgb);
}

static void draw_adjust(ph_ui *u)
{
	rect(u, 0, 0, u->w, u->h, C_BG);

	/* The whole raster, in red. Whatever of this frame cannot be seen is being hidden by the display. */
	frame(u, 0, 0, u->w, u->h, 4, C_RED);

	const int l = u->safe.l, t = u->safe.t;
	const int w = u->w - u->safe.l - u->safe.r;
	const int h = u->h - u->safe.t - u->safe.b;

	/* The safe rectangle, in amber, with the edge being moved drawn heavier and brighter. */
	frame(u, l, t, w, h, 2, C_AMBER_DK);
	switch (u->adj_edge)
	{
	case 0: rect(u, l, t, w, 4, C_AMBER_HI); break;
	case 1: rect(u, l, t + h - 4, w, 4, C_AMBER_HI); break;
	case 2: rect(u, l, t, 4, h, C_AMBER_HI); break;
	default: rect(u, l + w - 4, t, 4, h, C_AMBER_HI); break;
	}
	/* Corner brackets, so the corners are findable on a display that rounds them. */
	for (int c = 0; c < 4; c++)
	{
		int cx = (c & 1) ? l + w - 24 : l;
		int cy = (c & 2) ? t + h - 24 : t;
		rect(u, cx, (c & 2) ? cy + 20 : cy, 24, 4, C_AMBER);
		rect(u, (c & 1) ? cx + 20 : cx, cy, 4, 24, C_AMBER);
	}

	int cx = u->w / 2;
	text_sh(u, cx - (int)strlen("SCREEN ADJUST") * 12, t + 18, "SCREEN ADJUST", C_AMBER, C_AMBER_DK, 3);

	const char *help1 = "Move each edge in until the whole amber box is on screen.";
	const char *help2 = "UP shrinks the box, DOWN grows it. Everything the launcher shows stays inside it.";
	text_n(u, cx - ntw(help1) / 2, t + 62, help1, C_TEXT_DIM);
	text_n(u, cx - ntw(help2) / 2, t + 82, help2, C_TEXT_DIM);

	static const char *names[4] = { "TOP", "BOTTOM", "LEFT", "RIGHT" };
	int vals[4] = { u->safe.t, u->safe.b, u->safe.l, u->safe.r };
	int ry = t + 120;
	for (int i = 0; i < 4; i++)
	{
		int sel = (i == u->adj_edge);
		int rx = cx - 130;
		rect(u, rx, ry, 260, 26, sel ? C_ROW_SEL : C_CARD);
		if (sel) rect(u, rx, ry, 6, 26, C_AMBER);
		text_n(u, rx + 18, ry + 5, names[i], sel ? C_AMBER_HI : C_TEXT_DIM);
		char v[16];
		snprintf(v, sizeof(v), "%d px", vals[i]);
		text_n_right(u, rx + 244, ry + 5, v, sel ? C_AMBER_HI : C_TEXT);
		ry += 30;
	}

	/* This page's own key, centred near the bottom of the safe area. */
	int ky = t + h - 48;
	enum { GAP = 14 };
	int kw = (20 + 6 + ntw("EDGE")) + GAP + (20 + 6 + ntw("SIZE")) + GAP + (18 + 6 + ntw("SAVE")) +
	         GAP + (18 + 6 + ntw("CANCEL")) + GAP + (18 + 6 + ntw("RESET"));
	int kx = cx - kw / 2;
	dpad(u, kx, ky, 0);       text_n(u, kx + 26, ky + 1, "EDGE", C_TEXT_DIM);   kx += 20 + 6 + ntw("EDGE") + GAP;
	dpad(u, kx, ky, 1);       text_n(u, kx + 26, ky + 1, "SIZE", C_TEXT_DIM);   kx += 20 + 6 + ntw("SIZE") + GAP;
	badge(u, kx, ky, C_BTN_A, 'A');  text_n(u, kx + 24, ky + 1, "SAVE", C_TEXT);        kx += 18 + 6 + ntw("SAVE") + GAP;
	badge(u, kx, ky, C_BTN_B, 'B');  text_n(u, kx + 24, ky + 1, "CANCEL", C_TEXT_DIM);  kx += 18 + 6 + ntw("CANCEL") + GAP;
	badge(u, kx, ky, C_BTN_Y, 'Y');  text_n(u, kx + 24, ky + 1, "RESET", C_TEXT_DIM);
}

/* ---- the settings page --------------------------------------------------------- */

#define SET_ROW_H 28

static void draw_settings(ph_ui *u)
{
	LAYOUT;
	draw_header(u);

	/* The title strip stands where the tabs are. */
	rect(u, 0, L.tab_y, u->w, TAB_H, C_TABBAR);
	text_n(u, L.l, L.tab_y + 6, "SETTINGS", C_AMBER_HI);
	rect(u, 0, L.tab_y + TAB_H, u->w, 2, C_BORDER);

	const int x0 = L.l, x1 = u->w - L.r;
	int y = L.list_y + 4;
	for (int i = 0; i < u->set_count && i < PH_SET_MAX; i++)
	{
		const ph_set_row *r = &u->set_rows[i];
		if (r->kind == PH_SET_INFO)
		{
			text_n_clip(u, x0 + 14, y + 3, r->label, C_TEXT_MUTE, x1 - x0 - 28 - (r->value[0] ? ntw(r->value) + 16 : 0));
			if (r->value[0]) text_n_right(u, x1 - 14, y + 3, r->value, C_TEXT_DIM);
			y += SET_ROW_H;
			continue;
		}
		const int sel = (i == u->set_sel);
		rect(u, x0, y, x1 - x0, SET_ROW_H - 4, sel ? C_ROW_SEL : ((i & 1) ? C_ROW_B : C_ROW_A));
		if (sel) OPAQUE(u, rect(u, x0, y, 6, SET_ROW_H - 4, C_AMBER));
		text_n(u, x0 + 18, y + 3, r->label, sel ? C_AMBER_HI : C_TEXT);

		char v[56];
		if (r->kind == PH_SET_VALUE && sel) snprintf(v, sizeof(v), "< %s >", r->value);
		else                                snprintf(v, sizeof(v), "%s", r->value);
		text_n_right(u, x1 - 14, y + 3, v, sel ? C_AMBER_HI : C_TEXT_DIM);
		y += SET_ROW_H;
	}

	/* What the selected line does, in the detail band. */
	rect(u, 0, L.detail_y, u->w, DETAIL_H, C_PANEL_DK);
	if (u->set_note[0]) text_n_clip(u, L.l, L.detail_y + 4, u->set_note, C_CYAN, u->w - L.l - L.r);

	/* This page's key. */
	rect(u, 0, L.foot_y, u->w, u->h - L.foot_y, C_PANEL_DK);
	rect(u, 0, L.foot_y, u->w, 2, C_BORDER);
	enum { GAP = 14 };
	int ky = L.foot_y + 6, kx = L.l;
	dpad(u, kx, ky, 1);               text_n(u, kx + 26, ky + 1, "CHOOSE", C_TEXT_DIM);   kx += 20 + 6 + ntw("CHOOSE") + GAP;
	dpad(u, kx, ky, 0);               text_n(u, kx + 26, ky + 1, "CHANGE", C_TEXT_DIM);   kx += 20 + 6 + ntw("CHANGE") + GAP;
	badge(u, kx, ky, C_BTN_A, 'A');   text_n(u, kx + 24, ky + 1, "DO", C_TEXT);           kx += 18 + 6 + ntw("DO") + GAP;
	badge(u, kx, ky, C_BTN_B, 'B');   text_n(u, kx + 24, ky + 1, "BACK", C_TEXT_DIM);
}

/* The busy overlay. There is deliberately no percentage and no countdown: the core
 * cannot know how long the host will take to start an emulator, and Phantom Arcade's
 * original fixed delay is exactly the guess this integration removes. The launcher
 * simply waits, and the first arriving frame tears this screen down (groovy.cpp's
 * setInit path), so the transition is driven by the real event. */
static void draw_busy(ph_ui *u)
{
	LAYOUT;
	int bw = 560;
	int bh = 150;
	int span = u->w - L.l - L.r;
	if (bw > span) bw = span;
	int bx = L.l + (span - bw) / 2;
	int by = u->safe.t + (u->h - u->safe.t - u->safe.b - bh) / 2;
	if (by < 0) by = 0;

	OPAQUE(u, box(u, bx, by, bw, bh, 0x161826, C_AMBER, C_AMBER_DK));

	text(u, bx + 20, by + 16, "STARTING STREAM", C_AMBER, 2);
	text_n_clip(u, bx + 20, by + 56, u->busy_title, C_TEXT, bw - 40);
	text_n_clip(u, bx + 20, by + 80, u->status, C_CYAN, bw - 44 - 7 * PH_NARROW_W);

	/* Elapsed time, not a progress bar. Starting an emulator takes as long as it takes -
	 * RPCS3 compiling shaders on a first boot can run for minutes - and a bar that
	 * pretends to know how far along it is would be a lie. A clock counting up says the
	 * same useful thing ("it is still working") and is true.
	 *
	 * phantom.cpp redraws this once a second rather than every tick. That matters: at
	 * the full field rate it was a megabyte of uncached writes into the DDR region the
	 * arriving video is about to fill, 60 times a second, for the whole wait. Once a
	 * second it is nothing, and a clock does not need to tick faster than that. */
	uint32_t secs = (u->busy_t0 && u->tick_ms >= u->busy_t0) ? (u->tick_ms - u->busy_t0) / 1000 : 0;
	char clock[16];
	snprintf(clock, sizeof(clock), "%u:%02u", secs / 60, secs % 60);
	text_n_right(u, bx + bw - 20, by + 80, clock, C_TEXT_DIM);

	/* Activity strip: one cell per second, cycling. Reads as "working" without
	 * claiming to measure anything. */
	int cells = 24;
	int cw = (bw - 40) / cells;
	int lit = (int)(secs % (uint32_t)cells);
	for (int i = 0; i < cells; i++)
	{
		rect(u, bx + 20 + i * cw, by + 112, cw - 2, 6,
		     (i <= lit) ? C_AMBER : C_AMBER_DK);
	}

	badge(u, bx + 20, by + 122, C_BTN_B, 'B');
	text_n(u, bx + 44, by + 123, "CANCEL", C_TEXT_MUTE);
}

void ph_ui_render(ph_ui *u)
{
	if (!u->fb) return;

	u->alpha = 0;
	if (u->view == PH_VIEW_ADJUST)
	{
		/* The screen-adjust page is a measuring tool and stays plain: a picture behind the margins would make
		 * the edges harder to see, not easier. */
		draw_adjust(u);
		u->dirty = 0;
		return;
	}

	if (u->bg)
	{
		/* Start from the picture, then lay the panels over it translucently. */
		memcpy(u->fb, u->bg, (size_t)u->w * u->h * 3);
		u->alpha = u->bg_alpha > 0 ? u->bg_alpha : PH_BG_PANEL_ALPHA;
	}
	else
	{
		rect(u, 0, 0, u->w, u->h, C_BG);
	}

	if (u->view == PH_VIEW_SETTINGS)
	{
		draw_settings(u);
		u->dirty = 0;
		return;
	}

	draw_header(u);
	draw_tabs(u);
	draw_list(u);
	draw_detail(u);
	draw_footer(u);

	if (u->view == PH_VIEW_BUSY) draw_busy(u);

	u->dirty = 0;
}
