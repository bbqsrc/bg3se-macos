/**
 * BG3SE-macOS - Runtime Symbol Resolver
 *
 * Parses the main executable's LC_SYMTAB (located via __LINKEDIT) once and
 * resolves mangled symbol names to slid runtime addresses. BG3 ships its binary
 * un-stripped (local symbols included), so singletons/functions/TypeId globals
 * can be located by name regardless of game version.
 */

#include "symbol_resolver.h"
#include "version_detect.h"
#include "logging.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdlib.h>
#include <string.h>

#define GHIDRA_BASE 0x100000000ULL

// Cached symbol-table location for the main executable.
static bool g_inited = false;
static const struct nlist_64 *g_symtab = NULL;
static uint32_t g_nsyms = 0;
static const char *g_strtab = NULL;
static uint32_t g_strsize = 0;
static intptr_t g_slide = 0;
static uintptr_t g_binary_base = 0;

// ============================================================================
// Init
// ============================================================================

bool symbol_resolver_init(void) {
    if (g_inited) return true;

    // Find the main executable image (exactly one MH_EXECUTE).
    uint32_t count = _dyld_image_count();
    const struct mach_header_64 *header = NULL;
    intptr_t slide = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header_64 *h =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (h && h->magic == MH_MAGIC_64 && h->filetype == MH_EXECUTE) {
            header = h;
            slide = _dyld_get_image_vmaddr_slide(i);
            break;
        }
    }
    if (!header) {
        log_message("[SymbolResolver] Main executable image not found");
        return false;
    }

    // Walk load commands for __LINKEDIT (maps the symtab) and LC_SYMTAB.
    uint64_t linkedit_vmaddr = 0, linkedit_fileoff = 0;
    bool have_linkedit = false;
    const struct symtab_command *symcmd = NULL;

    const uint8_t *ptr = (const uint8_t *)header + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)ptr;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)ptr;
            if (strcmp(seg->segname, "__LINKEDIT") == 0) {
                linkedit_vmaddr = seg->vmaddr;
                linkedit_fileoff = seg->fileoff;
                have_linkedit = true;
            }
        } else if (lc->cmd == LC_SYMTAB) {
            symcmd = (const struct symtab_command *)ptr;
        }
        ptr += lc->cmdsize;
    }

    if (!have_linkedit || !symcmd) {
        log_message("[SymbolResolver] No __LINKEDIT/LC_SYMTAB (binary stripped?)");
        return false;
    }

    // File offsets within __LINKEDIT map to runtime addresses via:
    //   runtime = linkedit_vmaddr + slide + (fileoff_of_item - linkedit_fileoff)
    uintptr_t le_runtime = (uintptr_t)linkedit_vmaddr + (uintptr_t)slide;
    g_symtab = (const struct nlist_64 *)(le_runtime +
                                         (symcmd->symoff - linkedit_fileoff));
    g_strtab = (const char *)(le_runtime + (symcmd->stroff - linkedit_fileoff));
    g_nsyms = symcmd->nsyms;
    g_strsize = symcmd->strsize;
    g_slide = slide;
    g_binary_base = (uintptr_t)header;
    g_inited = true;

    log_message("[SymbolResolver] Initialized: %u symbols, slide=0x%lx, base=%p",
                g_nsyms, (unsigned long)g_slide, (void *)g_binary_base);
    return true;
}

bool symbol_resolver_available(void) {
    return g_inited;
}

// ============================================================================
// Resolution
// ============================================================================

static inline bool name_matches(const char *sym, const char *want) {
    if (strcmp(sym, want) == 0) return true;
    // Tolerate a single leading-underscore difference (Mach-O vs Itanium form).
    if (sym[0] == '_' && strcmp(sym + 1, want) == 0) return true;
    if (want[0] == '_' && strcmp(sym, want + 1) == 0) return true;
    return false;
}

void *symbol_resolve(const char *mangled_name) {
    if (!g_inited || !mangled_name) return NULL;

    for (uint32_t i = 0; i < g_nsyms; i++) {
        const struct nlist_64 *s = &g_symtab[i];
        if (s->n_value == 0) continue;            // undefined / no address
        uint32_t strx = s->n_un.n_strx;
        if (strx == 0 || strx >= g_strsize) continue;
        const char *sym = g_strtab + strx;
        if (name_matches(sym, mangled_name)) {
            return (void *)(uintptr_t)(s->n_value + (uint64_t)g_slide);
        }
    }
    return NULL;
}

// --- batch resolution (single symtab pass, sorted wanted-set + bsearch) ---

typedef struct {
    const char *name;
    int idx;
} WantEntry;

static int want_cmp(const void *a, const void *b) {
    return strcmp(((const WantEntry *)a)->name, ((const WantEntry *)b)->name);
}

static int want_key_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const WantEntry *)elem)->name);
}

int symbol_resolve_batch(const char *const *names, void **out, int count) {
    for (int i = 0; i < count; i++) out[i] = NULL;
    if (!g_inited || count <= 0) return 0;

    WantEntry *want = (WantEntry *)malloc(sizeof(WantEntry) * (size_t)count);
    if (!want) return 0;
    for (int i = 0; i < count; i++) {
        want[i].name = names[i];
        want[i].idx = i;
    }
    qsort(want, (size_t)count, sizeof(WantEntry), want_cmp);

    int resolved = 0;
    for (uint32_t i = 0; i < g_nsyms; i++) {
        const struct nlist_64 *s = &g_symtab[i];
        if (s->n_value == 0) continue;
        uint32_t strx = s->n_un.n_strx;
        if (strx == 0 || strx >= g_strsize) continue;
        const char *sym = g_strtab + strx;

        WantEntry *hit = (WantEntry *)bsearch(sym, want, (size_t)count,
                                              sizeof(WantEntry), want_key_cmp);
        if (!hit && sym[0] == '_') {
            hit = (WantEntry *)bsearch(sym + 1, want, (size_t)count,
                                       sizeof(WantEntry), want_key_cmp);
        }
        if (hit && out[hit->idx] == NULL) {
            out[hit->idx] = (void *)(uintptr_t)(s->n_value + (uint64_t)g_slide);
            resolved++;
        }
    }
    free(want);
    return resolved;
}

void *resolve_addr(const char *mangled_name, uint64_t ghidra_fallback) {
    void *addr = symbol_resolve(mangled_name);
    if (addr) return addr;

    // Symbol missing — fall back to the legacy hardcoded address, but only when
    // the game version matches the build those addresses were derived from.
    if (ghidra_fallback && g_binary_base && version_detect_matches()) {
        return (void *)(g_binary_base + (uintptr_t)(ghidra_fallback - GHIDRA_BASE));
    }
    return NULL;
}

// ============================================================================
// Component TypeId enumeration (demangle pass)
// ============================================================================

// Itanium ABI demangler (libc++abi). Declared here to keep this a C TU.
extern char *__cxa_demangle(const char *mangled, char *buf, size_t *n, int *status);

// Constant mangled suffixes that uniquely identify the two component TypeId
// global flavours. The COMPONENT template arg varies (and may use substitution
// compression, e.g. NS_4uuid9Component for ls::uuid::Component), but these
// suffixes are invariant, so they make a precise, mangling-agnostic prefilter.
#define MANGLED_REG_SUFFIX "N3ecs22ComponentTypeIdContextEE11m_TypeIndexE"
#define MANGLED_OF_SUFFIX  "N3ecs30OneFrameComponentTypeIdContextEE11m_TypeIndexE"

// Demangled wrappers around the COMPONENT name.
static const char DM_PREFIX[]     = "ls::TypeId<";
static const char DM_REG_SUFFIX[] = ", ecs::ComponentTypeIdContext>::m_TypeIndex";
static const char DM_OF_SUFFIX[]  = ", ecs::OneFrameComponentTypeIdContext>::m_TypeIndex";

int symbol_resolver_enumerate_typeids(TypeIdSymbolCb cb, void *user) {
    if (!g_inited || !cb) return 0;

    // Reused across demangle calls; __cxa_demangle realloc's as needed.
    char *dmbuf = NULL;
    size_t dmlen = 0;
    int reported = 0;

    for (uint32_t i = 0; i < g_nsyms; i++) {
        const struct nlist_64 *s = &g_symtab[i];
        if (s->n_value == 0) continue;
        uint32_t strx = s->n_un.n_strx;
        if (strx == 0 || strx >= g_strsize) continue;
        const char *sym = g_strtab + strx;

        // Cheap prefilter: must be an ls::TypeId<...> with a component context.
        if (!strstr(sym, "6TypeIdI")) continue;
        bool one_frame;
        if (strstr(sym, MANGLED_REG_SUFFIX)) {
            one_frame = false;
        } else if (strstr(sym, MANGLED_OF_SUFFIX)) {
            one_frame = true;
        } else {
            continue;
        }

        int status = -1;
        char *dm = __cxa_demangle(sym, dmbuf, &dmlen, &status);
        if (!dm || status != 0) continue;
        dmbuf = dm;  // keep the (possibly grown) buffer for reuse

        // dm = "ls::TypeId<COMPONENT, ecs::...Context>::m_TypeIndex"
        if (strncmp(dm, DM_PREFIX, sizeof(DM_PREFIX) - 1) != 0) continue;
        char *comp = dm + (sizeof(DM_PREFIX) - 1);
        char *suffix = strstr(comp, one_frame ? DM_OF_SUFFIX : DM_REG_SUFFIX);
        if (!suffix) continue;
        *suffix = '\0';  // terminate COMPONENT in place (buffer is reused next iter)

        void *addr = (void *)(uintptr_t)(s->n_value + (uint64_t)g_slide);
        cb(comp, addr, one_frame, user);
        reported++;
    }

    free(dmbuf);
    return reported;
}
