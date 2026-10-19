#include "integrity_check.h"

#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#define TAG "CS-Integrity"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {

// Compares one executable file-backed mapping against the same bytes of its
// backing file. Exactly what a byte-compare detector does, and therefore what
// a self-check has to pass.
void check_mapping(const char* lib, const char* path, uintptr_t start, uintptr_t end, long offset) {
    const uintptr_t len = end - start;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGI("%s region=%lx-%lx file=%s diff_bytes=-1 verdict=UNREADABLE", lib, start, end, path);
        return;
    }

    size_t diffs = 0;
    uintptr_t first_diff = 0;
    const size_t kChunk = 64 * 1024;
    static __thread unsigned char mem_buf[kChunk];
    static __thread unsigned char file_buf[kChunk];

    for (uintptr_t at = 0; at < len; at += kChunk) {
        size_t want = len - at;
        if (want > kChunk) want = kChunk;
        memcpy(mem_buf, reinterpret_cast<const void*>(start + at), want);
        ssize_t got = pread(fd, file_buf, want, offset + (long)at);
        if (got < 0 || (size_t)got < want) {
            diffs = (size_t)-1;
            break;
        }
        for (size_t i = 0; i < want; i++) {
            if (mem_buf[i] != file_buf[i]) {
                if (!diffs) first_diff = at + i;
                diffs++;
            }
        }
    }
    close(fd);

    if (diffs == (size_t)-1) {
        LOGI("%s region=%lx-%lx file=%s diff_bytes=-1 verdict=UNREADABLE", lib, start, end, path);
    } else {
        LOGI("%s region=%lx-%lx+%lx file=%s diff_bytes=%zu first=%lx verdict=%s", lib, start, end,
             len, path, diffs, first_diff, diffs == 0 ? "PRISTINE" : "MODIFIED");
    }
}

}  // namespace

void integrity_report(const char* lib_name) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return;

    const size_t nlen = strlen(lib_name);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0, off = 0, ino = 0;
        char perms[5] = {0};
        char path[400] = {0};
        unsigned devmaj = 0, devmin = 0;
        if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %399s", &start, &end, perms, &off, &devmaj,
                   &devmin, &ino, path) < 7) {
            continue;
        }
        const size_t plen = strlen(path);
        if (plen < nlen || strcmp(path + plen - nlen, lib_name) != 0) continue;
        if (perms[2] != 'x') continue;  // only the executable segment carries code
        check_mapping(lib_name, path, start, end, (long)off);
    }
    fclose(f);
}
