/*
 * rom_enum_test.cpp - the folder walk finds exactly what std::filesystem finds, and finds it fast.
 *
 *   rom_enum_test                  builds a tree of 60,000 files in the temp folder, checks the walk
 *                                  against std::filesystem, and times it with 1, 4 and 8 threads
 *   rom_enum_test <folder>         the same checks and timings on a real folder (nothing is written)
 *   rom_enum_test --make <folder> <files>
 *                                  only creates a synthetic tree of that many tiny files, for the
 *                                  large end-to-end tests
 *
 * Exit status is the number of failed checks.
 */
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "rom_enum.h"

namespace fs = std::filesystem;

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void touch(const fs::path& p, const char* body = "x") {
    std::ofstream f(p, std::ios::binary);
    f << body;
}

// A library shaped like a real one: one big flat folder of sets, some one-folder-per-game, some nesting.
static void make_tree(const fs::path& root, int files) {
    fs::create_directories(root / "flat");
    const int flat = files * 6 / 10;
    for (int i = 0; i < flat; i++) touch(root / "flat" / ("set" + std::to_string(i) + ".zip"));
    const int perDir = 40;
    int made = flat, d = 0;
    while (made < files) {
        const fs::path dir = root / "bygame" / ("g" + std::to_string(d / 20)) / ("game" + std::to_string(d));
        fs::create_directories(dir);
        for (int k = 0; k < perDir && made < files; k++, made++) touch(dir / ("f" + std::to_string(k) + ".chd"));
        d++;
    }
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--make") {
        const fs::path root = argv[2];
        const int files = atoi(argv[3]);
        auto t0 = std::chrono::steady_clock::now();
        make_tree(root, files);
        printf("made %d files under %s in %.1f s\n", files, root.string().c_str(), ms_since(t0) / 1000.0);
        return 0;
    }

    fs::path root;
    bool synthetic = (argc < 2);
    if (synthetic) {
        root = fs::temp_directory_path() / "pa_enum_data";
        std::error_code ec;
        fs::remove_all(root, ec);
        auto t0 = std::chrono::steady_clock::now();
        make_tree(root, 60000);
        printf("made 60000 files in %.1f s\n", ms_since(t0) / 1000.0);
        // a few awkward ones
        fs::create_directories(root / "deep" / "a" / "b" / "c" / "d" / "e" / "f" / "g");
        touch(root / "deep" / "top.zip");
        touch(root / "deep" / "a" / "b" / "c" / "d" / "e" / "five.zip");
        touch(root / "deep" / "a" / "b" / "c" / "d" / "e" / "f" / "six.zip");
        touch(root / "deep" / "a" / "b" / "c" / "d" / "e" / "f" / "g" / "seven.zip");
        touch(root / "UPPER.ZIP");
        fs::create_directories(root / "unicode");
        touch(root / "unicode" / L"café ゲーム.zip");
    } else {
        root = argv[1];
    }

    // the reference: std::filesystem, to the same depth the walk uses
    std::set<std::wstring> ref;
    auto t0 = std::chrono::steady_clock::now();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file()) ref.insert(it->path().wstring());
    }
    const double refMs = ms_since(t0);
    printf("std::filesystem: %zu files in %.0f ms\n", ref.size(), refMs);

    double best = 1e18;
    for (int threads : { 1, 4, 8 }) {
        std::vector<std::vector<std::wstring>> got(threads);
        t0 = std::chrono::steady_clock::now();
        const phantom::EnumStats st = phantom::EnumTree(root.wstring(), 5, threads,
            [&](int w, const std::wstring& dir, const WIN32_FIND_DATAW& fd, int) { got[w].push_back(dir + L"\\" + fd.cFileName); });
        const double ms = ms_since(t0);
        best = std::min(best, ms);

        std::set<std::wstring> all;
        size_t n = 0;
        for (auto& v : got) { n += v.size(); all.insert(v.begin(), v.end()); }
        printf("EnumTree, %d thread(s): %zu files, %llu folders, in %.0f ms\n", threads, n, (unsigned long long)st.dirs, ms);
        CHECK(n == all.size(), "%d threads: a file was reported twice (%zu reported, %zu distinct)", threads, n, all.size());
        CHECK(all == ref, "%d threads: found %zu files, std::filesystem found %zu", threads, all.size(), ref.size());
        CHECK(st.files == ref.size() || st.files >= ref.size(), "file count %llu", (unsigned long long)st.files);
    }
    printf("best walk %.0f ms against %.0f ms for std::filesystem (%.1fx)\n", best, refMs, refMs / best);
    CHECK(best < refMs, "the fast walk is not faster than std::filesystem");

    if (synthetic) {
        // size and modification time arrive with the entry
        bool sized = false;
        phantom::EnumTree((root / "deep").wstring(), 0, 1, [&](int, const std::wstring&, const WIN32_FIND_DATAW& fd, int) {
            if (!wcscmp(fd.cFileName, L"top.zip")) sized = (fd.nFileSizeLow == 1 && phantom::FileTimeToU64(fd.ftLastWriteTime) != 0);
        });
        CHECK(sized, "size/mtime did not come with the entry");

        // depth limit: sixth level is walked, seventh is not
        std::set<std::wstring> names;
        phantom::EnumTree((root / "deep").wstring(), 5, 2, [&](int, const std::wstring&, const WIN32_FIND_DATAW& fd, int) { names.insert(fd.cFileName); });
        CHECK(names.count(L"five.zip") && names.count(L"six.zip") == 0 && names.count(L"seven.zip") == 0 && names.count(L"top.zip"),
              "depth limit not honoured (%zu names)", names.size());

        // a folder that is not there is an empty walk, not a crash
        const phantom::EnumStats none = phantom::EnumTree((root / "nothing-here").wstring(), 5, 4, [](int, const std::wstring&, const WIN32_FIND_DATAW&, int) {});
        CHECK(none.files == 0, "walking a missing folder found files");

        // a junction is not followed, so a loop cannot trap the walk
        const fs::path loop = root / "deep" / "loop";
        const std::wstring cmd = L"cmd /c mklink /J \"" + loop.wstring() + L"\" \"" + (root / "deep").wstring() + L"\" >nul 2>&1";
        _wsystem(cmd.c_str());
        if (fs::exists(loop)) {
            std::set<std::wstring> seen;
            phantom::EnumTree((root / "deep").wstring(), 5, 2, [&](int, const std::wstring& dir, const WIN32_FIND_DATAW& fd, int) { seen.insert(dir + L"\\" + fd.cFileName); });
            CHECK(seen.size() == 2, "walk with a junction in it found %zu files, not the 2 that are really there", seen.size());
            RemoveDirectoryW(loop.wstring().c_str());
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
