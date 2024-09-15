#ifndef NATURE_CROSS_H
#define NATURE_CROSS_H

#include "types.h"
#include "src/build/config.h"
#include "src/lir.h"



// -------- linker/elf start -----------

#define AMD64_ELF_START_ADDR 0x400000
#define AMD64_64_ELF_PAGE_SIZE 0x200000
#define AMD64_PTR_SIZE 8 // 单位 byte
#define AMD64_NUMBER_SIZE 8 // 单位 byte

uint64_t amd64_create_plt_entry(elf_context_t *ctx, uint64_t got_offset, sym_attr_t *attr);

int amd64_gotplt_entry_type(uint64_t relocate_type);

int arm64_gotplt_entry_type(uint64_t relocate_type);

int8_t amd64_is_code_relocate(uint64_t relocate_type);

int8_t arm64_is_code_relocate(uint64_t relocate_type);

void amd64_relocate(elf_context_t *ctx, Elf64_Rela *rel, int type, uint8_t *ptr, addr_t addr, addr_t val);

static inline uint64_t cross_create_plt_entry(elf_context_t *ctx, uint64_t got_offset, sym_attr_t *attr) {
    if (BUILD_ARCH == ARCH_AMD64) {
        return amd64_create_plt_entry(ctx, got_offset, attr);
    }

    assert(false && "not support arch");
}


static inline int8_t cross_is_code_relocate(uint64_t relocate_type) {
    if (BUILD_ARCH == ARCH_AMD64) {
        return amd64_is_code_relocate(relocate_type);
    } else if (BUILD_ARCH == ARCH_ARM64) {
        return arm64_is_code_relocate(relocate_type);
    }
    assert(false && "not support arch");
}

static inline int cross_got_rel_type(bool is_code_rel) {
    if (BUILD_ARCH == ARCH_AMD64) {
        if (is_code_rel) {
            return R_X86_64_JUMP_SLOT;
        } else {
            return R_X86_64_GLOB_DAT;
        }
    }
    assert(false && "not support arch");
}

static inline int cross_gotplt_entry_type(uint64_t relocate_type) {
    if (BUILD_ARCH == ARCH_AMD64) {
        return amd64_gotplt_entry_type(relocate_type);
    } else if (BUILD_ARCH == ARCH_ARM64) {
        return arm64_gotplt_entry_type(relocate_type);
    }
    assert(false && "not support arch");
}

static inline uint8_t cross_ptr_size() {
    if (BUILD_ARCH == ARCH_AMD64) {
        return AMD64_PTR_SIZE;
    }

    return sizeof(void *);
//    assert(false && "not support arch");
}

#ifndef POINTER_SIZE
#define POINTER_SIZE cross_ptr_size()
#endif

static inline uint8_t cross_number_size() {
    if (BUILD_ARCH == ARCH_AMD64) {
        return AMD64_NUMBER_SIZE;
    }
    return sizeof(int);
//    assert(false && "not support arch");
}

static inline uint64_t cross_elf_start_addr() {
    if (BUILD_ARCH == ARCH_AMD64) {
        return AMD64_ELF_START_ADDR;
    }
    assert(false && "not support arch");
}

static inline uint64_t cross_elf_page_size() {
    if (BUILD_ARCH == ARCH_AMD64) {
        return AMD64_64_ELF_PAGE_SIZE;
    }
    assert(false && "not support arch");
}

static inline void cross_relocate(elf_context_t *l, Elf64_Rela *rel, int type, uint8_t *ptr, addr_t addr, addr_t val) {
    if (BUILD_ARCH == ARCH_AMD64) {
        return amd64_relocate(l, rel, type, ptr, addr, val);
    }
    assert(false && "not support arch");
}

static inline uint16_t cross_ehdr_machine() {
    if (BUILD_ARCH == ARCH_AMD64) {
        return EM_X86_64;
    }
    assert(false && "not support arch");
}

// -------- linker/elf end -----------
#endif //NATURE_CROSS_H
