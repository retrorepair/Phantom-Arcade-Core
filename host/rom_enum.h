/*
 * rom_enum.h - walk a ROM folder fast.
 *
 * The scan used std::filesystem::recursive_directory_iterator and then, for every file it kept,
 * fs::relative() - which canonicalises both paths against the real filesystem, several system
 * calls a file - and a stat or two more besides. Fine for a thousand games, far too slow for a
 * hundred thousand, and all of it on the window's own thread.
 *
 * This asks Windows for a directory the way Explorer's search does: FindFirstFileExW with
 * FIND_FIRST_EX_LARGE_FETCH (the file system hands back a buffer-full of entries per call instead of
 * a few) and FindExInfoBasic (no 8.3 short names to compute). Name, size and modification time come
 * with each entry, so nothing is stat'ed afterwards. Directories are walked by several threads
 * at once, which is what turns a network share's per-directory latency from a sum into an overlap;
 * on a local disk it simply costs nothing.
 *
 * Windows only. The callback runs on the worker threads and is given the worker number so the caller
 * can keep a per-thread result without a lock.
 */
#ifndef PHANTOM_ROM_ENUM_H
#define PHANTOM_ROM_ENUM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace phantom {

inline uint64_t FileTimeToU64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// A path Windows will open however long it is. Short paths are returned as they were.
inline std::wstring LongPath(const std::wstring& p) {
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

struct EnumStats {
    uint64_t files = 0;
    uint64_t dirs = 0;
};

// Calls onFile(worker, dir, entry, depth) for every regular file under root, to maxDepth levels
// (root's own files are depth 0). `dir` is the containing folder's path with no trailing separator,
// in the spelling it was reached by. Reparse points (junctions, symlinks) are not followed, so a
// loop cannot trap the walk. A folder that cannot be opened is skipped, as the old scan did.
//
// onDir(dir) may be given to refuse a folder before it is entered.
template <class FileFn>
EnumStats EnumTree(const std::wstring& root, int maxDepth, int threads, FileFn&& onFile) {
    struct Item { std::wstring dir; int depth; };

    std::deque<Item> queue;
    std::mutex m;
    std::condition_variable cv;
    size_t busy = 0;          // directories being listed right now
    bool done = false;
    EnumStats total;
    std::mutex totalM;

    std::wstring r = root;
    while (r.size() > 1 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    queue.push_back({ r, 0 });

    auto worker = [&](int id) {
        EnumStats mine;
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return !queue.empty() || done; });
                if (queue.empty()) break;      // done and nothing left
                it = std::move(queue.front());
                queue.pop_front();
                busy++;
            }

            std::vector<Item> subdirs;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((LongPath(it.dir) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
            if (h != INVALID_HANDLE_VALUE) {
                mine.dirs++;
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                        if (it.depth < maxDepth) subdirs.push_back({ it.dir + L"\\" + fd.cFileName, it.depth + 1 });
                    } else {
                        mine.files++;
                        onFile(id, it.dir, fd, it.depth);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }

            {
                std::lock_guard<std::mutex> lk(m);
                for (Item& s : subdirs) queue.push_back(std::move(s));
                busy--;
                if (queue.empty() && busy == 0) done = true;
            }
            cv.notify_all();
        }
        std::lock_guard<std::mutex> lk(totalM);
        total.files += mine.files;
        total.dirs += mine.dirs;
    };

    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; i++) pool.emplace_back(worker, i);
    worker(0);
    for (auto& t : pool) t.join();
    return total;
}

}  // namespace phantom

#endif
