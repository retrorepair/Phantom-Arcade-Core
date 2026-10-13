// fbneo_sets_test.cpp - the parser for FBNeo's -listinfo list.
//
//   cl /nologo /std:c++17 /EHsc host\fbneo_sets_test.cpp /Fe:fbneo_sets_test.exe
//
// Runs on a small hand-written sample, and additionally on a real capture of
// `fcadefbneo.exe -listinfo` if its path is given as the first argument. Exit status is the
// number of failed checks.
#include "fbneo_sets.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    const char* sample =
        "<?xml version=\"1.0\"?>\n<datafile>\n\t<header><name>x</name></header>\n"
        "\t<game name=\"ssf2t\">\n\t\t<description>Super Street Fighter II Turbo (super street fighter 2 X 940223 etc)</description>\n"
        "\t\t<year>1994</year>\n\t\t<manufacturer>Capcom</manufacturer>\n\t\t<rom name=\"sfxe.03c\" size=\"524288\" crc=\"2fa1f396\"/>\n\t</game>\n"
        "\t<game isbios=\"yes\" name=\"neogeo\">\n\t\t<description>Neo Geo MVS &amp; AES System BIOS</description>\n\t\t<year>1990</year>\n"
        "\t\t<manufacturer>SNK</manufacturer>\n\t</game>\n"
        "\t<game name=\"kof98\" cloneof=\"neogeo\" romof=\"neogeo\">\n\t\t<description>The King of Fighters &apos;98 &quot;Amazing&quot;</description>\n"
        "\t\t<year>1998</year>\n\t\t<manufacturer>SNK</manufacturer>\n\t</game>\n"
        "\t<game name=\"nodesc\">\n\t\t<rom name=\"a\" size=\"1\" crc=\"00000000\"/>\n\t</game>\n"
        "</datafile>\n";

    std::printf("a small DAT\n");
    auto sets = ParseFbneoDat(sample);
    check(sets.size() == 4, "four games found, and the header's <name> is not mistaken for one");
    check(sets.count("ssf2t") == 1, "ssf2t is there");
    check(sets["ssf2t"].year == "1994" && sets["ssf2t"].manufacturer == "Capcom", "its year and manufacturer are read");
    check(sets["ssf2t"].description.find("Super Street Fighter II Turbo") == 0, "its description is read");
    check(!sets["ssf2t"].isBios, "a game is not a BIOS");
    check(sets["neogeo"].isBios, "isbios=\"yes\" marks a BIOS set, even written before name= as FBNeo does");
    check(sets["neogeo"].description == "Neo Geo MVS & AES System BIOS", "&amp; is decoded");
    check(sets["kof98"].description == "The King of Fighters '98 \"Amazing\"", "&apos; and &quot; are decoded");
    check(!sets["kof98"].isBios, "a clone with cloneof/romof attributes is still a game");
    check(sets["nodesc"].description.empty() && sets["nodesc"].year.empty(), "a game with no description or year gets empty fields, not its neighbour's");
    check(sets.count("mame_only_set") == 0, "a set the list does not hold is not supported");

    std::printf("input that is not a DAT\n");
    check(ParseFbneoDat("").empty(), "empty input gives an empty map");
    check(ParseFbneoDat("this is not xml at all").empty(), "garbage gives an empty map");
    check(ParseFbneoDat("<game name=\"truncat").empty(), "a truncated tag gives an empty map rather than crashing");

    if (argc > 1) {
        std::printf("a real capture: %s\n", argv[1]);
        std::ifstream f(argv[1], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        auto real = ParseFbneoDat(ss.str());
        std::printf("  parsed %zu games\n", real.size());
        check(real.size() > 5000, "thousands of games parsed");
        check(real.count("ssf2t") == 1 && real["ssf2t"].manufacturer == "Capcom", "ssf2t is in it, by Capcom");
        size_t bios = 0;
        for (auto& kv : real) if (kv.second.isBios) bios++;
        check(bios > 0 && bios < 100, "a handful of BIOS sets are marked");
        check(real.count("neogeo") == 1 && real["neogeo"].isBios, "neogeo is a BIOS");
    }

    std::printf("\n%d check(s) failed\n", g_fail);
    return g_fail;
}
