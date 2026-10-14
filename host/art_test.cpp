/*
 * art_test.cpp - the rules in art.h: names, listings, matching, folders, URLs and the picture on the canvas.
 *
 *   art_test            runs everything; exit status is the number of failed checks
 *
 * The names and listing markup below are real ones (libretro's FBNeo, MAME, PS2 and SNES folders), kept small.
 */
#include <cstdio>

#include "art.h"

using namespace phantom::art;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static void test_names() {
    CHECK(FileSafe("1941: Counter Attack (World 900227)") == "1941_ Counter Attack (World 900227)", "colon becomes _");
    CHECK(FileSafe("Tom & Jerry") == "Tom _ Jerry", "ampersand becomes _");
    CHECK(FileSafe("What?/Why*") == "What__Why_", "? / * all become _");
    CHECK(FileSafe("a\"b|c\\d<e>f`g") == "a_b_c_d_e_f_g", "quote, bar, backslash, angle brackets, backtick");
    CHECK(FileSafe("Plain Name (USA)") == "Plain Name (USA)", "an ordinary name is untouched");

    CHECK(Norm("Daytona USA (Revision A)") == Norm("Daytona USA (Rev A)"), "Revision and Rev are one word");
    CHECK(Norm("The House of the Dead") == Norm("House of the Dead, The"), "a moved article folds together");
    CHECK(Norm("Tom & Jerry") == Norm("Tom and Jerry"), "& and 'and' fold together");
    CHECK(Norm("Street Fighter II'") == Norm("street fighter ii"), "case and punctuation do not matter");
    CHECK(Norm("Version 2") == Norm("Ver 2"), "Version and Ver");
    CHECK(Norm("Pokemon") != Norm("Pokemon Red"), "different games stay different");

    CHECK(BaseOf("Super Mario World (USA) (Rev 1)") == "Super Mario World", "groups come off the end");
    CHECK(Norm(BaseOf("Super Mario World (USA) (Rev 1)")) == Norm("Super Mario World"), "and the base is the bare name");
    CHECK(BaseOf("Game [!] (Europe)") == "Game", "square brackets too");
    CHECK(BaseOf("Nested (a (b) c) end") == "Nested end", "nesting does not leak");

    auto w = GroupWords("Super Mario World (USA) (Rev 1)");
    CHECK(w.size() == 3 && w[0] == "usa" && w[1] == "rev" && w[2] == "1", "words in the groups");
    auto v = GroupWords("Daytona USA (Revision A)");
    CHECK(v.size() == 2 && v[0] == "rev" && v[1] == "a", "Revision is read as rev");

    CHECK(StemOf("C:/Games/PS2/Burnout 3 - Takedown (USA).iso") == "Burnout 3 - Takedown (USA)", "stem of a path");
    CHECK(StemOf("sfa3.zip") == "sfa3", "stem of a bare file");
    CHECK(StemOf("C:\\x.y\\noext") == "noext", "a dot in a folder is not an extension");
    CHECK(ExtOf("C:/g/Game.ISO") == ".iso", "extension is lower case");
    CHECK(ExtOf("C:/g.d/noext").empty(), "no extension");
}

static void test_listing() {
    const char* page =
        "<html><body><table>\n"
        "<tr><th><a href=\"?C=N;O=D\">Name</a></th></tr>\n"
        "<tr><td><a href=\"/FBNeo%20-%20Arcade%20Games/\">Parent Directory</a></td></tr>\n"
        "<tr><td><a href=\"'88%20Games.png\">'88 Games.png</a></td><td>2025-04-09 11:53</td></tr>\n"
        "<tr><td><a href=\"1941_%20Counter%20Attack%20%28World%20900227%29.png\">x</a></td></tr>\n"
        "<tr><td><a href=\"Tom%20%26%20Jerry.png\">x</a></td></tr>\n"
        "<tr><td><a href=\"Fish&amp;Chips.png\">x</a></td></tr>\n"
        "<tr><td><a href=\"notes.txt\">notes.txt</a></td></tr>\n"
        "<tr><td><a href=\"Caf%C3%A9.png\">x</a></td></tr>\n"
        "</table></body></html>";
    const auto names = ParseListing(page);
    CHECK(names.size() == 5, "five pictures in the page, got %zu", names.size());
    if (names.size() == 5) {
        CHECK(names[0] == "'88 Games", "apostrophe name: '%s'", names[0].c_str());
        CHECK(names[1] == "1941_ Counter Attack (World 900227)", "percent-decoded: '%s'", names[1].c_str());
        CHECK(names[2] == "Tom & Jerry", "%%26 decoded: '%s'", names[2].c_str());
        CHECK(names[3] == "Fish&Chips", "&amp; unescaped: '%s'", names[3].c_str());
        CHECK(names[4] == "Caf\xC3\xA9", "UTF-8 survives: '%s'", names[4].c_str());
    }
    CHECK(ParseListing("").empty(), "an empty page lists nothing");
    CHECK(ParseListing("<a href=\"unterminated").empty(), "a truncated page does not crash");
}

static Matcher make(std::initializer_list<const char*> stems) {
    Matcher m;
    for (const char* s : stems) m.Add(s);
    m.Build();
    return m;
}

static void test_matcher() {
    Matcher m = make({
        "Daytona USA (Rev A)",
        "Super Mario World (USA)", "Super Mario World (Europe)", "Super Mario World (Japan)",
        "Super Mario World (USA) (Rev 1)",
        "House of the Dead, The",
        "1941_ Counter Attack (World 900227)",
        "Burnout 3 - Takedown (USA)", "Burnout 3 - Takedown (Europe)",
        "Street Fighter II_ The World Warrior (World 910522)",
        "Pokemon - Red Version (USA, Europe)",
    });

    // the same name, as the server spells it
    CHECK(m.Find("1941: Counter Attack (World 900227)", "") == "1941_ Counter Attack (World 900227)", "exact, after FileSafe");
    CHECK(m.Find("Street Fighter II: The World Warrior (World 910522)", "sf2") == "Street Fighter II_ The World Warrior (World 910522)", "exact beats the file name");
    // the file name is a key too, and the better one for a disc
    CHECK(m.Find("Burnout 3", "Burnout 3 - Takedown (Europe)") == "Burnout 3 - Takedown (Europe)", "the file name picks the release");
    CHECK(m.Find("", "Burnout 3 - Takedown (USA)") == "Burnout 3 - Takedown (USA)", "a file name alone");
    // spelled a little differently
    CHECK(m.Find("Daytona USA (Revision A)", "") == "Daytona USA (Rev A)", "Revision / Rev");
    CHECK(m.Find("The House of the Dead", "") == "House of the Dead, The", "the moved article");
    CHECK(m.Find("daytona usa (rev a)", "") == "Daytona USA (Rev A)", "case");
    // the same game, another release
    CHECK(m.Find("Super Mario World", "") == "Super Mario World (USA)", "no groups at all: the plain USA release");
    CHECK(m.Find("Super Mario World (Europe) (Beta)", "") == "Super Mario World (Europe)", "the region it names wins");
    CHECK(m.Find("Super Mario World (USA) (Rev 1)", "") == "Super Mario World (USA) (Rev 1)", "the exact release when it exists");
    CHECK(m.Find("Super Mario World (Brazil)", "") == "Super Mario World (USA)", "an unknown region falls back to the likeliest");
    CHECK(m.Find("Burnout 3 - Takedown", "") == "Burnout 3 - Takedown (USA)", "USA before Europe");
    // not there
    CHECK(m.Find("Nothing Like This", "") == "", "unknown title");
    CHECK(m.Find("", "") == "", "no names at all");
    CHECK(m.Find("A", "") == "", "a one-letter base is too short to trust");
    CHECK(m.Find("Pokemon", "") == "", "a different game that shares a word is not a match");
    CHECK(m.Find("Pokemon - Red Version", "") == "Pokemon - Red Version (USA, Europe)", "Version in the base still matches");

    Matcher empty;
    empty.Build();
    CHECK(empty.Find("Anything", "") == "", "an empty listing finds nothing");
    CHECK(empty.Size() == 0, "and has no entries");
}

static void test_repos() {
    auto one = [](std::vector<std::string> v) { return v.size() == 1 ? v[0] : std::string("<") + std::to_string(v.size()) + ">"; };
    CHECK(one(Repos("fbneo", "x/sfa3.zip")) == "FBNeo - Arcade Games", "fbneo");
    CHECK(one(Repos("groovymame", "x/sfa3.zip")) == "MAME", "mame");
    CHECK(one(Repos("pcsx2", "x/a.iso")) == "Sony - PlayStation 2", "ps2");
    CHECK(one(Repos("rpcs3", "x/a.iso")) == "Sony - PlayStation 3", "ps3");
    CHECK(one(Repos("xemu", "x/a.iso")) == "Microsoft - Xbox", "xbox");
    CHECK(one(Repos("flycast", "x/game.gdi")) == "Sega - Dreamcast", "dreamcast disc");
    CHECK(Repos("flycast", "x/ikaruga.zip").size() == 3 && Repos("flycast", "x/ikaruga.zip")[0] == "Sega - Naomi", "a zip is an arcade board first");
    CHECK(Repos("dolphin", "x/a.gcm")[0] == "Nintendo - GameCube", "gamecube first for a gcm");
    CHECK(Repos("dolphin", "x/a.wbfs")[0] == "Nintendo - Wii", "wii first for a wbfs");
    CHECK(Repos("dolphin", "x/a.iso").size() == 2, "an iso could be either");
    CHECK(Repos("86box", "x/86box.cfg").empty(), "pc machines have no pictures");
    CHECK(Repos("nothing", "x").empty(), "unknown system");

    CHECK(one(Repos("retroarch", "C:/Games/RetroArch/Mario.sfc")) == "Nintendo - Super Nintendo Entertainment System", "retroarch by extension");
    CHECK(one(Repos("retroarch", "C:/Games/RetroArch/Sonic.MD")) == "Sega - Mega Drive - Genesis", "extension is case-blind");
    CHECK(one(Repos("retroarch", "C:/Games/RetroArch/SNES/Mario.zip")) == "Nintendo - Super Nintendo Entertainment System", "retroarch by folder");
    CHECK(one(Repos("retroarch", "C:/Games/RetroArch/PSX/Game.cue")) == "Sony - PlayStation", "a cue in a PSX folder");
    CHECK(Repos("retroarch", "C:/Games/RetroArch/Misc/Saturn Bomberman.zip").empty(), "the file name is not read for a system");
}

static void test_urls() {
    CHECK(PictureUrl("Sony - PlayStation 2", kTitle, "Burnout 3 - Takedown (USA)") ==
              "https://thumbnails.libretro.com/Sony%20-%20PlayStation%202/Named_Titles/Burnout%203%20-%20Takedown%20%28USA%29.png",
          "title url: %s", PictureUrl("Sony - PlayStation 2", kTitle, "Burnout 3 - Takedown (USA)").c_str());
    CHECK(PictureUrl("MAME", kSnap, "1941_ Counter Attack") == "https://thumbnails.libretro.com/MAME/Named_Snaps/1941_%20Counter%20Attack.png", "snap url");
    CHECK(ListingUrl("FBNeo - Arcade Games", kTitle) == "https://thumbnails.libretro.com/FBNeo%20-%20Arcade%20Games/Named_Titles/", "listing url");
    CHECK(PictureUrl("DOS", kTitle, "Caf\xC3\xA9") == "https://thumbnails.libretro.com/DOS/Named_Titles/Caf%C3%A9.png", "UTF-8 is percent-encoded byte by byte");
    CHECK(PercentDecode(UrlEncode("a b&c/d%e\xC3\xA9")) == "a b&c/d%e\xC3\xA9", "encode and decode round-trip");
}

static void test_fit() {
    auto eq = [](Rect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; };
    Rect r;
    r = FitPicture(320, 240); CHECK(eq(r, 0, 0, 720, 480), "4:3 fills: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    r = FitPicture(256, 224); CHECK(eq(r, 0, 0, 720, 480), "SNES fills");
    r = FitPicture(384, 224); CHECK(eq(r, 0, 0, 720, 480), "a wide arcade board still fills (it was 4:3 on the tube)");
    r = FitPicture(240, 160); CHECK(eq(r, 0, 0, 720, 480), "3:2 fills");
    r = FitPicture(640, 480); CHECK(eq(r, 0, 0, 720, 480), "a PS2 snap fills");
    r = FitPicture(1920, 1080); CHECK(eq(r, 0, 60, 720, 360), "16:9 is letter-boxed: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    r = FitPicture(1280, 720); CHECK(eq(r, 0, 60, 720, 360), "720p too");
    r = FitPicture(224, 320); CHECK(r.w == 378 && r.h == 480 && r.x == 170 && r.y == 0, "a vertical board is pillar-boxed: %d,%d %dx%d", r.x, r.y, r.w, r.h);
    r = FitPicture(0, 0); CHECK(eq(r, 0, 0, 720, 480), "nonsense size falls back to the canvas");
    for (int w : { 100, 224, 256, 320, 480, 640, 720, 1280, 1920, 3840 })
        for (int h : { 100, 224, 240, 320, 480, 720, 1080, 2160 }) {
            r = FitPicture(w, h);
            CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 720 && r.y + r.h <= 480 && r.w >= 2 && r.h >= 2, "%dx%d lands inside the canvas", w, h);
            CHECK(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1), "%dx%d: even edges (%d,%d %dx%d)", w, h, r.x, r.y, r.w, r.h);
        }
}

static void fill(std::vector<uint8_t>& px, int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    px.resize((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) { px[i * 4] = b; px[i * 4 + 1] = g; px[i * 4 + 2] = r; px[i * 4 + 3] = a; }
}

static void test_compose() {
    std::vector<uint8_t> out((size_t)kCanvasW * kCanvasH * 3);
    std::vector<uint8_t> px;

    // a solid picture stays solid, whichever way it is scaled
    fill(px, 4, 3, 10, 20, 200);
    ComposeBGR(px.data(), 4, 3, out.data());
    bool solid = true;
    for (size_t i = 0; i < out.size(); i += 3) solid &= (out[i] == 10 && out[i + 1] == 20 && out[i + 2] == 200);
    CHECK(solid, "an enlarged solid colour is that colour everywhere (B,G,R order)");

    fill(px, 1440, 960, 90, 80, 70);
    ComposeBGR(px.data(), 1440, 960, out.data());
    solid = true;
    for (size_t i = 0; i < out.size(); i += 3) solid &= (out[i] == 90 && out[i + 1] == 80 && out[i + 2] == 70);
    CHECK(solid, "a shrunk solid colour is that colour everywhere");

    // letter-boxing leaves black bars
    fill(px, 1920, 1080, 255, 255, 255);
    ComposeBGR(px.data(), 1920, 1080, out.data());
    auto at = [&](int x, int y, int c) { return out[((size_t)y * kCanvasW + x) * 3 + c]; };
    CHECK(at(360, 10, 0) == 0 && at(360, 59, 1) == 0 && at(360, 420, 2) == 0 && at(360, 479, 0) == 0, "bars above and below are black");
    CHECK(at(360, 60, 0) == 255 && at(360, 239, 1) == 255 && at(0, 100, 2) == 255 && at(719, 419, 0) == 255, "the picture fills the middle");

    // a see-through pixel shows black
    fill(px, 8, 6, 200, 200, 200, 0);
    ComposeBGR(px.data(), 8, 6, out.data());
    solid = true;
    for (size_t i = 0; i < out.size(); i++) solid &= (out[i] == 0);
    CHECK(solid, "alpha 0 is black");

    // two columns, black and white, enlarged: dark on the left, light on the right, a soft join in the middle
    px.resize(2 * 2 * 4);
    for (int y = 0; y < 2; y++) {
        uint8_t* a = &px[(size_t)y * 8];
        a[0] = a[1] = a[2] = 0; a[3] = 255;
        a[4] = a[5] = a[6] = 255; a[7] = 255;
    }
    ComposeBGR(px.data(), 2, 2, out.data());
    CHECK(at(0, 240, 0) == 0 && at(719, 240, 0) == 255, "ends keep their colours");
    CHECK(at(360, 240, 0) > 60 && at(360, 240, 0) < 200, "the middle is a blend, not a hard edge: %d", at(360, 240, 0));
    bool mono = true;
    for (int x = 1; x < 720; x++) mono &= at(x, 240, 0) >= at(x - 1, 240, 0);
    CHECK(mono, "and the ramp only goes up");

    // a null or empty source just gives a black canvas
    ComposeBGR(nullptr, 0, 0, out.data());
    solid = true;
    for (size_t i = 0; i < out.size(); i++) solid &= (out[i] == 0);
    CHECK(solid, "no picture, black canvas");
}

int main() {
    test_names();
    test_listing();
    test_matcher();
    test_repos();
    test_urls();
    test_fit();
    test_compose();
    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
