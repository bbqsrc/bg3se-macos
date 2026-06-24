/**
 * BG3SE-macOS - Osiris Function Cache
 *
 * Caches Osiris function metadata (name, ID, arity, type) for fast lookup.
 * Supports both enumeration at init time and dynamic caching from events.
 */

#ifndef BG3SE_OSIRIS_FUNCTIONS_H
#define BG3SE_OSIRIS_FUNCTIONS_H

#include <stdint.h>
#include "osiris_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration
// ============================================================================

// The full Osiris function name-hash holds ~20k functions (engine + story).
// Capacity must exceed that; the lookup tables index g_funcCache via int16_t,
// so MAX_CACHED_FUNCTIONS must stay < 32767.
#define MAX_CACHED_FUNCTIONS 28000
#define FUNC_HASH_SIZE 32768
#define FUNC_NAME_HASH_SIZE 32768
#define MAX_SEEN_FUNC_IDS 256

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the function cache system.
 * Must be called before using any other function cache operations.
 */
void osi_func_cache_init(void);

/**
 * Set the runtime pointers needed for function enumeration.
 * These come from dlsym on libOsiris.dylib.
 *
 * @param pFunctionData Pointer to pFunctionData function
 * @param ppOsiFunctionMan Pointer to global OsiFunctionMan pointer
 */
void osi_func_cache_set_runtime(pFunctionDataFn pFunctionData, void **ppOsiFunctionMan);

/**
 * Set the known events table for static event name lookups.
 * This is a null-terminated array of KnownEvent.
 */
void osi_func_cache_set_known_events(KnownEvent *events);

// ============================================================================
// Enumeration
// ============================================================================

/**
 * Enumerate all Osiris functions by probing ID ranges.
 * Call this after runtime pointers are set and game is initialized.
 * NOTE: only finds engine DIV functions (~1300). Use osi_func_enumerate_hash()
 * after a save's story compiles to also get story QRY_/PROC_/DB_ functions.
 */
void osi_func_enumerate(void);

/**
 * Enumerate ALL Osiris functions by walking the COsiFunctionMan name-hash
 * (1023 red-black-tree buckets). Captures both engine and story-defined
 * functions (~20k once a save is loaded), so Osi.* can call story PROC_/QRY_
 * and Osi.DB_* can query story databases. Idempotent (dedups by funcId).
 */
void osi_func_enumerate_hash(void);

// ============================================================================
// Caching
// ============================================================================

/**
 * Cache a function with known metadata.
 * Used when we observe function calls and already know the details.
 */
void osi_func_cache(const char *name, uint32_t funcId, uint8_t arity, uint8_t type);

/**
 * Try to cache a function by probing its ID.
 * Uses pFunctionData to get metadata if available.
 * @return 1 if successfully cached, 0 otherwise
 */
int osi_func_cache_by_id(uint32_t funcId);

/**
 * Try to cache a function from an observed event.
 * Only caches if not already in cache.
 */
void osi_func_cache_from_event(uint32_t funcId);

// ============================================================================
// Lookup
// ============================================================================

/**
 * Get function name from function ID.
 * @return Function name, or NULL if not found
 */
const char *osi_func_get_name(uint32_t funcId);

/**
 * Look up function ID by name.
 * @return Function ID, or INVALID_FUNCTION_ID if not found
 */
uint32_t osi_func_lookup_id(const char *name);

/**
 * Get function info (arity and type) by name.
 * @return 1 on success, 0 if not found
 */
int osi_func_get_info(const char *name, uint8_t *out_arity, uint8_t *out_type);

/**
 * Get the encoded OsirisFunctionHandle for a function by name.
 * @return Encoded handle, or 0 if not found/not yet computed
 */
uint32_t osi_func_get_handle(const char *name);

/**
 * Get the rete-dispatch info for a story function by name.
 * @param out_funcDef receives the COsiFunctionData* (may be NULL)
 * @param out_nodeId  receives OsiFunctionDef.Node.Id (0 = engine, >0 = story)
 * @return 1 if the function is cached, 0 otherwise
 */
int osi_func_get_node(const char *name, void **out_funcDef, uint32_t *out_nodeId);

/**
 * Resolve the best overload variant of `name` for a given argument count.
 * Story functions are overloaded by arity (each a distinct cache entry); this
 * picks the variant whose arity matches `preferArity` (exact preferred, else the
 * smallest arity >= it, else the first by name). Pass preferArity < 0 to take the
 * first match. Fills any non-NULL outputs. Returns 1 on success, 0 if not cached.
 */
int osi_func_resolve(const char *name, int preferArity, uint8_t *out_arity,
                     uint8_t *out_type, uint32_t *out_id, uint32_t *out_nodeId);

/**
 * Set the encoded handle for a cached function.
 */
void osi_func_cache_set_handle(uint32_t funcId, uint32_t handle);

/**
 * Store per-parameter Osiris types (declaration order) for a cached function.
 * Types are read from funcDef->Signature->Params node list during caching.
 */
void osi_func_cache_set_param_types(uint32_t funcId, const uint8_t *types, uint8_t count);

/**
 * Get per-parameter Osiris types (declaration order) for a function by name.
 * Fills `out` with up to `max` type bytes. Returns the number written (= arity),
 * or 0 if the function is unknown. A type of 0 means "unknown/not read".
 * Types: 1=INTEGER 2=INTEGER64 3=REAL 4=STRING 5=GUIDSTRING (and GUID subtype
 * aliases >=5, all string-pointer storage).
 */
int osi_func_get_param_types(const char *name, uint8_t *out, int max);

/**
 * Probe and dump OsiFunctionDef layout for the first N cached functions.
 * Writes hex dumps to log for offset discovery/validation.
 */
void osi_func_probe_layout(int count);

/**
 * Probe a function by name and print detailed info to console.
 * Shows cached arity/type/handle, known table match, and re-probes
 * the pointer chain (Signature→ParamList→Size) for live offset validation.
 * @param name Function name to probe
 * @param out Function pointer for console output (must not be NULL)
 */
void osi_func_probe_info(const char *name, void (*out)(const char *fmt, ...));

/**
 * Update a known event's function ID when discovered at runtime.
 * This fixes placeholder entries (funcId=0) in the known events table.
 */
void osi_func_update_known_event_id(const char *name, uint32_t funcId);

// ============================================================================
// Statistics
// ============================================================================

/**
 * Get the number of cached functions.
 */
int osi_func_get_cache_count(void);

/**
 * Track a seen function ID (for analysis/debugging).
 */
void osi_func_track_seen(uint32_t funcId, uint8_t arity);

/**
 * Get the count of unique function IDs seen.
 */
int osi_func_get_seen_count(void);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_OSIRIS_FUNCTIONS_H
