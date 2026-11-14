#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <elf.h>
#include "so_util.h"

#define EM_AARCH64 183
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

so_module *so_list = NULL;

static uint8_t *g_tramp_pool = NULL;
static int g_tramp_used = 0;

void so_set_tramp_pool(void *at) {
  if (!at) return;
  g_tramp_pool = (uint8_t *)at;
  g_tramp_used = 0;
}

void hook_arm64(uintptr_t addr, uintptr_t dst) {
    if (addr == 0) return;

    if (!g_tramp_pool) {
        g_tramp_pool = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (!g_tramp_pool || g_tramp_pool == (uint8_t *)MAP_FAILED)
        return;

    uint32_t *slot = (uint32_t *)(g_tramp_pool + (size_t)g_tramp_used * 16);
    g_tramp_used++;
    slot[0] = 0x58000051u;          // LDR X17, #8
    slot[1] = 0xd61f0220u;          // BR X17
    *(uint64_t *)(slot + 2) = dst;  // 64-bit target address
    __builtin___clear_cache((char *)slot, (char *)slot + 16);

    uint32_t *hook = (uint32_t *)addr;
    intptr_t off = (intptr_t)((uintptr_t)slot - addr);
    hook[0] = 0x14000000u | (((uint32_t)(off >> 2)) & 0x03FFFFFFu);
    __builtin___clear_cache((char *)hook, (char *)hook + 4);
}

void so_flush_caches(so_module *mod) {
    __builtin___clear_cache((char *)mod->load_virtbase, (char *)mod->load_virtbase + mod->load_size);
}

void so_free_temp(so_module *mod) {
    if (mod->so_base) {
        free(mod->so_base);
        mod->so_base = NULL;
    }
}

void so_finalize(so_module *mod) {
    // Keep mmap's RWX permissions intact
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
    memset(mod, 0, sizeof(*mod));
    strncpy(mod->name, filename, sizeof(mod->name) - 1);

    FILE *fd = fopen(filename, "rb");
    if (!fd) return -1;
    fseek(fd, 0, SEEK_END);
    mod->so_size = ftell(fd);
    fseek(fd, 0, SEEK_SET);

    mod->so_base = malloc(mod->so_size);
    fread(mod->so_base, mod->so_size, 1, fd);
    fclose(fd);

    mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
    if (mod->elf_hdr->e_machine != EM_AARCH64) return -1;

    mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
    mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
    mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

    mod->phnum = mod->elf_hdr->e_phnum;
    memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

    for (int i = 0; i < mod->phnum; i++) {
        if (mod->prog_hdr[i].p_type == PT_LOAD) {
            size_t end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
            if (end > mod->load_size) mod->load_size = end;
        }
    }
    mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
    mod->load_base = base;
    mod->load_virtbase = base;
    memset(mod->load_base, 0, mod->load_size);

    for (int i = 0; i < mod->phnum; i++) {
        Elf64_Phdr *p = &mod->prog_hdr[i];
        if (p->p_type == PT_LOAD) {
            memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
                   (void *)((uintptr_t)mod->so_base + p->p_offset), p->p_filesz);
        }
        p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
    }

    for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
        char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
        if (!strcmp(sh_name, ".dynsym")) {
            mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
            mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
        } else if (!strcmp(sh_name, ".dynstr")) {
            mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
        }
    }

    if (!so_list) so_list = mod;
    else {
        so_module *m = so_list;
        while (m->next) m = m->next;
        m->next = mod;
    }
    return 0;
}

static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
    for (int i = 0; i < mod->phnum; i++) {
        if (mod->phdr[i].p_type == PT_DYNAMIC) {
            const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
            for (; dyn->d_tag != DT_NULL; dyn++)
                if (dyn->d_tag == tag) return dyn->d_un.d_val;
        }
    }
    return 0;
}

static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
    uintptr_t where = 0;
    size_t count = relrsz / sizeof(Elf64_Xword);
    for (size_t i = 0; i < count; i++) {
        Elf64_Xword entry = relr[i];
        if ((entry & 1) == 0) {
            where = (uintptr_t)entry;
            *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
            where += 8;
        } else {
            for (int bit = 1; bit < 64; bit++) {
                if (entry & (1ull << bit))
                    *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
            }
            where += 63 * 8;
        }
    }
}

static void relocate_rela_block(so_module *mod, Elf64_Rela *rels, int count) {
    for (int j = 0; j < count; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);

        if (type == 1027 /*R_AARCH64_RELATIVE*/) {
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
        } else if (type == 257 /*R_AARCH64_ABS64*/) {
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            else
                *ptr = rels[j].r_addend;
        } else if (type == 1025 /*R_AARCH64_GLOB_DAT*/ || type == 1026 /*R_AARCH64_JUMP_SLOT*/) {
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
        }
    }
}

int so_relocate(so_module *mod) {
    // 1. Relocate via section headers
    for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
        char *sh_name = mod->shstrtab ? (mod->shstrtab + mod->sec_hdr[i].sh_name) : "";
        if (!strcmp(sh_name, ".rela.dyn") || !strcmp(sh_name, ".rela.plt")) {
            Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
            relocate_rela_block(mod, rels, mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela));
        }
    }

    // 2. Relocate via PT_DYNAMIC (DT_RELA)
    Elf64_Xword rela_off = so_dynamic_tag(mod, DT_RELA);
    Elf64_Xword rela_size = so_dynamic_tag(mod, DT_RELASZ);
    Elf64_Xword rela_ent = so_dynamic_tag(mod, DT_RELAENT);
    if (rela_ent == 0) rela_ent = sizeof(Elf64_Rela);
    if (rela_off && rela_size) {
        Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + rela_off);
        relocate_rela_block(mod, rels, rela_size / rela_ent);
    }

    // 3. Process RELR / ANDROID_RELR
    Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
    Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
    if (!relr_off) {
        relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
        relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
    }
    if (relr_off && relr_size)
        so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);

    return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
    for (int i = 0; i < mod->num_syms; i++) {
        if (mod->syms[i].st_shndx == SHN_UNDEF || ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL) continue;
        if (!strcmp(mod->dynstrtab + mod->syms[i].st_name, name))
            return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
    }
    return 0;
}

static void resolve_rela_block(so_module *mod, Elf64_Rela *rels, int count, DynLibFunction *funcs, int num_funcs, int taint_missing_imports, void *h_libc, void *h_libm, int *missing_count) {
    for (int j = 0; j < count; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);

        if (sym->st_shndx == SHN_UNDEF && (type == 257 || type == 1025 || type == 1026)) {
            char *name = mod->dynstrtab + sym->st_name;
            uintptr_t addr = 0;
            for (int k = 0; k < num_funcs; k++) {
                if (!strcmp(name, funcs[k].symbol)) {
                    addr = funcs[k].func; break;
                }
            }
            if (!addr) {
                for (so_module *m = so_list; m; m = m->next) {
                    if (m != mod && (addr = so_lookup_export(m, name))) break;
                }
            }
            if (!addr) addr = (uintptr_t)dlsym(RTLD_DEFAULT, name);
            if (!addr && h_libc) addr = (uintptr_t)dlsym(h_libc, name);
            if (!addr && h_libm) addr = (uintptr_t)dlsym(h_libm, name);

            if (addr) {
                *ptr = addr + rels[j].r_addend;
            } else {
                fprintf(stderr, "[so_util] UNRESOLVED IMPORT: '%s' (GOT offset 0x%lx)\n", name, (unsigned long)rels[j].r_offset);
                (*missing_count)++;
                if (taint_missing_imports) *ptr = rels[j].r_offset;
            }
        }
    }
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
    static void *h_libc = NULL;
    static void *h_libm = NULL;
    if (!h_libc) h_libc = dlopen("libc.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!h_libm) h_libm = dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);

    int missing_count = 0;

    for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
        char *sh_name = mod->shstrtab ? (mod->shstrtab + mod->sec_hdr[i].sh_name) : "";
        if (!strcmp(sh_name, ".rela.dyn") || !strcmp(sh_name, ".rela.plt")) {
            Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
            resolve_rela_block(mod, rels, mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela), funcs, num_funcs, taint_missing_imports, h_libc, h_libm, &missing_count);
        }
    }

    Elf64_Xword jmprel = so_dynamic_tag(mod, DT_JMPREL);
    Elf64_Xword pltrelsz = so_dynamic_tag(mod, DT_PLTRELSZ);
    if (jmprel && pltrelsz) {
        Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + jmprel);
        resolve_rela_block(mod, rels, pltrelsz / sizeof(Elf64_Rela), funcs, num_funcs, taint_missing_imports, h_libc, h_libm, &missing_count);
    }

    if (missing_count > 0) {
        fprintf(stderr, "[so_util] WARNING: %s has %d unresolved imports!\n", mod->name, missing_count);
    }
    return 0;
}

char *so_find_nearest_symbol(so_module *mod, uintptr_t virt_addr) {
    if (!mod->syms || mod->num_syms <= 0 || !mod->dynstrtab) return NULL;
    uintptr_t offset = virt_addr - (uintptr_t)mod->load_virtbase;
    uintptr_t best_diff = (uintptr_t)-1;
    int best_idx = -1;
    for (int i = 0; i < mod->num_syms; i++) {
        if (mod->syms[i].st_shndx == SHN_UNDEF || mod->syms[i].st_value == 0 || mod->syms[i].st_name == 0)
            continue;
        uintptr_t val = mod->syms[i].st_value;
        if (offset >= val) {
            uintptr_t diff = offset - val;
            if (diff < best_diff) {
                best_diff = diff;
                best_idx = i;
            }
        }
    }
    if (best_idx >= 0)
        return mod->dynstrtab + mod->syms[best_idx].st_name;
    return NULL;
}

void so_execute_init_array(so_module *mod) {
    for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
        if (!strcmp(mod->shstrtab + mod->sec_hdr[i].sh_name, ".init_array")) {
            int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
            int count = mod->sec_hdr[i].sh_size / 8;
            fprintf(stderr, "[so_util] Executing %d init_array constructors for %s...\n", count, mod->name);
            fflush(stderr);
            for (int j = 0; j < count; j++) {
                if (init_array[j]) {
                    char *sym_name = so_find_nearest_symbol(mod, (uintptr_t)init_array[j]);
                    fprintf(stderr, "[so_util]   [%s] constructor %d/%d at %p (%s)...\n",
                            mod->name, j + 1, count, (void *)init_array[j], sym_name ? sym_name : "?");
                    fflush(stderr);
                    init_array[j]();
                }
            }
        }
    }
}

uintptr_t so_find_addr_safe(so_module *mod, const char *symbol) {
    for (int i = 0; i < mod->num_syms; i++) {
        if (!strcmp(mod->dynstrtab + mod->syms[i].st_name, symbol))
            return (uintptr_t)mod->load_base + mod->syms[i].st_value;
    }
    return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
    for (int i = 0; i < mod->num_syms; i++) {
        if (!strcmp(mod->dynstrtab + mod->syms[i].st_name, symbol))
            return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
    }
    return 0;
}

struct dl_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long int dlpi_adds;
    unsigned long long int dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
    int ret = 0;
    for (so_module *mod = so_list; mod; mod = mod->next) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
        info.dlpi_name = mod->name;
        info.dlpi_phdr = mod->phdr;
        info.dlpi_phnum = mod->phnum;
        ret = callback(&info, sizeof(info), data);
        if (ret) break;
    }
    return ret;
}
