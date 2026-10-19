#pragma once
#include <cstdint>

// GOT/PLT hooking: retarget every GOT entry that resolves `symbol`, across
// every module loaded in this process, instead of patching the callee's code.
//
// Why: an inline hook rewrites the first instructions of the target function,
// so the in-memory bytes of libaaudio.so / libOpenSLES.so no longer match the
// on-disk image - exactly what a memory-integrity (byte-compare) check looks
// for. A GOT entry lives in the *caller's* data section, is ordinary relocated
// data, and leaves every system library's .text byte-for-byte pristine.
//
// The dlopen/dlsym interposition below exists for coverage, not for stealth:
// a module loaded (or resolved) after install would otherwise bind its GOT to
// the real functions and slip past the audio hooks. When either is called we
// re-run the scan (idempotent - already-patched entries are skipped) or hand
// back our wrapper for a hooked symbol directly.
namespace got_hook {

// Registers `symbol` and scans all currently loaded modules. Returns the
// number of GOT entries patched. `*orig` receives the first real address
// seen (what the entries pointed at before us).
int hook_all(const char* symbol, void* replacement, void** orig);

// Re-scans every module for every registered hook (new dlopen'd callers).
// Idempotent. Returns the total number of GOT entries now pointing at our
// replacements across all hooks, so a caller driving periodic rescans can
// log only when the number moves.
int refresh();

// How many GOT entries the hook registered for `symbol` has patched so far;
// 0 for a symbol that was never registered.
int hits_of(const char* symbol);

// Pointers the interposition needs wired up once, before hook_all installs
// its own hooks for dlopen/dlsym (they are captured via dlsym first).
void set_real_dlopen(void* fn);
void set_real_dlsym(void* fn);

// One-pointer store through the same crash-proof channel as the scanner
// (used by the OpenSL ES vtable swap as well).
bool write_ptr(void* addr, void* value);

// One-stop wiring: captures the real dlopen/dlsym/android_dlopen_ext via
// dlsym, then registers GOT hooks for all three. Call once, before the
// symbol hooks, so any dlopen afterwards rescans for new callers.
void install_loader_hooks();

}  // namespace got_hook
