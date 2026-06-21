/**
 * BG3SE-macOS - Runtime Symbol Resolver
 *
 * Resolves game addresses by symbol name from the main executable's LC_SYMTAB
 * (which BG3 ships un-stripped, including local symbols). This makes
 * address-dependent features version-independent: the same mangled symbol name
 * resolves correctly across game patches, where hardcoded Ghidra addresses
 * shift and break.
 *
 * Use resolve_addr() at call sites: it tries the symbol first and falls back to
 * the legacy hardcoded Ghidra address only when the game version matches the
 * known-good build (so stripped/older binaries keep working exactly as before).
 */

#ifndef SYMBOL_RESOLVER_H
#define SYMBOL_RESOLVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Locate the main executable's symbol table (LC_SYMTAB via __LINKEDIT) and
 * cache it. Safe to call multiple times; the first successful call wins.
 * @return true if a usable symbol table was found.
 */
bool symbol_resolver_init(void);

/** @return true if the symbol table was located and resolution is available. */
bool symbol_resolver_available(void);

/**
 * Resolve a single mangled symbol (e.g. "__ZN8RPGStats5m_ptrE") to its slid
 * runtime address. A single leading underscore difference is tolerated.
 * @return runtime address, or NULL if not found / not initialized.
 */
void *symbol_resolve(const char *mangled_name);

/**
 * Resolve up to `count` mangled names in a SINGLE symbol-table pass.
 * out[i] is set to the resolved address or NULL. Use for large batches
 * (e.g. component TypeId globals) where per-name scans would be too slow.
 * @return number of names resolved (non-NULL outputs).
 */
int symbol_resolve_batch(const char *const *names, void **out, int count);

/**
 * Symbol-first address resolution with a version-gated hardcoded fallback.
 *   1. symbol_resolve(mangled_name)
 *   2. else if version_detect_matches(): binary_base + (ghidra_fallback - 0x100000000)
 *   3. else NULL
 * @param ghidra_fallback Legacy Ghidra address (0 to skip the fallback).
 */
void *resolve_addr(const char *mangled_name, uint64_t ghidra_fallback);

#ifdef __cplusplus
}
#endif

#endif // SYMBOL_RESOLVER_H
