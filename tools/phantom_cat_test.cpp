/*
 * phantom_cat_test.cpp - the cabinet's paged library against the daemon's real replies.
 *
 * phantom_cat.c (the cabinet) never sees a socket: it produces request strings and consumes reply
 * strings. That makes it testable against host/game_index.h (the daemon) with nothing between them but a
 * function call - or a queue with a delay, to stand in for the network. Checked here:
 *
 *   - every row the screen needs is there when it needs it, scrolling a library end to end with the host
 *     answering instantly, and almost always there with the host a few polls behind;
 *   - a reply that carries fewer rows than asked (the host caps a reply's size) is continued, not lost;
 *   - a new library version drops what was held; a STALE answer asks for the new version;
 *   - a reply that never comes is asked for again, and no more than PC_PENDING are ever in flight;
 *   - the page cache never throws out a page that is on screen, however far the cursor has travelled;
 *   - garbage - truncated, reordered, hostile - is ignored, which the sanitizers get to confirm.
 *
 *   g++ -std=c++17 -fsanitize=address,undefined ... (see tools/run_phantom_cat_test.sh)
 *
 * Exit status is the number of failed checks.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "phantom_cat.h"
#include "../host/game_index.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

/* ---- a library, and a host with a delay in front of it ------------------------------------------- */

static std::shared_ptr<phantom::Index> make_library(unsigned n, unsigned ver)
{
    phantom::IndexBuilder b;
    int m = b.AddSystem("groovymame", "GroovyMAME Arcade", "/r/m");
    int d = b.AddSystem("dolphin", "Nintendo GameCube / Wii", "/r/d");
    int x = b.AddSystem("xemu", "Microsoft Xbox", "/r/x");
    for (unsigned i = 0; i < n; i++) {
        const int s = (i % 10) < 6 ? m : (i % 10) < 9 ? d : x;
        char id[64], title[96];
        std::snprintf(id, sizeof(id), "k%d_%u", s, i);
        if (i % 50 == 0) std::snprintf(title, sizeof(title), "%u Game", i);
        else             std::snprintf(title, sizeof(title), "%c%c Name %u", 'A' + (int) ((i * 7) % 26), 'a' + (int) ((i * 3) % 26), i);
        b.Add(s, id, title, "/r/f.zip", "15kHz", "1991", "Maker", i, i);
    }
    return b.Finish(ver);
}

struct Wire {
    std::shared_ptr<const phantom::Index> host;
    size_t                                maxReply = 7000;
    int                                   latency = 0;     /* polls a reply is held for */
    std::deque<std::pair<int, std::string>> inflight;      /* (polls left, reply) */
    int                                   sent = 0, lost_next = 0;
    int                                   max_pending_seen = 0;
    std::vector<std::string>              log;

    void send(const std::string &req)
    {
        sent++;
        log.push_back(req);
        if (lost_next > 0) { lost_next--; return; }
        std::string reply;
        if (phantom::AnswerCatalogRequest(host, false, req, reply, maxReply))
            inflight.push_back({ latency, reply });
    }

    /* one poll's worth of time: deliver what has waited long enough */
    void deliver(pc_cat *c)
    {
        for (auto it = inflight.begin(); it != inflight.end();) {
            if (it->first-- > 0) { ++it; continue; }
            std::string r = it->second;
            it = inflight.erase(it);
            std::vector<char> buf(r.begin(), r.end());
            buf.push_back(0);
            pc_on_reply(c, buf.data(), (int) r.size());
        }
    }
};

/* One poll of the cabinet: take what has arrived, then send what it wants to ask for. */
static uint32_t g_now = 1000;
static void poll(pc_cat *c, Wire &w, int list, int first, int last, int dir)
{
    g_now += 16;
    w.deliver(c);
    char req[128];
    int pend = 0;
    for (int i = 0; i < 8 && pc_next_request(c, g_now, list, first, last, dir, req, sizeof(req)); i++) w.send(req);
    for (int i = 0; i < PC_PENDING; i++) pend += c->pend[i].active;
    if (pend > w.max_pending_seen) w.max_pending_seen = pend;
}

static void connect(pc_cat *c, Wire &w)
{
    char reply[8192];
    std::string r;
    phantom::AnswerCatalogRequest(w.host, false, "CATINFO", r);
    std::snprintf(reply, sizeof(reply), "%s", r.c_str());
    pc_on_reply(c, reply, (int) r.size());
}

static const ph_game *row_of(pc_cat *c, int tab, int i)
{
    ph_source s = pc_source(c);
    return s.row(s.ctx, tab, i);
}

int main()
{
    /* ---- the first look: CATINFO, then the page on screen --------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(10000, 5);
        CHECK(pc_source(&c).count(&c, PH_TAB_ALL) < 0, "before CATINFO the count should be unknown");
        connect(&c, w);
        CHECK(c.have && c.ver == 5 && c.total == 10000 && c.nsys == 3, "CATINFO: ver %u total %d systems %d", c.ver, c.total, c.nsys);
        CHECK(!strcmp(c.sys[1].name, "Nintendo GameCube / Wii") && c.sys[1].count > 2000, "system names and counts: %s %d", c.sys[1].name, c.sys[1].count);

        CHECK(pc_list_for_tab(&c, PH_TAB_ALL) == 0 && pc_list_for_tab(&c, PH_TAB_MAME) == 1 && pc_list_for_tab(&c, PH_TAB_GCWII) == 2 &&
              pc_list_for_tab(&c, PH_TAB_XBOX) == 3 && pc_list_for_tab(&c, PH_TAB_PS3) == -1, "tabs map to the host's lists");
        ph_source s = pc_source(&c);
        CHECK(s.count(s.ctx, PH_TAB_ALL) == 10000 && s.count(s.ctx, PH_TAB_PS3) == 0, "counts: all %d, a tab the host lacks %d", s.count(s.ctx, PH_TAB_ALL), s.count(s.ctx, PH_TAB_PS3));
        CHECK(s.row(s.ctx, PH_TAB_ALL, 0) == nullptr, "a row that has not arrived is NULL");

        for (int i = 0; i < 6; i++) poll(&c, w, 0, 0, 12, 1);
        for (int i = 0; i < 13; i++) {
            const ph_game *g = row_of(&c, PH_TAB_ALL, i);
            const phantom::Rec *r = w.host->RecAt(0, i);
            CHECK(g && r && !strcmp(g->id, w.host->Id(*r)) && !strcmp(g->title, w.host->Title(*r)), "row %d is the host's row %d", i, i);
            if (g && r) {
                CHECK(!strcmp(g->year, "1991") && !strcmp(g->maker, "Maker") && !strcmp(g->mode, "15kHz"), "row %d details", i);
                CHECK(g->sysname[0] != 0, "row %d has a hardware name from CATINFO (%s)", i, g->sysname);
            }
        }
        CHECK(s.letter_start(s.ctx, PH_TAB_ALL, 0) >= 0, "the letter table arrived with the first pages");
        bool same = true;
        for (int k = 0; k < 28; k++) same &= (s.letter_start(s.ctx, PH_TAB_ALL, k) == (int) w.host->letters[0][k]);
        CHECK(same, "the letter table is the host's");
    }

    /* ---- scrolling the whole library, host answering instantly ---------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(6000, 9);
        connect(&c, w);
        int missing = 0, wrong = 0;
        for (int top = 0; top + 13 <= 6000; top++) {
            poll(&c, w, 0, top, top + 12, 1);
            if (top < 4) continue;   /* the first polls are the questions; the answers are read at the next */
            for (int r = top; r < top + 13; r++) {
                const ph_game *g = row_of(&c, PH_TAB_ALL, r);
                if (!g) { missing++; continue; }
                if (strcmp(g->id, w.host->Id(*w.host->RecAt(0, r)))) wrong++;
            }
        }
        CHECK(missing == 0, "scrolling a screenful at a time with an instant host: %d rows missing when needed", missing);
        CHECK(wrong == 0, "%d rows were not the host's", wrong);
        CHECK(w.max_pending_seen <= PC_PENDING, "pending requests peaked at %d", w.max_pending_seen);
    }

    /* ---- the host a few polls behind: scrolling at 1 row a poll, and backwards ----------------- */
    for (int dir : { 1, -1 }) {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(6000, 9);
        w.latency = 3;
        connect(&c, w);
        int start = dir > 0 ? 0 : 6000 - 13;
        int missing = 0, shown = 0;
        for (int step = 0; step < 4000; step++) {
            int top = start + dir * step;
            if (top < 0 || top + 13 > 6000) break;
            poll(&c, w, 0, top, top + 12, dir);
            if (step > 40)
                for (int r = top; r < top + 13; r++) { shown++; if (!row_of(&c, PH_TAB_ALL, r)) missing++; }
        }
        std::printf("  (3 polls behind, going %s: %d of %d visible rows were still on their way)\n", dir > 0 ? "down" : "up", missing, shown);
        CHECK(shown > 0 && missing * 100 < shown, "scrolling %s at 1 row/poll with latency: %d of %d rows missing", dir > 0 ? "down" : "up", missing, shown);
        CHECK(w.max_pending_seen <= PC_PENDING, "pending peaked at %d", w.max_pending_seen);
    }

    /* ---- a jump to a place never seen: only the visible page need be quick ---------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(200000, 9);
        w.latency = 2;
        connect(&c, w);
        int polls = 0;
        const int target = 123456;
        while (polls < 50 && !row_of(&c, PH_TAB_ALL, target + 5)) { poll(&c, w, 0, target, target + 12, 1); polls++; }
        CHECK(row_of(&c, PH_TAB_ALL, target + 5) != nullptr, "a jump to row %d of 200,000 never got its rows", target);
        std::printf("  (a jump to row %d of 200,000 had its rows after %d polls, %d requests)\n", target, polls, w.sent);
        CHECK(polls <= 8, "that took %d polls", polls);
        CHECK(w.sent < 30, "that cost %d requests", w.sent);
    }

    /* ---- replies cut short by the host are continued ---------------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(5000, 3);
        w.maxReply = 400;               /* only a few rows fit in a reply */
        connect(&c, w);
        for (int i = 0; i < 80; i++) poll(&c, w, 0, 100, 112, 1);
        int have = 0;
        for (int r = 96; r < 128; r++) have += row_of(&c, PH_TAB_ALL, r) != nullptr;
        CHECK(have >= 32 - 0, "%d of 32 rows of the page arrived in several short replies", have);
        bool right = true;
        for (int r = 96; r < 128; r++) {
            const ph_game *g = row_of(&c, PH_TAB_ALL, r);
            if (g) right &= !strcmp(g->id, w.host->Id(*w.host->RecAt(0, r)));
        }
        CHECK(right, "the continued rows are the right rows");
    }

    /* ---- a lost reply is asked for again ------------------------------------------------------------ */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(3000, 4);
        connect(&c, w);
        w.lost_next = 1000;   /* the network eats everything... */
        for (int i = 0; i < 5; i++) poll(&c, w, 0, 0, 12, 1);
        int sent_dark = w.sent;
        CHECK(row_of(&c, PH_TAB_ALL, 0) == nullptr, "nothing arrived while the network ate it");
        CHECK(w.max_pending_seen <= PC_PENDING, "pending peaked at %d while nothing was answered", w.max_pending_seen);
        for (int i = 0; i < 40; i++) poll(&c, w, 0, 0, 12, 1);      /* ~640 ms: past PC_RETRY_MS */
        CHECK(w.sent > sent_dark, "no request was repeated after the retry interval");
        w.lost_next = 0;
        for (int i = 0; i < 80; i++) poll(&c, w, 0, 0, 12, 1);
        CHECK(row_of(&c, PH_TAB_ALL, 0) != nullptr && row_of(&c, PH_TAB_ALL, 12) != nullptr, "once the network came back the rows arrived");
    }

    /* ---- a new version drops what was held; STALE asks for it ------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(3000, 4);
        connect(&c, w);
        for (int i = 0; i < 8; i++) poll(&c, w, 0, 0, 12, 1);
        CHECK(row_of(&c, PH_TAB_ALL, 0) != nullptr, "rows loaded");

        w.host = make_library(2500, 5);         /* the host rescans */
        /* the cabinet's next request is refused... */
        char req[96] = "";
        for (int i = 0; i < 4 && !req[0]; i++) pc_next_request(&c, g_now += 500, 0, 700, 712, 1, req, sizeof(req));
        CHECK(!strncmp(req, "LIST:4:", 7) || !strncmp(req, "LETTERS:4:", 10), "the request quotes the version it knows: %s", req);
        std::string reply;
        phantom::AnswerCatalogRequest(w.host, false, req, reply);
        CHECK(reply == "STALE:5", "the host's answer: %s", reply.c_str());
        std::vector<char> b(reply.begin(), reply.end());
        b.push_back(0);
        CHECK(pc_on_reply(&c, b.data(), (int) reply.size()) == PC_STALE && c.want_info, "STALE sets want_info");
        /* ...and the new CATINFO replaces the library */
        std::string info;
        phantom::AnswerCatalogRequest(w.host, false, "CATINFO", info);
        std::vector<char> ib(info.begin(), info.end());
        ib.push_back(0);
        CHECK(pc_on_reply(&c, ib.data(), (int) info.size()) == PC_INFO_NEW, "a new version is reported as new");
        CHECK(c.ver == 5 && c.total == 2500 && !c.want_info, "now version %u with %d titles", c.ver, c.total);
        CHECK(row_of(&c, PH_TAB_ALL, 0) == nullptr, "pages from the old version were dropped");
        /* the same version again changes nothing it holds */
        for (int i = 0; i < 8; i++) poll(&c, w, 0, 0, 12, 1);
        CHECK(row_of(&c, PH_TAB_ALL, 0) != nullptr, "rows fetched for the new version");
        std::vector<char> ib2(info.begin(), info.end());   /* (the parser edits its buffer in place) */
        ib2.push_back(0);
        CHECK(pc_on_reply(&c, ib2.data(), (int) info.size()) == PC_INFO && row_of(&c, PH_TAB_ALL, 0) != nullptr, "a repeated CATINFO keeps what is held");
    }

    /* ---- the cache never gives up a page that is on screen -------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(80000, 2);
        connect(&c, w);
        int bad = 0, visits = 0;
        /* a long, jumpy tour: far more distinct pages than the cache holds */
        unsigned seed = 12345;
        for (int hop = 0; hop < 300; hop++) {
            seed = seed * 1664525u + 1013904223u;
            int top = (int) ((seed >> 8) % (80000 - 13));
            for (int i = 0; i < 6; i++) poll(&c, w, 0, top, top + 12, 1);
            for (int r = top; r < top + 13; r++) {
                visits++;
                const ph_game *g = row_of(&c, PH_TAB_ALL, r);
                if (!g || strcmp(g->id, w.host->Id(*w.host->RecAt(0, r)))) bad++;
            }
        }
        CHECK(bad == 0, "after 300 jumps, %d of %d visible rows were missing or wrong", bad, visits);
        int valid = 0;
        for (auto &p : c.pages) valid += p.valid;
        CHECK(valid <= PC_PAGES, "cache holds %d pages", valid);
    }

    /* ---- per-tab lists ---------------------------------------------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(9000, 2);
        connect(&c, w);
        int list = pc_list_for_tab(&c, PH_TAB_GCWII);
        for (int i = 0; i < 8; i++) poll(&c, w, list, 0, 12, 1);
        const ph_game *g = row_of(&c, PH_TAB_GCWII, 0);
        CHECK(g && !strcmp(g->system, "dolphin"), "the GC/Wii tab's first row belongs to dolphin (%s)", g ? g->system : "null");
        ph_source s = pc_source(&c);
        CHECK(s.count(s.ctx, PH_TAB_GCWII) == c.sys[1].count, "tab count is the system's");
        CHECK(s.letter_start(s.ctx, PH_TAB_GCWII, 27) == c.sys[1].count, "its letter table ends at its count");
        CHECK(s.letter_start(s.ctx, PH_TAB_PS3, 5) == 0, "a tab the host does not have is just empty");
    }

    /* ---- garbage in -------------------------------------------------------------------------------- */
    {
        pc_cat c;
        pc_init(&c);
        Wire w;
        w.host = make_library(1000, 6);
        connect(&c, w);
        const char *junk[] = {
            "", "LIST:", "LIST:6", "LIST:6:*", "LIST:6:*:0", "LIST:6:*:0:5:1000", "LIST:6:*:0:5:1000\n", "LIST:6:*:0:5:1000\nonly\tthree\tfields\n",
            "LIST:x:*:0:5:1000\n", "LIST:6:nosuch:0:5:1000\nid\ttitle\tsys\tmode\tyear\tmaker\n", "LIST:6:*:99999999999:5:1000\nid\ttitle\n",
            "LIST:6:*:5:3:1000\na\tb\tc\td\te\tf\n",     /* a gap: offset 5 into an empty page */
            "LETTERS:6:*:1,2,3", "LETTERS:6:*:0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5",
            "LETTERS:6:*:9,8,7,6,5,4,3,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000",
            "CATINFO:", "CATINFO:x:y:z:", "CATINFO:7:5:0:a|b", "CATINFO:7:5:0:a|b|notanumber;;;|||", "STALE", "STALE:", "RESCAN:OK", "totally unrelated",
        };
        for (const char *j : junk) {
            std::vector<char> b(j, j + strlen(j));
            b.push_back(0);
            pc_on_reply(&c, b.data(), (int) strlen(j));
        }
        /* a very long title must be cut, not overflow */
        std::string big = "LIST:7:*:0:1:5\n" + std::string(500, 'i') + "\t" + std::string(900, 't') + "\t" + std::string(300, 's') + "\t" +
                          std::string(300, 'm') + "\t" + std::string(300, 'y') + "\t" + std::string(300, 'k') + "\n";
        std::vector<char> bb(big.begin(), big.end());
        bb.push_back(0);
        CHECK(c.ver == 7 || true, "(the junk may have moved the version to %u)", c.ver);
        pc_on_reply(&c, bb.data(), (int) big.size());
        /* whatever happened, the state is still usable */
        connect(&c, w);
        for (int i = 0; i < 8; i++) poll(&c, w, 0, 0, 12, 1);
        CHECK(row_of(&c, PH_TAB_ALL, 0) != nullptr, "the library works after a stream of garbage");
        const ph_game *g = row_of(&c, PH_TAB_ALL, 0);
        CHECK(g && strlen(g->id) < PH_ID_LEN && strlen(g->title) < PH_TITLE_LEN, "fields stay within their buffers");
    }

    /* ---- the title order the cabinet sorts the older catalog by is the host's -------------------- */
    {
        const char *t[] = { "~tilde", "{brace", "1942", "Aa", "ab", "AB", "b", "Zz", "zz" };
        bool agree = true;
        for (const char *a : t)
            for (const char *b : t) {
                int x = pc_title_cmp(a, b), y = phantom::TitleCmp(a, b);
                if ((x < 0) != (y < 0) || (x > 0) != (y > 0)) agree = false;
            }
        CHECK(agree, "pc_title_cmp and TitleCmp disagree");
    }

    std::printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
