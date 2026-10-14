/*
 * art_store_test.cpp - art_store.h with a fake server and a fake decoder: what is fetched, kept, remembered and
 * asked again, and in what order.
 *
 *   art_store_test      runs everything in a scratch folder; exit status is the number of failed checks
 *
 * The fake picture is "P" + width + height + a shade; the fake decoder turns that into a solid-colour picture, so
 * what comes out of Get() can be checked by eye-less arithmetic.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "art_store.h"

namespace fs = std::filesystem;
using namespace phantom::art;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

// ---- a fake server ----------------------------------------------------------------------------------

struct FakeServer {
    std::mutex m;
    std::map<std::string, std::vector<uint8_t>> files;     // url -> body (200)
    std::map<std::string, std::vector<std::string>> lists; // listing url -> picture names
    std::vector<std::string> requested;
    std::atomic<bool> down{ false };
    std::atomic<int> delay_ms{ 0 };

    void AddList(const std::string& repo, Kind k, std::vector<std::string> stems) { lists[ListingUrl(repo, k)] = std::move(stems); }
    void AddPicture(const std::string& repo, Kind k, const std::string& stem, uint8_t shade, int w = 4, int h = 3) {
        files[PictureUrl(repo, k, stem)] = { 'P', (uint8_t)w, (uint8_t)h, shade };
    }
    int Count(const std::string& url) {
        std::lock_guard<std::mutex> lk(m);
        int n = 0;
        for (const std::string& r : requested) n += (r == url);
        return n;
    }
    size_t Total() { std::lock_guard<std::mutex> lk(m); return requested.size(); }

    int Get(const std::string& url, std::vector<uint8_t>& body) {
        if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
        std::lock_guard<std::mutex> lk(m);
        requested.push_back(url);
        if (down) return -1;
        auto l = lists.find(url);
        if (l != lists.end()) {
            std::string page = "<html><h1>Index of /listing</h1><table>";
            for (const std::string& s : l->second) page += "<tr><td><a href=\"" + UrlEncode(s) + ".png\">x</a></td></tr>";
            page += "</table></html>";
            body.assign(page.begin(), page.end());
            return 200;
        }
        auto f = files.find(url);
        if (f != files.end()) { body = f->second; return 200; }
        return 404;
    }
};

static bool FakeDecode(const std::vector<uint8_t>& file, int& w, int& h, std::vector<uint8_t>& bgra) {
    if (file.size() < 4 || file[0] != 'P') return false;
    w = file[1];
    h = file[2];
    bgra.assign((size_t)w * h * 4, file[3]);
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    return true;
}

static Services Make(FakeServer& s) {
    Services v;
    v.get = [&s](const std::string& u, std::vector<uint8_t>& b) { return s.Get(u, b); };
    v.decode = FakeDecode;
    v.politeness_ms = 0;
    return v;
}

static GameRef Game(const char* id, const char* title, const char* sys, const char* rom) { return { id, title, sys, rom }; }

static fs::path scratch(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("pa_art_store_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static bool shade_is(const std::vector<uint8_t>& canvas, uint8_t v) {
    if (canvas.size() != (size_t)kCanvasW * kCanvasH * 3) return false;
    // a 4x3 picture fills the canvas: every pixel is the shade
    for (size_t i = 0; i < canvas.size(); i++)
        if (canvas[i] != v) return false;
    return true;
}

// ---- tests ---------------------------------------------------------------------------------------------

static void test_fetch_and_keep() {
    const fs::path root = scratch("fetch");
    FakeServer srv;
    srv.AddList("FBNeo - Arcade Games", kTitle, { "Street Fighter Alpha 3 (World 980904)", "1942 (Revision B)" });
    srv.AddPicture("FBNeo - Arcade Games", kTitle, "Street Fighter Alpha 3 (World 980904)", 77);

    GameRef sfa3 = Game("fbneo_sfa3", "Street Fighter Alpha 3 (World 980904)", "fbneo", "C:/r/sfa3.zip");
    std::vector<uint8_t> canvas;
    {
        Store st(root, Make(srv), 2);
        CHECK(st.Get(kTitle, sfa3, &canvas) == Got::Pending, "first ask: pending");
        CHECK(st.WaitIdle(5000), "the fetch finishes");
        CHECK(st.Get(kTitle, sfa3, &canvas) == Got::Ready, "then it is there");
        CHECK(shade_is(canvas, 77), "and is the picture the server had, on the canvas");
        CHECK(srv.Count(ListingUrl("FBNeo - Arcade Games", kTitle)) == 1, "the listing was asked for once");
        CHECK(srv.Count(PictureUrl("FBNeo - Arcade Games", kTitle, "Street Fighter Alpha 3 (World 980904)")) == 1, "the picture once");
        Stats s = st.GetStats();
        CHECK(s.have[kTitle] == 1 && s.have[kSnap] == 0, "counted: %d titles, %d snaps", s.have[kTitle], s.have[kSnap]);
        CHECK(s.online, "the server answered, so it is online");
    }

    // a later run needs no network at all
    {
        const size_t before = srv.Total();
        srv.down = true;
        Store st(root, Make(srv), 2);
        CHECK(st.GetStats().have[kTitle] == 1, "the picture is counted at start-up");
        CHECK(st.Get(kTitle, sfa3, &canvas) == Got::Ready && shade_is(canvas, 77), "served from disk");
        CHECK(srv.Total() == before, "without touching the server");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

static void test_no_picture() {
    const fs::path root = scratch("none");
    FakeServer srv;
    srv.AddList("FBNeo - Arcade Games", kTitle, { "Some Other Game" });
    srv.AddList("MAME", kTitle, {});
    GameRef g = Game("fbneo_x", "Not On The Server", "fbneo", "x.zip");
    GameRef noart = Game("86box_pc", "A PC", "86box", "C:/vm/86box.cfg");
    std::vector<uint8_t> c;
    {
        Store st(root, Make(srv), 1);
        CHECK(st.Get(kTitle, noart, &c) == Got::None, "a system with no pictures is None at once");
        CHECK(st.Get(kTitle, g, &c) == Got::Pending, "a title not in the listing starts as pending");
        CHECK(st.WaitIdle(5000), "settles");
        CHECK(st.Get(kTitle, g, &c) == Got::None, "then None");
        const size_t n = srv.Total();
        for (int i = 0; i < 5; i++) st.Get(kTitle, g, &c);
        CHECK(st.WaitIdle(2000) && srv.Total() == n, "and it is not asked about again");
    }
    {
        // remembered across runs, in the folder
        Store st(root, Make(srv), 1);
        const size_t n = srv.Total();
        CHECK(st.Get(kTitle, g, &c) == Got::None && srv.Total() == n, "the miss is on disk");
    }

    // the listing names it but the file is gone: a 404 is also "no picture"
    FakeServer srv2;
    srv2.AddList("FBNeo - Arcade Games", kTitle, { "Ghost Game" });
    const fs::path root2 = scratch("none404");
    Store st2(root2, Make(srv2), 1);
    GameRef gg = Game("fbneo_ghost", "Ghost Game", "fbneo", "ghost.zip");
    st2.Get(kTitle, gg, &c);
    CHECK(st2.WaitIdle(5000) && st2.Get(kTitle, gg, &c) == Got::None, "a 404 on the picture is None");

    // a file that is not a picture is not kept
    FakeServer srv3;
    srv3.AddList("FBNeo - Arcade Games", kTitle, { "Bad Game" });
    srv3.files[PictureUrl("FBNeo - Arcade Games", kTitle, "Bad Game")] = { 'X', 'X', 'X' };
    const fs::path root3 = scratch("badpng");
    Store st3(root3, Make(srv3), 1);
    GameRef bg = Game("fbneo_bad", "Bad Game", "fbneo", "bad.zip");
    st3.Get(kTitle, bg, &c);
    CHECK(st3.WaitIdle(5000) && st3.Get(kTitle, bg, &c) == Got::None, "bytes that do not decode are None, not stored as a picture");
    CHECK(st3.GetStats().have[kTitle] == 0, "and not counted");
}

static void test_empty_and_garbage_listings() {
    std::vector<uint8_t> c;

    // a real folder with nothing in it: no pictures, and it is not asked again
    FakeServer srv;
    srv.AddList("Sony - PlayStation 3", kTitle, {});
    const fs::path root = scratch("emptylist");
    GameRef g = Game("rpcs3_x", "Demon's Souls", "rpcs3", "C:/g/Demon's Souls.iso");
    {
        Store st(root, Make(srv), 1);
        CHECK(st.Get(kTitle, g, &c) == Got::Pending, "pending while the listing is read");
        CHECK(st.WaitIdle(5000) && st.Get(kTitle, g, &c) == Got::None, "an empty folder means no picture");
        CHECK(fs::exists(root / "lists" / "Sony - PlayStation 3__title.txt"), "and the empty listing is kept");
    }
    {
        const size_t n = srv.Total();
        Store st(root, Make(srv), 1);
        GameRef g2 = Game("rpcs3_y", "Another", "rpcs3", "C:/g/Another.iso");
        st.Get(kTitle, g2, &c);
        st.WaitIdle(5000);
        CHECK(srv.Total() == n, "the next run does not ask for the empty folder again");
    }

    // a 200 that is not a directory index (a captive portal, an error page) is not "no pictures"
    FakeServer bad;
    struct Portal { static int Get(const std::string&, std::vector<uint8_t>& b) { const char* t = "<html>Please log in</html>"; b.assign(t, t + strlen(t)); return 200; } };
    Services sv = Make(bad);
    sv.get = Portal::Get;
    const fs::path root2 = scratch("garbage");
    Store st2(root2, sv, 1);
    GameRef g3 = Game("fbneo_z", "Some Game", "fbneo", "z.zip");
    st2.Get(kTitle, g3, &c);
    st2.WaitIdle(5000);
    CHECK(!fs::exists(root2 / "pic" / "title" / "fbneo" / "fbneo_z.none"), "a login page does not become a permanent 'no picture'");
    CHECK(!fs::exists(root2 / "lists" / "FBNeo - Arcade Games__title.txt"), "and is not kept as a listing");
}

static void test_mask() {
    const fs::path root = scratch("mask");
    FakeServer srv;
    srv.AddList("FBNeo - Arcade Games", kTitle, { "A Game" });
    srv.AddList("FBNeo - Arcade Games", kSnap, { "A Game" });
    srv.AddPicture("FBNeo - Arcade Games", kTitle, "A Game", 10);
    srv.AddPicture("FBNeo - Arcade Games", kSnap, "A Game", 20);
    GameRef g = Game("fbneo_a", "A Game", "fbneo", "a.zip");
    std::vector<uint8_t> c;
    Store st(root, Make(srv), 2);

    st.SetFetchMask(0);
    CHECK(st.Get(kTitle, g, &c) == Got::None && st.Get(kSnap, g, &c) == Got::None, "off: nothing is fetched");
    CHECK(srv.Total() == 0, "and the server is not touched");

    st.SetFetchMask(2);
    CHECK(st.Get(kTitle, g, &c) == Got::None, "snaps only: titles are None");
    CHECK(st.Get(kSnap, g, &c) == Got::Pending, "snaps are fetched");
    st.WaitIdle(5000);
    CHECK(st.Get(kSnap, g, &c) == Got::Ready && shade_is(c, 20), "and arrive");

    // a picture already on disk is shown whatever is allowed now
    st.SetFetchMask(0);
    CHECK(st.Get(kSnap, g, &c) == Got::Ready, "turning fetching off does not hide what was fetched");

    st.SetFetchMask(1);
    CHECK(st.Get(kTitle, g, &c) == Got::Pending, "titles on");
    st.WaitIdle(5000);
    CHECK(st.Get(kTitle, g, &c) == Got::Ready && shade_is(c, 10), "and arrive");
}

static void test_outage() {
    const fs::path root = scratch("outage");
    FakeServer srv;
    srv.AddList("FBNeo - Arcade Games", kTitle, { "A Game" });
    srv.AddPicture("FBNeo - Arcade Games", kTitle, "A Game", 33);
    GameRef g = Game("fbneo_a", "A Game", "fbneo", "a.zip");
    std::vector<uint8_t> c;
    Store st(root, Make(srv), 1);

    srv.down = true;
    CHECK(st.Get(kTitle, g, &c) == Got::Pending, "pending while it tries");
    st.WaitIdle(5000);
    CHECK(!st.GetStats().online, "the outage is noticed");
    CHECK(st.Get(kTitle, g, &c) == Got::Pending, "a failed try is not 'no picture'");
    // no marker on disk for an outage
    CHECK(!fs::exists(root / "pic" / "title" / "fbneo" / "fbneo_a.none"), "an outage leaves no .none file");

    // it comes back: the next ask (after the back-off) succeeds
    srv.down = false;
    bool ready = false;
    for (int i = 0; i < 80 && !ready; i++) {
        Got r = st.Get(kTitle, g, &c);
        st.WaitIdle(2000);
        ready = (r == Got::Ready);
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(ready || st.Get(kTitle, g, &c) == Got::Ready, "and it recovers when the server returns");
    CHECK(st.GetStats().online, "back online");
}

static void test_priority() {
    const fs::path root = scratch("prio");
    FakeServer srv;
    std::vector<std::string> names;
    for (int i = 0; i < 6; i++) names.push_back("Game " + std::to_string(i));
    srv.AddList("FBNeo - Arcade Games", kTitle, names);
    for (int i = 0; i < 6; i++) srv.AddPicture("FBNeo - Arcade Games", kTitle, names[i], (uint8_t)(10 + i));
    srv.delay_ms = 60;   // slow enough that the queue builds up behind a single worker
    Store st(root, Make(srv), 1);
    std::vector<uint8_t> c;
    std::vector<GameRef> gs;
    for (int i = 0; i < 6; i++) gs.push_back(Game(("fbneo_g" + std::to_string(i)).c_str(), names[i].c_str(), "fbneo", "x.zip"));

    // the worker picks up the first prefetch (and loads the listing); the rest queue behind it
    for (int i = 0; i < 5; i++) st.Prefetch(kTitle, gs[i]);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // now the cursor lands on the last one: it must not wait behind four others
    CHECK(st.Get(kTitle, gs[5], &c) == Got::Pending, "asked for");
    st.WaitIdle(10000);

    std::vector<std::string> pics;
    {
        std::lock_guard<std::mutex> lk(srv.m);
        for (const std::string& r : srv.requested)
            if (r.find(".png") != std::string::npos && r.find("Named_Titles/Game") != std::string::npos) pics.push_back(r);
    }
    int pos5 = -1;
    for (size_t i = 0; i < pics.size(); i++)
        if (pics[i].find("Game%205.png") != std::string::npos) pos5 = (int)i;
    CHECK(pics.size() == 6, "all six were fetched (%zu)", pics.size());
    CHECK(pos5 >= 0 && pos5 <= 2, "the one the cursor is on came %d-th of 6, ahead of the prefetches", pos5 + 1);

    // asking again for something already queued moves it up
    st.Get(kTitle, gs[0], &c);   // already fetched: Ready; no change
    CHECK(st.GetStats().have[kTitle] == 6, "six pictures kept");
}

static void test_listing_shared() {
    const fs::path root = scratch("listing");
    FakeServer srv;
    srv.AddList("Sony - PlayStation 2", kTitle, { "Burnout 3 - Takedown (USA)", "Gradius V (USA)" });
    srv.AddPicture("Sony - PlayStation 2", kTitle, "Burnout 3 - Takedown (USA)", 1);
    srv.AddPicture("Sony - PlayStation 2", kTitle, "Gradius V (USA)", 2);
    std::vector<uint8_t> c;
    {
        Store st(root, Make(srv), 3);
        st.Get(kTitle, Game("pcsx2_Burnout 3 - Takedown (USA)", "Burnout 3 - Takedown (USA)", "pcsx2", "C:/g/Burnout 3 - Takedown (USA).iso"), &c);
        st.Get(kTitle, Game("pcsx2_Gradius V (USA)", "Gradius V (USA)", "pcsx2", "C:/g/Gradius V (USA).iso"), &c);
        st.WaitIdle(5000);
        CHECK(srv.Count(ListingUrl("Sony - PlayStation 2", kTitle)) == 1, "two games, one folder, one listing request (%d)", srv.Count(ListingUrl("Sony - PlayStation 2", kTitle)));
    }
    CHECK(fs::exists(root / "lists" / "Sony - PlayStation 2__title.txt"), "the listing is kept");
    {
        // a new run reads the kept listing instead of asking again, even for a game not fetched yet
        FakeServer srv2;
        srv2.files = srv.files;
        const fs::path root2 = root;
        Store st(root2, Make(srv2), 1);
        GameRef g = Game("pcsx2_other", "Gradius V", "pcsx2", "C:/g/Other.iso");
        st.Get(kTitle, g, &c);
        st.WaitIdle(5000);
        CHECK(srv2.Count(ListingUrl("Sony - PlayStation 2", kTitle)) == 0, "the second run did not fetch the listing");
        CHECK(st.Get(kTitle, g, &c) == Got::Ready, "and found the picture through the kept one");
    }
    // the file name picks the release when the title is bare
    FakeServer srv3;
    srv3.AddList("Sony - PlayStation 2", kTitle, { "Burnout 3 - Takedown (USA)", "Burnout 3 - Takedown (Europe)" });
    srv3.AddPicture("Sony - PlayStation 2", kTitle, "Burnout 3 - Takedown (USA)", 5);
    srv3.AddPicture("Sony - PlayStation 2", kTitle, "Burnout 3 - Takedown (Europe)", 6);
    const fs::path root3 = scratch("listing3");
    Store st3(root3, Make(srv3), 1);
    st3.Get(kTitle, Game("pcsx2_b", "Burnout 3", "pcsx2", "C:/g/Burnout 3 - Takedown (Europe).iso"), &c);
    st3.WaitIdle(5000);
    CHECK(st3.Get(kTitle, Game("pcsx2_b", "Burnout 3", "pcsx2", "C:/g/Burnout 3 - Takedown (Europe).iso"), &c) == Got::Ready && shade_is(c, 6), "the ROM's own name picked the European one");
}

static void test_bulk() {
    const fs::path root = scratch("bulk");
    FakeServer srv;
    std::vector<std::string> names;
    for (int i = 0; i < 40; i++) names.push_back("Bulk " + std::to_string(i));
    srv.AddList("FBNeo - Arcade Games", kTitle, names);
    srv.AddList("FBNeo - Arcade Games", kSnap, names);
    for (int i = 0; i < 40; i++) {
        srv.AddPicture("FBNeo - Arcade Games", kTitle, names[i], 50);
        srv.AddPicture("FBNeo - Arcade Games", kSnap, names[i], 60);
    }
    Store st(root, Make(srv), 3);
    auto at = [&](size_t i, GameRef& g) {
        if (i >= names.size()) return false;
        g = Game(("fbneo_bulk" + std::to_string(i)).c_str(), names[i].c_str(), "fbneo", "x.zip");
        return true;
    };
    st.SetFetchMask(3);
    st.BulkStart(at, names.size());
    CHECK(st.WaitIdle(20000), "the bulk run finishes");
    Stats s = st.GetStats();
    CHECK(s.have[kTitle] == 40 && s.have[kSnap] == 40, "everything fetched: %d titles, %d snaps", s.have[kTitle], s.have[kSnap]);
    CHECK(!s.bulk_running && s.bulk_done == 40 && s.bulk_total == 40, "progress %zu / %zu", s.bulk_done, s.bulk_total);

    // run again: nothing left to fetch, no requests
    const size_t n = srv.Total();
    st.BulkStart(at, names.size());
    st.WaitIdle(10000);
    CHECK(srv.Total() == n, "a second run asks for nothing (%zu new requests)", srv.Total() - n);

    // stop part-way
    const fs::path root2 = scratch("bulk2");
    FakeServer slow;
    slow.lists = srv.lists;
    slow.files = srv.files;
    slow.delay_ms = 40;
    Store st2(root2, Make(slow), 1);
    st2.SetFetchMask(1);
    st2.BulkStart(at, names.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    st2.BulkStop();
    Stats s2 = st2.GetStats();
    CHECK(!s2.bulk_running, "stopped");
    CHECK(s2.bulk_done < 40, "before the end (%zu)", s2.bulk_done);
    st2.WaitIdle(5000);
    CHECK(st2.GetStats().have[kTitle] < 40, "so not all were fetched");
}

int main() {
    test_fetch_and_keep();
    test_no_picture();
    test_empty_and_garbage_listings();
    test_mask();
    test_outage();
    test_priority();
    test_listing_shared();
    test_bulk();
    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
