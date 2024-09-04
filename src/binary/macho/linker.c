#include "src/binary/linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "src/build/config.h"
#include "src/cross.h"

/**
 * https://repo.or.cz/tinycc.git/commit/b86d82c8b31aa37138ac274b1fc7a82f2a8084d1 apple new format
 * support: macOS >= 12 || iOS >= 15
 */

#define MH_EXECUTE (0x2)
#define MH_DYLDLINK (0x4)
#define MH_DYLIB (0x6)
#define MH_PIE (0x200000)

#define CPU_SUBTYPE_LIB64 (0x80000000)
#define CPU_SUBTYPE_X86_ALL (3)
#define CPU_SUBTYPE_ARM64_ALL (0)

#define CPU_ARCH_ABI64 (0x01000000)

#define CPU_TYPE_X86 (7)
#define CPU_TYPE_X86_64 (CPU_TYPE_X86 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM (12)
#define CPU_TYPE_ARM64 (CPU_TYPE_ARM | CPU_ARCH_ABI64)

static void macho_qsort(void *base, size_t nel, size_t width, int (*comp)(void *, void *, void *), void *arg);

typedef struct {
    uint32_t magic;          /* FAT_MAGIC or FAT_MAGIC_64 */
    uint32_t nfat_arch;      /* number of structs that follow */
} fat_header;

typedef int cpu_type_t;
typedef int cpu_subtype_t;

typedef struct {
    cpu_type_t cputype;        /* cpu specifier (int) */
    cpu_subtype_t cpusubtype;     /* machine specifier (int) */
    uint32_t offset;         /* file offset to this object file */
    uint32_t size;           /* size of this object file */
    uint32_t align;          /* alignment as a power of 2 */
} fat_arch;

#define FAT_MAGIC       0xcafebabe
#define FAT_CIGAM       0xbebafeca
#define FAT_MAGIC_64    0xcafebabf
#define FAT_CIGAM_64    0xbfbafeca

typedef struct {
    uint32_t magic;          /* mach magic number identifier */
    cpu_type_t cputype;        /* cpu specifier */
    cpu_subtype_t cpusubtype;     /* machine specifier */
    uint32_t filetype;       /* type of file */
    uint32_t ncmds;          /* number of load commands */
    uint32_t sizeofcmds;     /* the size of all the load commands */
    uint32_t flags;          /* flags */
} mach_header;

typedef struct {
    mach_header mh;
    uint32_t reserved;       /* reserved, pad to 64bit */
} mach_header_64;


/* Constant for the magic field of the mach_header (32-bit architectures) */
#define MH_MAGIC        0xfeedface      /* the mach magic number */
#define MH_CIGAM        0xcefaedfe      /* NXSwapInt(MH_MAGIC) */
#define MH_MAGIC_64     0xfeedfacf      /* the 64-bit mach magic number */
#define MH_CIGAM_64     0xcffaedfe      /* NXSwapInt(MH_MAGIC_64) */

typedef struct {
    uint32_t cmd;            /* type of load command */
    uint32_t cmdsize;        /* total size of command in bytes */
} load_command;

#define LC_REQ_DYLD 0x80000000
#define LC_SYMTAB        0x2
#define LC_DYSYMTAB      0xb
#define LC_LOAD_DYLIB    0xc
#define LC_ID_DYLIB      0xd
#define LC_LOAD_DYLINKER 0xe
#define LC_SEGMENT_64    0x19
#define LC_RPATH (0x1c | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB (0x1f | LC_REQ_DYLD)
#define LC_DYLD_INFO_ONLY (0x22|LC_REQ_DYLD)
#define LC_MAIN (0x28|LC_REQ_DYLD)
#define LC_SOURCE_VERSION 0x2A
#define LC_BUILD_VERSION 0x32
#define LC_DYLD_EXPORTS_TRIE (0x33 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)

#define SG_READ_ONLY    0x10 /* This segment is made read-only after fixups */

typedef int vm_prot_t;

typedef struct { /* for 64-bit architectures */
    uint32_t cmd;            /* LC_SEGMENT_64 */
    uint32_t cmdsize;        /* includes sizeof section_64 structs */
    char segname[16];    /* segment name */
    uint64_t vmaddr;         /* memory address of this segment */
    uint64_t vmsize;         /* memory size of this segment */
    uint64_t fileoff;        /* file offset of this segment */
    uint64_t filesize;       /* amount to map from the file */
    vm_prot_t maxprot;        /* maximum VM protection */
    vm_prot_t initprot;       /* initial VM protection */
    uint32_t nsects;         /* number of sections in segment */
    uint32_t flags;          /* flags */
} segment_command_64;

typedef struct { /* for 64-bit architectures */
    char sectname[16];   /* name of this section */
    char segname[16];    /* segment this section goes in */
    uint64_t addr;           /* memory address of this section */
    uint64_t size;           /* size in bytes of this section */
    uint32_t offset;         /* file offset of this section */
    uint32_t align;          /* section alignment (power of 2) */
    uint32_t reloff;         /* file offset of relocation entries */
    uint32_t nreloc;         /* number of relocation entries */
    uint32_t flags;          /* flags (section type and attributes)*/
    uint32_t reserved1;      /* reserved (for offset or index) */
    uint32_t reserved2;      /* reserved (for count or sizeof) */
    uint32_t reserved3;      /* reserved */
} section_64;


enum {
    DYLD_CHAINED_IMPORT = 1,
};

typedef struct {
    uint32_t fixups_version; ///< 0
    uint32_t starts_offset;  ///< Offset of dyld_chained_starts_in_image.
    uint32_t imports_offset; ///< Offset of imports table in chain_data.
    uint32_t symbols_offset; ///< Offset of symbol strings in chain_data.
    uint32_t imports_count;  ///< Number of imported symbol names.
    uint32_t imports_format; ///< DYLD_CHAINED_IMPORT*
    uint32_t symbols_format; ///< 0 => uncompressed, 1 => zlib compressed
} dyld_chained_fixups_header;

typedef struct {
    uint32_t seg_count;
    uint32_t seg_info_offset[1];  // each entry is offset into this struct for that segment
    // followed by pool of dyld_chain_starts_in_segment data
} dyld_chained_starts_in_image;


enum {
    DYLD_CHAINED_PTR_64 = 2,    // target is vmaddr
    DYLD_CHAINED_PTR_64_OFFSET = 6,    // target is vm offset
};

enum {
    DYLD_CHAINED_PTR_START_NONE = 0xFFFF, // used in page_start[] to denote a page with no fixups
};

#define SEG_PAGE_SIZE 16384

typedef struct {
    uint32_t size;               // size of this (amount kernel needs to copy)
    uint16_t page_size;          // 0x1000 or 0x4000
    uint16_t pointer_format;     // DYLD_CHAINED_PTR_*
    uint64_t segment_offset;     // offset in memory to start of segment
    uint32_t max_valid_pointer;  // for 32-bit OS, any value beyond this is not a pointer
    uint16_t page_count;         // how many pages are in array
    uint16_t page_start[1];      // each entry is offset in each page of first element in chain
    // or DYLD_CHAINED_PTR_START_NONE if no fixups on page
} dyld_chained_starts_in_segment;


typedef enum {
    BIND_SPECIAL_DYLIB_FLAT_LOOKUP = -2,
} BindSpecialDylib;

typedef struct {
    uint32_t lib_ordinal: 8,
            weak_import: 1,
            name_offset: 23;
} dyld_chained_import;

typedef struct {
    uint64_t target: 36,    // vmaddr, 64GB max image size
    high8: 8,    // top 8 bits set to this after slide added
    reserved: 7,    // all zeros
    next: 12,    // 4-byte stride
    bind: 1;    // == 0
} dyld_chained_ptr_64_rebase;

typedef struct {
    uint64_t ordinal: 24,
            addend: 8,   // 0 thru 255
    reserved: 19,   // all zeros
    next: 12,   // 4-byte stride
    bind: 1;   // == 1
} dyld_chained_ptr_64_bind;

#define S_REGULAR                       0x0
#define S_ZEROFILL                      0x1
#define S_NON_LAZY_SYMBOL_POINTERS      0x6
#define S_LAZY_SYMBOL_POINTERS          0x7
#define S_SYMBOL_STUBS                  0x8
#define S_MOD_INIT_FUNC_POINTERS        0x9
#define S_MOD_TERM_FUNC_POINTERS        0xa

#define S_ATTR_PURE_INSTRUCTIONS        0x80000000
#define S_ATTR_SOME_INSTRUCTIONS        0x00000400
#define S_ATTR_DEBUG                0x02000000


typedef uint32_t lc_str;

typedef struct {
    uint32_t cmd;                   /* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB,
                                       LC_REEXPORT_DYLIB */
    uint32_t cmdsize;               /* includes pathname string */
    lc_str name;                  /* library's path name */
    uint32_t timestamp;             /* library's build time stamp */
    uint32_t current_version;       /* library's current version number */
    uint32_t compatibility_version; /* library's compatibility vers number*/
} dylib_command;

typedef struct {
    uint32_t cmd;           /* LC_RPATH */
    uint32_t cmdsize;       /* includes string */
    lc_str path;          /* path to add to run path */
} rpath_command;

typedef struct {
    uint32_t cmd;            /* LC_ID_DYLINKER, LC_LOAD_DYLINKER or
                                       LC_DYLD_ENVIRONMENT */
    uint32_t cmdsize;        /* includes pathname string */
    lc_str name;           /* dynamic linker's path name */
} dylinker_command;

typedef struct {
    uint32_t cmd;            /* LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO,
                                   LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
                                   LC_DYLIB_CODE_SIGN_DRS,
                                   LC_LINKER_OPTIMIZATION_HINT,
                                   LC_DYLD_EXPORTS_TRIE, or
                                   LC_DYLD_CHAINED_FIXUPS. */
    uint32_t cmdsize;        /* sizeof(struct linkedit_data_command) */
    uint32_t dataoff;        /* file offset of data in __LINKEDIT segment */
    uint32_t datasize;       /* file size of data in __LINKEDIT segment  */
} linkedit_data_command;

#define PLATFORM_MACOS 1

typedef struct {
    uint32_t cmd;            /* LC_BUILD_VERSION */
    uint32_t cmdsize;        /* sizeof(struct build_version_command) plus */
    /* ntools * sizeof(struct build_tool_version) */
    uint32_t platform;       /* platform */
    uint32_t minos;          /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
    uint32_t sdk;            /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
    uint32_t ntools;         /* number of tool entries following this */
} build_version_command;

typedef struct {
    uint32_t cmd;      /* LC_SOURCE_VERSION */
    uint32_t cmdsize;  /* 16 */
    uint64_t version;  /* A.B.C.D.E packed as a24.b10.c10.d10.e10 */
} source_version_command;

typedef struct {
    uint32_t cmd;            /* LC_SYMTAB */
    uint32_t cmdsize;        /* sizeof(struct symtab_command) */
    uint32_t symoff;         /* symbol table offset */
    uint32_t nsyms;          /* number of symbol table entries */
    uint32_t stroff;         /* string table offset */
    uint32_t strsize;        /* string table size in bytes */
} symtab_command;

typedef struct {
    uint32_t cmd;       /* LC_DYSYMTAB */
    uint32_t cmdsize;   /* sizeof(struct dysymtab_command) */

    uint32_t ilocalsym; /* index to local symbols */
    uint32_t nlocalsym; /* number of local symbols */

    uint32_t iextdefsym;/* index to externally defined symbols */
    uint32_t nextdefsym;/* number of externally defined symbols */

    uint32_t iundefsym; /* index to undefined symbols */
    uint32_t nundefsym; /* number of undefined symbols */

    uint32_t tocoff;    /* file offset to table of contents */
    uint32_t ntoc;      /* number of entries in table of contents */

    uint32_t modtaboff; /* file offset to module table */
    uint32_t nmodtab;   /* number of module table entries */

    uint32_t extrefsymoff;  /* offset to referenced symbol table */
    uint32_t nextrefsyms;   /* number of referenced symbol table entries */

    uint32_t indirectsymoff;/* file offset to the indirect symbol table */
    uint32_t nindirectsyms; /* number of indirect symbol table entries */

    uint32_t extreloff; /* offset to external relocation entries */
    uint32_t nextrel;   /* number of external relocation entries */
    uint32_t locreloff; /* offset to local relocation entries */
    uint32_t nlocrel;   /* number of local relocation entries */
} dysymtab_command;

#define BIND_OPCODE_DONE                                        0x00
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM                       0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM               0x40
#define BIND_OPCODE_SET_TYPE_IMM                                0x50
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB                 0x70
#define BIND_OPCODE_DO_BIND                                     0x90

#define BIND_SYMBOL_FLAGS_WEAK_IMPORT                           0x1

#define BIND_TYPE_POINTER                                       1
#define BIND_SPECIAL_DYLIB_FLAT_LOOKUP                          -2

#define REBASE_OPCODE_DONE                                      0x00
#define REBASE_OPCODE_SET_TYPE_IMM                              0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB               0x20
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES                       0x50

#define REBASE_TYPE_POINTER                                     1

#define EXPORT_SYMBOL_FLAGS_KIND_REGULAR                        0x00
#define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE                       0x02
#define EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION                     0x04

typedef struct {
    uint32_t cmd;             /* LC_DYLD_INFO or LC_DYLD_INFO_ONLY */
    uint32_t cmdsize;         /* sizeof(struct dyld_info_command) */
    uint32_t rebase_off;      /* file offset to rebase info  */
    uint32_t rebase_size;     /* size of rebase info   */
    uint32_t bind_off;        /* file offset to binding info   */
    uint32_t bind_size;       /* size of binding info  */
    uint32_t weak_bind_off;   /* file offset to weak binding info   */
    uint32_t weak_bind_size;  /* size of weak binding info  */
    uint32_t lazy_bind_off;   /* file offset to lazy binding info */
    uint32_t lazy_bind_size;  /* size of lazy binding infs */
    uint32_t export_off;      /* file offset to lazy binding info */
    uint32_t export_size;     /* size of lazy binding infs */
} dyld_info_command;

#define INDIRECT_SYMBOL_LOCAL   0x80000000

typedef struct {
    uint32_t cmd;      /* LC_MAIN only used in MH_EXECUTE filetypes */
    uint32_t cmdsize;  /* 24 */
    uint64_t entryoff; /* file (__TEXT) offset of main() */
    uint64_t stacksize;/* if not zero, initial stack size */
} entry_point_command;

typedef enum {
    sk_unknown = 0,
    sk_discard,
    sk_text,
    sk_stubs,
    sk_stub_helper,
    sk_ro_data,
    sk_uw_info,
    sk_nl_ptr,  // non-lazy pointers, aka GOT
    sk_debug_info,
    sk_debug_abbrev,
    sk_debug_line,
    sk_debug_aranges,
    sk_debug_str,
    sk_debug_line_str,
    sk_stab,
    sk_stab_str,
    sk_la_ptr,  // lazy pointers
    sk_init,
    sk_fini,
    sk_rw_data,
    sk_bss,
    sk_linkedit,
    sk_last
} skind;

typedef struct {
    uint32_t n_strx;      /* index into the string table */
    uint8_t n_type;        /* type flag, see below */
    uint8_t n_sect;        /* section number or NO_SECT */
    uint16_t n_desc;       /* see <mach-o/stab.h> */
    uint64_t n_value;      /* value of this symbol (or stab offset) */
} nlist_64;

#define N_UNDF  0x0
#define N_ABS   0x2
#define N_EXT   0x1
#define N_SECT  0xe

#define N_WEAK_REF      0x0040
#define N_WEAK_DEF      0x0080

typedef struct {
    int seg_initial;
    uint32_t flags;
    const char *name;
} skinfo_t;

const skinfo_t skinfo[sk_last] = {
        /*[sk_unknown] =*/{0},
        /*[sk_discard] =*/
                          {0},
        /*[sk_text] =*/
                          {1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, "__text"},
        /*[sk_stubs] =*/
                          {1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_SYMBOL_STUBS |
                              S_ATTR_SOME_INSTRUCTIONS,                                        "__stubs"},
        /*[sk_stub_helper] =*/
                          {1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, "__stub_helper"},
        /*[sk_ro_data] =*/
                          {2, S_REGULAR,                  "__rodata"},
        /*[sk_uw_info] =*/
                          {0},
        /*[sk_nl_ptr] =*/
                          {2, S_NON_LAZY_SYMBOL_POINTERS, "__got"},
        /*[sk_debug_info] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_info"},
        /*[sk_debug_abbrev] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_abbrev"},
        /*[sk_debug_line] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_line"},
        /*[sk_debug_aranges] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_aranges"},
        /*[sk_debug_str] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_str"},
        /*[sk_debug_line_str] =*/
                          {3, S_REGULAR | S_ATTR_DEBUG,                                        "__debug_line_str"},
        /*[sk_stab] =*/
                          {4, S_REGULAR,                  "__stab"},
        /*[sk_stab_str] =*/
                          {4, S_REGULAR,                  "__stab_str"},
        /*[sk_la_ptr] =*/
                          {4, S_LAZY_SYMBOL_POINTERS,     "__la_symbol_ptr"},
        /*[sk_init] =*/
                          {4, S_MOD_INIT_FUNC_POINTERS,   "__mod_init_func"},
        /*[sk_fini] =*/
                          {4, S_MOD_TERM_FUNC_POINTERS,   "__mod_term_func"},
        /*[sk_rw_data] =*/
                          {4, S_REGULAR,                  "__data"},
        /*[sk_bss] =*/
                          {4, S_ZEROFILL,                 "__bss"},
        /*[sk_linkedit] =*/
                          {5, S_REGULAR,                                                       NULL},
};

#define    START    ((uint64_t)1 << 32)

const struct {
    int used;
    const char *name;
    uint64_t vmaddr;
    uint64_t vmsize;
    vm_prot_t maxprot;
    vm_prot_t initprot;
    uint32_t flags;
} all_segment[] = {
        {1, "__PAGEZERO",   0, START, 0, 0, 0},
        {0, "__TEXT", START,    0,    5, 5, 0},
        {0, "__DATA_CONST", -1, 0,    3, 3, SG_READ_ONLY},
        {0, "__DWARF",      -1, 0,    7, 3, 0},
        {0, "__DATA",       -1, 0,    3, 3, 0},
        {1, "__LINKEDIT",   -1, 0,    1, 1, 0},
};

#define    N_SEGMENT    (sizeof(all_segment)/sizeof(all_segment[0]))

typedef struct {
    mach_header_64 mh;
    int *seg2lc, nseg;
    load_command **lc;
    entry_point_command *ep;
    int nlc;
    struct {
        section_t *s;
        int machosect;
    } sk_to_sect[sk_last];
    int *elfsectomacho;
    int *e2msym;
    section_t *symtab, *strtab, *indirsyms, *stubs, *exports;
    uint32_t ilocal, iextdef, iundef;
    int stubsym, n_got, nr_plt;
    int segment[sk_last];
#ifdef CONFIG_NEW_MACHO
    section_t *chained_fixups;
    int n_bind;
    int n_bind_rebase;
    struct bind_rebase {
        int section;
        int bind;
        Elf64_Rela rel;
    } *bind_rebase;
#else
    section_t *rebase, *binding, *weak_binding, *lazy_binding;
    section_t *stub_helper, *la_symbol_ptr;
    dyld_info_command *dyldinfo;
    int helpsym, lasym, dyld_private, dyld_stub_binder;
    int n_lazy_bind;
    struct s_lazy_bind {
        int section;
        int bind_offset;
        int la_symbol_offset;
        Elf64_Rela rel;
    } *s_lazy_bind;
    int n_rebase;
    struct s_rebase {
        int section;
        Elf64_Rela rel;
    } *s_rebase;
    int n_bind;
    struct bind {
        int section;
        Elf64_Rel rel;
    } *bind;
#endif
} macho;

#define SHT_LINKEDIT (SHT_LOOS + 42)
#define SHN_FROMDLL  (SHN_LOOS + 2)  /* Symbol is undefined, comes from a DLL */


static int macho_symcmp(void *_a, void *_b, void *arg) {
    linker_context *ctx = arg;
    int ea = ((nlist_64 *) _a)->n_value;
    int eb = ((nlist_64 *) _b)->n_value;
    Elf64_Sym *sa = (Elf64_Sym *) ctx->symtab_section->data + ea;
    Elf64_Sym *sb = (Elf64_Sym *) ctx->symtab_section->data + eb;
    int r;

    /* locals, then defined externals, then undefined externals, the
       last two sections also by name, otherwise stable sort */
    r = (ELF64_ST_BIND(sb->st_info) == STB_LOCAL) - (ELF64_ST_BIND(sa->st_info) == STB_LOCAL);
    if (r) {
        return r;
    }

    r = (sa->st_shndx == SHN_UNDEF) - (sb->st_shndx == SHN_UNDEF);
    if (r) {
        return r;
    }

    if (ELF64_ST_BIND(sa->st_info) != STB_LOCAL) {
        const char *na = (char *) ctx->symtab_section->link->data + sa->st_name;
        const char *nb = (char *) ctx->symtab_section->link->data + sb->st_name;
        r = strcmp(na, nb);
        if (r) {
            return r;
        }
    }
    return ea - eb;
}


static void macho_create_symtab(linker_context *ctx, macho *mo) {
    int sym_index, sym_end;
    nlist_64 *pn;

    /* Stub creation belongs to check_relocs, but we need to create
       the symbol now, so its included in the sorting.  */

    mo->stubs = elf_new_section(ctx, "__stubs", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    ctx->got = elf_new_section(ctx, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);

    mo->stubsym = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &(Elf64_Sym) {
            .st_value = 0,
            .st_size = 0,
            .st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
            .st_other = 0,
            .st_shndx = mo->stubs->sh_index,
    }, ".__stubs");

#ifdef CONFIG_NEW_MACHO
    mo->chained_fixups = elf_new_section(ctx, "CHAINED_FIXUPS",
                                         SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
#else
    assert(false);
    mo->stub_helper = new_section(ctx, "__stub_helper", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    mo->la_symbol_ptr = new_section(ctx, "__la_symbol_ptr", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    mo->helpsym = put_elf_sym(ctx->symtab, 0, 0,
                              ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0,
                              mo->stub_helper->sh_num, ".__stub_helper");
    mo->lasym = put_elf_sym(ctx->symtab, 0, 0,
                            ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0,
                            mo->la_symbol_ptr->sh_num, ".__la_symbol_ptr");
    section_ptr_add(data_section, -data_section->data_count & (PTR_SIZE - 1));
    mo->dyld_private = put_elf_sym(ctx->symtab, data_section->data_count, PTR_SIZE,
                                   ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0,
                                   data_section->sh_num, ".__dyld_private");
    section_ptr_add(data_section, PTR_SIZE);
    mo->dyld_stub_binder = put_elf_sym(ctx->symtab, 0, 0,
                                       ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0,
                                       SHN_UNDEF, "dyld_stub_binder");
    mo->rebase = new_section(ctx, "REBASE", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->binding = new_section(ctx, "BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->weak_binding = new_section(ctx, "WEAK_BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->lazy_binding = new_section(ctx, "LAZY_BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
#endif

    mo->exports = elf_new_section(ctx, "EXPORT", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->indirsyms = elf_new_section(ctx, "LEINDIR", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);

    mo->symtab = elf_new_section(ctx, "LESYMTAB", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->strtab = elf_new_section(ctx, "LESTRTAB", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    elf_put_str(mo->strtab, " "); /* Mach-O starts strtab with a space */

    sym_end = ctx->symtab_section->data_count / sizeof(Elf64_Sym);

    pn = section_ptr_add(mo->symtab, sizeof(*pn) * (sym_end - 1));
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        Elf64_Sym *sym = (Elf64_Sym *) ctx->symtab_section->data + sym_index;
        char *name = (char *) ctx->symtab_section->link->data + sym->st_name;
        pn[sym_index - 1].n_strx = elf_put_str(mo->strtab, name);
        pn[sym_index - 1].n_value = sym_index;
    }
    section_ptr_add(mo->strtab, -mo->strtab->data_count & (POINTER_SIZE - 1));
    macho_qsort(pn, sym_end - 1, sizeof(*pn), macho_symcmp, ctx);
    mo->e2msym = mallocz(sym_end * sizeof(*mo->e2msym));
    mo->e2msym[0] = -1;
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        mo->e2msym[pn[sym_index - 1].n_value] = sym_index - 1;
    }
}

static void macho_bind_rebase_add(macho *mo, int bind, int sh_info, Elf64_Rela *rel, sym_attr_t *attr) {
    mo->bind_rebase = realloc(mo->bind_rebase, (mo->n_bind_rebase + 1) * sizeof(struct bind_rebase));
    mo->bind_rebase[mo->n_bind_rebase].section = sh_info;
    mo->bind_rebase[mo->n_bind_rebase].bind = bind;
    mo->bind_rebase[mo->n_bind_rebase].rel = *rel;
    if (attr) {
        mo->bind_rebase[mo->n_bind_rebase].rel.r_offset = attr->got_offset;
    }
    mo->n_bind_rebase++;
    mo->n_bind += bind;
}


static void macho_check_relocs(linker_context *ctx, macho *mo) {
    section_t *s;
    Elf64_Rela *rel, save_rel;
    Elf64_Sym *sym;
    int i, j, type, gotplt_entry, sym_index;
    int8_t for_code;
    uint32_t *pi, *goti;
    sym_attr_t *attr;

    goti = NULL;
    mo->nr_plt = mo->n_got = 0;
    for (i = 1; i < ctx->sections->count; i++) {
        s = ctx->sections->take[i];
        if (s->sh_type != SHT_RELA ||
            !strncmp(SEC_TACK(s->sh_info)->name, ".debug_", 7)) {
            continue;
        }

        for (rel = SEC_START(Elf64_Rela, s); rel < SEC_END(Elf64_Rela, s); rel++) {

            save_rel = *rel;
            type = ELF64_R_TYPE(rel->r_info);
            gotplt_entry = cross_gotplt_entry_type(type);

            for_code = cross_is_code_relocate(type);
            /* We generate a non-lazy pointer for used undefined symbols
               and for defined symbols that must have a place for their
               address due to codegen (i.e. a reloc requiring a got slot).  */
            sym_index = ELF64_R_SYM(rel->r_info);
            sym = &((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
            if (sym->st_shndx == SHN_UNDEF || gotplt_entry == ALWAYS_GOTPLT_ENTRY) {
                attr = elf_get_sym_attr(ctx, sym_index, 1);
                if (!attr->dyn_index) {
                    attr->got_offset = ctx->got->data_count;
                    attr->plt_offset = -1;
                    attr->dyn_index = 1; /* used as flag */
                    section_ptr_add(ctx->got, POINTER_SIZE);

                    elf_put_relocate(ctx, ctx->symtab_section, ctx->got, attr->got_offset,
                                     R_JMP_SLOT, sym_index, 0);

                    goti = linker_realloc(goti, (mo->n_got + 1) * sizeof(*goti));
                    if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL) {
                        if (sym->st_shndx == SHN_UNDEF) {
                            assertf(false, "undefined local symbo: '%s'",
                                    (char *) ctx->symtab_section->link->data + sym->st_name);
                        }
                        goti[mo->n_got++] = INDIRECT_SYMBOL_LOCAL;
                    } else {
                        goti[mo->n_got++] = mo->e2msym[sym_index];

                        int expect_type = R_X86_64_GOTPCREL;
                        if (BUILD_ARCH == ARCH_ARM64) {
                            expect_type = R_AARCH64_ADR_GOT_PAGE;
                        }

                        if (sym->st_shndx == SHN_UNDEF && type == expect_type) {
                            attr->plt_offset = -mo->n_bind_rebase - 2;

                            macho_bind_rebase_add(mo, 1, ctx->got->relocate->sh_info, &save_rel, attr);
                            ctx->got->relocate->data_count -= sizeof(Elf64_Rela);
                        }
                        if (for_code && sym->st_shndx == SHN_UNDEF) {
                            ctx->got->relocate->data_count -= sizeof(Elf64_Rela);
                        }
                    }
                }
                if (for_code && sym->st_shndx == SHN_UNDEF) {
                    if ((int) attr->plt_offset < -1) {
                        /* remove above bind and replace with plt */
                        mo->bind_rebase[-attr->plt_offset - 2].bind = 2;
                        attr->plt_offset = -1;
                    }
                    if (attr->plt_offset == -1) {
                        uint8_t *jmp;

                        attr->plt_offset = mo->stubs->data_count;

                        if (BUILD_ARCH == ARCH_AMD64) {
                            if (type != R_X86_64_PLT32)
                                continue;
                            jmp = section_ptr_add(mo->stubs, 6);
                            jmp[0] = 0xff; /* jmpq *ofs(%rip) */
                            jmp[1] = 0x25;
                            elf_put_relocate(ctx, ctx->symtab_section, mo->stubs,
                                             attr->plt_offset + 2,
                                             R_X86_64_GOTPCREL, sym_index, 0);
                        } else {
                            if (type != R_AARCH64_CALL26)
                                continue;
                            jmp = section_ptr_add(mo->stubs, 12);
                            elf_put_relocate(ctx, ctx->symtab_section, mo->stubs,
                                             attr->plt_offset,
                                             R_AARCH64_ADR_GOT_PAGE, sym_index, 0);
                            write32le(jmp, // adrp x16, #sym
                                      0x90000010);
                            elf_put_relocate(ctx, ctx->symtab_section, mo->stubs,
                                             attr->plt_offset + 4,
                                             R_AARCH64_LD64_GOT_LO12_NC, sym_index, 0);
                            write32le(jmp + 4, // ld x16,[x16, #sym]
                                      0xf9400210);
                            write32le(jmp + 8, // br x16
                                      0xd61f0200);
                        }

                        macho_bind_rebase_add(mo, 1, ctx->got->relocate->sh_info, &save_rel, attr);
                        pi = section_ptr_add(mo->indirsyms, sizeof(*pi));
                        *pi = mo->e2msym[sym_index];
                        mo->nr_plt++;
                    }
                    rel->r_info = ELF64_R_INFO(mo->stubsym, type);
                    rel->r_addend += attr->plt_offset;
                }
            }
            if (type == R_DATA_PTR || type == R_JMP_SLOT) {
                macho_bind_rebase_add(mo, sym->st_shndx == SHN_UNDEF ? 1 : 0, s->sh_info, &save_rel, NULL);
            }

        }
    }
    /* remove deleted binds */
    for (i = 0, j = 0; i < mo->n_bind_rebase; i++)
        if (mo->bind_rebase[i].bind == 2)
            mo->n_bind--;
        else
            mo->bind_rebase[j++] = mo->bind_rebase[i];
    mo->n_bind_rebase = j;
    pi = section_ptr_add(mo->indirsyms, mo->n_got * sizeof(*pi));
    memcpy(pi, goti, mo->n_got * sizeof(*pi));
    free(goti);
}

static int macho_check_symbols(linker_context *ctx, macho *mo) {
    int sym_index, sym_end;
    int ret = 0;

    mo->ilocal = mo->iextdef = mo->iundef = -1;
    sym_end = ctx->symtab_section->data_count / sizeof(Elf64_Sym);
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        int elf_index = ((nlist_64 *) mo->symtab->data + sym_index - 1)->n_value;
        Elf64_Sym *sym = (Elf64_Sym *) ctx->symtab_section->data + elf_index;
        const char *name = (char *) ctx->symtab_section->link->data + sym->st_name;
        unsigned type = ELF64_ST_TYPE(sym->st_info);
        unsigned bind = ELF64_ST_BIND(sym->st_info);
        unsigned vis = ELF64_ST_VISIBILITY(sym->st_other);

        log_debug("%4d (%4d): %09lx %4d %4d %4d %3d %s\n", sym_index, elf_index, (long) sym->st_value, type, bind, vis,
                  sym->st_shndx, name);

        if (bind == STB_LOCAL) {
            if (mo->ilocal == -1) {
                mo->ilocal = sym_index - 1;
            }

            if (mo->iextdef != -1 || mo->iundef != -1) {
                assertf(false, "local syms after global ones");
            }
        } else if (sym->st_shndx != SHN_UNDEF) {
            if (mo->iextdef == -1) {
                mo->iextdef = sym_index - 1;
            }
            if (mo->iundef != -1) {
                assertf(false, "external defined symbol after undefined");
            }
        } else if (sym->st_shndx == SHN_UNDEF) {
            if (mo->iundef == -1) {
                mo->iundef = sym_index - 1;
            }

            if (ELF64_ST_BIND(sym->st_info) == STB_WEAK || ctx->output_type != OUTPUT_EXE
//                find_elf_sym(s1->dynsymtab_section, name)
                    ) {
                /* Mark the symbol as coming from a dylib so that
                   relocate_syms doesn't complain.  Normally bind_exe_dynsyms
                   would do this check, and place the symbol into dynsym
                   which is checked by relocate_syms.  But Mach-O doesn't use
                   bind_exe_dynsyms.  */
                sym->st_shndx = SHN_FROMDLL;
                continue;
            }

            assertf(false, "undefined symbol '%s'", name);
        }
    }
    return ret;
}

static segment_command_64 *macho_get_segment(macho *mo, int i) {
    return (segment_command_64 *) (mo->lc[mo->seg2lc[i]]);
}


static void *macho_add_lc(macho *mo, uint32_t cmd, uint32_t cmdsize) {
    load_command *lc = mallocz(cmdsize);
    lc->cmd = cmd;
    lc->cmdsize = cmdsize;
    mo->lc = linker_realloc(mo->lc, sizeof(mo->lc[0]) * (mo->nlc + 1));
    mo->lc[mo->nlc++] = lc;
    return lc;
}

static segment_command_64 *macho_add_segment(macho *mo, const char *name) {
    segment_command_64 *sc = macho_add_lc(mo, LC_SEGMENT_64, sizeof(*sc));
    strncpy(sc->segname, name, 16);

    mo->seg2lc = linker_realloc(mo->seg2lc, sizeof(*mo->seg2lc) * (mo->nseg + 1));
    mo->seg2lc[mo->nseg++] = mo->nlc - 1;
    return sc;
}

static void calc_fixup_size(linker_context *ctx, macho *mo) {
    uint64_t i, size;

    size = (sizeof(dyld_chained_fixups_header) + 7) & -8;
    size += (sizeof(dyld_chained_starts_in_image) + (mo->nseg - 1) * sizeof(uint32_t) + 7) & -8;
    for (i = (ctx->output_type == OUTPUT_EXE); i < mo->nseg - 1; i++) {
        uint64_t page_count = (macho_get_segment(mo, i)->vmsize + SEG_PAGE_SIZE - 1) / SEG_PAGE_SIZE;
        size += (sizeof(dyld_chained_starts_in_segment) + (page_count - 1) * sizeof(uint16_t) + 7) & -8;
    }
    size += mo->n_bind * sizeof(dyld_chained_import) + 1;
    for (i = 0; i < mo->n_bind_rebase; i++) {
        if (mo->bind_rebase[i].bind) {
            int sym_index = ELF64_R_SYM(mo->bind_rebase[i].rel.r_info);
            Elf64_Sym *sym = &((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
            const char *name = (char *) ctx->symtab_section->link->data + sym->st_name;
            size += strlen(name) + 1;
        }
    }
    size = (size + 7) & -8;

    section_ptr_add(mo->chained_fixups, size);
}


typedef struct {
    const char *name;
    int flag;
    addr_t addr;
    int str_size;
    int term_size;
} trie_info;

typedef struct trie_node {
    int start;
    int end;
    int index_start;
    int index_end;
    int n_child;
    struct trie_node *child;
} trie_node;

typedef struct trie_seq {
    int n_child;
    struct trie_node *node;
    int offset;
    int nest_offset;
} trie_seq;

static void create_trie(trie_node *node,
                        int from, int to, int index_start,
                        int n_trie, trie_info *trie) {
    int i;
    int start, end, index_end;
    char cur;
    trie_node *child;

    for (i = from; i < to; i = end) {
        cur = trie[i].name[index_start];
        start = i++;
        for (; i < to; i++)
            if (cur != trie[i].name[index_start])
                break;
        end = i;
        if (start == end - 1 ||
            (trie[start].name[index_start] &&
             trie[start].name[index_start + 1] == 0))
            index_end = trie[start].str_size - 1;
        else {
            index_end = index_start + 1;
            for (;;) {
                cur = trie[start].name[index_end];
                for (i = start + 1; i < end; i++)
                    if (cur != trie[i].name[index_end])
                        break;
                if (trie[start].name[index_end] &&
                    trie[start].name[index_end + 1] == 0) {
                    end = start + 1;
                    index_end = trie[start].str_size - 1;
                    break;
                }
                if (i != end)
                    break;
                index_end++;
            }
        }
        node->child = linker_realloc(node->child,
                                     (node->n_child + 1) *
                                     sizeof(trie_node));
        child = &node->child[node->n_child];
        child->start = start;
        child->end = end;
        child->index_start = index_start;
        child->index_end = index_end;
        child->n_child = 0;
        child->child = NULL;
        node->n_child++;
        if (start != end - 1) {
            create_trie(child, start, end, index_end, n_trie, trie);
        }
    }
}

static int create_seq(int *offset, int *n_seq, trie_seq **seq, trie_node *node, int n_trie, trie_info *trie) {
    int i, nest_offset, last_seq = *n_seq, retval = *offset;
    trie_seq *p_seq;
    trie_node *p_nest;

    for (i = 0; i < node->n_child; i++) {
        p_nest = &node->child[i];
        *seq = linker_realloc(*seq, (*n_seq + 1) * sizeof(trie_seq));
        p_seq = &(*seq)[(*n_seq)++];
        p_seq->n_child = i == 0 ? node->n_child : -1;
        p_seq->node = p_nest;
        p_seq->offset = *offset;
        p_seq->nest_offset = 0;
        *offset += (i == 0 ? 1 + 1 : 0) +
                   p_nest->index_end - p_nest->index_start + 1 + 3;
    }
    for (i = 0; i < node->n_child; i++) {
        nest_offset =
                create_seq(offset, n_seq, seq, &node->child[i], n_trie, trie);
        p_seq = &(*seq)[last_seq + i];
        p_seq->nest_offset = nest_offset;
    }
    return retval;
}

static void node_free(trie_node *node) {
    int i;

    for (i = 0; i < node->n_child; i++) {
        node_free(&node->child[i]);
    }

    free(node->child);
}

static int triecmp(void *_a, void *_b, void *arg) {
    trie_info *a = (trie_info *) _a;
    trie_info *b = (trie_info *) _b;
    int len_a = strlen(a->name);
    int len_b = strlen(b->name);

    /* strange sorting needed. Name 'xx' should be after 'xx1' */
    if (!strncmp(a->name, b->name, len_a < len_b ? len_a : len_b)) {
        return len_a < len_b ? 1 : (len_a > len_b ? -1 : 0);
    }
    return strcmp(a->name, b->name);
}


static int uleb128_size(unsigned long long value) {
    int size = 0;

    do {
        value >>= 7;
        size++;
    } while (value != 0);
    return size;
}

/* cannot use qsort because code has to be reentrant */
static void macho_qsort(void *base, size_t nel, size_t width, int (*comp)(void *, void *, void *), void *arg) {
    size_t wnel, gap, wgap, i, j, k;
    char *a, *b, tmp;

    wnel = width * nel;
    for (gap = 0; ++gap < nel;)
        gap *= 3;
    while (gap /= 3) {
        wgap = width * gap;
        for (i = wgap; i < wnel; i += width) {
            for (j = i - wgap;; j -= wgap) {
                a = j + (char *) base;
                b = a + wgap;
                if ((*comp)(a, b, arg) <= 0)
                    break;
                k = width;
                do {
                    tmp = *a;
                    *a++ = *b;
                    *b++ = tmp;
                } while (--k);
                if (j < wgap)
                    break;
            }
        }
    }
}

static void write_uleb128(section_t *section, uint64_t value) {
    do {
        unsigned char byte = value & 0x7f;
        uint8_t *ptr = section_ptr_add(section, 1);

        value >>= 7;
        *ptr = byte | (value ? 0x80 : 0);
    } while (value != 0);
}

static int macho_add_section(macho *mo, segment_command_64 **_seg, const char *name) {
    segment_command_64 *seg = *_seg;
    int ret = seg->nsects;
    section_64 *sec;
    seg->nsects++;
    seg->cmdsize += sizeof(*sec);
    seg = linker_realloc(seg, sizeof(*seg) + seg->nsects * sizeof(*sec));
    sec = (section_64 *) ((char *) seg + sizeof(*seg)) + ret;
    memset(sec, 0, sizeof(*sec));
    strncpy(sec->sectname, name, 16);
    strncpy(sec->segname, seg->segname, 16);
    *_seg = seg;
    return ret;
}

static section_64 *macho_get_section(segment_command_64 *seg, int i) {
    return (section_64 *) ((char *) seg + sizeof(*seg)) + i;
}

static void export_trie(linker_context *ctx, macho *mo) {
    int i, size, offset = 0, save_offset;
    uint8_t *ptr;
    int sym_index;
    int sym_end = ctx->symtab_section->data_count / sizeof(Elf64_Sym);
    int n_trie = 0, n_seq = 0;
    trie_info *trie = NULL, *p_trie;
    trie_node node, *p_node;
    trie_seq *seq = NULL;
    addr_t vm_addr = macho_get_segment(mo, ctx->output_type == OUTPUT_EXE)->vmaddr;

    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        Elf64_Sym *sym = (Elf64_Sym *) ctx->symtab_section->data + sym_index;
        const char *name = (char *) ctx->symtab_section->link->data + sym->st_name;

        if (sym->st_shndx != SHN_UNDEF && sym->st_shndx < SHN_LORESERVE &&
            (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL ||
             ELF64_ST_BIND(sym->st_info) == STB_WEAK)) {
            int flag = EXPORT_SYMBOL_FLAGS_KIND_REGULAR;
            addr_t addr = sym->st_value + SEC_TACK(sym->st_shndx)->sh_addr - vm_addr;

            if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                flag |= EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;
            }

            log_debug("%s %d %llx\n", name, flag, (long long) addr + vm_addr);

            trie = linker_realloc(trie, (n_trie + 1) * sizeof(trie_info));
            trie[n_trie].name = name;
            trie[n_trie].flag = flag;
            trie[n_trie].addr = addr;
            trie[n_trie].str_size = strlen(name) + 1;
            trie[n_trie].term_size = uleb128_size(flag) + uleb128_size(addr);
            n_trie++;
        }
    }
    if (n_trie) {
        macho_qsort(trie, n_trie, sizeof(trie_info), triecmp, NULL);
        memset(&node, 0, sizeof(node));
        create_trie(&node, 0, n_trie, 0, n_trie, trie);
        create_seq(&offset, &n_seq, &seq, &node, n_trie, trie);
        save_offset = offset;
        for (i = 0; i < n_seq; i++) {
            p_node = seq[i].node;
            if (p_node->n_child == 0) {
                p_trie = &trie[p_node->start];
                seq[i].nest_offset = offset;
                offset += 1 + p_trie->term_size + 1;
            }
        }
        for (i = 0; i < n_seq; i++) {
            p_node = seq[i].node;
            p_trie = &trie[p_node->start];
            if (seq[i].n_child >= 0) {
                section_ptr_add(mo->exports,
                                seq[i].offset - mo->exports->data_count);
                ptr = section_ptr_add(mo->exports, 2);
                *ptr++ = 0;
                *ptr = seq[i].n_child;
            }
            size = p_node->index_end - p_node->index_start;
            ptr = section_ptr_add(mo->exports, size + 1);
            memcpy(ptr, &p_trie->name[p_node->index_start], size);
            ptr[size] = 0;
            write_uleb128(mo->exports, seq[i].nest_offset);
        }
        section_ptr_add(mo->exports, save_offset - mo->exports->data_count);
        for (i = 0; i < n_seq; i++) {
            p_node = seq[i].node;
            if (p_node->n_child == 0) {
                p_trie = &trie[p_node->start];
                write_uleb128(mo->exports, p_trie->term_size);
                write_uleb128(mo->exports, p_trie->flag);
                write_uleb128(mo->exports, p_trie->addr);
                ptr = section_ptr_add(mo->exports, 1);
                *ptr = 0;
            }
        }
        section_ptr_add(mo->exports, -mo->exports->data_count & 7);
        node_free(&node);
        free(seq);
    }
    free(trie);
}

/* If I is >= 1 and a power of two, returns log2(i)+1.
   If I is 0 returns 0.  */
static int exact_log2p1(int i) {
    int ret;
    if (!i)
        return 0;
    for (ret = 1; i >= 1 << 8; ret += 8)
        i >>= 8;
    if (i >= 1 << 4)
        ret += 4, i >>= 4;
    if (i >= 1 << 2)
        ret += 2, i >>= 2;
    if (i >= 1 << 1)
        ret++;
    return ret;
}

static void macho_collect_sections(linker_context *ctx, macho *mo) {
    int64_t i, sk, numsec;
    int used_segment[N_SEGMENT];
    uint64_t curaddr, fileofs;
    section_t *s;
    segment_command_64 *seg;
    dylib_command *dylib;
#ifdef CONFIG_NEW_MACHO
    linkedit_data_command *chained_fixups_lc;
    linkedit_data_command *export_trie_lc;
#endif
    build_version_command *dyldbv;
    source_version_command *dyldsv;
    rpath_command *rpath;
    dylinker_command *dyldlc;
    symtab_command *symlc;
    dysymtab_command *dysymlc;
    char *str;

    for (i = 0; i < N_SEGMENT; i++)
        used_segment[i] = all_segment[i].used;

    memset(mo->sk_to_sect, 0, sizeof(mo->sk_to_sect));
    for (i = ctx->sections->count; i-- > 1;) {
        int type, flags;
        s = ctx->sections->take[i];
        type = s->sh_type;
        flags = s->sh_flags;
        sk = sk_unknown;
        /* debug sections have sometimes no SHF_ALLOC */
        if ((flags & SHF_ALLOC) || !strncmp(s->name, ".debug_", 7)) {
            switch (type) {
                default:
                    sk = sk_unknown;
                    break;
                case SHT_INIT_ARRAY:
                    sk = sk_init;
                    break;
                case SHT_FINI_ARRAY:
                    sk = sk_fini;
                    break;
                case SHT_NOBITS:
                    sk = sk_bss;
                    break;
                case SHT_SYMTAB:
                    sk = sk_discard;
                    break;
                case SHT_STRTAB:
//                    if (s == stabstr_section)
//                        sk = sk_stab_str;
//                    else
                    sk = sk_discard;
                    break;
                case SHT_RELA:
                    sk = sk_discard;
                    break;
                case SHT_LINKEDIT:
                    sk = sk_linkedit;
                    break;
                case SHT_PROGBITS:
                    if (s == mo->stubs)
                        sk = sk_stubs;
#ifndef CONFIG_NEW_MACHO
                        else if (s == mo->stub_helper)
                            sk = sk_stub_helper;
                        else if (s == mo->la_symbol_ptr)
                            sk = sk_la_ptr;
#endif
//                    else if (s == rodata_section)
//                        sk = sk_ro_data;
//                    else if (s == ctx->got)
//                        sk = sk_nl_ptr;
//                    else if (s == stab_section)
//                        sk = sk_stab;
//                    else if (s == dwarf_info_section)
//                        sk = sk_debug_info;
//                    else if (s == dwarf_abbrev_section)
//                        sk = sk_debug_abbrev;
//                    else if (s == dwarf_line_section)
//                        sk = sk_debug_line;
//                    else if (s == dwarf_aranges_section)
//                        sk = sk_debug_aranges;
//                    else if (s == dwarf_str_section)
//                        sk = sk_debug_str;
//                    else if (s == dwarf_line_str_section)
//                        sk = sk_debug_line_str;
                    else if (flags & SHF_EXECINSTR)
                        sk = sk_text;
                    else if (flags & SHF_WRITE)
                        sk = sk_rw_data;
                    else
                        sk = sk_ro_data;
                    break;
            }
        } else
            sk = sk_discard;
        s->prev = mo->sk_to_sect[sk].s;
        mo->sk_to_sect[sk].s = s;
        used_segment[skinfo[sk].seg_initial] = 1;
    }

    if (ctx->output_type != OUTPUT_EXE) {
        used_segment[0] = 0;
    }

    for (i = 0; i < N_SEGMENT; i++)
        if (used_segment[i]) {
            seg = macho_add_segment(mo, all_segment[i].name);
            if (i == 1 && ctx->output_type != OUTPUT_EXE) {
                seg->vmaddr = 0;
            } else {
                seg->vmaddr = all_segment[i].vmaddr;
            }
            seg->vmsize = all_segment[i].vmsize;
            seg->maxprot = all_segment[i].maxprot;
            seg->initprot = all_segment[i].initprot;
            seg->flags = all_segment[i].flags;
            for (sk = sk_unknown; sk < sk_last; sk++) {
                if (skinfo[sk].seg_initial == i) {
                    mo->segment[sk] = mo->nseg - 1;
                }
            }

        }

    if (ctx->output_type != OUTPUT_EXE) {
        const char *name = ctx->macho_install_name ? ctx->macho_install_name : ctx->output;

        i = (sizeof(*dylib) + strlen(name) + 1 + 7) & -8;

        dylib = macho_add_lc(mo, LC_ID_DYLIB, i);
        dylib->name = sizeof(*dylib);
        dylib->timestamp = 1;
        dylib->current_version = ctx->macho_current_version ? ctx->macho_current_version : 1 << 16;
        dylib->compatibility_version =
                ctx->macho_compatibility_version ? ctx->macho_compatibility_version : 1 << 16;
        str = (char *) dylib + dylib->name;
        strcpy(str, name);
    }

#ifdef CONFIG_NEW_MACHO
    chained_fixups_lc = macho_add_lc(mo, LC_DYLD_CHAINED_FIXUPS,
                                     sizeof(linkedit_data_command));
    export_trie_lc = macho_add_lc(mo, LC_DYLD_EXPORTS_TRIE,
                                  sizeof(linkedit_data_command));
#else
    mo->dyldinfo = macho_add_lc(mo, LC_DYLD_INFO_ONLY, sizeof(*mo->dyldinfo));
#endif

    symlc = macho_add_lc(mo, LC_SYMTAB, sizeof(*symlc));
    dysymlc = macho_add_lc(mo, LC_DYSYMTAB, sizeof(*dysymlc));

    if (ctx->output_type == OUTPUT_EXE) {
        i = (sizeof(*dyldlc) + strlen("/usr/lib/dyld") + 1 + 7) & -8;
        dyldlc = macho_add_lc(mo, LC_LOAD_DYLINKER, i);
        dyldlc->name = sizeof(*dyldlc);
        str = (char *) dyldlc + dyldlc->name;
        strcpy(str, "/usr/lib/dyld");
    }

    dyldbv = macho_add_lc(mo, LC_BUILD_VERSION, sizeof(*dyldbv));
    dyldbv->platform = PLATFORM_MACOS;
    dyldbv->minos = (10 << 16) + (6 << 8);
    dyldbv->sdk = (10 << 16) + (6 << 8);
    dyldbv->ntools = 0;

    dyldsv = macho_add_lc(mo, LC_SOURCE_VERSION, sizeof(*dyldsv));
    dyldsv->version = 0;

    if (ctx->output_type == OUTPUT_EXE) {
        mo->ep = macho_add_lc(mo, LC_MAIN, sizeof(*mo->ep));
        mo->ep->entryoff = 4096;
    }

    // dll 是必须的么
//    for (i = 0; i < ctx->nb_loaded_dlls; i++) {
//        DLLReference *dllref = ctx->loaded_dlls[i];
//        if (dllref->level == 0)
//            add_dylib(mo, dllref->name);
//    }

//    if (ctx->rpath) {
//        char *path = ctx->rpath, *end;
//        do {
//            end = strchr(path, ':');
//            if (!end)
//                end = strchr(path, 0);
//            i = (sizeof(*rpath) + (end - path) + 1 + 7) & -8;
//            rpath = add_lc(mo, LC_RPATH, i);
//            rpath->path = sizeof(*rpath);
//            str = (char *) rpath + rpath->path;
//            memcpy(str, path, end - path);
//            str[end - path] = 0;
//            path = end + 1;
//        } while (*end);
//    }

    fileofs = 4096;  /* leave space for mach-o headers */
    curaddr = macho_get_segment(mo, ctx->output_type == OUTPUT_EXE)->vmaddr;
    curaddr += 4096;
    seg = NULL;
    numsec = 0;
    mo->elfsectomacho = mallocz(sizeof(*mo->elfsectomacho) * ctx->sections->count);
    for (sk = sk_unknown; sk < sk_last; sk++) {
        section_64 *sec = NULL;
        if (seg) {
            seg->vmsize = curaddr - seg->vmaddr;
            seg->filesize = fileofs - seg->fileoff;
        }
#ifdef CONFIG_NEW_MACHO
        if (sk == sk_linkedit) {
            calc_fixup_size(ctx, mo);
            export_trie(ctx, mo);
        }
#else
        assertf(false, "not support old os");
        if (sk == sk_linkedit) {
            bind_rebase(ctx, mo);
            export_trie(ctx, mo);
        }
#endif

        if (skinfo[sk].seg_initial &&
            (ctx->output_type != OUTPUT_EXE || mo->segment[sk]) &&
            mo->sk_to_sect[sk].s) {
            uint64_t al = 0;
            int si;
            seg = macho_get_segment(mo, mo->segment[sk]);
            if (skinfo[sk].name) {
                si = macho_add_section(mo, &seg, skinfo[sk].name);
                numsec++;
                mo->lc[mo->seg2lc[mo->segment[sk]]] = (load_command *) seg;
                mo->sk_to_sect[sk].machosect = si;

                sec = macho_get_section(seg, si);
                sec->flags = skinfo[sk].flags;
                if (sk == sk_stubs) {
                    if (BUILD_ARCH == ARCH_AMD64) {
                        sec->reserved2 = 6;
                    } else {
                        sec->reserved2 = 12;
                    }
                }


                if (sk == sk_nl_ptr)
                    sec->reserved1 = mo->nr_plt;
#ifndef CONFIG_NEW_MACHO
                if (sk == sk_la_ptr)
                    sec->reserved1 = mo->nr_plt + mo->n_got;
#endif
            }
            if (seg->vmaddr == -1) {
                curaddr = (curaddr + SEG_PAGE_SIZE - 1) & -SEG_PAGE_SIZE;
                seg->vmaddr = curaddr;
                fileofs = (fileofs + SEG_PAGE_SIZE - 1) & -SEG_PAGE_SIZE;
                seg->fileoff = fileofs;
            }

            for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
                int a = exact_log2p1(s->sh_addralign);
                if (a && al < (a - 1))
                    al = a - 1;
                s->sh_size = s->data_count;
            }
            if (sec)
                sec->align = al;
            al = 1ULL << al;
            if (al > 4096) {
                log_warn("alignment > 4096"), sec->align = 12, al = 4096;
            }
            curaddr = (curaddr + al - 1) & -al;
            fileofs = (fileofs + al - 1) & -al;
            if (sec) {
                sec->addr = curaddr;
                sec->offset = fileofs;
            }
            for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
                al = s->sh_addralign;
                curaddr = (curaddr + al - 1) & -al;

                log_debug("%s: cur addr now 0x%lx\n", s->name, (long) curaddr);

                s->sh_addr = curaddr;
                curaddr += s->sh_size;
                if (s->sh_type != SHT_NOBITS) {
                    fileofs = (fileofs + al - 1) & -al;
                    s->sh_offset = fileofs;
                    fileofs += s->sh_size;
                    log_debug("%s: fileofs now %ld\n", s->name, (long) fileofs);
                }
                if (sec) {
                    mo->elfsectomacho[s->sh_index] = numsec;
                }
            }
            if (sec)
                sec->size = curaddr - sec->addr;
        }
#ifdef DEBUG_MACHO
        for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
            int type = s->sh_type;
            int flags = s->sh_flags;
            DEBUGF("%d section %-16s %-10s %09lx %04x %02d %s,%s,%s\n",
                   sk,
                   s->name,
                   type == SHT_PROGBITS ? "progbits" :
                   type == SHT_NOBITS ? "nobits" :
                   type == SHT_SYMTAB ? "symtab" :
                   type == SHT_STRTAB ? "strtab" :
                   type == SHT_INIT_ARRAY ? "init" :
                   type == SHT_FINI_ARRAY ? "fini" :
                   type == SHT_RELa ? "rel" : "???",
                   (long) s->sh_addr,
                   (unsigned) s->data_count,
                   s->sh_addralign,
                   flags & SHF_ALLOC ? "alloc" : "",
                   flags & SHF_WRITE ? "write" : "",
                   flags & SHF_EXECINSTR ? "exec" : ""
            );
        }
#endif

    }
    if (seg) {
        seg->vmsize = curaddr - seg->vmaddr;
        seg->filesize = fileofs - seg->fileoff;
    }

    /* Fill symtab info */
    symlc->symoff = mo->symtab->sh_offset;
    symlc->nsyms = mo->symtab->data_count / sizeof(nlist_64);
    symlc->stroff = mo->strtab->sh_offset;
    symlc->strsize = mo->strtab->data_count;

    dysymlc->iundefsym = mo->iundef == -1 ? symlc->nsyms : mo->iundef;
    dysymlc->iextdefsym = mo->iextdef == -1 ? dysymlc->iundefsym : mo->iextdef;
    dysymlc->ilocalsym = mo->ilocal == -1 ? dysymlc->iextdefsym : mo->ilocal;
    dysymlc->nlocalsym = dysymlc->iextdefsym - dysymlc->ilocalsym;
    dysymlc->nextdefsym = dysymlc->iundefsym - dysymlc->iextdefsym;
    dysymlc->nundefsym = symlc->nsyms - dysymlc->iundefsym;
    dysymlc->indirectsymoff = mo->indirsyms->sh_offset;
    dysymlc->nindirectsyms = mo->indirsyms->data_count / sizeof(uint32_t);

#ifdef CONFIG_NEW_MACHO
    if (mo->chained_fixups->data_count) {
        chained_fixups_lc->dataoff = mo->chained_fixups->sh_offset;
        chained_fixups_lc->datasize = mo->chained_fixups->data_count;
    }
    if (mo->exports->data_count) {
        export_trie_lc->dataoff = mo->exports->sh_offset;
        export_trie_lc->datasize = mo->exports->data_count;
    }
#else
    if (mo->rebase->data_count) {
        mo->dyldinfo->rebase_off = mo->rebase->sh_offset;
        mo->dyldinfo->rebase_size = mo->rebase->data_count;
    }
    if (mo->binding->data_count) {
        mo->dyldinfo->bind_off = mo->binding->sh_offset;
        mo->dyldinfo->bind_size = mo->binding->data_count;
    }
    if (mo->weak_binding->data_count) {
        mo->dyldinfo->weak_bind_off = mo->weak_binding->sh_offset;
        mo->dyldinfo->weak_bind_size = mo->weak_binding->data_count;
    }
    if (mo->lazy_binding->data_count) {
        mo->dyldinfo->lazy_bind_off = mo->lazy_binding->sh_offset;
        mo->dyldinfo->lazy_bind_size = mo->lazy_binding->data_count;
    }
    if (mo->exports->data_count) {
        mo->dyldinfo->export_off = mo->exports->sh_offset;
        mo->dyldinfo->export_size = mo->exports->data_count;
    }
#endif
}

static void macho_convert_symbol(linker_context *ctx, macho *mo, nlist_64 *pn) {
    nlist_64 n = *pn;
    Elf64_Sym *sym = (Elf64_Sym *) ctx->symtab_section->data + pn->n_value;

    const char *name = (char *) ctx->symtab_section->link->data + sym->st_name;

    switch (ELF64_ST_TYPE(sym->st_info)) {
        case STT_NOTYPE:
        case STT_OBJECT:
        case STT_FUNC:
        case STT_SECTION:
            n.n_type = N_SECT;
            break;
        case STT_FILE:
            n.n_type = N_ABS;
            break;
        default:
            assertf(false, "unhandled ELF symbol type %d %s", ELF64_ST_TYPE(sym->st_info), name);
    }

    if (sym->st_shndx == SHN_UNDEF)
        assertf(false, "should have been rewritten to SHN_FROMDLL: %s", name);
    else if (sym->st_shndx == SHN_FROMDLL)
        n.n_type = N_UNDF, n.n_sect = 0;
    else if (sym->st_shndx == SHN_ABS)
        n.n_type = N_ABS, n.n_sect = 0;
    else if (sym->st_shndx >= SHN_LORESERVE)
        assertf(false, "unhandled ELF symbol section %d %s", sym->st_shndx, name);
    else if (!mo->elfsectomacho[sym->st_shndx]) {
        if (strncmp(SEC_TACK(sym->st_shndx)->name, ".debug_", 7))
            assertf(false, "ELF section %d(%s) not mapped into Mach-O for symbol %s",
                    sym->st_shndx, SEC_TACK(sym->st_shndx)->name, name);
    } else
        n.n_sect = mo->elfsectomacho[sym->st_shndx];
    if (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL)
        n.n_type |= N_EXT;
    else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
        n.n_desc |= N_WEAK_REF | (n.n_type != N_UNDF ? N_WEAK_DEF : 0);
    n.n_strx = pn->n_strx;
    n.n_value = sym->st_value;
    *pn = n;

}

static void macho_convert_symbols(linker_context *ctx, macho *mo) {
    nlist_64 *pn;
    for (pn = SEC_START(nlist_64, ctx->symtab_section); pn < SEC_END(nlist_64, ctx->symtab_section); pn++) {
        macho_convert_symbol(ctx, mo, pn);
    }
}


static section_t *have_section(linker_context *ctx, char *name) {
    SLICE_FOR(ctx->sections) {
        if (_i == 0) {
            continue;
        }

        section_t *sec = SLICE_VALUE(ctx->sections);
        if (str_equal(sec->name, name)) {
            return sec;
        }
    }

    return NULL;
}

/* return a reference to a section, and create it if it does not
   exists */
static section_t *find_section(linker_context *ctx, char *name) {
    section_t *sec = have_section(ctx, name);
    if (sec) {

        return sec;
    }
    /* sections are created as PROGBITS */
    return elf_new_section(ctx, name, SHT_PROGBITS, SHF_ALLOC);
}

static void macho_add_array(linker_context *ctx, char *sec, int c) {
    section_t *s;
    s = find_section(ctx, sec);
    s->sh_flags = ctx->shf_RELRO;
    s->sh_type = sec[1] == 'i' ? SHT_INIT_ARRAY : SHT_FINI_ARRAY;
    elf_put_relocate(ctx, ctx->symtab_section, s, s->data_count, R_DATA_PTR, c, 0);
    section_ptr_add(s, POINTER_SIZE);
}

static void macho_add_destructor(linker_context *ctx) {
    uint64_t init_sym, mh_execute_header, at_exit_sym;
    section_t *s;
    uint8_t *ptr;

    Elf64_Sym sym = {
            .st_value = -4096,
            .st_size = 0,
            .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
            .st_other = 0,
            .st_shndx = ctx->text_section->sh_index,
    };
    mh_execute_header = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &sym, "__mh_execute_header");

    s = find_section(ctx, ".fini_array");
    if (s->data_count == 0) {
        return;
    }

    sym.st_value = ctx->text_section->data_count;
    sym.st_size = 0;
    sym.st_info = ELF64_ST_INFO(STB_LOCAL, STT_FUNC);
    sym.st_shndx = ctx->text_section->sh_index;

    init_sym = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &sym, "___GLOBAL_init_65535");
    at_exit_sym = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &(Elf64_Sym) {
            .st_value = 0,
            .st_size = 0,
            .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
            .st_other = 0,
            .st_shndx = SHN_UNDEF,
    }, "___cxa_atexit");


    if (BUILD_ARCH == ARCH_AMD64) {
        ptr = section_ptr_add(ctx->text_section, 4);
        ptr[0] = 0x55; // pushq   %rbp
        ptr[1] = 0x48; // movq    %rsp, %rbp
        ptr[2] = 0x89;
        ptr[3] = 0xe5;

        for (Elf64_Rela *rel = SEC_START(Elf64_Rela, s->relocate); rel < SEC_END(Elf64_Rela, s->relocate); rel++) {
            int sym_index = ELF64_R_SYM(rel->r_info);

            ptr = section_ptr_add(ctx->text_section, 26);
            ptr[0] = 0x48; // lea destructor(%rip),%rax
            ptr[1] = 0x8d;
            ptr[2] = 0x05;
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section, ctx->text_section->data_count - 23,
                             R_X86_64_PC32, sym_index, -4);
            ptr[7] = 0x48; // mov %rax,%rdi
            ptr[8] = 0x89;
            ptr[9] = 0xc7;
            ptr[10] = 0x31; // xorl %ecx, %ecx
            ptr[11] = 0xc9;
            ptr[12] = 0x89; // movl %ecx, %esi
            ptr[13] = 0xce;
            ptr[14] = 0x48; // lea mh_execute_header(%rip),%rdx
            ptr[15] = 0x8d;
            ptr[16] = 0x15;
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 9,
                             R_X86_64_PC32, mh_execute_header, -4);
            ptr[21] = 0xe8; // call __cxa_atexit
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 4,
                             R_X86_64_PLT32, at_exit_sym, -4);

        }

        ptr = section_ptr_add(ctx->text_section, 2);
        ptr[0] = 0x5d; // pop   %rbp
        ptr[1] = 0xc3; // ret
    } else {
        ptr = section_ptr_add(ctx->text_section, 8);
        write32le(ptr, 0xa9bf7bfd);     // stp     x29, x30, [sp, #-16]!
        write32le(ptr + 4, 0x910003fd); // mov     x29, sp

        for (Elf64_Rela *rel = SEC_START(Elf64_Rela, s->relocate); rel < SEC_END(Elf64_Rela, s->relocate); rel++) {
            int sym_index = ELF64_R_SYM(rel->r_info);

            ptr = section_ptr_add(ctx->text_section, 24);
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 24, R_AARCH64_ADR_PREL_PG_HI21, sym_index, 0);
            write32le(ptr, 0x90000000); // adrp x0, destructor@page
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 20,
                             R_AARCH64_LDST8_ABS_LO12_NC, sym_index, 0);
            write32le(ptr + 4, 0x91000000); // add x0,x0,destructor@pageoff
            write32le(ptr + 8, 0xd2800001); // mov x1, #0
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 12,
                             R_AARCH64_ADR_PREL_PG_HI21, mh_execute_header, 0);
            write32le(ptr + 12, 0x90000002); // adrp x2, mh_execute_header@page
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 8,
                             R_AARCH64_LDST8_ABS_LO12_NC, mh_execute_header, 0);
            write32le(ptr + 16, 0x91000042); // add x2,x2,mh_execute_header@pageoff
            elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                             ctx->text_section->data_count - 4,
                             R_AARCH64_CALL26, at_exit_sym, 0);
            write32le(ptr + 20, 0x94000000); // bl __cxa_atexit
        }
        ptr = section_ptr_add(ctx->text_section, 8);
        write32le(ptr, 0xa8c17bfd);     // ldp     x29, x30, [sp], #16
        write32le(ptr + 4, 0xd65f03c0); // ret
    }

    s->relocate->data_count = s->data_count = 0;
    s->sh_flags &= ~SHF_ALLOC;


    macho_add_array(ctx, ".init_array", init_sym);
}

static int bind_rebase_cmp(void *_a, void *_b, void *arg) {
    linker_context *ctx = arg;
    struct bind_rebase *a = (struct bind_rebase *) _a;
    struct bind_rebase *b = (struct bind_rebase *) _b;
    addr_t aa = SEC_TACK(a->section)->sh_addr + a->rel.r_offset;
    addr_t ab = SEC_TACK(a->section)->sh_addr + b->rel.r_offset;

    return aa > ab ? 1 : aa < ab ? -1 : 0;
}

static void bind_rebase_import(linker_context *ctx, macho *mo) {
    int64_t i, j, k, bind_index, size, page_count, sym_index;
    const char *name;
    Elf64_Sym *sym;
    unsigned char *data = mo->chained_fixups->data;
    segment_command_64 *seg;
    dyld_chained_fixups_header *header;
    dyld_chained_starts_in_image *image;
    dyld_chained_starts_in_segment *segment;
    dyld_chained_import *import;

    macho_qsort(mo->bind_rebase, mo->n_bind_rebase, sizeof(struct bind_rebase),
                bind_rebase_cmp, ctx);
    for (i = 0; i < mo->n_bind_rebase - 1; i++)
        if (mo->bind_rebase[i].section == mo->bind_rebase[i + 1].section &&
            mo->bind_rebase[i].rel.r_offset == mo->bind_rebase[i + 1].rel.r_offset) {
            sym_index = ELF64_R_SYM(mo->bind_rebase[i].rel.r_info);
            sym = &((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
            name = (char *) ctx->symtab_section->link->data + sym->st_name;
            assertf(false, "Overlap %s/%s %s:%s",
                    mo->bind_rebase[i].bind ? "bind" : "rebase",
                    mo->bind_rebase[i + 1].bind ? "bind" : "rebase",
                    SEC_TACK(mo->bind_rebase[i].section)->name, name);
        }
    header = (dyld_chained_fixups_header *) data;
    data += (sizeof(dyld_chained_fixups_header) + 7) & -8;
    header->starts_offset = data - mo->chained_fixups->data;
    header->imports_count = mo->n_bind;
    header->imports_format = DYLD_CHAINED_IMPORT;
    header->symbols_format = 0;
    size = sizeof(dyld_chained_starts_in_image) + (mo->nseg - 1) * sizeof(uint32_t);
    image = (dyld_chained_starts_in_image *) data;
    data += (size + 7) & -8;
    image->seg_count = mo->nseg;
    for (i = (ctx->output_type == OUTPUT_EXE); i < mo->nseg - 1; i++) {
        image->seg_info_offset[i] = (data - mo->chained_fixups->data) -
                                    header->starts_offset;
        seg = macho_get_segment(mo, i);
        page_count = (seg->vmsize + SEG_PAGE_SIZE - 1) / SEG_PAGE_SIZE;
        size = sizeof(dyld_chained_starts_in_segment) + (page_count - 1) * sizeof(uint16_t);
        segment = (dyld_chained_starts_in_segment *) data;
        data += (size + 7) & -8;
        segment->size = size;
        segment->page_size = SEG_PAGE_SIZE;
//#if 1
#define PTR_64_OFFSET 0
#define PTR_64_MASK 0x7FFFFFFFFFFULL
        segment->pointer_format = DYLD_CHAINED_PTR_64;
//#else
//#define PTR_64_OFFSET 0x100000000ULL
//#define PTR_64_MASK 0xFFFFFFFFFFFFFFULL
//        segment->pointer_format = DYLD_CHAINED_PTR_64_OFFSET;
//#endif
        segment->segment_offset = seg->fileoff;
        segment->max_valid_pointer = 0;
        segment->page_count = page_count;
        // add bind/rebase
        bind_index = 0;
        k = 0;
        for (j = 0; j < page_count; j++) {
            addr_t start = seg->vmaddr + j * SEG_PAGE_SIZE;
            addr_t end = start + SEG_PAGE_SIZE;
            void *last = NULL;
            addr_t last_o = 0;
            addr_t cur_o, cur;
            dyld_chained_ptr_64_rebase *rebase;
            dyld_chained_ptr_64_bind *bind;

            segment->page_start[j] = DYLD_CHAINED_PTR_START_NONE;
            for (; k < mo->n_bind_rebase; k++) {
                section_t *s = SEC_TACK(mo->bind_rebase[k].section);
                addr_t r_offset = mo->bind_rebase[k].rel.r_offset;
                addr_t addr = s->sh_addr + r_offset;

                if ((addr & 3) || (addr & (SEG_PAGE_SIZE - 1)) > SEG_PAGE_SIZE - POINTER_SIZE) {
                    assertf(false, "Illegal rel_offset %s %lld", s->name, (long long) r_offset);
                }

                if (addr >= end) {
                    break;
                }

                if (addr >= start) {
                    cur_o = addr - start;
                    if (mo->bind_rebase[k].bind) {
                        if (segment->page_start[j] == DYLD_CHAINED_PTR_START_NONE)
                            segment->page_start[j] = cur_o;
                        else {
                            bind = (dyld_chained_ptr_64_bind *) last;
                            bind->next = (cur_o - last_o) / 4;
                        }
                        bind = (dyld_chained_ptr_64_bind *) (s->data + r_offset);
                        last = bind;
                        last_o = cur_o;
                        bind->ordinal = bind_index;
                        bind->addend = 0;
                        bind->reserved = 0;
                        bind->next = 0;
                        bind->bind = 1;
                    } else {
                        if (segment->page_start[j] == DYLD_CHAINED_PTR_START_NONE)
                            segment->page_start[j] = cur_o;
                        else {
                            rebase = (dyld_chained_ptr_64_rebase *) last;
                            rebase->next = (cur_o - last_o) / 4;
                        }
                        rebase = (dyld_chained_ptr_64_rebase *) (s->data + r_offset);
                        last = rebase;
                        last_o = cur_o;
                        cur = (*(uint64_t *) (s->data + r_offset)) -
                              PTR_64_OFFSET;
                        rebase->target = cur & PTR_64_MASK;
                        rebase->high8 = cur >> (64 - 8);
                        if (cur != ((uint64_t) rebase->high8 << (64 - 8)) + rebase->target) {
                            assertf(false, "rebase error");
                        }
                        rebase->reserved = 0;
                        rebase->next = 0;
                        rebase->bind = 0;
                    }
                }
                bind_index += mo->bind_rebase[k].bind;
            }
        }
    }
    // add imports
    header->imports_offset = data - mo->chained_fixups->data;
    import = (dyld_chained_import *) data;
    data += mo->n_bind * sizeof(dyld_chained_import);
    header->symbols_offset = data - mo->chained_fixups->data;
    data++;
    for (i = 0, bind_index = 0; i < mo->n_bind_rebase; i++) {
        if (mo->bind_rebase[i].bind) {
            import[bind_index].lib_ordinal =
                    BIND_SPECIAL_DYLIB_FLAT_LOOKUP & 0xffu;
            import[bind_index].name_offset =
                    (data - mo->chained_fixups->data) - header->symbols_offset;
            sym_index = ELF64_R_SYM(mo->bind_rebase[i].rel.r_info);
            sym = &((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
            import[bind_index].weak_import =
                    ELF64_ST_BIND(sym->st_info) == STB_WEAK;
            name = (char *) ctx->symtab_section->link->data + sym->st_name;
            strcpy((char *) data, name);
            data += strlen(name) + 1;
            bind_index++;
        }
    }
    free(mo->bind_rebase);
}

void macho_exe_file_format(linker_context *ctx) {
    // mo 初始化
    macho *mo = NEW(macho);

    macho_add_destructor(ctx);

    // 共用 elf 的 resolve common
    elf_resolve_common_symbols(ctx);

    macho_create_symtab(ctx, mo);

    macho_check_relocs(ctx, mo);

    macho_check_symbols(ctx, mo);

    // 缓存 output_type
    uint8_t output_type = ctx->output_type;

    macho_collect_sections(ctx, mo);

    elf_relocate_symbols(ctx, ctx->symtab_section);

    if (ctx->output_type == OUTPUT_EXE) {
        mo->ep->entryoff = elf_get_sym_addr(ctx, "runtime_main", 1) - macho_get_segment(mo, 1)->vmaddr;
    }

    // Macho uses bind/rebase instead of dynsym
    ctx->output_type = OUTPUT_EXE;
    elf_relocate_sections(ctx);
    ctx->output_type = output_type;

#ifdef CONFIG_NEW_MACHO
    bind_rebase_import(ctx, mo);
#endif

    macho_convert_symbols(ctx, mo);

    ctx->macho = mo;
}

/**
 * https://repo.or.cz/tinycc.git/commit/b86d82c8b31aa37138ac274b1fc7a82f2a8084d1 新的 apple macho 格式支持
 */
void macho_output(linker_context *ctx) {
    assert(ctx->macho);
    macho *mo = ctx->macho;

    FILE *f;
    unlink(ctx->output);
    int fd = open(ctx->output, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0777);
    if (fd < 0 || (f = fdopen(fd, "wb")) == NULL) {
        assertf(false, "[macho_output] could not write '%s: %s'", ctx->output);
        return;
    }

    int i, sk;
    uint64_t fileofs = 0;
    section_t *s;
    mo->mh.mh.magic = MH_MAGIC_64;

    assert(BUILD_ARCH);
    if (BUILD_ARCH == ARCH_AMD64) {
        mo->mh.mh.cputype = CPU_TYPE_X86_64;
        mo->mh.mh.cpusubtype = CPU_SUBTYPE_LIB64 | CPU_SUBTYPE_X86_ALL;
    } else {
        assert(BUILD_ARCH == ARCH_ARM64);
        mo->mh.mh.cputype = CPU_TYPE_ARM64;
        mo->mh.mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    }

    if (ctx->output_type == OUTPUT_EXE) {
        mo->mh.mh.filetype = MH_EXECUTE;
        mo->mh.mh.flags = MH_DYLDLINK | MH_PIE;
    } else {
        mo->mh.mh.filetype = MH_DYLIB;
        mo->mh.mh.flags = MH_DYLDLINK;
    }
    mo->mh.mh.ncmds = mo->nlc;
    mo->mh.mh.sizeofcmds = 0;
    for (i = 0; i < mo->nlc; i++)
        mo->mh.mh.sizeofcmds += mo->lc[i]->cmdsize;

    fwrite(&mo->mh, 1, sizeof(mo->mh), f);
    fileofs += sizeof(mo->mh);
    for (i = 0; i < mo->nlc; i++) {
        fwrite(mo->lc[i], 1, mo->lc[i]->cmdsize, f);
        fileofs += mo->lc[i]->cmdsize;
    }

    for (sk = sk_unknown; sk < sk_last; sk++) {
        // struct segment_command_64 *seg;
        if (skinfo[sk].seg_initial == 0 ||
            (ctx->output_type == OUTPUT_EXE && !mo->segment[sk]) ||
            !mo->sk_to_sect[sk].s)
            continue;
        /*seg =*/macho_get_segment(mo, mo->segment[sk]);
        for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
            if (s->sh_type != SHT_NOBITS) {
                while (fileofs < s->sh_offset)
                    fputc(0, f), fileofs++;
                if (s->sh_size) {
                    fwrite(s->data, 1, s->sh_size, f);
                    fileofs += s->sh_size;
                }
            }
        }
    }
}
