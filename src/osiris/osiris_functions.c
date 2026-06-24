/**
 * BG3SE-macOS - Osiris Function Cache Implementation
 */

#include "osiris_functions.h"
#include "logging.h"
#include "safe_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// Internal State
// ============================================================================

// Function cache
static CachedFunction g_funcCache[MAX_CACHED_FUNCTIONS];
static int g_funcCacheCount = 0;

// Hash table for fast ID lookup (-1 = empty, else index into g_funcCache)
static int16_t g_funcIdHashTable[FUNC_HASH_SIZE];

// Hash table for fast name lookup (-1 = empty, else index into g_funcCache)
static int16_t g_funcNameHashTable[FUNC_NAME_HASH_SIZE];

// Tracked function IDs (for analysis)
static uint32_t g_seenFuncIds[MAX_SEEN_FUNC_IDS];
static uint8_t g_seenFuncArities[MAX_SEEN_FUNC_IDS];
static int g_seenFuncIdCount = 0;

// Runtime pointers (set by caller)
static pFunctionDataFn s_pfn_pFunctionData = NULL;
static void **s_ppOsiFunctionMan = NULL;

// Known events table (set by caller)
static KnownEvent *s_knownEvents = NULL;

// ============================================================================
// Internal Helpers
// ============================================================================

/**
 * Hash function for function ID lookup
 */
static inline int func_id_hash(uint32_t funcId) {
    // Simple hash - use lower bits, handling type flag
    return (int)((funcId ^ (funcId >> 13)) & (FUNC_HASH_SIZE - 1));
}

/**
 * Hash function for function name lookup (djb2 variant)
 */
static inline int func_name_hash(const char *name) {
    uint32_t h = 5381;
    for (int i = 0; name[i] && i < 64; i++) {
        h = ((h << 5) + h) + (uint8_t)name[i];
    }
    return (int)((h ^ (h >> 13)) & (FUNC_NAME_HASH_SIZE - 1));
}

/**
 * Check if a pointer looks like it points to valid string data.
 * Must be in a reasonable address range for user-space memory.
 */
static int is_valid_string_ptr(void *ptr) {
    if (!ptr) return 0;
    uintptr_t addr = (uintptr_t)ptr;
    // Valid user-space addresses on macOS ARM64 are typically 0x100000000 - 0x7FFFFFFFFFFF
    return addr > 0x100000000ULL && addr < 0x800000000000ULL;
}

/**
 * Check if a character is a valid start for a function name.
 * Osiris function names start with uppercase letters, underscores, or 'PROC_'/'QRY_'/etc.
 */
static int is_valid_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/**
 * Try to extract function name from a function definition pointer.
 * Uses safe memory APIs to prevent SIGBUS crashes on invalid pointers.
 *
 * Structure layout based on Windows BG3SE Osiris.h (lines 902-918) with
 * ARM64 8-byte alignment adjustments:
 *
 * struct OsiFunctionDef {
 *     void* VMT;                     // 0x00: Virtual method table (8 bytes)
 *     uint32_t Line;                 // 0x08: Source line number (4 bytes)
 *     uint32_t Unknown1;             // 0x0C: (4 bytes)
 *     uint32_t Unknown2;             // 0x10: (4 bytes)
 *     uint32_t _padding;             // 0x14: Alignment padding (4 bytes)
 *     FunctionSignature* Signature;  // 0x18: Pointer to signature struct (8 bytes)
 *     // ... more fields
 * };
 *
 * struct FunctionSignature {
 *     void* VMT;                     // 0x00: Virtual method table (8 bytes)
 *     const char* Name;              // 0x08: Function name string pointer
 *     // ... more fields
 * };
 *
 * To get the name: funcDef->Signature->Name
 *   1. Read Signature* at funcDef + 0x18
 *   2. Read Name* at Signature + 0x08
 *   3. Read string at Name
 *
 * IMPORTANT: Previous documentation incorrectly stated Name was at +0x08.
 * The value at +0x08 is actually Line (e.g., 0x4a8 = 1192 decimal).
 */

/* OsiFunctionDef field offsets (ARM64 macOS, 8-byte aligned) */
#define OSIFUNCDEF_VMT_OFFSET        0x00
#define OSIFUNCDEF_LINE_OFFSET       0x08  /* uint32_t Line (NOT a name pointer!) */
#define OSIFUNCDEF_SIGNATURE_OFFSET  0x18  /* FunctionSignature* */

/* FunctionSignature field offsets (from Windows Osiris.h FunctionSignature struct) */
#define FUNCSIG_VMT_OFFSET           0x00
#define FUNCSIG_NAME_OFFSET          0x08  /* const char* Name */
#define FUNCSIG_PARAMS_OFFSET        0x10  /* FunctionParamList* Params */
#define FUNCSIG_OUTPARAMLIST_OFFSET  0x18  /* FuncSigOutParamList.Params* (bitmask) */
#define FUNCSIG_OUTPARAMCOUNT_OFFSET 0x20  /* FuncSigOutParamList.Count (uint32_t) */

/* FunctionParamList field offsets. The param list is a linked List:
 *   { void* VMT; Node* Head; Node* Tail; uint32_t Count; }
 * Count (total in+out params) is at +0x18 — NOT +0x10 (that's Tail, a pointer,
 * which read as a u32 gave garbage and clamped arity to 0). Verified live:
 * GetHostCharacter=1, AddExplorationExperience=2, GetGold=2, GetFlag=3, SetFlag=4. */
#define PARAMLIST_VMT_OFFSET         0x00
#define PARAMLIST_HEAD_OFFSET        0x08  /* List node Head* (= LAST param, declaration order) */
#define PARAMLIST_TAIL_OFFSET        0x10  /* List node Tail* (= FIRST param, declaration order) */
#define PARAMLIST_SIZE_OFFSET        0x18  /* List Count (uint32_t, total in+out params) */

/* Param-list node layout (each node is a separate heap alloc), verified live:
 *   { Node* Prev@0x00; Node* Next@0x08; uint32_t Type@0x10; ... }
 * The first param (declaration order) is at *(ParamList+0x10); walk forward via
 * Next@0x08; read the Osiris value type at +0x10. Verified:
 *   GetGold=[5,1] GetFlag=[16,5,1] SetFlag=[16,5,1,1] AddExplorationExperience=[6,1] */
#define PARAMNODE_NEXT_OFFSET        0x08  /* Node* Next (toward higher param index) */
#define PARAMNODE_TYPE_OFFSET        0x10  /* uint32_t Osiris value type (low byte) */

/* Thread-local buffer for extracted function names */
static __thread char s_extractedName[128];

/* Diagnostic counter for extract_func_name */
static int s_extractDiagCount = 0;
#define MAX_EXTRACT_DIAG 20

static const char *extract_func_name_from_def(void *funcDef) {
    if (!funcDef) return NULL;

    mach_vm_address_t funcDefAddr = (mach_vm_address_t)funcDef;
    bool shouldLog = (s_extractDiagCount < MAX_EXTRACT_DIAG);

    /* Skip GPU carveout region - these cause SIGBUS even if mapped */
    if (safe_memory_is_gpu_region(funcDefAddr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("funcDef 0x%llx: GPU region", (unsigned long long)funcDefAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /*
     * Two-level indirection: funcDef->Signature->Name
     *
     * Step 1: Read FunctionSignature* from funcDef + OSIFUNCDEF_SIGNATURE_OFFSET (0x18)
     * Step 2: Read const char* Name from Signature + FUNCSIG_NAME_OFFSET (0x08)
     * Step 3: Read the actual string from Name pointer
     *
     * NOTE: The old code incorrectly read offset +0x08 which contains Line (uint32_t),
     * not a name pointer. That's why we saw values like 0x4a8 (1192 = line number).
     */

    /* Step 1: Read Signature pointer at offset 0x18 */
    void *signaturePtr = NULL;
    if (!safe_memory_read_pointer(funcDefAddr + OSIFUNCDEF_SIGNATURE_OFFSET, &signaturePtr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("funcDef 0x%llx: failed to read Signature at +0x%x",
                       (unsigned long long)funcDefAddr, OSIFUNCDEF_SIGNATURE_OFFSET);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Validate Signature pointer */
    mach_vm_address_t sigAddr = (mach_vm_address_t)signaturePtr;
    if (!is_valid_string_ptr(signaturePtr)) {  /* Reuse pointer validation */
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("funcDef 0x%llx: Signature 0x%llx invalid",
                       (unsigned long long)funcDefAddr, (unsigned long long)sigAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Skip GPU region for Signature pointer */
    if (safe_memory_is_gpu_region(sigAddr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("funcDef 0x%llx: Signature 0x%llx in GPU region",
                       (unsigned long long)funcDefAddr, (unsigned long long)sigAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Step 2: Read Name pointer from Signature + 0x08 */
    void *namePtr = NULL;
    if (!safe_memory_read_pointer(sigAddr + FUNCSIG_NAME_OFFSET, &namePtr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("Signature 0x%llx: failed to read Name at +0x%x",
                       (unsigned long long)sigAddr, FUNCSIG_NAME_OFFSET);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Validate the Name pointer address */
    mach_vm_address_t nameAddr = (mach_vm_address_t)namePtr;
    if (!is_valid_string_ptr(namePtr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("Signature 0x%llx: Name 0x%llx not valid",
                       (unsigned long long)sigAddr, (unsigned long long)nameAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Skip GPU region for Name pointer too */
    if (safe_memory_is_gpu_region(nameAddr)) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("Signature 0x%llx: Name 0x%llx in GPU region",
                       (unsigned long long)sigAddr, (unsigned long long)nameAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Step 3: Safely read the name string */
    if (!safe_memory_read_string(nameAddr, s_extractedName, sizeof(s_extractedName))) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("Name 0x%llx: failed to read string",
                       (unsigned long long)nameAddr);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Validate the extracted name format */
    if (!is_valid_name_start(s_extractedName[0])) {
        if (shouldLog) {
            LOG_OSIRIS_DEBUG("Name 0x%llx: invalid start char 0x%02x",
                       (unsigned long long)nameAddr, (unsigned char)s_extractedName[0]);
            s_extractDiagCount++;
        }
        return NULL;
    }

    /* Validate all characters in the name */
    for (int j = 0; j < 64 && s_extractedName[j]; j++) {
        char c = s_extractedName[j];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            if (shouldLog) {
                LOG_OSIRIS_DEBUG("Name '%.*s': invalid char 0x%02x at pos %d",
                           j, s_extractedName, (unsigned char)c, j);
                s_extractDiagCount++;
            }
            return NULL;
        }
    }

    /* Success! Log first few successful extractions for verification */
    if (shouldLog) {
        LOG_OSIRIS_DEBUG("SUCCESS: funcDef 0x%llx -> Sig 0x%llx -> Name '%s'",
                   (unsigned long long)funcDefAddr, (unsigned long long)sigAddr, s_extractedName);
        s_extractDiagCount++;
    }

    return s_extractedName;
}

// ============================================================================
// Initialization
// ============================================================================

void osi_func_cache_init(void) {
    // Initialize hash tables
    for (int i = 0; i < FUNC_HASH_SIZE; i++) {
        g_funcIdHashTable[i] = -1;
    }
    for (int i = 0; i < FUNC_NAME_HASH_SIZE; i++) {
        g_funcNameHashTable[i] = -1;
    }
    g_funcCacheCount = 0;
    g_seenFuncIdCount = 0;
}

void osi_func_cache_set_runtime(pFunctionDataFn pFunctionData, void **ppOsiFunctionMan) {
    s_pfn_pFunctionData = pFunctionData;
    s_ppOsiFunctionMan = ppOsiFunctionMan;
}

void osi_func_cache_set_known_events(KnownEvent *events) {
    s_knownEvents = events;
}

// ============================================================================
// Caching
// ============================================================================

void osi_func_cache(const char *name, uint32_t funcId, uint8_t arity, uint8_t type) {
    if (g_funcCacheCount >= MAX_CACHED_FUNCTIONS) {
        return;
    }

    // Check for duplicate
    int hash = func_id_hash(funcId);
    if (g_funcIdHashTable[hash] >= 0) {
        // Linear probe to check if already exists
        for (int i = 0; i < g_funcCacheCount; i++) {
            if (g_funcCache[i].id == funcId) {
                return;  // Already cached
            }
        }
    }

    CachedFunction *cf = &g_funcCache[g_funcCacheCount];
    strncpy(cf->name, name, sizeof(cf->name) - 1);
    cf->name[sizeof(cf->name) - 1] = '\0';
    cf->id = funcId;
    cf->arity = arity;
    cf->type = type;
    cf->handle = 0;

    // Add to ID hash table (simple - just store first match at hash location)
    if (g_funcIdHashTable[hash] < 0) {
        g_funcIdHashTable[hash] = (int16_t)g_funcCacheCount;
    }

    // Add to name hash table for O(1) name→index lookups
    int nameHash = func_name_hash(cf->name);
    if (g_funcNameHashTable[nameHash] < 0) {
        g_funcNameHashTable[nameHash] = (int16_t)g_funcCacheCount;
    }

    g_funcCacheCount++;
}

void osi_func_cache_set_handle(uint32_t funcId, uint32_t handle) {
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (g_funcCache[i].id == funcId) {
            g_funcCache[i].handle = handle;
            return;
        }
    }
}

void osi_func_cache_set_param_types(uint32_t funcId, const uint8_t *types, uint8_t count) {
    if (!types || count == 0) return;
    if (count > MAX_OSI_PARAMS) count = MAX_OSI_PARAMS;
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (g_funcCache[i].id == funcId) {
            memcpy(g_funcCache[i].paramTypes, types, count);
            return;
        }
    }
}

int osi_func_get_param_types(const char *name, uint8_t *out, int max) {
    if (!name || !out || max <= 0) return 0;

    int idx = -1;
    /* Fast path: name hash table */
    int hash = func_name_hash(name);
    int16_t h = g_funcNameHashTable[hash];
    if (h >= 0 && strcmp(g_funcCache[h].name, name) == 0) {
        idx = h;
    } else {
        for (int i = 0; i < g_funcCacheCount; i++) {
            if (strcmp(g_funcCache[i].name, name) == 0) { idx = i; break; }
        }
    }
    if (idx < 0) return 0;

    int n = g_funcCache[idx].arity;
    if (n > max) n = max;
    if (n > MAX_OSI_PARAMS) n = MAX_OSI_PARAMS;
    memcpy(out, g_funcCache[idx].paramTypes, n);
    return n;
}

// Diagnostic counter to limit verbose logging
static int s_diagLogCount = 0;
static const int MAX_DIAG_LOGS = 20;

/* funcDef field offsets (authoritative, confirmed via COsiris::GetFunctionMappings
 * and Norbyte's OsiFunctionDef in GameDefinitions/Osiris.h):
 *   +0x18 Signature*   (→ +0x08 Name)
 *   +0x20 Node.Id      (uint32: rete node index; 0 for engine, >0 for story)
 *   +0x24 FunctionType (uint32: 1=Event 2=Query 3=Call 4=Database 5=Proc ... )
 *   +0x28 Key[4]       (Key[0]=type, [1]=part2, [2]=funcIndex, [3]=part4)
 *   +0x38 Handle/OsiFunctionId (uint32: DIV dispatch id; 0 for story functions) */
#define FUNCDEF_NODEID_OFFSET 0x20
#define FUNCDEF_TYPE_OFFSET   0x24
#define FUNCDEF_KEY_OFFSET    0x28
#define FUNCDEF_HANDLE_OFFSET 0x38
/* OSI_SYNTHETIC_ID_BASE is defined in osiris_types.h (shared with the dispatcher). */

/* Osiris function name hash (COsiFunctionMan = CSearchIndex<COsiFunctionData*,
 * COsiString, 1023>). All ~20k functions (engine + story) live here keyed by
 * "name/arity". Layout confirmed live:
 *   bucket array @ funcMan+0x10, 1023 buckets of 0x18 bytes each
 *   bucket = { uint64 size@+0, Node* begin@+8, Node* root@+0x10 }
 *   tree node = { Node* left@0, Node* right@8, Node* parent@0x10, ...,
 *                 COsiString key@0x20, COsiFunctionData* value@0x38 } */
#define FUNCHASH_BUCKETS_OFFSET 0x10
#define FUNCHASH_BUCKET_STRIDE  0x18
#define FUNCHASH_BUCKET_ROOT    0x10
#define FUNCHASH_NUM_BUCKETS    1023
#define TREENODE_LEFT_OFFSET    0x00
#define TREENODE_RIGHT_OFFSET   0x08
#define TREENODE_VALUE_OFFSET   0x38

/* Append a fully-populated cache entry. No dedup — the caller guarantees the
 * function is not already present (the name-hash walk visits each COsiFunctionData
 * exactly once, and osi_func_cache_from_event pre-checks). O(1) per call, so
 * caching all ~20k functions stays linear. Returns the cache index or -1. */
static int osi_func_cache_append_full(const char *name, uint32_t id, uint8_t arity,
                                      uint8_t type, uint32_t handle, void *funcDef,
                                      uint32_t nodeId, const uint8_t *ptypes,
                                      uint8_t ptypeCount) {
    if (g_funcCacheCount >= MAX_CACHED_FUNCTIONS) return -1;
    int idx = g_funcCacheCount;
    CachedFunction *cf = &g_funcCache[idx];
    memset(cf, 0, sizeof(*cf));
    strncpy(cf->name, name, sizeof(cf->name) - 1);
    cf->id = id;
    cf->arity = arity;
    cf->type = type;
    cf->handle = handle;
    cf->funcDef = funcDef;
    cf->nodeId = nodeId;
    if (ptypes && ptypeCount) {
        if (ptypeCount > MAX_OSI_PARAMS) ptypeCount = MAX_OSI_PARAMS;
        memcpy(cf->paramTypes, ptypes, ptypeCount);
    }
    int idHash = func_id_hash(id);
    if (g_funcIdHashTable[idHash] < 0) g_funcIdHashTable[idHash] = (int16_t)idx;
    int nameHash = func_name_hash(cf->name);
    if (g_funcNameHashTable[nameHash] < 0) g_funcNameHashTable[nameHash] = (int16_t)idx;
    g_funcCacheCount++;
    return idx;
}

/* Extract metadata from a COsiFunctionData* and cache it. Shared by the
 * id-probe path (osi_func_cache_by_id) and the name-hash walk
 * (osi_func_enumerate_hash). Engine functions cache under their DIV handle;
 * story functions (no handle) cache under a synthetic id and keep funcDef +
 * Node.Id for rete dispatch. hintId is the probed funcId or 0 (hash-walk). */
static int osi_func_cache_funcdef(void *funcDef, uint32_t hintId) {
    if (funcDef) {
        const char *name = extract_func_name_from_def(funcDef);
        if (name && name[0]) {
            /* Read total param count (in+out) via pointer chain:
             * funcDef+0x18 → Signature → Signature+0x10 → ParamList* → ParamList+0x10 → Size
             * This gives Params.Size which includes both input AND output params.
             * Windows BG3SE uses this for query dispatch (Function.inl:OsiQuery). */
            uint8_t arity = 0;
            uint8_t ptypes[MAX_OSI_PARAMS];
            memset(ptypes, 0, sizeof(ptypes));
            {
                /* We already know funcDef+0x18 → Signature works (name extraction uses it).
                 * Re-read Signature pointer for the param chain. */
                void *sigPtr = NULL;
                if (safe_memory_read_pointer((mach_vm_address_t)funcDef + OSIFUNCDEF_SIGNATURE_OFFSET, &sigPtr) && sigPtr) {
                    void *paramListPtr = NULL;
                    if (safe_memory_read_pointer((mach_vm_address_t)sigPtr + FUNCSIG_PARAMS_OFFSET, &paramListPtr) && paramListPtr) {
                        uint32_t paramSize = 0;
                        if (safe_memory_read_u32((mach_vm_address_t)paramListPtr + PARAMLIST_SIZE_OFFSET, &paramSize)) {
                            arity = (paramSize <= MAX_OSI_PARAMS) ? (uint8_t)paramSize : 0;
                        }
                        /* Walk the param-list nodes to read per-param Osiris types in
                         * declaration order: first node = *(ParamList+0x10), walk Next@0x08,
                         * type byte @ node+0x10. Needed so query out-slots and call inputs
                         * are typed correctly (e.g. GetFlag's INTEGER out, not GUIDSTRING). */
                        if (arity > 0) {
                            void *node = NULL;
                            if (safe_memory_read_pointer((mach_vm_address_t)paramListPtr + PARAMLIST_TAIL_OFFSET, &node)) {
                                for (int i = 0; i < arity && node; i++) {
                                    uint32_t t = 0;
                                    if (safe_memory_read_u32((mach_vm_address_t)node + PARAMNODE_TYPE_OFFSET, &t)) {
                                        ptypes[i] = (uint8_t)t;
                                    }
                                    void *nxt = NULL;
                                    if (!safe_memory_read_pointer((mach_vm_address_t)node + PARAMNODE_NEXT_OFFSET, &nxt)) break;
                                    node = nxt;
                                }
                            }
                        }
                    }
                }
            }

            /* FunctionType at funcDef+0x24 (confirmed via GetFunctionMappings).
             * Fallback: guess from name prefix (QRY_=Query, DB_=Database, etc.) */
            uint32_t rawType = 0;
            uint8_t type = osi_func_guess_type(name);
            if (safe_memory_read_u32((mach_vm_address_t)funcDef + FUNCDEF_TYPE_OFFSET, &rawType)) {
                if (rawType >= OSI_FUNC_EVENT && rawType <= OSI_FUNC_USERQUERY) {
                    type = (uint8_t)rawType;
                }
            }

            /* Rete node index at funcDef+0x20. 0 = engine (DIV-dispatched),
             * >0 = story function dispatched through Nodes->Db.Elements[nodeId-1]. */
            uint32_t nodeId = 0;
            safe_memory_read_u32((mach_vm_address_t)funcDef + FUNCDEF_NODEID_OFFSET, &nodeId);

            /* DIV dispatch handle: precomputed at funcDef+0x38. Fall back to
             * encoding Key[0..3] at +0x28, then to the probed hintId. Story
             * functions have no handle/Key — `handle` stays 0 for them. */
            uint32_t handle = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)funcDef + FUNCDEF_HANDLE_OFFSET, &handle) || handle == 0) {
                uint32_t keys[4] = {0};
                if (safe_memory_read((mach_vm_address_t)funcDef + FUNCDEF_KEY_OFFSET, keys, sizeof(keys))
                    && keys[0] >= OSI_FUNC_EVENT && keys[0] <= OSI_FUNC_USERQUERY) {
                    handle = osi_encode_handle(keys[0], keys[1], keys[2], keys[3]);
                } else if (hintId != 0) {
                    handle = hintId;
                }
            }

            /* Cache id: engine functions key on their DIV handle; story functions
             * (handle==0) get a unique synthetic id so they can be cached and
             * looked up (dispatch routes them via the rete node, not the handle). */
            uint32_t cacheId = (handle != 0) ? handle
                                             : (OSI_SYNTHETIC_ID_BASE | (uint32_t)g_funcCacheCount);

            if (s_diagLogCount < MAX_DIAG_LOGS) {
                LOG_OSIRIS_DEBUG("SUCCESS: '%s' (arity=%d, type=%s[%d], id=0x%08x, node=%u)",
                           name, arity, osi_func_type_str(type), type, cacheId, nodeId);
                s_diagLogCount++;
            }

            osi_func_cache_append_full(name, cacheId, arity, type, handle,
                                       funcDef, nodeId, ptypes, arity);
            return 1;
        }
    }

    return 0;
}

int osi_func_cache_by_id(uint32_t funcId) {
    if (!s_pfn_pFunctionData || !s_ppOsiFunctionMan) return 0;

    void *funcMan = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)s_ppOsiFunctionMan, &funcMan) || !funcMan) {
        return 0;
    }

    void *funcDef = s_pfn_pFunctionData(funcMan, funcId);
    return osi_func_cache_funcdef(funcDef, funcId);
}

/* Walk one red-black tree (a hash bucket) iteratively, caching every function.
 * Returns the number of nodes processed. */
static int osi_func_walk_tree(void *root) {
    void *stack[64];
    int sp = 0;
    int count = 0;
    if (root) stack[sp++] = root;
    while (sp > 0) {
        void *node = stack[--sp];
        SafeMemoryInfo ni = safe_memory_check_address((mach_vm_address_t)node);
        if (!ni.is_valid || !ni.is_readable) continue;

        void *funcDef = NULL;
        if (safe_memory_read_pointer((mach_vm_address_t)node + TREENODE_VALUE_OFFSET, &funcDef) && funcDef) {
            if (osi_func_cache_funcdef(funcDef, 0)) count++;
        }

        if (sp < 62) {
            void *left = NULL, *right = NULL;
            if (safe_memory_read_pointer((mach_vm_address_t)node + TREENODE_LEFT_OFFSET, &left) && left)
                stack[sp++] = left;
            if (safe_memory_read_pointer((mach_vm_address_t)node + TREENODE_RIGHT_OFFSET, &right) && right)
                stack[sp++] = right;
        }
    }
    return count;
}

void osi_func_enumerate_hash(void) {
    if (!s_ppOsiFunctionMan) return;
    void *funcMan = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)s_ppOsiFunctionMan, &funcMan) || !funcMan) {
        LOG_OSIRIS_WARN("enumerate_hash: OsiFunctionMan NULL");
        return;
    }

    /* Full rebuild: the name-hash is the authoritative source for both engine and
     * story functions, and each is keyed once by "name/arity", so resetting first
     * makes this idempotent (no duplicate story entries on re-run) and lets the
     * walk append without per-entry dedup. Known-event id mappings live in a
     * separate table and survive this reset. */
    osi_func_cache_init();
    s_diagLogCount = 0;

    int total = 0;
    for (int i = 0; i < FUNCHASH_NUM_BUCKETS; i++) {
        mach_vm_address_t bucket = (mach_vm_address_t)funcMan + FUNCHASH_BUCKETS_OFFSET
                                 + (mach_vm_address_t)i * FUNCHASH_BUCKET_STRIDE;
        void *root = NULL;
        if (safe_memory_read_pointer(bucket + FUNCHASH_BUCKET_ROOT, &root) && root) {
            total += osi_func_walk_tree(root);
        }
    }
    LOG_OSIRIS_INFO("enumerate_hash: walked function name-hash, %d cached (total %d)",
                    total, g_funcCacheCount);
}

void osi_func_cache_from_event(uint32_t funcId) {
    /* Skip if already cached */
    if (osi_func_get_name(funcId) != NULL) {
        return;
    }

    /* Try to get the function definition using safe memory APIs
     * The extract_func_name_from_def and osi_func_cache_by_id functions
     * now use mach_vm_read for safe memory access */
    osi_func_cache_by_id(funcId);
}

// ============================================================================
// Enumeration
// ============================================================================

void osi_func_enumerate(void) {
    if (!s_pfn_pFunctionData || !s_ppOsiFunctionMan || !*s_ppOsiFunctionMan) {
        LOG_OSIRIS_DEBUG("Cannot enumerate - pFunctionData or OsiFunctionMan not available");
        return;
    }

    LOG_OSIRIS_DEBUG("Starting function enumeration...");
    int found_count = 0;

    // Osiris function IDs are split into two ranges:
    // 1. Regular functions: 0 to ~64K (low IDs)
    // 2. Registered functions: 0x80000000 + offset (high bit set)

    // Probe low range (regular functions incl. databases like DB_Players).
    // No found_count cap — sweep the whole range so databases past the first ~1000
    // functions are discovered. osi_func_cache() self-limits at MAX_CACHED_FUNCTIONS.
    for (uint32_t id = 1; id < 20000 && g_funcCacheCount < MAX_CACHED_FUNCTIONS; id++) {
        if (osi_func_cache_by_id(id)) {
            found_count++;
        }
    }

    // Probe high range (registered functions) - 0x80000000 + offset.
    for (uint32_t offset = 0; offset < 30000 && g_funcCacheCount < MAX_CACHED_FUNCTIONS; offset++) {
        uint32_t id = 0x80000000 | offset;
        if (osi_func_cache_by_id(id)) {
            found_count++;
        }
    }

    LOG_OSIRIS_DEBUG("Enumeration complete: %d functions cached", found_count);

    // Log some key functions we're looking for
    const char *key_funcs[] = {
        "QRY_IsTagged", "IsTagged", "GetDistanceTo", "QRY_GetDistance",
        "DialogRequestStop", "QRY_StartDialog_Fixed", "StartDialog",
        "DB_Players", "CharacterGetDisplayName", NULL
    };

    LOG_OSIRIS_DEBUG("Checking key functions:");
    for (int i = 0; key_funcs[i]; i++) {
        uint32_t fid = osi_func_lookup_id(key_funcs[i]);
        if (fid != INVALID_FUNCTION_ID) {
            LOG_OSIRIS_DEBUG("  %s -> 0x%08x", key_funcs[i], fid);
        }
    }
}

// ============================================================================
// Lookup
// ============================================================================

const char *osi_func_get_name(uint32_t funcId) {
    // Check known events table first (hardcoded mappings)
    if (s_knownEvents) {
        for (int i = 0; s_knownEvents[i].name != NULL; i++) {
            if (s_knownEvents[i].funcId == funcId) {
                return s_knownEvents[i].name;
            }
        }
    }

    // Check hash table (fast path for dynamic cache)
    int hash = func_id_hash(funcId);
    int16_t idx = g_funcIdHashTable[hash];
    if (idx >= 0 && g_funcCache[idx].id == funcId) {
        return g_funcCache[idx].name;
    }

    // Linear search (for hash collisions)
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (g_funcCache[i].id == funcId) {
            return g_funcCache[i].name;
        }
    }

    return NULL;
}

uint32_t osi_func_lookup_id(const char *name) {
    if (!name) return INVALID_FUNCTION_ID;

    // Check known events first (fast path for common names)
    if (s_knownEvents) {
        for (int i = 0; s_knownEvents[i].name != NULL; i++) {
            if (strcmp(s_knownEvents[i].name, name) == 0 && s_knownEvents[i].funcId != 0) {
                return s_knownEvents[i].funcId;
            }
        }
    }

    // Fast path: name hash table
    int hash = func_name_hash(name);
    int16_t idx = g_funcNameHashTable[hash];
    if (idx >= 0 && strcmp(g_funcCache[idx].name, name) == 0) {
        return g_funcCache[idx].id;
    }

    // Slow path: linear search (hash collision)
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (strcmp(g_funcCache[i].name, name) == 0) {
            return g_funcCache[i].id;
        }
    }

    return INVALID_FUNCTION_ID;
}

int osi_func_get_info(const char *name, uint8_t *out_arity, uint8_t *out_type) {
    if (!name) return 0;

    // Check known functions table first (includes events, queries, calls)
    if (s_knownEvents) {
        for (int i = 0; s_knownEvents[i].name != NULL; i++) {
            if (strcmp(s_knownEvents[i].name, name) == 0) {
                if (out_arity) *out_arity = s_knownEvents[i].expectedArity;
                if (out_type) *out_type = s_knownEvents[i].funcType;
                return 1;
            }
        }
    }

    // Fast path: name hash table
    int hash = func_name_hash(name);
    int16_t idx = g_funcNameHashTable[hash];
    if (idx >= 0 && strcmp(g_funcCache[idx].name, name) == 0) {
        if (out_arity) *out_arity = g_funcCache[idx].arity;
        if (out_type) *out_type = g_funcCache[idx].type;
        return 1;
    }

    // Slow path: linear search (hash collision)
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (strcmp(g_funcCache[i].name, name) == 0) {
            if (out_arity) *out_arity = g_funcCache[i].arity;
            if (out_type) *out_type = g_funcCache[i].type;
            return 1;
        }
    }

    return 0;
}

uint32_t osi_func_get_handle(const char *name) {
    if (!name) return 0;

    // Fast path: name hash table
    int hash = func_name_hash(name);
    int16_t idx = g_funcNameHashTable[hash];
    if (idx >= 0 && strcmp(g_funcCache[idx].name, name) == 0) {
        return g_funcCache[idx].handle;
    }

    // Slow path: linear search (hash collision)
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (strcmp(g_funcCache[i].name, name) == 0) {
            return g_funcCache[i].handle;
        }
    }

    return 0;
}

int osi_func_resolve(const char *name, int preferArity, uint8_t *out_arity,
                     uint8_t *out_type, uint32_t *out_id, uint32_t *out_nodeId) {
    if (!name) return 0;

    /* Story functions are overloaded by arity (e.g. QRY_StartDialog_Fixed/2,/3,/4),
     * each a distinct cache entry sharing the name. Prefer the variant whose arity
     * exactly matches the caller's arg count; else the smallest arity >= it (covers
     * queries with OUT params, where in-args < total arity); else the first match. */
    int firstIdx = -1, exactIdx = -1, geIdx = -1;
    for (int i = 0; i < g_funcCacheCount; i++) {
        if (strcmp(g_funcCache[i].name, name) != 0) continue;
        if (firstIdx < 0) firstIdx = i;
        if (preferArity < 0) continue;
        uint8_t a = g_funcCache[i].arity;
        if (a == (uint8_t)preferArity) { exactIdx = i; break; }
        if (a >= (uint8_t)preferArity && (geIdx < 0 || a < g_funcCache[geIdx].arity)) geIdx = i;
    }
    int idx = (exactIdx >= 0) ? exactIdx : (geIdx >= 0 ? geIdx : firstIdx);
    if (idx < 0) return 0;

    if (out_arity)  *out_arity  = g_funcCache[idx].arity;
    if (out_type)   *out_type   = g_funcCache[idx].type;
    if (out_id)     *out_id     = g_funcCache[idx].id;
    if (out_nodeId) *out_nodeId = g_funcCache[idx].nodeId;
    return 1;
}

int osi_func_get_node(const char *name, void **out_funcDef, uint32_t *out_nodeId) {
    if (!name) return 0;

    int idx = -1;
    int hash = func_name_hash(name);
    int16_t h = g_funcNameHashTable[hash];
    if (h >= 0 && strcmp(g_funcCache[h].name, name) == 0) {
        idx = h;
    } else {
        for (int i = 0; i < g_funcCacheCount; i++) {
            if (strcmp(g_funcCache[i].name, name) == 0) { idx = i; break; }
        }
    }
    if (idx < 0) return 0;

    if (out_funcDef) *out_funcDef = g_funcCache[idx].funcDef;
    if (out_nodeId) *out_nodeId = g_funcCache[idx].nodeId;
    return 1;
}

void osi_func_update_known_event_id(const char *name, uint32_t funcId) {
    if (!name || funcId == 0) return;

    // Find matching entry with funcId=0 (placeholder) and update it
    if (s_knownEvents) {
        for (int i = 0; s_knownEvents[i].name != NULL; i++) {
            if (strcmp(s_knownEvents[i].name, name) == 0 &&
                s_knownEvents[i].funcId == 0) {
                // Update the placeholder with the discovered ID
                s_knownEvents[i].funcId = funcId;
                LOG_OSIRIS_INFO("Discovered event ID: %s = 0x%x", name, funcId);
                return;
            }
        }
    }
}

// ============================================================================
// Statistics
// ============================================================================

int osi_func_get_cache_count(void) {
    return g_funcCacheCount;
}

void osi_func_track_seen(uint32_t funcId, uint8_t arity) {
    // Check if already seen
    for (int i = 0; i < g_seenFuncIdCount; i++) {
        if (g_seenFuncIds[i] == funcId) return;
    }

    // Add to list
    if (g_seenFuncIdCount < MAX_SEEN_FUNC_IDS) {
        g_seenFuncIds[g_seenFuncIdCount] = funcId;
        g_seenFuncArities[g_seenFuncIdCount] = arity;
        g_seenFuncIdCount++;

        // Log new unique function ID
        LOG_OSIRIS_DEBUG("New unique: id=%u (0x%08x), arity=%d, total_unique=%d",
                   funcId, funcId, arity, g_seenFuncIdCount);
    }
}

int osi_func_get_seen_count(void) {
    return g_seenFuncIdCount;
}

// ============================================================================
// Struct Probe (on-demand layout discovery)
// ============================================================================

void osi_func_probe_layout(int count) {
    if (!s_pfn_pFunctionData || !s_ppOsiFunctionMan) {
        LOG_OSIRIS_WARN("PROBE: runtime pointers not set");
        return;
    }

    /* Read OsiFunctionMan pointer — same pattern as osi_func_cache_by_id.
     * Bug fix: was using safe_memory_read(*s_ppOsiFunctionMan, ...) which reads
     * FROM the OsiFunctionMan object (getting its VMT), not the pointer TO it.
     * That caused pFunctionData(garbage_this, ...) → PAC failure → SIGSEGV. */
    void *funcMan = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)s_ppOsiFunctionMan, &funcMan)
        || !funcMan) {
        LOG_OSIRIS_WARN("PROBE: failed to read OsiFunctionMan");
        return;
    }

    LOG_OSIRIS_INFO("PROBE: funcMan=%p, probing %d/%d cached functions",
                    funcMan, count, g_funcCacheCount);

    int probed = 0;
    for (int i = 0; i < g_funcCacheCount && probed < count; i++) {
        CachedFunction *cf = &g_funcCache[i];

        /* Validate funcId before calling pFunctionData — skip obviously invalid IDs */
        if (cf->id == 0 || cf->id == INVALID_FUNCTION_ID) {
            LOG_OSIRIS_DEBUG("PROBE: skipping invalid funcId=0x%08x for '%s'", cf->id, cf->name);
            continue;
        }

        void *funcDef = s_pfn_pFunctionData(funcMan, cf->id);
        if (!funcDef) continue;

        /* Validate funcDef pointer before reading from it */
        SafeMemoryInfo fdi = safe_memory_check_address((mach_vm_address_t)funcDef);
        if (!fdi.is_valid || !fdi.is_readable) {
            LOG_OSIRIS_WARN("PROBE: funcDef=%p not readable for '%s'", funcDef, cf->name);
            continue;
        }

        // Dump 0x80 bytes as hex (128B covers potential ARM64 padding beyond Windows layout)
        uint8_t raw[0x80];
        if (!safe_memory_read((mach_vm_address_t)funcDef, raw, sizeof(raw))) {
            LOG_OSIRIS_WARN("PROBE: can't read 0x80 bytes at %p for '%s'", funcDef, cf->name);
            continue;
        }

        // Format hex dump
        char hexline[256];
        LOG_OSIRIS_INFO("PROBE [%d] '%s' funcDef=%p id=0x%08x type=%s handle=0x%08x",
                        probed, cf->name, funcDef, cf->id,
                        osi_func_type_str(cf->type), cf->handle);

        for (int off = 0; off < 0x80; off += 16) {
            int pos = snprintf(hexline, sizeof(hexline), "  +0x%02x: ", off);
            for (int j = 0; j < 16 && off + j < 0x80; j++) {
                pos += snprintf(hexline + pos, sizeof(hexline) - pos, "%02x ", raw[off + j]);
            }
            // Annotate interesting offsets (assuming Windows layout — verify with probe)
            if (off == 0x28) {
                uint32_t type_val = *(uint32_t *)(raw + 0x28);
                pos += snprintf(hexline + pos, sizeof(hexline) - pos,
                               " | +0x28(Type?)=%u(%s)", type_val, osi_func_type_str((uint8_t)type_val));
            }
            if (off == 0x20) {
                uint32_t node_or_param = *(uint32_t *)(raw + 0x20);
                pos += snprintf(hexline + pos, sizeof(hexline) - pos,
                               " | +0x20(ParamCount?)=%u", node_or_param);
            }
            if (off == 0x30) {
                // Key[0..3] at 0x28-0x37 (verified via runtime probe)
                uint32_t k0 = *(uint32_t *)(raw + 0x28);
                uint32_t k2 = *(uint32_t *)(raw + 0x30);
                uint32_t k3 = *(uint32_t *)(raw + 0x34);
                pos += snprintf(hexline + pos, sizeof(hexline) - pos,
                               " | Key[0]=%u Key[2/funcIdx]=0x%x Key[3/part4]=%u", k0, k2, k3);
            }
            LOG_OSIRIS_INFO("%s", hexline);
        }
        probed++;
    }

    LOG_OSIRIS_INFO("PROBE: dumped %d/%d functions", probed, count);
}

void osi_func_probe_info(const char *name, void (*out)(const char *fmt, ...)) {
    if (!name || !out) return;

    /* 1. Cache lookup */
    uint32_t funcId = osi_func_lookup_id(name);
    uint8_t cachedArity = 0, cachedType = 0;
    int infoFound = osi_func_get_info(name, &cachedArity, &cachedType);
    uint32_t handle = osi_func_get_handle(name);

    void *cachedFuncDef = NULL;
    uint32_t cachedNodeId = 0;
    osi_func_get_node(name, &cachedFuncDef, &cachedNodeId);

    out("=== !osi_info %s ===", name);
    out("  funcId: 0x%08x (%s)", funcId, funcId == INVALID_FUNCTION_ID ? "NOT FOUND" : "found");
    out("  arity: %d (from %s)", cachedArity, infoFound ? "known table or cache" : "unknown");
    out("  type: %s[%d]", osi_func_type_str(cachedType), cachedType);
    out("  handle: 0x%08x", handle);
    out("  nodeId: %u (%s)", cachedNodeId,
        cachedNodeId ? "story -> rete dispatch" : "engine -> DIV dispatch");
    out("  funcDef(cached): %p", cachedFuncDef);

    /* Story functions have a synthetic id; pFunctionData can't re-probe them, so
     * stop here (the cached funcDef above is the live pointer used for dispatch). */
    if (funcId != INVALID_FUNCTION_ID && (funcId & OSI_SYNTHETIC_ID_BASE)) {
        out("  [story function: dispatched via rete node, not pFunctionData]");
        return;
    }

    /* 2. Re-probe the pointer chain from live memory */
    if (funcId == INVALID_FUNCTION_ID) {
        out("  [Cannot probe pointer chain - funcId unknown]");
        return;
    }

    if (!s_pfn_pFunctionData || !s_ppOsiFunctionMan) {
        out("  [Cannot probe - runtime pointers not set]");
        return;
    }

    void *funcMan = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)s_ppOsiFunctionMan, &funcMan) || !funcMan) {
        out("  [Cannot probe - OsiFunctionMan NULL]");
        return;
    }

    void *funcDef = s_pfn_pFunctionData(funcMan, funcId);
    if (!funcDef) {
        out("  [pFunctionData returned NULL for funcId=0x%08x]", funcId);
        return;
    }
    out("  funcDef: %p", funcDef);

    /* Step 1: Signature pointer at funcDef+0x18 */
    void *sigPtr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)funcDef + OSIFUNCDEF_SIGNATURE_OFFSET, &sigPtr) || !sigPtr) {
        out("  Signature: FAILED to read at funcDef+0x%x", OSIFUNCDEF_SIGNATURE_OFFSET);
        return;
    }
    out("  Signature: %p (at funcDef+0x%x)", sigPtr, OSIFUNCDEF_SIGNATURE_OFFSET);

    /* Step 2: Name at Signature+0x08 (sanity check) */
    void *namePtr = NULL;
    char nameBuf[64] = {0};
    if (safe_memory_read_pointer((mach_vm_address_t)sigPtr + FUNCSIG_NAME_OFFSET, &namePtr) && namePtr) {
        safe_memory_read((mach_vm_address_t)namePtr, nameBuf, sizeof(nameBuf) - 1);
        out("  Sig.Name: '%s' (at Sig+0x%x -> %p)", nameBuf, FUNCSIG_NAME_OFFSET, namePtr);
    } else {
        out("  Sig.Name: FAILED at Sig+0x%x", FUNCSIG_NAME_OFFSET);
    }

    /* Step 3: ParamList pointer at Signature+0x10 */
    void *paramListPtr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)sigPtr + FUNCSIG_PARAMS_OFFSET, &paramListPtr)) {
        out("  ParamList: FAILED to read at Sig+0x%x", FUNCSIG_PARAMS_OFFSET);
        return;
    }
    out("  ParamList: %p (at Sig+0x%x) %s", paramListPtr, FUNCSIG_PARAMS_OFFSET,
        paramListPtr ? "" : "*** NULL ***");

    if (!paramListPtr) {
        out("  [ParamList is NULL - arity will be 0]");
        /* Try reading alternative: OutParamList.Count at Sig+0x20 */
        uint32_t outCount = 0;
        if (safe_memory_read_u32((mach_vm_address_t)sigPtr + FUNCSIG_OUTPARAMCOUNT_OFFSET, &outCount)) {
            out("  OutParamList.Count: %u (at Sig+0x%x)", outCount, FUNCSIG_OUTPARAMCOUNT_OFFSET);
        }
        return;
    }

    /* Step 4: Size at ParamList+0x10 */
    uint32_t paramSize = 0;
    if (safe_memory_read_u32((mach_vm_address_t)paramListPtr + PARAMLIST_SIZE_OFFSET, &paramSize)) {
        out("  ParamList.Size: %u (at PL+0x%x) <- THIS IS ARITY", paramSize, PARAMLIST_SIZE_OFFSET);
    } else {
        out("  ParamList.Size: FAILED to read at PL+0x%x", PARAMLIST_SIZE_OFFSET);
    }

    /* Also read OutParamList.Count for cross-reference */
    uint32_t outCount = 0;
    void *outBitmapPtr = NULL;
    if (safe_memory_read_pointer((mach_vm_address_t)sigPtr + FUNCSIG_OUTPARAMLIST_OFFSET, &outBitmapPtr)) {
        out("  OutParamList.Params: %p (bitmap at Sig+0x%x)", outBitmapPtr, FUNCSIG_OUTPARAMLIST_OFFSET);
    }
    if (safe_memory_read_u32((mach_vm_address_t)sigPtr + FUNCSIG_OUTPARAMCOUNT_OFFSET, &outCount)) {
        out("  OutParamList.Count: %u (bitmap bytes at Sig+0x%x)", outCount, FUNCSIG_OUTPARAMCOUNT_OFFSET);
    }

    /* Also probe nearby offsets for Size discovery if ParamList.Size looks wrong */
    if (paramSize == 0 || paramSize > 20) {
        out("  [ParamList.Size=%u looks wrong, probing nearby offsets:]", paramSize);
        for (int off = 0; off <= 0x20; off += 4) {
            uint32_t val = 0;
            if (safe_memory_read_u32((mach_vm_address_t)paramListPtr + off, &val)) {
                out("    PL+0x%02x: 0x%08x (%u)", off, val, val);
            }
        }
    }

    /* Step 5: Walk the param-list nodes to discover node layout + per-param types.
     * The list is { VMT@0x00, Head@0x08, Tail@0x10, Count@0x18 }. Each node's
     * layout (Next offset, TypeId offset/width) is unknown — dump raw bytes plus
     * interpreted candidates so we can derive it from known functions:
     *   GetHostCharacter   -> [GUIDSTRING out]
     *   GetGold            -> [GUIDSTRING in, INTEGER out]
     *   AddExplorationExperience -> [GUIDSTRING in, INTEGER in]
     *   GetFlag            -> [GUIDSTRING in, GUIDSTRING in, INTEGER out] */
    void *headPtr = NULL;
    void *tailPtr = NULL;
    safe_memory_read_pointer((mach_vm_address_t)paramListPtr + PARAMLIST_HEAD_OFFSET, &headPtr);
    safe_memory_read_pointer((mach_vm_address_t)paramListPtr + PARAMLIST_TAIL_OFFSET, &tailPtr);
    out("  ParamList.Head: %p  Tail: %p", headPtr, tailPtr);

    int walkMax = (paramSize > 0 && paramSize <= 20) ? (int)paramSize + 1 : 8;
    void *node = headPtr;
    for (int n = 0; n < walkMax && node; n++) {
        SafeMemoryInfo ni = safe_memory_check_address((mach_vm_address_t)node);
        if (!ni.is_valid || !ni.is_readable) {
            out("    node[%d] %p: NOT READABLE", n, node);
            break;
        }
        uint8_t raw[0x20];
        if (!safe_memory_read((mach_vm_address_t)node, raw, sizeof(raw))) {
            out("    node[%d] %p: read failed", n, node);
            break;
        }
        /* Raw hex */
        char hexline[160];
        int pos = snprintf(hexline, sizeof(hexline), "    node[%d] %p:", n, node);
        for (int j = 0; j < 0x20; j++) {
            pos += snprintf(hexline + pos, sizeof(hexline) - pos, " %02x", raw[j]);
        }
        out("%s", hexline);
        /* Interpreted candidates: ptrs at 0x00/0x08 (Next candidates),
         * and small-int values at every 4-byte and 2-byte offset (TypeId candidates). */
        void *p0 = *(void **)(raw + 0x00);
        void *p8 = *(void **)(raw + 0x08);
        out("      next?@0x00=%p  ptr@0x08=%p  u16@0x08=%u u16@0x0c=%u u16@0x10=%u u32@0x08=%u u32@0x10=%u",
            p0, p8,
            *(uint16_t *)(raw + 0x08), *(uint16_t *)(raw + 0x0c), *(uint16_t *)(raw + 0x10),
            *(uint32_t *)(raw + 0x08), *(uint32_t *)(raw + 0x10));
        /* Advance via the most likely Next pointer (offset 0x00). */
        node = p0;
    }
}
