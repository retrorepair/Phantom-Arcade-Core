/*
 * art_http_test.cpp - what the cabinet is told: the /art request against a real index, and the ART: datagrams.
 *
 *   art_http_test       exit status is the number of failed checks
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include "art_http.h"

namespace fs = std::filesystem;
using namespace phantom;
using namespace phantom::art;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

struct Fake {
    std::mutex m;
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, std::vector<std::string>> lists;
    std::vector<std::string> requested;
    int Get(const std::string& url, std::vector<uint8_t>& body) {
        std::lock_guard<std::mutex> lk(m);
        requested.push_back(url);
        auto l = lists.find(url);
        if (l != lists.end()) {
            std::string page = "<h1>Index of /x</h1>";
            for (const std::string& s : l->second) page += "<a href=\"" + UrlEncode(s) + ".png\">x</a>";
            body.assign(page.begin(), page.end());
            return 200;
        }
        auto f = files.find(url);
        if (f != files.end()) { body = f->second; return 200; }
        return 404;
    }
    size_t Count(const std::string& needle) {
        std::lock_guard<std::mutex> lk(m);
        size_t n = 0;
        for (auto& r : requested) n += r.find(needle) != std::string::npos;
        return n;
    }
};

static Services Make(Fake& f) {
    Services s;
    s.get = [&f](const std::string& u, std::vector<uint8_t>& b) { return f.Get(u, b); };
    s.decode = [](const std::vector<uint8_t>& file, int& w, int& h, std::vector<uint8_t>& px) {
        if (file.size() < 4 || file[0] != 'P') return false;
        w = file[1]; h = file[2];
        px.assign((size_t)w * h * 4, file[3]);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        return true;
    };
    s.politeness_ms = 0;
    return s;
}

static std::shared_ptr<const Index> make_index(uint32_t ver) {
    IndexBuilder b;
    const int fb = b.AddSystem("fbneo", "FBNeo Arcade", "C:/r");
    const int ps2 = b.AddSystem("pcsx2", "PS2", "C:/p");
    const char* names[] = { "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel" };
    for (const char* n : names) b.Add(fb, std::string("fbneo_") + n, n, std::string("C:/r/") + n + ".zip", "", "", "", 1, 1);
    b.Add(ps2, "pcsx2_Burnout", "Burnout 3 - Takedown (USA)", "C:/p/Burnout 3 - Takedown (USA).iso", "", "", "", 1, 1);
    return b.Finish(ver);
}

static void wait_idle(Store& st) { st.WaitIdle(10000); }

static void test_parse() {
    HttpRequest r = ParseRequest("GET /art?v=1&l=*&i=5&k=title HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.ok && r.method == "GET" && r.path == "/art" && r.query == "v=1&l=*&i=5&k=title", "parse: %s %s %s", r.method.c_str(), r.path.c_str(), r.query.c_str());
    r = ParseRequest("GET / HTTP/1.0\r\n\r\n");
    CHECK(r.ok && r.path == "/" && r.query.empty(), "the root");
    r = ParseRequest("GET /catalog.json HTTP/1.1\r\n\r\n");
    CHECK(r.ok && r.path == "/catalog.json", "the catalog");
    r = ParseRequest("GET /art");
    CHECK(r.ok && r.path == "/art", "no version is tolerated");
    CHECK(!ParseRequest("").ok, "empty");
    CHECK(!ParseRequest("garbage").ok, "no spaces");
    CHECK(!ParseRequest("GET nothing HTTP/1.1\r\n").ok, "a target that is not a path");
    CHECK(!ParseRequest("\r\n\r\n").ok, "a blank request");

    CHECK(QueryParam("v=1&l=*&i=5&k=title", "l") == "*", "star");
    CHECK(QueryParam("v=1&l=pcsx2&i=5", "k").empty(), "missing");
    CHECK(QueryParam("l=a%20b+c", "l") == "a b c", "percent and plus: '%s'", QueryParam("l=a%20b+c", "l").c_str());
    CHECK(QueryParam("kk=1&k=2", "k") == "2", "a key is not matched by its prefix");
    CHECK(QueryParam("", "k").empty(), "empty query");
    CHECK(QueryParam("k=", "k").empty(), "empty value");
}

static void test_art_request() {
    const fs::path root = fs::temp_directory_path() / "pa_art_http";
    std::error_code ec;
    fs::remove_all(root, ec);
    Fake f;
    std::vector<std::string> names = { "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel" };
    f.lists[ListingUrl("FBNeo - Arcade Games", kTitle)] = names;
    for (size_t i = 0; i < names.size(); i++) f.files[PictureUrl("FBNeo - Arcade Games", kTitle, names[i])] = { 'P', 4, 3, (uint8_t)(100 + i) };
    Store st(root, Make(f), 2);
    auto ix = make_index(7);

    // find where Charlie sits in the "all" list
    uint32_t pos = 0;
    for (size_t i = 0; i < ix->Total(); i++)
        if (std::string(ix->Title(ix->recs[ix->lists[0][i]])) == "Charlie") pos = (uint32_t)i;

    const std::string q = "v=7&l=*&i=" + std::to_string(pos) + "&k=title";
    ArtReply r = HandleArtRequest(st, ix, q);
    CHECK(r.code == 202 && r.body.empty() && r.headers.find("Retry-After") != std::string::npos, "first ask: 202 with Retry-After (%d)", r.code);
    wait_idle(st);
    r = HandleArtRequest(st, ix, q);
    CHECK(r.code == 200, "then 200 (%d)", r.code);
    CHECK(r.body.size() == (size_t)kCanvasW * kCanvasH * 3, "a whole canvas: %zu bytes", r.body.size());
    CHECK(!r.body.empty() && r.body[0] == 102 && r.body.back() == 102, "Charlie's own picture (shade %d)", r.body.empty() ? -1 : r.body[0]);
    CHECK(ReplyHead(r).find("Content-Length: 1036800") != std::string::npos && ReplyHead(r).rfind("HTTP/1.1 200 OK", 0) == 0, "the header says so");

    // asking for one fetched the rows around it too
    wait_idle(st);
    CHECK(f.Count("Delta.png") >= 1 && f.Count("Bravo.png") >= 1, "the neighbours were fetched ahead of being asked for");
    ArtReply nb = HandleArtRequest(st, ix, "v=7&l=*&i=" + std::to_string(pos + 1) + "&k=title");
    CHECK(nb.code == 200, "so the next row is ready at once (%d)", nb.code);

    // a stale library
    r = HandleArtRequest(st, ix, "v=6&l=*&i=0&k=title");
    CHECK(r.code == 409 && r.headers.find("X-Phantom-Version: 7") != std::string::npos, "an old version is 409 and says what is current");

    // bad requests
    CHECK(HandleArtRequest(st, ix, "v=7&l=*&i=0&k=poster").code == 400, "unknown kind");
    CHECK(HandleArtRequest(st, ix, "v=7&i=0&k=title").code == 400, "no list");
    CHECK(HandleArtRequest(st, ix, "l=*&i=0&k=title").code == 400, "no version");
    CHECK(HandleArtRequest(st, ix, "v=7&l=*&i=x&k=title").code == 400, "a non-number");
    CHECK(HandleArtRequest(st, ix, "v=7&l=*&i=-1&k=title").code == 400, "a negative number");
    CHECK(HandleArtRequest(st, ix, "v=99999999999&l=*&i=0&k=title").code == 400, "a number too big");
    CHECK(HandleArtRequest(st, ix, "v=7&l=*&i=999&k=title").code == 404, "a row that is not there");
    CHECK(HandleArtRequest(st, ix, "v=7&l=nosuch&i=0&k=title").code == 404, "a list that is not there");
    CHECK(HandleArtRequest(st, nullptr, "v=7&l=*&i=0&k=title").code == 503, "no library yet");

    // no picture on the server for a game: 202 then 404
    uint32_t bpos = 0;
    for (size_t i = 0; i < ix->Total(); i++)
        if (std::string(ix->Title(ix->recs[ix->lists[0][i]])) == "Burnout 3 - Takedown (USA)") bpos = (uint32_t)i;
    f.lists[ListingUrl("Sony - PlayStation 2", kTitle)] = { "Something Else" };
    const std::string bq = "v=7&l=*&i=" + std::to_string(bpos) + "&k=title";
    // (it sorts next to Charlie, so Charlie's request has already prefetched it)
    wait_idle(st);
    CHECK(f.Count("PlayStation%202/Named_Titles/") >= 1, "the neighbour's folder was looked up");
    CHECK(HandleArtRequest(st, ix, bq).code == 404, "a game with no picture on the server is 404");

    // a snap when only titles may be fetched
    st.SetFetchMask(1);
    CHECK(HandleArtRequest(st, ix, "v=7&l=*&i=" + std::to_string(pos) + "&k=snap").code == 404, "a kind that may not be fetched is 404, not an endless 202");
}

static void test_commands() {
    const fs::path root = fs::temp_directory_path() / "pa_art_http2";
    std::error_code ec;
    fs::remove_all(root, ec);
    Fake f;
    std::vector<std::string> names = { "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel" };
    f.lists[ListingUrl("FBNeo - Arcade Games", kTitle)] = names;
    f.lists[ListingUrl("FBNeo - Arcade Games", kSnap)] = names;
    for (const std::string& n : names) {
        f.files[PictureUrl("FBNeo - Arcade Games", kTitle, n)] = { 'P', 4, 3, 1 };
        f.files[PictureUrl("FBNeo - Arcade Games", kSnap, n)] = { 'P', 4, 3, 2 };
    }
    Store st(root, Make(f), 2);
    auto ix = make_index(1);
    int persisted = -1;
    auto persist = [&](int m) { persisted = m; };

    CHECK(HandleArtCommand(st, ix, "PING", persist).empty(), "not an ART message");
    CHECK(HandleArtCommand(st, ix, "ART:NOPE", persist).empty(), "an unknown ART message is ignored");
    std::string r = HandleArtCommand(st, ix, "ART:INFO", persist);
    CHECK(r == "ART:INFO:3:0:0:0:0:0:0:0", "INFO at the start: %s", r.c_str());

    r = HandleArtCommand(st, ix, "ART:SET:1", persist);
    CHECK(persisted == 1 && r.rfind("ART:INFO:1:", 0) == 0 && st.FetchMask() == 1, "SET:1 -> titles only, saved: %s", r.c_str());
    CHECK(HandleArtCommand(st, ix, "ART:SET:4", persist) == "ART:BAD" && persisted == 1 && st.FetchMask() == 1, "a mask over 3 is refused");
    CHECK(HandleArtCommand(st, ix, "ART:SET:x", persist) == "ART:BAD", "a mask that is not a number is refused");
    r = HandleArtCommand(st, ix, "ART:SET:0", persist);
    CHECK(persisted == 0 && st.FetchMask() == 0, "SET:0 -> off");

    // the whole library
    HandleArtCommand(st, ix, "ART:SET:3", persist);
    r = HandleArtCommand(st, ix, "ART:ALL:1", persist);
    CHECK(r.rfind("ART:INFO:3:", 0) == 0, "ALL:1 answers with INFO: %s", r.c_str());
    CHECK(st.WaitIdle(15000), "the bulk run finishes");
    r = HandleArtCommand(st, ix, "ART:INFO", persist);
    // 8 arcade games have pictures; the PS2 one has no listing, so it is counted as done but has none
    CHECK(r.rfind("ART:INFO:3:8:8:0:1:0:9:9", 0) == 0, "all 8 titles and 8 snaps, bulk finished 9 of 9: %s", r.c_str());
    r = HandleArtCommand(st, ix, "ART:ALL:0", persist);
    CHECK(r.rfind("ART:INFO:", 0) == 0, "ALL:0 is harmless when nothing runs");

    auto empty = std::make_shared<Index>();
    r = HandleArtCommand(st, empty, "ART:ALL:1", persist);
    CHECK(r.rfind("ART:INFO:", 0) == 0, "ALL:1 with no library does nothing, and says so as INFO");
    r = HandleArtCommand(st, nullptr, "ART:ALL:1", persist);
    CHECK(r.rfind("ART:INFO:", 0) == 0, "and with no index at all");
}

int main() {
    test_parse();
    test_art_request();
    test_commands();
    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
