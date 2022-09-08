#include "x86_64.h"
#include "elf.h"
#include "linker.h"
#include "utils/helper.h"
#include "utils/error.h"
#include "string.h"

/**
 * mov 0x0(%rip),%rsi = 48 8b 35 00 00 00 00，return 3
 * call 0x0(%rip) // ff 15 00 00 00 00
 * call rel32 // e8 00 00 00 00
 * jmp rel32 // e9 00 00 00 00
 * 其中偏移是从第四个字符开始, offset 从 0 开始算，所以使用 + 3, 但是不同指令需要不同对待
 * @param opcode
 * @return
 */
uint64_t opcode_rip_offset(x86_64_opcode_t *opcode) {
    if (str_equal(opcode->name, "mov")) {
        // rip 相对寻址
        return 3;
    }

    if (str_equal(opcode->name, "call")) {
//        if (opcode->operands[0]->type == ASM_OPERAND_TYPE_RIP_RELATIVE) {
//            return 2;
//        }

        return 1;
    }
    if (opcode->name[0] == 'j') {
        return 1;
    }

    error_exit("[opcode_rip_offset] cannot ident opcode: %s", opcode->name);
    return 0;
}

uint8_t jmp_rewrite_rel8_reduce_count(x86_64_opcode_t *opcode);

/**
 * symbol to rel32 or rel8
 * @param opcode
 * @param operand
 * @param rel_diff
 */
static void x86_64_rewrite_symbol(x86_64_opcode_t *opcode, x86_64_operand_t *operand, int rel_diff) {

}

// TODO
static x86_64_operand_t *has_symbol_operand(x86_64_opcode_t *opcode) {

}

static bool is_call_opcode(char *name) {
    return str_equal(name, "call");
}

static bool is_jmp_opcode(char *name) {
    return name[0] == 'j';
}

static x86_64_build_temp_t *build_temp_new(x86_64_opcode_t *opcode) {
    x86_64_build_temp_t *temp = NEW(x86_64_build_temp_t);
    temp->data = NULL;
    temp->data_count = 0;
    temp->offset = NULL;
    temp->opcode = opcode;
    temp->rel_operand = NULL;
    temp->rel_symbol = NULL;
    temp->may_need_reduce = false;
    temp->reduce_count = 0;
    temp->sym_index = 0;
    temp->elf_rel = NULL;
    return temp;
}

static void x86_64_rewrite_rel32_to_rel8(x86_64_build_temp_t *temp) {
    asm_operand_uint8_t *operand = NEW(asm_operand_uint8_t);
    operand->value = 0; // 仅占位即可
    temp->opcode->count = 1;
    temp->opcode->operands[0]->type = ASM_OPERAND_TYPE_UINT8;
    temp->opcode->operands[0]->size = BYTE;
    temp->opcode->operands[0]->value = operand;
    temp->data = x86_64_opcode_encoding(*temp->opcode, &temp->data_count);
    temp->may_need_reduce = false;
}

static void x86_64_confirm_rel(section_t *symtab, slice_t *build_temps, uint64_t *section_offset, string name) {
    if (build_temps->count == 0) {
        return;
    }

    x86_64_build_temp_t *temp;
    int i;
    uint8_t reduce_count = 0;

    // 从尾部开始查找,
    for (i = build_temps->count; i > 0; --i) {
        temp = build_temps->take[i];
        // 直到总指令长度超过 128 就可以结束查找
        if ((*section_offset - reduce_count - *temp->offset) > 128) {
            break;
        }

        // 前 128 个指令内找到了符号引用
        if (temp->may_need_reduce && str_equal(temp->rel_symbol, name)) {
            reduce_count += temp->reduce_count;
        }
    }

    if (reduce_count == 0) {
        return;
    }

    // 指令偏移修复
    uint64_t temp_section_offset = *temp->offset;
    // 从 temp 开始遍历
    for (int j = i; j < build_temps->count; ++j) {
        temp = build_temps->take[j];
        *temp->offset = temp_section_offset;

        if (temp->may_need_reduce && str_equal(temp->rel_symbol, name)) {
            x86_64_rewrite_rel32_to_rel8(temp);
        }
        // 如果存在符号表引用了位置数据，则修正符号表中的数据
        if (temp->sym_index > 0) {
            ((Elf64_Sym *) symtab->data)[temp->sym_index].st_value = *temp->offset;
        }
        if (temp->elf_rel) {
            temp->elf_rel->r_offset = *temp->offset += opcode_rip_offset(temp->opcode);
        }

        temp_section_offset += temp->data_count;
    }

    // 更新 section offset
    *section_offset = temp_section_offset;
}

int x86_64_gotplt_entry_type(uint relocate_type) {
    switch (relocate_type) {
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_COPY:
        case R_X86_64_RELATIVE:
            return NO_GOTPLT_ENTRY;

            /* The following relocs wouldn't normally need GOT or PLT
               slots, but we need them for simplicity in the link
               editor part.  See our caller for comments.  */
        case R_X86_64_32:
        case R_X86_64_32S:
        case R_X86_64_64:
        case R_X86_64_PC32:
        case R_X86_64_PC64:
            return AUTO_GOTPLT_ENTRY;

        case R_X86_64_GOTTPOFF:
            return BUILD_GOT_ONLY;

        case R_X86_64_GOT32:
        case R_X86_64_GOT64:
        case R_X86_64_GOTPC32:
        case R_X86_64_GOTPC64:
        case R_X86_64_GOTOFF64:
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_TLSGD:
        case R_X86_64_TLSLD:
        case R_X86_64_DTPOFF32:
        case R_X86_64_TPOFF32:
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64:
        case R_X86_64_REX_GOTPCRELX:
        case R_X86_64_PLT32:
        case R_X86_64_PLTOFF64:
            return ALWAYS_GOTPLT_ENTRY;
    }

    return -1;
}

uint x86_64_create_plt_entry(elf_context *l, uint got_offset, sym_attr_t *attr) {
    section_t *plt = l->plt;

    int modrm = 0x25;
    if (plt->data_count == 0) {
        uint8_t *p = elf_section_data_add_ptr(plt, 16);
        p[0] = 0xff; // pushl got + PTR_SIZE
        p[1] = modrm + 0x10;
        write32le(p + 2, 8);
        p[6] = 0xff;
        p[7] = modrm;
        write32le(p + 8, X86_64_PTR_SIZE * 2);
    }
    uint plt_offset = plt->data_count;
    uint8_t plt_rel_offset = plt->relocate ? plt->relocate->data_count : 0;

    uint8_t *p = elf_section_data_add_ptr(plt, 16);
    p[0] = 0xff; /* jmp *(got + x) */
    p[1] = modrm;
    write32le(p + 2, got_offset);
    p[6] = 0x68; /* push $xxx */
    /* On x86-64, the relocation is referred to by _index_ */
    write32le(p + 7, plt_rel_offset / sizeof(Elf64_Rela) - 1);
    p[11] = 0xe9; /* jmp plt_start */
    write32le(p + 12, -(plt->data_count));
    return plt_offset;
}

int8_t x86_64_is_code_relocate(uint relocate_type) {
    switch (relocate_type) {
        case R_X86_64_32:
        case R_X86_64_32S:
        case R_X86_64_64:
        case R_X86_64_GOTPC32:
        case R_X86_64_GOTPC64:
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX:
        case R_X86_64_GOTTPOFF:
        case R_X86_64_GOT32:
        case R_X86_64_GOT64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_COPY:
        case R_X86_64_RELATIVE:
        case R_X86_64_GOTOFF64:
        case R_X86_64_TLSGD:
        case R_X86_64_TLSLD:
        case R_X86_64_DTPOFF32:
        case R_X86_64_TPOFF32:
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64:
            return 0;

        case R_X86_64_PC32:
        case R_X86_64_PC64:
        case R_X86_64_PLT32:
        case R_X86_64_PLTOFF64:
        case R_X86_64_JUMP_SLOT:
            return 1;
    }
    return -1;
}

void x86_64_relocate(elf_context *l, Elf64_Rela *rel, int type, uint8_t *ptr, uint64_t addr, uint64_t val) {
    int sym_index = ELF64_R_SYM(rel->r_info);
    switch (type) {
        case R_X86_64_64:
            // 应该是绝对地址定位，所以直接写入符号的绝对位置即可
            add64le(ptr, val);
            break;
        case R_X86_64_32:
        case R_X86_64_32S:
            add32le(ptr, val);
            break;

        case R_X86_64_PC32:
            goto plt32pc32;

        case R_X86_64_PLT32:
            /* fallthrough: val already holds the PLT slot address */

        plt32pc32:
        {
            // 相对地址计算，
            // addr 保存了符号的使用位置（加载到虚拟内存中的位置）
            // val 保存了符号的定义的位置（加载到虚拟内存中的位置）
            // ptr 真正的段数据,存储在编译时内存中，相对地址修正的填充点
            int64_t diff;
            diff = (int64_t) (val - addr);
            if (diff < -2147483648LL || diff > 2147483647LL) {
                error_exit("[x86_64_relocate]internal error: relocation failed");

            }
            // 小端写入
            add32le(ptr, diff);
        }
            break;

        case R_X86_64_COPY:
            break;

        case R_X86_64_PLTOFF64:
            add64le(ptr, val - l->got->sh_addr + rel->r_addend);
            break;

        case R_X86_64_PC64:
            add64le(ptr, val - addr);
            break;

        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            /* They don't need addend */ // 还有不少这种方式的重定位，got 表的重定位应该都来了？
            write64le(ptr, val - rel->r_addend);
            break;
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX:
            add32le(ptr, l->got->sh_addr - addr +
                         elf_get_sym_attr(l, sym_index, 0)->got_offset - 4);
            break;
        case R_X86_64_GOTPC32:
            add32le(ptr, l->got->sh_addr - addr + rel->r_addend);
            break;
        case R_X86_64_GOTPC64:
            add64le(ptr, l->got->sh_addr - addr + rel->r_addend);
            break;
        case R_X86_64_GOTTPOFF:
            add32le(ptr, val - l->got->sh_addr);
            break;
        case R_X86_64_GOT32:
            /* we load the got offset */
            add32le(ptr, elf_get_sym_attr(l, sym_index, 0)->got_offset);
            break;
        case R_X86_64_GOT64:
            /* we load the got offset */
            add64le(ptr, elf_get_sym_attr(l, sym_index, 0)->got_offset);
            break;
        case R_X86_64_GOTOFF64:
            add64le(ptr, val - l->got->sh_addr);
            break;
        case R_X86_64_TLSGD: {
            static const unsigned char expect[] = {
                    /* .byte 0x66; lea 0(%rip),%rdi */
                    0x66, 0x48, 0x8d, 0x3d, 0x00, 0x00, 0x00, 0x00,
                    /* .word 0x6666; rex64; call __tls_get_addr@PLT */
                    0x66, 0x66, 0x48, 0xe8, 0x00, 0x00, 0x00, 0x00};
            static const unsigned char replace[] = {
                    /* mov %fs:0,%rax */
                    0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
                    /* lea -4(%rax),%rax */
                    0x48, 0x8d, 0x80, 0x00, 0x00, 0x00, 0x00};

            if (memcmp(ptr - 4, expect, sizeof(expect)) == 0) {
                Elf64_Sym *sym;
                section_t *section;
                int32_t x;

                memcpy(ptr - 4, replace, sizeof(replace));
                rel[1].r_info = ELF64_R_INFO(0, R_X86_64_NONE);
                sym = &((Elf64_Sym *) l->symtab_section->data)[sym_index];
                section = SEC_TACK(sym->st_shndx);
                x = sym->st_value - section->sh_addr - section->data_count;
                add32le(ptr + 8, x);
            } else {
                error_exit("[x86_64_relocate]unexpected R_X86_64_TLSGD pattern");
            }

            break;
        }
        case R_X86_64_TLSLD: {
            static const unsigned char expect[] = {
                    /* lea 0(%rip),%rdi */
                    0x48, 0x8d, 0x3d, 0x00, 0x00, 0x00, 0x00,
                    /* call __tls_get_addr@PLT */
                    0xe8, 0x00, 0x00, 0x00, 0x00};
            static const unsigned char replace[] = {
                    /* data16 data16 data16 mov %fs:0,%rax */
                    0x66, 0x66, 0x66, 0x64, 0x48, 0x8b, 0x04, 0x25,
                    0x00, 0x00, 0x00, 0x00};

            if (memcmp(ptr - 3, expect, sizeof(expect)) == 0) {
                memcpy(ptr - 3, replace, sizeof(replace));
                rel[1].r_info = ELF64_R_INFO(0, R_X86_64_NONE);
            } else {
                error_exit("[x86_64_relocate] unexpected R_X86_64_TLSLD pattern");
            }

            break;
        }
        case R_X86_64_DTPOFF32:
        case R_X86_64_TPOFF32: {
            Elf64_Sym *sym = &((Elf64_Sym *) l->symtab_section->data)[sym_index];
            section_t *s = SEC_TACK(sym->st_shndx);
            int32_t x;

            x = val - s->sh_addr - s->data_count;
            add32le(ptr, x);
            break;
        }
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64: {
            Elf64_Sym *sym = &((Elf64_Sym *) l->symtab_section->data)[sym_index];
            section_t *s = SEC_TACK(sym->st_shndx);
            int32_t x;

            x = val - s->sh_addr - s->data_count;
            add64le(ptr, x);
            break;
        }
        case R_X86_64_NONE:
            break;
        case R_X86_64_RELATIVE:
            /* do nothing */
            break;
        default:
            error_exit("[x86_64_relocate] unknown rel type");
            break;
    }
}

void x86_64_opcode_encodings(elf_context *ctx, slice_t *opcodes) {
    if (opcodes->count == 0) {
        return;
    }

    slice_t *build_temps = slice_new();

    uint64_t section_offset = 0; // text section offset
    // 一次遍历
    for (int i = 0; i < opcodes->count; ++i) {
        x86_64_opcode_t *opcode = opcodes->take[i];
        x86_64_build_temp_t *temp = build_temp_new(opcode);
        slice_push(build_temps, temp);

        *temp->offset = section_offset;

        // 定义符号
        if (str_equal(opcode->name, "label")) {
            // 解析符号值，并添加到符号表
            x86_64_operand_symbol_t *s = opcode->operands[0]->value;
            // 之前的指令由于找不到相应的符号，所以暂时使用了 rel32 来填充
            // 一旦发现定义点,就需要反推
            x86_64_confirm_rel(ctx->symtab_section, build_temps, &section_offset, s->name);

            // confirm_rel32 可能会修改 section_offset， 所以需要重新计算
            *temp->offset = section_offset;
            Elf64_Sym sym = {
                    .st_shndx = ctx->text_section->sh_index,
                    .st_size = 0,
                    .st_info = ELF64_ST_INFO(!s->is_local, STT_FUNC),
                    .st_other = 0,
                    .st_value = *temp->offset,
            };
            uint64_t sym_index = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &sym, s->name);
            temp->sym_index = sym_index;
            continue;
        }

        x86_64_operand_t *rel_operand = has_symbol_operand(opcode);
        if (rel_operand != NULL) {
            // 指令引用了符号，符号可能是数据符号的引用，也可能是标签符号的引用
            // 1. 数据符号引用(直接改写成 0x0(rip))
            // 2. 标签符号引用(在符号表中,表明为内部符号,否则使用 rel32 先占位)
            x86_64_operand_symbol_t *symbol_operand = rel_operand->value;
            // 判断是否为标签符号引用, 比如 call symbol call
            if (is_call_opcode(opcode->name) || is_jmp_opcode(opcode->name)) {
                // 标签符号
                uint64_t sym_index = (uint64_t) table_get(ctx->symtab_hash, symbol_operand->name);
                if (sym_index > 0) {
                    // 引用了已经存在的符号，直接计算相对位置即可
                    Elf64_Sym sym = ((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
                    int rel_diff = sym.st_value - section_offset;
                    x86_64_rewrite_symbol(opcode, rel_operand, rel_diff);
                } else {
                    // 引用了 label 符号，但是符号确不在符号表中
                    // 此时使用 rel32 占位，其中 jmp 指令后续可能需要替换 rel8
                    x86_64_rewrite_symbol(opcode, rel_operand, 0);
                    temp->rel_operand = rel_operand; // 等到二次遍历时再确认是否需要改写
                    temp->rel_symbol = symbol_operand->name;
                    uint8_t reduce_count = jmp_rewrite_rel8_reduce_count(opcode);
                    if (reduce_count > 0) {
                        temp->may_need_reduce = true;
                        temp->reduce_count = reduce_count;
                    }
                }
            } else {
                // TODO 可以通过读取全局符号表判断符号是 fn 还是 var
                // 其他指令引用了符号，由于不用考虑指令重写的问题,所以直接写入 0(%rip) 让重定位阶段去找改符号进行重定位即可
                // 完全不用考虑是标签符号还是数据符号
                // 添加到重定位表(.rela.text)
                // 根据指令计算 offset 重定位 rip offset
                uint64_t sym_index = (uint64_t) table_get(ctx->symtab_hash, symbol_operand->name);
                if (sym_index == 0) {
                    Elf64_Sym sym = {
                            .st_shndx = 0,
                            .st_size = 0,
                            .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                            .st_other = 0,
                            .st_value = 0,
                    };
                    sym_index = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &sym, symbol_operand->name);
                }

                uint64_t rel_offset = *temp->offset += opcode_rip_offset(opcode);
                temp->elf_rel = elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                                                 rel_offset, R_X86_64_PC32, (int) sym_index, -4);
            }
        }

        // 编码
        temp->data = x86_64_opcode_encoding(*opcode, &temp->data_count);
        section_offset += temp->data_count;
    }

    // 二次遍历,基于 build_temps
    for (int i = 0; i < build_temps->count; ++i) {
        x86_64_build_temp_t *temp = build_temps->take[i];
        if (!temp->rel_symbol) {
            continue;
        }

        // 二次扫描时再符号表中找到了符号
        // rel_symbol 仅记录了 label, 数据符号直接使用 rip 寻址
        uint64_t sym_index = (uint64_t) table_get(ctx->symtab_hash, temp->rel_symbol);
        if (sym_index == 0) {
            Elf64_Sym sym = {
                    .st_shndx = 0,
                    .st_size = 0,
                    .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                    .st_other = 0,
                    .st_value = 0,
            };
            sym_index = elf_put_sym(ctx->symtab_section, ctx->symtab_hash, &sym, temp->rel_symbol);
        }

        Elf64_Sym *sym = &((Elf64_Sym *) ctx->symtab_section->data)[sym_index];
        if (sym->st_value > 0) {
            int rel_diff = sym->st_value - *temp->offset;
            x86_64_rewrite_symbol(temp->opcode, temp->rel_operand, rel_diff); // 仅仅重写了符号，长度不会再变了，
            temp->data = x86_64_opcode_encoding(*temp->opcode, &temp->data_count);
        } else {
            // 外部符号添加重定位信息
            uint64_t rel_offset = *temp->offset += opcode_rip_offset(temp->opcode);
            temp->elf_rel = elf_put_relocate(ctx, ctx->symtab_section, ctx->text_section,
                                             rel_offset, R_X86_64_PC32, (int) sym_index, -4);
        }

    }

    // 代码段已经确定，生成 text 数据
    for (int i = 0; i < build_temps->count; ++i) {
        x86_64_build_temp_t *temp = build_temps->take[i];
        elf_put_opcode_data(ctx->text_section, temp->data, temp->data_count);
    }
}
