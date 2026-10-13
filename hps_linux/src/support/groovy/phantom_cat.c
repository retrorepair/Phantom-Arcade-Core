/*
 * phantom_cat.c - see phantom_cat.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phantom_cat.h"

/* ---- the host's title order ---------------------------------------------------------------------- */

static int order_key(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
	return (c >= 'a' && c <= 'z') ? 256 + c : c;
}

int pc_title_cmp(const char *a, const char *b)
{
	for (;; a++, b++)
	{
		int ka = order_key((unsigned char)*a), kb = order_key((unsigned char)*b);
		if (ka != kb) return ka < kb ? -1 : 1;
		if (!*a) return 0;
	}
}

/* ---- small parsing helpers ----------------------------------------------------------------------- */

static void copy_str(char *dst, int dstsz, const char *src)
{
	snprintf(dst, (size_t)dstsz, "%s", src ? src : "");
}

/* Split `s` at `delim` in place: returns the field and moves *s past the delimiter (to NULL after the last). */
static char *next_field(char **s, char delim)
{
	char *f = *s;
	if (!f) return 0;
	char *d = strchr(f, delim);
	if (d)
	{
		*d = 0;
		*s = d + 1;
	}
	else
	{
		*s = 0;
	}
	return f;
}

static int to_uint(const char *s, uint32_t *out)
{
	if (!s || !*s) return 0;
	char *end = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (!end || *end || v > 0xFFFFFFFFull) return 0;
	*out = (uint32_t)v;
	return 1;
}

/* ---- state --------------------------------------------------------------------------------------- */

void pc_reset(pc_cat *c)
{
	memset(c, 0, sizeof(*c));
}

void pc_init(pc_cat *c)
{
	pc_reset(c);
}

static void forget_contents(pc_cat *c)
{
	memset(c->pages, 0, sizeof(c->pages));
	memset(c->pend, 0, sizeof(c->pend));
	memset(c->letters_have, 0, sizeof(c->letters_have));
	memset(c->letters_asked, 0, sizeof(c->letters_asked));
}

int pc_list_count(const pc_cat *c, int list)
{
	if (list == 0) return c->total;
	if (list > 0 && list <= c->nsys) return c->sys[list - 1].count;
	return 0;
}

const char *pc_list_key(const pc_cat *c, int list)
{
	if (list > 0 && list <= c->nsys) return c->sys[list - 1].key;
	return "*";
}

static int list_by_key(const pc_cat *c, const char *key)
{
	if (!strcmp(key, "*")) return 0;
	for (int s = 0; s < c->nsys; s++)
		if (!strcmp(c->sys[s].key, key)) return s + 1;
	return -1;
}

int pc_list_for_tab(const pc_cat *c, int tab)
{
	if (tab == PH_TAB_ALL) return 0;
	for (int s = 0; s < c->nsys; s++)
		if (ph_tab_matches(tab, c->sys[s].key)) return s + 1;
	return -1;
}

/* ---- the pages ------------------------------------------------------------------------------------ */

static pc_page *find_page(pc_cat *c, int list, int page)
{
	for (int i = 0; i < PC_PAGES; i++)
		if (c->pages[i].valid && c->pages[i].list == list && c->pages[i].page == page) return &c->pages[i];
	return 0;
}

static pc_page *alloc_page(pc_cat *c, int list, int page)
{
	pc_page *victim = 0;
	for (int i = 0; i < PC_PAGES; i++)
	{
		if (!c->pages[i].valid) { victim = &c->pages[i]; break; }
		if (!victim || c->pages[i].used < victim->used) victim = &c->pages[i];
	}
	victim->valid = 1;
	victim->list = list;
	victim->page = page;
	victim->n = 0;
	victim->used = ++c->tick;
	return victim;
}

/* ---- replies ------------------------------------------------------------------------------------- */

static int parse_info(pc_cat *c, char *p)
{
	/* <ver>:<total>:<scanning>:<key>|<name>|<count>;... */
	char *fver = next_field(&p, ':');
	char *ftot = next_field(&p, ':');
	char *fscan = next_field(&p, ':');
	char *systems = p ? p : (char *)"";
	uint32_t ver, total, scan;
	if (!to_uint(fver, &ver) || !to_uint(ftot, &total) || !to_uint(fscan, &scan)) return PC_NONE;

	int fresh = !c->have || c->ver != ver;
	if (fresh)
	{
		forget_contents(c);
		c->nsys = 0;
		char *s = systems;
		while (s && *s && c->nsys < PC_MAX_SYS)
		{
			char *rec = next_field(&s, ';');
			char *key = next_field(&rec, '|');
			char *name = next_field(&rec, '|');
			char *cnt = rec;
			uint32_t n;
			if (!key || !*key || !name || !to_uint(cnt, &n)) continue;
			pc_sys *d = &c->sys[c->nsys++];
			copy_str(d->key, sizeof(d->key), key);
			copy_str(d->name, sizeof(d->name), name);
			d->count = (int)n;
		}
		c->ver = ver;
		c->total = (int)total;
		c->have = 1;
	}
	c->scanning = scan ? 1 : 0;
	c->want_info = 0;
	return fresh ? PC_INFO_NEW : PC_INFO;
}

static int parse_letters(pc_cat *c, char *p)
{
	/* <ver>:<list>:<s0>,...,<s27> */
	char *fver = next_field(&p, ':');
	char *flist = next_field(&p, ':');
	uint32_t ver;
	if (!to_uint(fver, &ver) || !flist || !p) return PC_NONE;
	if (!c->have || ver != c->ver) { c->want_info = 1; return PC_STALE; }

	int list = list_by_key(c, flist);
	if (list < 0) return PC_NONE;

	int vals[28];
	char *s = p;
	for (int k = 0; k < 28; k++)
	{
		char *f = next_field(&s, ',');
		uint32_t v;
		if (!f || !to_uint(f, &v)) return PC_NONE;
		vals[k] = (int)v;
	}
	/* A table that does not add up is not used: a letter jump to a wrong place is worse than none. */
	if (vals[27] != pc_list_count(c, list)) return PC_NONE;
	for (int k = 0; k < 27; k++)
		if (vals[k] > vals[k + 1] || vals[k] < 0) return PC_NONE;

	memcpy(c->letters[list], vals, sizeof(vals));
	c->letters_have[list] = 1;
	return PC_LETTERS_IN;
}

static void fill_game(const pc_cat *c, ph_game *g, char *row)
{
	memset(g, 0, sizeof(*g));
	char *s = row;
	char *id = next_field(&s, '\t');
	char *title = next_field(&s, '\t');
	char *sys = next_field(&s, '\t');
	char *mode = next_field(&s, '\t');
	char *year = next_field(&s, '\t');
	char *maker = s ? s : (char *)"";

	copy_str(g->id, sizeof(g->id), id);
	copy_str(g->title, sizeof(g->title), title && *title ? title : id);
	copy_str(g->system, sizeof(g->system), sys);
	copy_str(g->mode, sizeof(g->mode), mode);
	copy_str(g->year, sizeof(g->year), year);
	copy_str(g->maker, sizeof(g->maker), maker);

	/* the hardware name shown beside the title comes from CATINFO, once per system rather than once per row */
	for (int i = 0; i < c->nsys; i++)
		if (g->system[0] && !strcmp(c->sys[i].key, g->system)) { copy_str(g->sysname, sizeof(g->sysname), c->sys[i].name); break; }
}

static int parse_list(pc_cat *c, char *p)
{
	/* <ver>:<list>:<offset>:<n>:<total>\n then n rows */
	char *nl = strchr(p, '\n');
	if (!nl) return PC_NONE;
	*nl = 0;
	char *rows = nl + 1;

	char *fver = next_field(&p, ':');
	char *flist = next_field(&p, ':');
	char *foff = next_field(&p, ':');
	char *fn = next_field(&p, ':');
	uint32_t ver, off, n;
	if (!to_uint(fver, &ver) || !flist || !to_uint(foff, &off) || !to_uint(fn, &n)) return PC_NONE;
	if (!c->have || ver != c->ver) { c->want_info = 1; return PC_STALE; }

	int list = list_by_key(c, flist);
	if (list < 0) return PC_NONE;

	/* whatever happens to the rows, this request has been answered */
	for (int i = 0; i < PC_PENDING; i++)
		if (c->pend[i].active && c->pend[i].list == list && c->pend[i].offset == (int)off) c->pend[i].active = 0;

	int total = pc_list_count(c, list);
	if ((int)off >= total || n == 0) return PC_NONE;

	int pageno = (int)off / PC_PAGE;
	int slot = (int)off % PC_PAGE;
	pc_page *pg = find_page(c, list, pageno);
	if (!pg) pg = alloc_page(c, list, pageno);
	if (slot > pg->n) return PC_NONE;         /* would leave a gap; the next request starts from pg->n */
	pg->used = ++c->tick;

	int got = 0;
	char *line = rows;
	while (line && *line && (uint32_t)got < n && slot + got < PC_PAGE)
	{
		char *e = strchr(line, '\n');
		if (e) *e = 0;
		fill_game(c, &pg->rows[slot + got], line);
		got++;
		line = e ? e + 1 : 0;
	}
	if (slot + got > pg->n) pg->n = slot + got;
	return got ? PC_PAGE_IN : PC_NONE;
}

int pc_on_reply(pc_cat *c, char *buf, int len)
{
	(void)len;
	if (!strncmp(buf, "CATINFO:", 8)) return parse_info(c, buf + 8);
	if (!strncmp(buf, "LETTERS:", 8)) return parse_letters(c, buf + 8);
	if (!strncmp(buf, "LIST:", 5))    return parse_list(c, buf + 5);
	if (!strncmp(buf, "STALE:", 6))
	{
		c->want_info = 1;
		return PC_STALE;
	}
	return PC_NONE;
}

/* ---- what to ask for ----------------------------------------------------------------------------- */

static int free_pending_slot(pc_cat *c, uint32_t now, int list, int offset, int *already)
{
	int slot = -1, active = 0;
	*already = 0;
	for (int i = 0; i < PC_PENDING; i++)
	{
		pc_pend *p = &c->pend[i];
		if (p->active && (int32_t)(now - p->sent) >= PC_RETRY_MS) p->active = 0;   /* presumed lost */
		if (p->active)
		{
			active++;
			if (p->list == list && p->offset == offset) *already = 1;
		}
		else if (slot < 0)
		{
			slot = i;
		}
	}
	(void)active;
	return slot;
}

int pc_next_request(pc_cat *c, uint32_t now, int list, int first, int last, int dir, char *out, int outsz)
{
	if (!c->have || list < 0) return 0;
	int total = pc_list_count(c, list);
	if (total <= 0) return 0;

	/* the letter table is wanted before the first press of L2/R2, not after it */
	if (!c->letters_have[list] && (!c->letters_asked[list] || (int32_t)(now - c->letters_sent[list]) >= PC_RETRY_MS))
	{
		c->letters_asked[list] = 1;
		c->letters_sent[list] = now;
		snprintf(out, (size_t)outsz, "LETTERS:%u:%s", (unsigned)c->ver, pc_list_key(c, list));
		return 1;
	}

	int lastpage = (total - 1) / PC_PAGE;
	if (first < 0) first = 0;
	if (last >= total) last = total - 1;
	if (last < first) last = first;
	int p0 = first / PC_PAGE, p1 = last / PC_PAGE;
	if (p0 > lastpage) p0 = lastpage;
	if (p1 > lastpage) p1 = lastpage;

	/* In the order they will be wanted: what is on screen, then a page behind and PC_AHEAD in front when moving
	 * down (the other way round when moving up). */
	int want[2 + 8 + 2 * PC_AHEAD + 4];
	int nw = 0;
	for (int p = p0; p <= p1 && nw < 8; p++) want[nw++] = p;
	int ahead = (dir < 0) ? 1 : PC_AHEAD, behind = (dir < 0) ? PC_AHEAD : 1;
	for (int k = 1; k <= (ahead > behind ? ahead : behind); k++)
	{
		if (dir < 0)
		{
			if (k <= behind && p0 - k >= 0) want[nw++] = p0 - k;
			if (k <= ahead && p1 + k <= lastpage) want[nw++] = p1 + k;
		}
		else
		{
			if (k <= ahead && p1 + k <= lastpage) want[nw++] = p1 + k;
			if (k <= behind && p0 - k >= 0) want[nw++] = p0 - k;
		}
	}

	for (int i = 0; i < nw; i++)
	{
		int p = want[i];
		int base = p * PC_PAGE;
		int expect = total - base < PC_PAGE ? total - base : PC_PAGE;
		pc_page *pg = find_page(c, list, p);
		int have = 0;
		if (pg)
		{
			pg->used = ++c->tick;     /* a page that is wanted must not be the one evicted to make room for the next */
			have = pg->n;
		}
		if (have >= expect) continue;

		int offset = base + have;
		int already;
		int slot = free_pending_slot(c, now, list, offset, &already);
		if (already) continue;
		if (slot < 0) return 0;       /* all in flight: wait for an answer before asking more */

		c->pend[slot].active = 1;
		c->pend[slot].list = list;
		c->pend[slot].offset = offset;
		c->pend[slot].sent = now;
		snprintf(out, (size_t)outsz, "LIST:%u:%s:%d:%d", (unsigned)c->ver, pc_list_key(c, list), offset, expect - have);
		return 1;
	}
	return 0;
}

/* ---- the renderer's source ----------------------------------------------------------------------- */

static int s_count(void *ctx, int tab)
{
	pc_cat *c = (pc_cat *)ctx;
	if (!c->have) return -1;
	int list = pc_list_for_tab(c, tab);
	return list < 0 ? 0 : pc_list_count(c, list);
}

static const ph_game *s_row(void *ctx, int tab, int index)
{
	pc_cat *c = (pc_cat *)ctx;
	if (!c->have || index < 0) return 0;
	int list = pc_list_for_tab(c, tab);
	if (list < 0 || index >= pc_list_count(c, list)) return 0;
	pc_page *pg = find_page(c, list, index / PC_PAGE);
	int slot = index % PC_PAGE;
	if (!pg || slot >= pg->n) return 0;
	pg->used = ++c->tick;
	return &pg->rows[slot];
}

static int s_letter(void *ctx, int tab, int k)
{
	pc_cat *c = (pc_cat *)ctx;
	if (!c->have || k < 0 || k > 27) return -1;
	int list = pc_list_for_tab(c, tab);
	if (list < 0) return 0;                           /* no such tab on this host: an empty list */
	if (!c->letters_have[list]) return -1;
	return c->letters[list][k];
}

ph_source pc_source(pc_cat *c)
{
	ph_source s;
	s.count = s_count;
	s.row = s_row;
	s.letter_start = s_letter;
	s.ctx = c;
	return s;
}
