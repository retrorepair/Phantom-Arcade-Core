/*
 * library_scan_test.cpp - the scan finds the right games, and the second scan does almost nothing.
 *
 * Runs the real scanner (library_scan.h) over folders it builds itself, with stand-ins for the three
 * things that live in the daemon - MAME, the PARAM.SFO reader and the Xbox disc reader - that count how
 * often they are called. "Almost nothing" is then a number: an unchanged library asks MAME zero times
 * and reads zero files.
 *
 *   library_scan_test                 correctness and incremental behaviour, a few thousand files
 *   library_scan_test --bench <dir>   time a first and second scan of an existing folder tree as the
 *                                     MAME library (built with rom_enum_test --make)
 *
 * Exit status is the number of failed checks.
 */
#include <cstdio>

#include "library_scan.h"

namespace fs = std::filesystem;
using namespace phantom;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static void touch(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

struct Counters {
    std::atomic<int> ps3{0}, xbox{0};
    int mameCalls = 0, mameNames = 0, maxBatchChars = 0;
};

static std::set<std::string> g_badSets;   // what the stand-in MAME calls unrunnable

static ScanServices services(Counters& c, const std::map<std::string, std::string>& titles, const std::map<std::string, FbneoSet>* fb) {
    ScanServices s;
    s.mameTitles = &titles;
    s.fbneoSets = fb;
    s.ps3Title = [&c](const std::wstring& p) { c.ps3++; return std::string("PS3 game from ") + scan_detail::Utf8(fs::path(p).parent_path().filename().wstring()); };
    s.xboxMeta = [&c](const std::wstring&) { c.xbox++; Derived d; d.title = "Halo 2 (Xbox)"; d.year = "2004"; d.maker = "Microsoft"; return d; };
    s.fold = [](const std::string& x) {
        std::string o;
        for (unsigned char ch : x) o += (ch < 0x80) ? (char)ch : 'e';   // stand-in for AsciiFold: all that matters is that it ran
        return o;
    };
    s.mameVerify = [&c](const std::vector<std::string>& names, std::vector<MameVerdict>& out) {
        c.mameCalls++;
        c.mameNames += (int)names.size();
        int chars = 0;
        for (const std::string& n : names) chars += (int)n.size() + 1;
        c.maxBatchChars = std::max(c.maxBatchChars, chars);
        for (const std::string& n : names) {
            MameVerdict v;
            v.runnable = !g_badSets.count(n);
            v.mode = "320x240 @ 60.00Hz";
            v.year = "1994";
            v.maker = "Capcom";
            out.push_back(v);
        }
    };
    s.mameFingerprint = "mame-0.1";
    return s;
}

struct Result {
    std::shared_ptr<Index> last;
    int publishes = 0;
    size_t firstCount = 0;
    ScanStats st;
};

static Result scan(const std::vector<ScanTarget>& targets, const ScanServices& svc, const ScanOptions& opt, std::shared_ptr<const Index> old) {
    Result r;
    r.st = RunScan(targets, svc, opt, old,
        [&](std::shared_ptr<Index> ix, bool) {
            if (!r.publishes) r.firstCount = ix->Total();
            r.publishes++;
            r.last = ix;
        }, nullptr);
    return r;
}

static const Rec* find(const Index& ix, const char* id) { return ix.Find(id); }

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--bench") {
        Counters c;
        std::map<std::string, std::string> titles;
        ScanTarget t;
        t.key = argc >= 4 ? argv[3] : "groovymame"; t.name = t.key; t.videoMode = "15kHz Native";
        t.roots.push_back(fs::path(argv[2]).wstring());
        t.exts = L".zip,.7z,.chd";
        ScanServices svc = services(c, titles, nullptr);
        ScanOptions opt;
        opt.rejectsFile = fs::temp_directory_path() / "pa_bench_rejects.txt";
        std::error_code ec;
        fs::remove(opt.rejectsFile, ec);

        Result a = scan({ t }, svc, opt, nullptr);
        printf("first scan : %llu files seen, %llu games listed in %.0f ms (walk %.0f, MAME asked about %llu in %d batches, %.0f ms)\n",
               (unsigned long long)a.st.files, (unsigned long long)a.st.listed, a.st.totalMs, a.st.enumMs,
               (unsigned long long)a.st.mameAsked, c.mameCalls, a.st.verifyMs);
        double t0 = 0;
        const auto s0 = std::chrono::steady_clock::now();
        SaveIndex(*a.last, fs::temp_directory_path() / "pa_bench_index.bin");
        t0 = MsSince(s0);
        printf("             saved the index in %.0f ms\n", t0);
        const auto l0 = std::chrono::steady_clock::now();
        std::shared_ptr<Index> loaded = LoadIndex(fs::temp_directory_path() / "pa_bench_index.bin", 1);
        printf("             loaded it back in %.0f ms (%zu games)\n", MsSince(l0), loaded ? loaded->Total() : 0);

        Counters c2;
        ScanServices svc2 = services(c2, titles, nullptr);
        Result b = scan({ t }, svc2, opt, loaded);
        printf("second scan: %llu files seen, %llu games listed in %.0f ms (reused %llu, MAME asked %d times)\n",
               (unsigned long long)b.st.files, (unsigned long long)b.st.listed, b.st.totalMs, (unsigned long long)b.st.reused, c2.mameCalls);
        CHECK(a.last && b.last && IndexEquivalent(*a.last, *b.last), "the second scan differs from the first");
        CHECK(c2.mameCalls == 0, "the second scan asked MAME %d times", c2.mameCalls);

        // and what the cabinet would then do: sort, and serve pages
        const auto p0 = std::chrono::steady_clock::now();
        std::string rep;
        std::shared_ptr<const Index> served = b.last;
        for (int i = 0; i < 1000; i++) AnswerCatalogRequest(served, false, "LIST:" + std::to_string(served->ver) + ":*:" + std::to_string(i * 37 % std::max<size_t>(1, served->Total())) + ":32", rep);
        printf("1000 page requests: %.1f ms\n", MsSince(p0));
        printf("%d check(s), %d failed\n", checks, fails);
        return fails;
    }

    const fs::path root = fs::temp_directory_path() / "pa_scan_data";
    std::error_code ec;
    fs::remove_all(root, ec);

    // ---- a small library with every kind of layout the scan has to understand ----
    touch(root / "mame" / "sfa3.zip");
    touch(root / "mame" / "bgaregga.zip");
    touch(root / "mame" / "brokenset.zip");               // MAME will call this one unrunnable
    touch(root / "mame" / "sfiii3.zip");
    touch(root / "mame" / "sfiii3" / "cap-33s-1.chd");    // belongs to sfiii3, is not a game of its own
    touch(root / "mame" / "sfiii3" / "cap-33s-2.chd");
    touch(root / "mame" / "readme.txt");                  // not a ROM
    touch(root / "mame" / "UPPER.ZIP");                   // extension case
    touch(root / "mame" / "kinst.zip");                   // the same set, twice: Windows does not tell the folder's
    touch(root / "mame" / "Kinst" / "kinst.chd");         // name from the zip's, and neither does MAME
    touch(root / "fbneo" / "ssf2t.zip");
    touch(root / "fbneo" / "neogeo.zip");                 // a BIOS
    touch(root / "fbneo" / "notsupported.zip");
    touch(root / "ps3" / "NPUA80105" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN");
    touch(root / "ps3" / "Installer.pkg");
    touch(root / "ps3" / "Another.pkg");
    touch(root / "xbox" / "eps-h2u.iso");
    touch(root / "gc" / "Game.nkit.iso");
    touch(root / "gc" / "Plain Game.rvz");
    touch(root / "pc" / "Win98 Voodoo" / "86box.cfg");
    touch(root / "pc" / "DOS Box" / "86box.cfg");
    touch(root / "dc" / "crazytaxi.gdi");

    auto target = [&](const char* key, const char* name, const char* sub, const wchar_t* exts) {
        ScanTarget t;
        t.key = key; t.name = name; t.videoMode = std::string("mode-") + key;
        t.roots.push_back((root / sub).wstring());
        t.exts = exts;
        return t;
    };
    std::vector<ScanTarget> targets = {
        target("groovymame", "GroovyMAME Arcade", "mame", L".zip,.7z,.chd"),
        target("fbneo", "FBNeo Arcade", "fbneo", L".zip,.7z"),
        target("flycast", "Dreamcast", "dc", L".zip,.gdi,.cdi"),
        target("rpcs3", "PS3", "ps3", L".iso,.pkg,.self,.elf,.bin"),
        target("xemu", "Xbox", "xbox", L".iso,.xiso"),
        target("dolphin", "GC/Wii", "gc", L".iso,.rvz"),
        target("86box", "PC", "pc", L".cfg"),
    };

    std::map<std::string, std::string> titles = {
        { "sfa3", "Street Fighter Alpha 3 (Euro 980904)" }, { "bgaregga", "Battle Garegga (Europe)" },
        { "brokenset", "Broken Set" }, { "sfiii3", "Street Fighter III 3rd Strike" }, { "ssf2t", "Super Street Fighter II Turbo" },
        { "UPPER", "Upper Case Set" },
    };
    std::map<std::string, FbneoSet> fbsets;
    { FbneoSet s; s.description = "Super Street Fighter II Turbo (World)"; s.year = "1994"; s.manufacturer = "Capcom"; fbsets["ssf2t"] = s; }
    { FbneoSet s; s.description = "Neo Geo"; s.isBios = true; fbsets["neogeo"] = s; }
    for (int i = 0; i < 20; i++) { FbneoSet s; s.description = "filler"; fbsets["filler" + std::to_string(i)] = s; }

    g_badSets = { "brokenset" };
    ScanOptions opt;
    opt.rejectsFile = root / "rejects.txt";

    Counters c1;
    ScanServices svc1 = services(c1, titles, &fbsets);
    Result r1 = scan(targets, svc1, opt, nullptr);
    CHECK(r1.last != nullptr, "nothing was published");
    if (!r1.last) { printf("%d check(s), %d failed\n", checks, fails); return fails; }
    const Index& ix = *r1.last;

    CHECK(r1.publishes == 2, "publishes: %d (what needs no vetting first, then the MAME sets)", r1.publishes);
    CHECK(r1.firstCount > 0 && r1.firstCount < ix.Total(), "the first publish held %zu of %zu: MAME sets should still be pending", r1.firstCount, ix.Total());

    // MAME: set names, not file names; nested CHDs belong to their set; the incomplete one is out
    CHECK(find(ix, "groovymame_sfa3") && find(ix, "groovymame_bgaregga") && find(ix, "groovymame_sfiii3") && find(ix, "groovymame_UPPER"), "MAME sets missing");
    CHECK(!find(ix, "groovymame_brokenset"), "an unrunnable set is listed");
    CHECK(!find(ix, "groovymame_cap-33s-1") && !find(ix, "groovymame_cap-33s-2"), "a CHD inside a set's folder became a game of its own");
    CHECK(!find(ix, "groovymame_readme"), "a text file became a game");
    if (auto r = find(ix, "groovymame_sfa3")) {
        CHECK(!strcmp(ix.Title(*r), "Street Fighter Alpha 3 (Euro 980904)"), "MAME title: %s", ix.Title(*r));
        CHECK(!strcmp(ix.Str(r->year), "1994") && !strcmp(ix.Str(r->maker), "Capcom") && !strcmp(ix.Str(r->mode), "320x240 @ 60.00Hz"), "MAME details");
    }
    CHECK(c1.mameNames == 6, "MAME was asked about %d sets, expected 6", c1.mameNames);
    CHECK(find(ix, "groovymame_kinst") != nullptr && !find(ix, "groovymame_Kinst"), "one set listed under two spellings of its name");

    // FBNeo: only what it supports, BIOS left out, MAME's title preferred, its own details
    CHECK(find(ix, "fbneo_ssf2t") != nullptr, "ssf2t missing");
    CHECK(!find(ix, "fbneo_neogeo") && !find(ix, "fbneo_notsupported"), "an unsupported set or a BIOS is listed");
    if (auto r = find(ix, "fbneo_ssf2t")) CHECK(!strcmp(ix.Title(*r), "Super Street Fighter II Turbo") && !strcmp(ix.Str(r->year), "1994"), "fbneo details %s", ix.Title(*r));

    // PS3: the EBOOT is named after the game's folder, read for its title; installers are counted, not listed
    CHECK(r1.st.pkgs == 2, "PS3 installers counted: %llu", (unsigned long long)r1.st.pkgs);
    CHECK(find(ix, "rpcs3_NPUA80105") != nullptr, "the EBOOT game is missing or misnamed");
    if (auto r = find(ix, "rpcs3_NPUA80105")) CHECK(!strncmp(ix.Title(*r), "PS3 game from", 13), "PS3 title: %s", ix.Title(*r));
    CHECK(!find(ix, "rpcs3_Installer") && !find(ix, "rpcs3_Another"), "a .pkg installer is listed");
    // Xbox: read from the disc
    if (auto r = find(ix, "xemu_eps-h2u")) CHECK(!strcmp(ix.Title(*r), "Halo 2 (Xbox)") && !strcmp(ix.Str(r->maker), "Microsoft"), "Xbox: %s", ix.Title(*r));
    else CHECK(0, "xbox game missing");
    // container suffix off a title, 86Box named for its folder, a plain game by its file
    CHECK(find(ix, "dolphin_Game") != nullptr && !find(ix, "dolphin_Game.nkit"), "'.nkit' left on a title");
    CHECK(find(ix, "dolphin_Plain Game") != nullptr, "plain game");
    CHECK(find(ix, "86box_Win98 Voodoo") != nullptr && find(ix, "86box_DOS Box") != nullptr && !find(ix, "86box_86box"), "86Box machines are named for their folder");
    CHECK(find(ix, "flycast_crazytaxi") != nullptr, "dreamcast game");
    CHECK(c1.ps3 == 1 && c1.xbox == 1, "metadata reads: ps3 %d, xbox %d", c1.ps3.load(), c1.xbox.load());

    // every game is under its own system and the lists add up
    size_t sum = 0;
    for (size_t s = 1; s < ix.lists.size(); s++) sum += ix.lists[s].size();
    CHECK(sum == ix.Total(), "system lists add up to %zu of %zu", sum, ix.Total());

    // ---- the second scan, nothing changed: no MAME, no file reads, same library ----
    SaveIndex(ix, root / "index.bin");
    std::shared_ptr<Index> loaded = LoadIndex(root / "index.bin", 5);
    CHECK(loaded != nullptr, "the saved index did not load");
    Counters c2;
    ScanServices svc2 = services(c2, titles, &fbsets);
    Result r2 = scan(targets, svc2, opt, loaded);
    CHECK(r2.last && IndexEquivalent(ix, *r2.last), "an unchanged library came out different");
    CHECK(c2.mameCalls == 0, "unchanged library: MAME asked %d times (the rejection should be remembered)", c2.mameCalls);
    CHECK(c2.ps3 == 0 && c2.xbox == 0, "unchanged library: files re-read (ps3 %d, xbox %d)", c2.ps3.load(), c2.xbox.load());
    CHECK(r2.st.reused >= 5, "reused only %llu records", (unsigned long long)r2.st.reused);
    CHECK(r2.publishes == 1, "unchanged library published %d times", r2.publishes);

    // ---- a full rescan ignores what it knew ----
    Counters c3;
    ScanServices svc3 = services(c3, titles, &fbsets);
    ScanOptions full = opt;
    full.full = true;
    Result r3 = scan(targets, svc3, full, loaded);
    CHECK(c3.mameNames == 6 && c3.ps3 == 1 && c3.xbox == 1, "a full rescan re-examined mame %d ps3 %d xbox %d", c3.mameNames, c3.ps3.load(), c3.xbox.load());
    CHECK(r3.last && IndexEquivalent(ix, *r3.last), "the full rescan differs");

    // ---- one new MAME set and one changed Xbox disc: only they are examined ----
    touch(root / "mame" / "newset.zip");
    touch(root / "xbox" / "eps-h2u.iso", "a longer body, so the size differs");
    Counters c4;
    ScanServices svc4 = services(c4, titles, &fbsets);
    Result r4 = scan(targets, svc4, opt, loaded);
    CHECK(find(*r4.last, "groovymame_newset") != nullptr, "the new set is missing");
    CHECK(c4.xbox == 1 && c4.ps3 == 0, "changed disc re-read %d times, ps3 %d", c4.xbox.load(), c4.ps3.load());
    // the MAME tree changed, so its rejection is no longer certain (a parent may have appeared): the new set and the old reject are asked again
    CHECK(c4.mameNames == 2, "after a change MAME was asked about %d sets, expected the new one and the one it rejected", c4.mameNames);
    CHECK(find(*r4.last, "groovymame_sfa3") != nullptr && !find(*r4.last, "groovymame_brokenset"), "known-good sets kept, reject still rejected");

    // ---- a missing parent turns up: the rejected set becomes playable on the next scan ----
    g_badSets.clear();
    Counters c5;
    ScanServices svc5 = services(c5, titles, &fbsets);
    touch(root / "mame" / "parent.zip");   // anything under the MAME folder changing is enough to re-ask
    Result r5 = scan(targets, svc5, opt, r4.last);
    CHECK(find(*r5.last, "groovymame_brokenset") != nullptr, "a set that became complete stayed hidden");

    // ---- a changed folder setting does not inherit the old folder's games ----
    {
        std::vector<ScanTarget> moved = targets;
        moved[2].roots[0] = (root / "gc").wstring();    // flycast now points at the GameCube folder
        moved[2].exts = L".rvz";
        Counters c6;
        ScanServices svc6 = services(c6, titles, &fbsets);
        Result r6 = scan(moved, svc6, opt, r5.last);
        CHECK(!find(*r6.last, "flycast_crazytaxi"), "a game from the old folder survived the folder change");
        CHECK(find(*r6.last, "flycast_Plain Game") != nullptr, "the new folder was not scanned");
    }

    // ---- MAME that cannot be asked: the sets are kept, and nothing is remembered ----
    {
        fs::remove(root / "rejects.txt", ec);
        Counters c7;
        ScanServices svc7 = services(c7, titles, &fbsets);
        svc7.mameVerify = [&](const std::vector<std::string>&, std::vector<MameVerdict>&) { c7.mameCalls++; };   // no answer
        Result r7 = scan(targets, svc7, opt, nullptr);
        CHECK(find(*r7.last, "groovymame_sfa3") && find(*r7.last, "groovymame_brokenset"), "an unanswered MAME hid sets");
        CHECK(!fs::exists(root / "rejects.txt"), "rejections were remembered from a failed run");
    }

    // ---- no MAME configured at all ----
    {
        Counters c8;
        ScanServices svc8 = services(c8, titles, &fbsets);
        svc8.mameVerify = nullptr;
        Result r8 = scan(targets, svc8, opt, nullptr);
        CHECK(find(*r8.last, "groovymame_sfa3") != nullptr && r8.publishes == 1, "no-MAME scan: %d publishes", r8.publishes);
    }

    // ---- a library whose names would not fit on one command line ----
    {
        fs::remove_all(root / "big", ec);
        const int n = 6000;
        for (int i = 0; i < n; i++) touch(root / "big" / ("a_fairly_long_set_name_number_" + std::to_string(i) + ".zip"));
        ScanTarget t = target("groovymame", "GroovyMAME Arcade", "big", L".zip");
        Counters c9;
        ScanServices svc9 = services(c9, titles, nullptr);
        g_badSets.clear();
        ScanOptions o9;   // no rejects file
        Result r9 = scan({ t }, svc9, o9, nullptr);
        CHECK(r9.last && (int)r9.last->Total() == n, "the big library lists %zu of %d", r9.last ? r9.last->Total() : 0, n);
        CHECK(c9.mameCalls >= 6 && c9.mameNames == n, "%d batches for %d names", c9.mameCalls, c9.mameNames);
        CHECK(c9.maxBatchChars <= 24000, "a batch was %d characters, over the command-line budget", c9.maxBatchChars);
    }

    // ---- a stop request abandons the scan between batches ----
    {
        std::atomic<bool> stop(false);
        Counters c10;
        ScanServices svc10 = services(c10, titles, nullptr);
        auto inner = svc10.mameVerify;
        svc10.mameVerify = [&](const std::vector<std::string>& n, std::vector<MameVerdict>& o) { inner(n, o); stop = true; };
        ScanTarget t = target("groovymame", "GroovyMAME Arcade", "big", L".zip");
        ScanOptions o10;
        o10.stop = &stop;
        o10.rejectsFile = root / "rejects10.txt";
        scan({ t }, svc10, o10, nullptr);
        CHECK(c10.mameCalls == 1, "a stopped scan went on to %d batches", c10.mameCalls);
        CHECK(!fs::exists(root / "rejects10.txt"), "a stopped scan remembered a partial answer");
    }

    fs::remove_all(root, ec);
    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
