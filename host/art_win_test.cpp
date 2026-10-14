/*
 * art_win_test.cpp - the Windows half of the artwork code: WIC decodes a picture, WinHTTP reaches a server.
 *
 *   art_win_test            offline: decode a small embedded PNG; refuse bytes that are not a picture; an unreachable
 *                           server gives -1 rather than a hang or a crash
 *   art_win_test --live <out-folder>
 *                           also goes to the real thumbnail server for a handful of games across systems, through the
 *                           whole store, and writes each canvas the cabinet would get as a .ppm in <out-folder>
 *
 * Exit status is the number of failed checks.
 */
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "art_win.h"

namespace fs = std::filesystem;
using namespace phantom::art;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

// 2x2, RGBA: red, green / blue, white at half opacity
static const uint8_t kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf,
    0xc0, 0xf0, 0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x28, 0xa0, 0xdb, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

static void test_decode() {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    CHECK(DecodeImage(std::vector<uint8_t>(kPng, kPng + sizeof(kPng)), w, h, px), "a small PNG decodes");
    CHECK(w == 2 && h == 2 && px.size() == 16, "2x2, 16 bytes (%dx%d, %zu)", w, h, px.size());
    if (px.size() == 16) {
        CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255, "top left is red, stored B,G,R,A");
        CHECK(px[4] == 0 && px[5] == 255 && px[6] == 0, "top right is green");
        CHECK(px[8] == 255 && px[9] == 0 && px[10] == 0, "bottom left is blue");
        CHECK(px[15] >= 126 && px[15] <= 129, "bottom right keeps its half opacity (%d)", px[15]);
    }
    std::vector<uint8_t> junk(200, 'x');
    CHECK(!DecodeImage(junk, w, h, px), "bytes that are not a picture are refused");
    CHECK(!DecodeImage(std::vector<uint8_t>(), w, h, px), "an empty file is refused");
    std::vector<uint8_t> cut(kPng, kPng + 40);
    CHECK(!DecodeImage(cut, w, h, px), "a truncated PNG is refused");

    // from several threads at once, as the store's workers do
    std::atomic<int> good{ 0 };
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 20; k++) {
                int ww, hh;
                std::vector<uint8_t> p;
                if (DecodeImage(std::vector<uint8_t>(kPng, kPng + sizeof(kPng)), ww, hh, p) && ww == 2) good++;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(good == 120, "decoding from six threads: %d of 120", good.load());

    // and the whole way to the canvas
    std::vector<uint8_t> canvas((size_t)kCanvasW * kCanvasH * 3);
    DecodeImage(std::vector<uint8_t>(kPng, kPng + sizeof(kPng)), w, h, px);
    ComposeBGR(px.data(), w, h, canvas.data());
    CHECK(canvas[(size_t)(10 * kCanvasW + 10) * 3 + 2] > 200, "red shows at the top left of the canvas");
    CHECK(canvas[(size_t)(10 * kCanvasW + 700) * 3 + 1] > 200, "green at the top right");
}

static void test_http_offline() {
    std::vector<uint8_t> body;
    // .invalid is reserved and never resolves
    CHECK(HttpGet("https://no-such-host.invalid/x.png", body) == -1, "an unreachable host is -1");
    CHECK(body.empty(), "and leaves no body");
    CHECK(HttpGet("not a url", body) == -1, "a bad url is -1");
    CHECK(HttpGet("", body) == -1, "an empty url is -1");
}

static void write_ppm(const fs::path& p, const std::vector<uint8_t>& bgr) {
    std::ofstream f(p, std::ios::binary);
    f << "P6\n720 480\n255\n";
    std::vector<uint8_t> rgb(bgr.size());
    for (size_t i = 0; i + 2 < bgr.size(); i += 3) { rgb[i] = bgr[i + 2]; rgb[i + 1] = bgr[i + 1]; rgb[i + 2] = bgr[i]; }
    f.write((const char*)rgb.data(), (std::streamsize)rgb.size());
}

static void test_live(const fs::path& out) {
    std::error_code ec;
    fs::create_directories(out, ec);
    const fs::path root = out / "store";
    fs::remove_all(root, ec);

    struct Sample { const char* id; const char* title; const char* sys; const char* rom; };
    const Sample samples[] = {
        { "pcsx2_Burnout 3 - Takedown (USA)", "Burnout 3 - Takedown (USA)", "pcsx2", "C:/Games/PS2/Burnout 3 - Takedown (USA).iso" },
        { "pcsx2_Gradius V (USA)", "Gradius V (USA)", "pcsx2", "C:/Games/PS2/Gradius V (USA).iso" },
        { "fbneo_sfa3", "Street Fighter Alpha 3 (World 980904)", "fbneo", "C:/Emulators/fbneo/ROMs/sfa3.zip" },
        { "fbneo_ssf2t", "Super Street Fighter II Turbo (World 940223)", "fbneo", "C:/Emulators/fbneo/ROMs/ssf2t.zip" },
        { "fbneo_1942", "1942 (Revision B)", "fbneo", "C:/Emulators/fbneo/ROMs/1942.zip" },
        { "groovymame_dodonpachi", "DoDonPachi Dai-Ou-Jou (2002.04.05 Master Ver)", "groovymame", "C:/Emulators/GroovyMAME/roms/ddpdoj.zip" },
        { "rpcs3_Demon's Souls", "Demon's Souls", "rpcs3", "C:/Games/PS3/Demon's Souls (USA).iso" },
        { "xemu_Halo", "Halo - Combat Evolved", "xemu", "C:/Games/Xbox/Halo - Combat Evolved (USA).iso" },
    };

    Store st(root, WindowsServices(), 3);
    std::vector<std::pair<Kind, GameRef>> asked;
    for (const Sample& s : samples)
        for (int k = 0; k < kKinds; k++) {
            GameRef g{ s.id, s.title, s.sys, s.rom };
            std::vector<uint8_t> c;
            st.Get((Kind)k, g, &c);
            asked.push_back({ (Kind)k, g });
        }
    CHECK(st.WaitIdle(180000), "the downloads finish");
    int ready = 0;
    for (auto& a : asked) {
        std::vector<uint8_t> c;
        const Got r = st.Get(a.first, a.second, &c);
        printf("  %-6s %-48s %s\n", KindName(a.first), a.second.title.c_str(), r == Got::Ready ? "ready" : r == Got::None ? "none" : "PENDING");
        if (r == Got::Ready) {
            ready++;
            CHECK(c.size() == (size_t)kCanvasW * kCanvasH * 3, "a canvas");
            std::string name = std::string(KindName(a.first)) + "_" + FileSafe(a.second.id) + ".ppm";
            write_ppm(out / name, c);
        }
    }
    const Stats s = st.GetStats();
    printf("  %d ready; kept %d titles, %d snaps; online=%d\n", ready, s.have[kTitle], s.have[kSnap], (int)s.online);
    CHECK(ready >= 8, "most of the samples have pictures (%d of %zu)", ready, asked.size());
    CHECK(s.online == 1, "the server was reached");
}

int main(int argc, char** argv) {
    test_decode();
    test_http_offline();
    if (argc >= 3 && !strcmp(argv[1], "--live")) test_live(argv[2]);
    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
