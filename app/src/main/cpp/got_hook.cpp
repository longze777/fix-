// GOT/PLT hooking - see got_hook.h for the why.
//
// The scanner is a focused re-implementation of the classic xHook/ByteHook
// walk: parse /proc/self/maps, for every mapped ELF find its PT_DYNAMIC, walk
// the relocation tables (JMPREL + RELA/REL) and retarget the entries whose
// symbol name matches. Only R_*_GLOB_DAT and R_*_JUMP_SLOT are touched, and
// only on the three ABIs this APK ships (arm64-v8a, armeabi-v7a, x86_64).
//
// Every table read goes through safe_read() (pread on /proc/self/mem), never
// through a raw pointer: a module whose tables lie about their own bounds
// then reads back EFAULT instead of handing this process a SIGSEGV. That
// matters twice over - the crash itself, and what follows it: the crash
// SDKs inside the host app resolve symbols from their own signal handler,
// their dlopen routes through our hook, and a re-entrant lock grab on the
// scan's own mutex is a self-deadlock. Hence also the try-lock + in-hook
// guard on the interposers: a signal-handler dlopen never waits, it just
// skips the rescan.

#include "got_hook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define TAG "CS-GotHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace got_hook {
namespace {

struct HookSlot {
    std::string symbol;
    void* replacement;
    void** orig;      // first real address seen (null until a GOT hit)
    int hits;         // entries patched so far
};

std::mutex g_lock;
std::vector<HookSlot> g_hooks;

void* g_real_dlopen = nullptr;
void* g_real_dlext = nullptr;
void* g_real_dlsym = nullptr;

// Set while any thread is inside scan_all_locked, so a signal-handler
// re-entry (the host crash SDK dlopening from its SIGSEGV handler) is
// recognised and stepped over instead of waiting on the scan's own lock.
thread_local bool t_in_scan = false;

// ---------------------------------------------------------------------------
// Crash-proof reads: every dereference of possibly-lying ELF metadata goes
// through this. EFAULT reads are a skip, not a signal.
// ---------------------------------------------------------------------------

int g_self_mem = -1;

bool open_self_mem() {
    if (g_self_mem < 0) {
        g_self_mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (g_self_mem < 0) return false;
    }
    return true;
}

bool safe_read(uintptr_t addr, void* buf, size_t len) {
    if (!open_self_mem()) return false;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(g_self_mem, reinterpret_cast<char*>(buf) + done, len - done,
                          (off64_t)(addr + done));
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

// Writes one pointer through /proc/self/mem: page protections are bypassed
// (no mprotect dance on RELRO) and a target unmapped since the maps snapshot
// - a dlclose that raced the scan - comes back as an error instead of
// delivering a SIGSEGV to the host.
bool safe_write_ptr(uintptr_t addr, void* value) {
    if (!open_self_mem()) return false;
    ssize_t n = pwrite(g_self_mem, &value, sizeof(value), (off64_t)addr);
    return n == (ssize_t)sizeof(value);
}

template <typename T>
bool safe_read_val(uintptr_t addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// ---------------------------------------------------------------------------
// /proc/self/maps
// ---------------------------------------------------------------------------

struct Mapping {
    uintptr_t start;
    uintptr_t end;
    char perms[5];  // "r-xp"
};

struct Module {
    std::string path;
    uintptr_t base;                 // lowest mapping start
    std::vector<Mapping> mappings;  // all mappings of this file
};

bool parse_maps(std::vector<Module>& out) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;

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
        // Only file-backed entries matter; anon/linker-internal regions have
        // no path or a bracketed pseudo-name.
        if (path[0] == '\0' || path[0] == '[') continue;
        if (ino == 0) continue;
        if (strstr(path, "/linker")) continue;  // linker + linker64
        if (strstr(path, "ld-android")) continue;
        if (strstr(path, "/memfd:")) continue;

        Module* m = nullptr;
        for (auto& cand : out) {
            if (cand.path == path) {
                m = &cand;
                break;
            }
        }
        if (!m) {
            out.push_back(Module());
            m = &out.back();
            m->path = path;
            m->base = start;
        }
        Mapping mp;
        mp.start = start;
        mp.end = end;
        memcpy(mp.perms, perms, sizeof(mp.perms));
        m->mappings.push_back(mp);
        if (start < m->base) m->base = start;
    }
    fclose(f);
    return true;
}

const Mapping* find_mapping(const Module& m, uintptr_t addr) {
    for (const auto& mp : m.mappings) {
        if (addr >= mp.start && addr < mp.end) return &mp;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ELF walking
// ---------------------------------------------------------------------------

// The dynamic table's pointer entries are link-time vaddrs; some ancient
// linkers pre-biased them. Values below `base` need the bias added.
inline uintptr_t dyn_addr(uintptr_t base, uintptr_t val) {
    return val < base ? base + val : val;
}

// Upper bounds so a corrupt table cannot turn into a gigabyte allocation.
constexpr size_t kMaxTableBytes = 32u << 20;
constexpr size_t kMaxRelocs = 400000;

struct DynTables {
    uintptr_t strtab = 0, strsz = 0, symtab = 0;
    uintptr_t jmprel = 0, pltrelsz = 0;
    uintptr_t rel = 0, relsz = 0;
    size_t syment = 0;
};

// The Ehdr for whichever class the module turns out to be; kept tiny since
// only phoff/phnum/phentsize are needed before the Phdr read.
template <typename Phdr>
struct EhdrProbe;

template <>
struct EhdrProbe<Elf64_Phdr> {
    Elf64_Ehdr ehdr;
};

template <>
struct EhdrProbe<Elf32_Phdr> {
    Elf32_Ehdr ehdr;
};

template <typename Phdr, typename Dyn>
bool read_dynamic(uintptr_t base, const std::vector<Mapping>& maps, DynTables& dt) {
    // Find PT_DYNAMIC from a safely-read copy of the program headers.
    EhdrProbe<Phdr> probe;
    if (!safe_read_val(base, &probe.ehdr)) return false;
    if (probe.ehdr.e_phnum == 0 || probe.ehdr.e_phnum > 512) return false;
    if (probe.ehdr.e_phentsize != (decltype(probe.ehdr.e_phentsize))sizeof(Phdr)) return false;
    std::vector<Phdr> phdrs(probe.ehdr.e_phnum);
    if (!safe_read(base + probe.ehdr.e_phoff, phdrs.data(),
                   phdrs.size() * sizeof(Phdr))) {
        return false;
    }
    uintptr_t dynv = 0;
    for (const auto& p : phdrs) {
        if (p.p_type == PT_DYNAMIC) {
            dynv = base + p.p_vaddr;
            break;
        }
    }
    if (!dynv) return false;

    // Walk the dynamic array in bounded chunks; stop at DT_NULL or at the
    // first entry that cannot be read.
    Dyn chunk[32];
    for (size_t i = 0; i < 4096; i += 32) {
        if (!safe_read(dynv + i * sizeof(Dyn), chunk, sizeof(chunk))) return false;
        for (int k = 0; k < 32; k++) {
            const Dyn& d = chunk[k];
            switch (d.d_tag) {
                case DT_NULL: return dt.strtab != 0 && dt.symtab != 0;
                case DT_STRTAB: dt.strtab = dyn_addr(base, d.d_un.d_ptr); break;
                case DT_STRSZ: dt.strsz = d.d_un.d_val; break;
                case DT_SYMTAB: dt.symtab = dyn_addr(base, d.d_un.d_ptr); break;
                case DT_SYMENT: dt.syment = d.d_un.d_val; break;
                case DT_JMPREL: dt.jmprel = dyn_addr(base, d.d_un.d_ptr); break;
                case DT_PLTRELSZ: dt.pltrelsz = d.d_un.d_val; break;
#if defined(__LP64__)
                case DT_RELA: dt.rel = dyn_addr(base, d.d_un.d_ptr); break;
                case DT_RELASZ: dt.relsz = d.d_un.d_val; break;
#else
                case DT_REL: dt.rel = dyn_addr(base, d.d_un.d_ptr); break;
                case DT_RELSZ: dt.relsz = d.d_un.d_val; break;
#endif
                default: break;
            }
        }
    }
    return dt.strtab != 0 && dt.symtab != 0;
}

// Reads `count` relocation entries through safe_read and hands each to the
// callback as (r_offset, type, symidx). Returns false on a read failure.
template <typename Rel>
bool for_each_reloc(uintptr_t table, size_t count, const std::vector<Mapping>& maps,
                    bool (*cb)(void*, uintptr_t, uint32_t, uint32_t), void* ctx) {
    std::vector<Rel> block;
    block.resize(count < 1024 ? count : 1024);
    size_t done = 0;
    while (done < count) {
        size_t want = count - done;
        if (want > block.size()) want = block.size();
        if (!safe_read(table + done * sizeof(Rel), block.data(), want * sizeof(Rel))) return false;
        for (size_t i = 0; i < want; i++) {
#if defined(__LP64__)
            uintptr_t off = block[i].r_offset;
            uint32_t type = (uint32_t)(block[i].r_info & 0xffffffffULL);
            uint32_t sym = (uint32_t)(block[i].r_info >> 32);
#else
            uintptr_t off = block[i].r_offset;
            uint32_t type = (uint32_t)(block[i].r_info & 0xff);
            uint32_t sym = (uint32_t)(block[i].r_info >> 8);
#endif
            if (!cb(ctx, off, type, sym)) return true;  // callback said stop
        }
        done += want;
    }
    return true;
}

struct ScanCtx {
    uintptr_t bias;
    uintptr_t symtab;
    uintptr_t strtab;
    size_t syment;
    std::vector<char> strtab_buf;  // whole string table, safely read
    const Module* mod = nullptr;
    size_t reloc_seen = 0;
    int patched = 0;
};

// Reads the string table once (bounded by DT_STRSZ). The symbol table has no
// size tag; the usual layout places strtab right after it, which bounds it.
static bool preload_tables(ScanCtx& c, const DynTables& dt) {
    c.symtab = dt.symtab;
    c.strtab = dt.strtab;
    c.syment = dt.syment;
    size_t strsz = dt.strsz && dt.strsz < kMaxTableBytes ? (size_t)dt.strsz : 0;
    if (strsz == 0) return false;
    c.strtab_buf.resize(strsz);
    if (!safe_read(dt.strtab, c.strtab_buf.data(), strsz)) return false;
    return true;
}

static bool reloc_cb(void* ctxp, uintptr_t r_offset, uint32_t type, uint32_t symidx) {
    ScanCtx& c = *reinterpret_cast<ScanCtx*>(ctxp);
#if defined(__aarch64__)
    if (type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) return true;
#elif defined(__x86_64__)
    if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) return true;
#elif defined(__arm__)
    if (type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) return true;
#endif
    if (symidx == 0 || c.reloc_seen >= kMaxRelocs) return true;
    c.reloc_seen++;

    // One symbol-table entry, safely read. A symidx past where strtab begins
    // (the symtab's usual end) is rejected without touching memory.
    uintptr_t sym_addr = c.symtab + (uintptr_t)symidx * c.syment;
    if (c.strtab > c.symtab && sym_addr + c.syment > c.strtab) return true;
    uint8_t sym[64];
    if (c.syment > sizeof(sym)) return true;
    if (!safe_read(sym_addr, sym, c.syment)) return true;
    uint32_t st_name =
#if defined(__LP64__)
        reinterpret_cast<Elf64_Sym*>(sym)->st_name;
#else
        reinterpret_cast<Elf32_Sym*>(sym)->st_name;
#endif
    if (st_name >= c.strtab_buf.size()) return true;
    const char* name = c.strtab_buf.data() + st_name;
    // NUL-termination is guaranteed by the table read bounds check above only
    // for the last byte; scan to the buffer end conservatively.
    size_t room = c.strtab_buf.size() - st_name;
    if (room == 0 || memchr(name, '\0', room) == nullptr) return true;

    HookSlot* slot = nullptr;
    for (auto& h : g_hooks) {
        if (h.symbol == name) {
            slot = &h;
            break;
        }
    }
    if (!slot) return true;

    void** got = reinterpret_cast<void**>(c.bias + r_offset);
    void* cur = nullptr;
    if (!safe_read_val(reinterpret_cast<uintptr_t>(got), &cur)) return true;
    if (cur == slot->replacement) return true;  // ours already
    // An entry still holding null belongs to a module mid-load: the linker
    // has not applied this relocation yet, and patching it now would leave
    // the wrapper with a null original once the linker binds the real
    // address elsewhere. Skip; a later rescan takes it once it is bound.
    if (cur == nullptr) return true;

    // The GOT address was validated against this module's own mappings, and
    // the store itself goes through /proc/self/mem: a page that went away
    // since the snapshot (dlclose race) fails the write instead of crashing.
    if (!find_mapping(*c.mod, reinterpret_cast<uintptr_t>(got))) return true;
    if (slot->orig && *slot->orig == nullptr && cur) *slot->orig = cur;
    if (!safe_write_ptr(reinterpret_cast<uintptr_t>(got), slot->replacement)) return true;
    slot->hits++;
    c.patched++;
    return true;
}

// Pulls the pieces together per module. `base` is the ELF's lowest mapping;
// magic-checked by the caller.
int scan_module(Module& mod) {
    if (t_in_scan) return 0;
    t_in_scan = true;
    struct Guard {
        ~Guard() { t_in_scan = false; }
    } guard;

    // Read the ELF identity directly: magic + class.
    uint8_t ident[5] = {0};
    if (!safe_read(mod.base, ident, sizeof(ident))) return 0;
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') return 0;
    const uint8_t eclass = ident[4];

    // bias = base - lowest PT_LOAD vaddr; Android .so files link at 0 so this
    // is normally base itself. Recomputed below from safely-read phdrs.
    uintptr_t lo = ~uintptr_t(0);
    DynTables dt;
    bool ok = false;
    if (eclass == ELFCLASS64) {
        ok = read_dynamic<Elf64_Phdr, Elf64_Dyn>(mod.base, mod.mappings, dt);
        if (ok && dt.syment == 0) dt.syment = sizeof(Elf64_Sym);
        if (ok && dt.syment != sizeof(Elf64_Sym)) ok = false;
    } else if (eclass == ELFCLASS32) {
        ok = read_dynamic<Elf32_Phdr, Elf32_Dyn>(mod.base, mod.mappings, dt);
        if (ok && dt.syment == 0) dt.syment = sizeof(Elf32_Sym);
        if (ok && dt.syment != sizeof(Elf32_Sym)) ok = false;
    }
    if (!ok) return 0;

    // Recompute the bias from the (safely read) program headers.
    uintptr_t bias = mod.base;
    {
        if (eclass == ELFCLASS64) {
            Elf64_Ehdr eh;
            if (safe_read_val(mod.base, &eh) && eh.e_phnum > 0 && eh.e_phnum <= 512) {
                std::vector<Elf64_Phdr> ph(eh.e_phnum);
                if (safe_read(mod.base + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf64_Phdr))) {
                    for (const auto& p : ph) {
                        if (p.p_type == PT_LOAD && (uintptr_t)p.p_vaddr < lo) lo = p.p_vaddr;
                    }
                }
            }
        } else {
            Elf32_Ehdr eh;
            if (safe_read_val(mod.base, &eh) && eh.e_phnum > 0 && eh.e_phnum <= 512) {
                std::vector<Elf32_Phdr> ph(eh.e_phnum);
                if (safe_read(mod.base + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf32_Phdr))) {
                    for (const auto& p : ph) {
                        if (p.p_type == PT_LOAD && (uintptr_t)p.p_vaddr < lo) lo = p.p_vaddr;
                    }
                }
            }
        }
        if (lo != ~uintptr_t(0)) bias = mod.base - lo;
    }

    ScanCtx ctx;
    ctx.bias = bias;
    ctx.mod = &mod;
    if (!preload_tables(ctx, dt)) return 0;

    size_t jmp_count = dt.pltrelsz / (eclass == ELFCLASS64 ? sizeof(Elf64_Rela) : sizeof(Elf32_Rel));
    size_t rel_count = dt.relsz / (eclass == ELFCLASS64 ? sizeof(Elf64_Rela) : sizeof(Elf32_Rel));
    if (jmp_count > kMaxRelocs) jmp_count = kMaxRelocs;
    if (rel_count > kMaxRelocs) rel_count = kMaxRelocs;

    if (dt.jmprel && jmp_count) {
        if (eclass == ELFCLASS64) {
            for_each_reloc<Elf64_Rela>(dt.jmprel, jmp_count, mod.mappings, reloc_cb, &ctx);
        } else {
            for_each_reloc<Elf32_Rel>(dt.jmprel, jmp_count, mod.mappings, reloc_cb, &ctx);
        }
    }
    if (dt.rel && rel_count) {
        if (eclass == ELFCLASS64) {
            for_each_reloc<Elf64_Rela>(dt.rel, rel_count, mod.mappings, reloc_cb, &ctx);
        } else {
            for_each_reloc<Elf32_Rel>(dt.rel, rel_count, mod.mappings, reloc_cb, &ctx);
        }
    }
    return ctx.patched;
}

void scan_all_locked() {
    std::vector<Module> modules;
    if (!parse_maps(modules)) return;
    Dl_info info{};
    const char* self = dladdr((void*)&scan_all_locked, &info) ? info.dli_fname : nullptr;
    size_t self_len = self ? strlen(self) : 0;
    for (auto& m : modules) {
        if (self && m.path == self) continue;
        scan_module(m);
    }
}

// ---------------------------------------------------------------------------
// dlopen / dlsym interposition (coverage for late-loaded callers)
// ---------------------------------------------------------------------------

typedef void* (*dlopen_fn)(const char*, int);
typedef void* (*dlext_fn)(const char*, int, const void*);
typedef void* (*dlsym_fn)(void*, const char*);

// try_lock: if the scan is running (on this thread - a crash SDK resolving
// symbols from its own signal handler - or on another), skipping the rescan
// is always better than waiting. The next dlopen tries again.
static void* hooked_dlopen(const char* filename, int flags) {
    void* r = reinterpret_cast<dlopen_fn>(g_real_dlopen)(filename, flags);
    if (r && !t_in_scan) {
        std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
        if (lock.owns_lock()) scan_all_locked();
    }
    return r;
}

static void* hooked_dlext(const char* filename, int flags, const void* info) {
    void* r = reinterpret_cast<dlext_fn>(g_real_dlext)(filename, flags, info);
    if (r && !t_in_scan) {
        std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
        if (lock.owns_lock()) scan_all_locked();
    }
    return r;
}

static void* hooked_dlsym(void* handle, const char* symbol) {
    void* r = reinterpret_cast<dlsym_fn>(g_real_dlsym)(handle, symbol);
    if (!r || !symbol || t_in_scan) return r;
    std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
    if (!lock.owns_lock()) return r;
    for (auto& h : g_hooks) {
        if (h.symbol != symbol) continue;
        if (h.orig && *h.orig == nullptr) *h.orig = r;  // first real address lands here
        return h.replacement;
    }
    return r;
}

}  // namespace

int hook_all(const char* symbol, void* replacement, void** orig) {
    if (!symbol || !replacement) return 0;
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& h : g_hooks) {
        if (h.symbol == symbol) return h.hits;  // one hook per symbol
    }
    HookSlot slot;
    slot.symbol = symbol;
    slot.replacement = replacement;
    slot.orig = orig;
    slot.hits = 0;
    if (orig) *orig = nullptr;
    g_hooks.push_back(slot);
    scan_all_locked();
    return g_hooks.back().hits;
}

int refresh() {
    std::lock_guard<std::mutex> lock(g_lock);
    scan_all_locked();
    int total = 0;
    for (const auto& h : g_hooks) total += h.hits;
    return total;
}

int hits_of(const char* symbol) {
    if (!symbol) return 0;
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& h : g_hooks) {
        if (h.symbol == symbol) return h.hits;
    }
    return 0;
}

void set_real_dlopen(void* fn) { g_real_dlopen = fn; }
void set_real_dlsym(void* fn) { g_real_dlsym = fn; }

bool write_ptr(void* addr, void* value) {
    return safe_write_ptr(reinterpret_cast<uintptr_t>(addr), value);
}

// Registering these two symbols is what turns hook_all's coverage into
// "everything loaded now or later". Wire the real pointers first.
//
// Off by default: reaching into every module's dlopen/dlsym call graph buys
// coverage for callers loaded *after* this init, which the audio capture
// rarely needs - the scoped app's audio stack is up long before we run. It
// is also the widest surface this module touches. persist.camswap.loader_hook=1
// turns it back on for a device where a late-loading caller is proven.
void install_loader_hooks() {
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.camswap.loader_hook", prop) <= 0 || prop[0] != '1') {
        LOGI("loader interposition off (persist.camswap.loader_hook unset)");
        return;
    }
    if (!g_real_dlopen) g_real_dlopen = dlsym(RTLD_DEFAULT, "dlopen");
    if (!g_real_dlsym) g_real_dlsym = dlsym(RTLD_DEFAULT, "dlsym");
    if (!g_real_dlext) g_real_dlext = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    hook_all("dlopen", (void*)&hooked_dlopen, nullptr);
    if (g_real_dlext) hook_all("android_dlopen_ext", (void*)&hooked_dlext, nullptr);
    hook_all("dlsym", (void*)&hooked_dlsym, nullptr);
    LOGI("loader interposition installed (dlopen/dlsym/lext)");
}

}  // namespace got_hook
