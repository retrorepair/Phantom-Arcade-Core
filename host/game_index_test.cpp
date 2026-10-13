/*
 * game_index_test.cpp - the paged catalog at the size it has to survive.
 *
 * Builds a synthetic library of hundreds of thousands of games and checks what a cabinet relies on:
 * every list sorted, every letter's titles one run that the letter table points at exactly, pages that
 * tile the list with nothing missing or repeated, launches that resolve to the right game, a cache
 * file that comes back identical, and requests that are refused (not mis-answered) when the version
 * has moved on. It also times the operations that have to be fast.
 *
 *   cl /nologo /std:c++17 /EHsc /O2 game_index_test.cpp           (Windows)
 *   g++ -std=c++17 -O2 game_index_test.cpp -o game_index_test      (anywhere else)
 *
 * Exit status is the number of failed checks.
 */
#include <chrono>
#include <cstdio>
#include <set>

#include "game_index.h"

using namespace phantom;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Deterministic, so a failure is the same failure next time.
static uint64_t rng_state = 88172645463325252ull;
static uint32_t rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

static std::string make_title(uint32_t i) {
    static const char* words[] = { "Super", "Street", "Fighter", "Metal", "Slug", "Dragon", "Ninja", "Turbo", "King",
                                   "Of", "The", "Fighters", "Space", "Invaders", "Pac", "Man", "Final", "Fantasy",
                                   "Mega", "Zelda", "Sonic", "Hedgehog", "Mario", "Kart", "Gran", "Turismo" };
    const int n = (int)(sizeof(words) / sizeof(words[0]));
    std::string t;
    const int count = 1 + (int)(rnd() % 5);
    for (int w = 0; w < count; w++) {
        if (w) t += ' ';
        t += words[rnd() % n];
    }
    // some start with a digit, a symbol or lower case, so the '#' group and the case folding get used
    switch (rnd() % 40) {
    case 0: t = "1942 " + t; break;
    case 1: t = "~" + t; break;
    case 2: t = "{" + t; break;
    case 3: t = "'" + t; break;
    case 4: for (char& c : t) c = (char)tolower((unsigned char)c); break;
    default: break;
    }
    return t + " (" + std::to_string(i) + ")";   // the number keeps titles distinct, as ids are in life
}

static std::shared_ptr<Index> build(uint32_t n, uint32_t ver) {
    IndexBuilder b;
    static const char* keys[] = { "groovymame", "fbneo", "flycast", "pcsx2", "dolphin", "retroarch" };
    static const char* names[] = { "GroovyMAME Arcade", "FBNeo Arcade", "Sega Dreamcast", "Sony PlayStation 2",
                                   "Nintendo GameCube / Wii", "RetroArch" };
    for (int s = 0; s < 6; s++) b.AddSystem(keys[s], names[s], std::string("C:/roms/") + keys[s]);
    for (uint32_t i = 0; i < n; i++) {
        const int s = (int)(rnd() % 100 < 55 ? 0 : rnd() % 6);   // MAME-heavy, like a real library
        const std::string stem = "set" + std::to_string(i);
        b.Add(s, std::string(keys[s]) + "_" + stem, make_title(i), std::string("C:/roms/") + keys[s] + "/" + stem + ".zip",
              "15kHz Native", (rnd() % 3) ? std::to_string(1980 + rnd() % 30) : "", (rnd() % 4) ? "Capcom" : "", 1000 + i, 5000 + i);
    }
    return b.Finish(ver);
}

int main() {
    const uint32_t N = 300000;

    auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<Index> ix = build(N, 7);
    printf("built and sorted %u games in %.0f ms (pool %.1f MB)\n", N, ms_since(t0), ix->pool.size() / 1048576.0);

    // ---- the lists ----
    CHECK(ix->Total() == N, "total %zu", ix->Total());
    size_t sumSys = 0;
    for (size_t s = 1; s < ix->lists.size(); s++) sumSys += ix->lists[s].size();
    CHECK(sumSys == N, "system lists add up to %zu, not %u", sumSys, N);

    for (size_t l = 0; l < ix->lists.size(); l++) {
        const auto& v = ix->lists[l];
        bool sorted = true;
        for (size_t i = 1; i < v.size() && sorted; i++)
            if (TitleCmp(ix->Title(ix->recs[v[i - 1]]), ix->Title(ix->recs[v[i]])) > 0) sorted = false;
        CHECK(sorted, "list %zu is out of order", l);

        // each letter's titles are one run, and the table points at exactly its first and one past its last
        const auto& s = ix->letters[l];
        CHECK(s[kLetterSlots - 1] == v.size(), "list %zu letter total %u != %zu", l, s[kLetterSlots - 1], v.size());
        bool runs = true;
        for (size_t i = 0; i < v.size(); i++) {
            const int k = LetterOf(ix->Title(ix->recs[v[i]]));
            if (!(s[k] <= i && i < s[k + 1])) { runs = false; break; }
        }
        CHECK(runs, "list %zu: a title lies outside its letter's run", l);
        for (int k = 0; k + 1 < kLetterSlots; k++) CHECK(s[k] <= s[k + 1], "list %zu letter table not monotonic at %d", l, k);
    }
    CHECK(ix->letters[0][0] == 0, "the '#' group does not come first");
    CHECK(LetterOf("~tilde") == 0 && LetterOf("{brace") == 0 && LetterOf("1942") == 0 && LetterOf("a") == 1 && LetterOf("Z") == 26,
          "LetterOf");
    // the point of OrderKey: '{' and '~' must not sort after Z
    CHECK(TitleCmp("{x", "Aa") < 0 && TitleCmp("~x", "Aa") < 0 && TitleCmp("1942", "Aa") < 0, "symbols sort ahead of A");
    CHECK(TitleCmp("abc", "ABC") == 0 && TitleCmp("ab", "abc") < 0 && TitleCmp("Zebra", "apple") > 0, "case folding");

    // ---- pages tile the list ----
    {
        const size_t PAGE = 32;
        std::set<std::string> seen;
        size_t got = 0;
        bool order = true;
        std::string prevTitle;
        for (size_t off = 0; off < N; off += PAGE) {
            const std::string r = ListReply(*ix, "*", off, PAGE, 7000);
            const size_t nl = r.find('\n');
            CHECK(nl != std::string::npos, "no header at %zu", off);
            unsigned ver, n, total;
            char list[32];
            unsigned long o;
            if (sscanf(r.c_str(), "LIST:%u:%31[^:]:%lu:%u:%u", &ver, list, &o, &n, &total) != 5) { CHECK(0, "header '%s'", r.substr(0, nl).c_str()); break; }
            CHECK(ver == 7 && o == off && total == N, "header fields at %zu", off);
            size_t at = nl + 1;
            for (unsigned i = 0; i < n; i++) {
                const size_t e = r.find('\n', at);
                const std::string row = r.substr(at, e - at);
                at = e + 1;
                int tabs = 0;
                for (char c : row) tabs += (c == '\t');
                if (tabs != 5) { CHECK(0, "row has %d tabs: %s", tabs, row.c_str()); break; }
                const std::string id = row.substr(0, row.find('\t'));
                if (!seen.insert(id).second) CHECK(0, "id %s repeated", id.c_str());
                const std::string title = row.substr(row.find('\t') + 1, row.find('\t', row.find('\t') + 1) - row.find('\t') - 1);
                if (!prevTitle.empty() && TitleCmp(prevTitle.c_str(), title.c_str()) > 0) order = false;
                prevTitle = title;
                got++;
            }
            if (n == 0) break;
        }
        CHECK(got == N, "pages delivered %zu of %u rows", got, N);
        CHECK(seen.size() == N, "distinct ids %zu", seen.size());
        CHECK(order, "pages arrived out of order");
    }

    // a page is small enough to be one datagram's worth, and a request past the end is empty, not an error
    {
        const std::string r = ListReply(*ix, "*", 100, 128, 7000);
        CHECK(r.size() <= 7000 + 200, "a page is %zu bytes", r.size());
        const std::string past = ListReply(*ix, "*", N + 50, 32, 7000);
        CHECK(past.find(":0:") != std::string::npos && past.back() == '\n', "past the end: %s", past.c_str());
        const std::string unknown = ListReply(*ix, "nosuchsystem", 0, 32, 7000);
        CHECK(unknown.find(":0:0\n") != std::string::npos, "unknown list: %s", unknown.c_str());
    }

    // ---- the protocol entry point ----
    {
        std::shared_ptr<const Index> cix = ix;
        std::string rep;
        CHECK(AnswerCatalogRequest(cix, false, "CATINFO", rep), "CATINFO not recognised");
        CHECK(rep.rfind("CATINFO:7:300000:0:groovymame|GroovyMAME Arcade|", 0) == 0, "CATINFO: %s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, true, "CATINFO", rep) && rep.find(":300000:1:") != std::string::npos, "scanning flag");

        CHECK(AnswerCatalogRequest(cix, false, "LETTERS:7:*", rep) && rep.rfind("LETTERS:7:*:0,", 0) == 0, "LETTERS: %s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, false, "LETTERS:7:fbneo", rep) && rep.rfind("LETTERS:7:fbneo:", 0) == 0, "LETTERS fbneo");

        CHECK(AnswerCatalogRequest(cix, false, "LIST:7:*:0:8", rep) && rep.rfind("LIST:7:*:0:8:300000\n", 0) == 0, "LIST: %.40s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, false, "LIST:6:*:0:8", rep) && rep == "STALE:7", "stale list: %s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, false, "LETTERS:6:*", rep) && rep == "STALE:7", "stale letters: %s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, false, "LIST:7:*:x:8", rep) && rep == "ERR:BAD_REQUEST", "bad number: %s", rep.c_str());
        CHECK(AnswerCatalogRequest(cix, false, "LIST:7", rep) && rep == "ERR:BAD_REQUEST", "short request: %s", rep.c_str());
        CHECK(!AnswerCatalogRequest(cix, false, "STATUS", rep) && !AnswerCatalogRequest(cix, false, "LAUNCH:x", rep), "other messages are not ours");
        CHECK(AnswerCatalogRequest(std::shared_ptr<const Index>(), false, "LIST:1:*:0:8", rep) && rep == "STALE:0", "no index yet: %s", rep.c_str());
        // a request cannot ask for an unbounded page
        CHECK(AnswerCatalogRequest(cix, false, "LIST:7:*:0:4000000000", rep) && rep.size() <= 7300, "huge count: %zu bytes", rep.size());
    }

    // ---- launches resolve to the right game ----
    {
        bool ok = true;
        for (int probe = 0; probe < 2000 && ok; probe++) {
            const size_t i = rnd() % N;
            const Rec* r = ix->RecAt(0, i);
            const Rec* back = r ? ix->Find(ix->Id(*r)) : nullptr;
            ok = r && back == r;
        }
        CHECK(ok, "a game does not resolve back to itself by id");
        CHECK(ix->Find("groovymame_nosuch") == nullptr, "found a game that is not there");
        CHECK(ix->RecAt(0, N) == nullptr && ix->RecAt(99, 0) == nullptr && ix->RecAt(-1, 0) == nullptr, "out-of-range RecAt");
        // an index into a system list lands on a game of that system
        const int fb = ix->ListFor("fbneo");
        CHECK(fb > 0, "no fbneo list");
        const Rec* g = ix->RecAt(fb, 5);
        CHECK(g && ix->sys[g->sys].key == "fbneo", "fbneo list row 5 belongs to %s", g ? ix->sys[g->sys].key.c_str() : "nothing");
    }

    // ---- the cache file ----
    {
        const std::filesystem::path f = std::filesystem::temp_directory_path() / "game_index_test.bin";
        t0 = std::chrono::steady_clock::now();
        CHECK(SaveIndex(*ix, f), "save");
        const double saveMs = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        std::shared_ptr<Index> back = LoadIndex(f, 9);
        const double loadMs = ms_since(t0);
        printf("cache: %.1f MB, saved in %.0f ms, loaded and rebuilt in %.0f ms\n",
               (double)std::filesystem::file_size(f) / 1048576.0, saveMs, loadMs);
        CHECK(back != nullptr, "load failed");
        if (back) {
            CHECK(back->ver == 9 && back->recs.size() == ix->recs.size() && back->sys.size() == ix->sys.size(), "loaded shape");
            CHECK(back->lists == ix->lists, "loaded lists differ from the saved index's");
            CHECK(back->letters == ix->letters, "loaded letter tables differ");
            bool same = true;
            for (size_t i = 0; i < ix->recs.size() && same; i += 97)
                same = !strcmp(back->Id(back->recs[i]), ix->Id(ix->recs[i])) && !strcmp(back->Title(back->recs[i]), ix->Title(ix->recs[i])) &&
                       back->recs[i].size == ix->recs[i].size && back->recs[i].mtime == ix->recs[i].mtime;
            CHECK(same, "loaded records differ");
            CHECK(back->sys[1].root == ix->sys[1].root, "system root kept");
        }

        // a truncated or foreign file is refused, never half-loaded
        {
            std::ifstream in(f, std::ios::binary);
            std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            const std::filesystem::path cut = f.string() + ".cut";
            { std::ofstream o(cut, std::ios::binary); o.write(whole.data(), (std::streamsize)(whole.size() / 2)); }
            CHECK(LoadIndex(cut, 1) == nullptr, "a truncated cache loaded");
            { std::ofstream o(cut, std::ios::binary); o << "not a cache file at all, just some text of a decent length"; }
            CHECK(LoadIndex(cut, 1) == nullptr, "a foreign file loaded");
            // an offset pointing outside the pool must not survive the load
            std::string bad = whole;
            const size_t recsAt = bad.size() - ix->pool.size() - ix->recs.size() * sizeof(Rec);
            uint32_t wild = 0x7FFFFFFF;
            memcpy(&bad[recsAt], &wild, 4);
            { std::ofstream o(cut, std::ios::binary); o.write(bad.data(), (std::streamsize)bad.size()); }
            CHECK(LoadIndex(cut, 1) == nullptr, "a cache with a wild offset loaded");
            std::error_code ec;
            std::filesystem::remove(cut, ec);
            CHECK(LoadIndex(std::filesystem::temp_directory_path() / "no_such_cache.bin", 1) == nullptr, "a missing file loaded");
        }
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }

    // ---- pending and rejected records are not listed, and are not lost to the builder ----
    {
        IndexBuilder b;
        const int s = b.AddSystem("groovymame", "GroovyMAME", "C:/r");
        const uint32_t a = b.Add(s, "groovymame_a", "Alpha", "C:/r/a.zip", "", "", "", 1, 1);
        const uint32_t p = b.Add(s, "groovymame_p", "Pending", "C:/r/p.zip", "", "", "", 1, 1, kRecPending);
        const uint32_t x = b.Add(s, "groovymame_x", "Rejected", "C:/r/x.zip", "", "", "", 1, 1, kRecRejected);
        (void)a;
        auto first = b.Finish(1);
        CHECK(first->Total() == 1, "only the vetted game is listed (%zu)", first->Total());
        b.SetFlags(p, 0);
        b.SetTitle(p, "Pending, now vetted");
        b.SetMeta(p, "320x240 @ 60Hz", "1991", "Capcom");
        auto second = b.Finish(2);
        CHECK(second->Total() == 2, "the vetted game joins (%zu)", second->Total());
        CHECK(first->Total() == 1, "a published index does not change after the fact");
        const Rec* r = second->Find("groovymame_p");
        CHECK(r && !strcmp(second->Title(*r), "Pending, now vetted") && !strcmp(second->Str(r->year), "1991") &&
              !strcmp(second->Str(r->maker), "Capcom") && !strcmp(second->Str(r->mode), "320x240 @ 60Hz"), "late details");
        CHECK(second->Find("groovymame_x") == nullptr, "a rejected game is listed");
        (void)x;
    }

    // ---- text that would break the wire format is made safe ----
    {
        IndexBuilder b;
        const int s = b.AddSystem("fbneo", "FB|Neo;x", "C:/r");
        b.Add(s, "fbneo_t", "Tab\there\nand newline", "C:/r/t.zip", "mo\tde", "19\t99", "Mak\ner", 1, 1);
        auto ix2 = b.Finish(1);
        const std::string r = ListReply(*ix2, "*", 0, 8, 7000);
        int lines = 0;
        for (char c : r) lines += (c == '\n');
        CHECK(lines == 2, "a title with control characters made %d lines", lines);
        const std::string info = CatInfoReply(*ix2, false);
        CHECK(info == "CATINFO:1:1:0:fbneo|FB Neo x|1", "name made safe: %s", info.c_str());
    }

    // ---- the older cabinet path keeps every emulator, not just the first ----
    {
        const std::string json = CompactCatalogJson(*ix);
        CHECK(json.size() <= 59000, "compact catalog is %zu bytes", json.size());
        CHECK(json.rfind("{\"games\":[", 0) == 0 && json.substr(json.size() - 2) == "]}", "compact catalog is not a closed document");
        size_t games = 0;
        for (size_t p = 0; (p = json.find("\"id\":", p)) != std::string::npos; p++) games++;
        CHECK(games > 100 && games <= 500, "compact catalog carries %zu games", games);
        // small systems come first, so a big arcade list cannot crowd the rest out
        CHECK(json.find("\"system\":\"flycast\"") != std::string::npos, "the small systems are missing");
    }

    // ---- speed: what a button press costs ----
    {
        t0 = std::chrono::steady_clock::now();
        std::string sink;
        for (int i = 0; i < 2000; i++) {
            std::shared_ptr<const Index> c = ix;
            std::string rep;
            AnswerCatalogRequest(c, false, "LIST:7:*:" + std::to_string(rnd() % N) + ":32", rep);
            sink = rep;
        }
        const double per = ms_since(t0) / 2000.0;
        printf("a 32-row page request: %.3f ms\n", per);
        CHECK(per < 1.0, "a page request takes %.3f ms", per);

        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; i++) ix->Find(ix->Id(*ix->RecAt(0, rnd() % N)));
        const double find = ms_since(t0) / 20000.0;
        printf("a launch lookup: %.4f ms\n", find);
        CHECK(find < 0.05, "a launch lookup takes %.4f ms", find);
    }

    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
