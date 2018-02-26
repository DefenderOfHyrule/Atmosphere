#include "utils.h"
#include "mmu.h"
#include "memory_map.h"

/*
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void _init(void);

extern uint8_t __warmboot_crt0_start__[], __warmboot_crt0_end__[], __warmboot_crt0_lma__[];
extern uint8_t __main_start__[], __main_end__[], __main_lma__[];
extern uint8_t __pk2ldr_start__[], __pk2ldr_end__[], __pk2ldr_lma__[];
extern uint8_t __vectors_start__[], __vectors_end__[], __vectors_lma__[];*/
extern void flush_dcache_all_tzram_pa(void);
extern void invalidate_icache_all_tzram_pa(void);

uintptr_t get_coldboot_crt0_stack_address(void);

static void identity_map_all_mappings(uintptr_t *mmu_l1_tbl, uintptr_t *mmu_l3_tbl) {
    static const uintptr_t addrs[]      =   { TUPLE_FOLD_LEFT_0(EVAL(IDENTIY_MAPPING_ID_MAX), _MMAPID, COMMA) };
    static const size_t sizes[]         =   { TUPLE_FOLD_LEFT_1(EVAL(IDENTIY_MAPPING_ID_MAX), _MMAPID, COMMA) };
    static const uint64_t attribs[]     =   { TUPLE_FOLD_LEFT_2(EVAL(IDENTIY_MAPPING_ID_MAX), _MMAPID, COMMA) };
    static const uint64_t is_block[]    =   { TUPLE_FOLD_LEFT_3(EVAL(IDENTIY_MAPPING_ID_MAX), _MMAPID, COMMA) };

    for(size_t i = 0; i < IDENTIY_MAPPING_ID_MAX; i++) {
        identity_map_mapping(mmu_l1_tbl, mmu_l3_tbl, addrs[i], sizes[i], attribs[i], is_block[i]);
    }
}

static void mmio_map_all_devices(uintptr_t *mmu_l3_tbl) {
    static const uintptr_t pas[]        =   { TUPLE_FOLD_LEFT_0(EVAL(MMIO_DEVID_MAX), _MMAPDEV, COMMA) };
    static const size_t sizes[]         =   { TUPLE_FOLD_LEFT_1(EVAL(MMIO_DEVID_MAX), _MMAPDEV, COMMA) };
    static const bool is_secure[]       =   { TUPLE_FOLD_LEFT_2(EVAL(MMIO_DEVID_MAX), _MMAPDEV, COMMA) };

    for(size_t i = 0, offset = 0; i < MMIO_DEVID_MAX; i++) {
        mmio_map_device(mmu_l3_tbl, MMIO_BASE + offset, pas[i], sizes[i], is_secure[i]);
        offset += sizes[i];
        offset += 0x1000;
    }
}

static void lp0_entry_map_all_ram_segments(uintptr_t *mmu_l3_tbl) {
    static const uintptr_t pas[]        =   { TUPLE_FOLD_LEFT_0(EVAL(LP0_ENTRY_RAM_SEGMENT_ID_MAX), _MMAPLP0ES, COMMA) };
    static const size_t sizes[]         =   { TUPLE_FOLD_LEFT_1(EVAL(LP0_ENTRY_RAM_SEGMENT_ID_MAX), _MMAPLP0ES, COMMA) };
    static const uint64_t attribs[]     =   { TUPLE_FOLD_LEFT_2(EVAL(LP0_ENTRY_RAM_SEGMENT_ID_MAX), _MMAPLP0ES, COMMA) };

    for(size_t i = 0, offset = 0; i < LP0_ENTRY_RAM_SEGMENT_ID_MAX; i++) {
        lp0_entry_map_ram_segment(mmu_l3_tbl, LP0_ENTRY_RAM_SEGMENT_BASE + offset, pas[i], sizes[i], attribs[i]);
        offset += 0x10000;
    }
}

static void warmboot_map_all_ram_segments(uintptr_t *mmu_l3_tbl) {
    static const uintptr_t pas[]        =   { TUPLE_FOLD_LEFT_0(EVAL(WARMBOOT_RAM_SEGMENT_ID_MAX), _MMAPWBS, COMMA) };
    static const size_t sizes[]         =   { TUPLE_FOLD_LEFT_1(EVAL(WARMBOOT_RAM_SEGMENT_ID_MAX), _MMAPWBS, COMMA) };
    static const uint64_t attribs[]     =   { TUPLE_FOLD_LEFT_2(EVAL(WARMBOOT_RAM_SEGMENT_ID_MAX), _MMAPWBS, COMMA) };

    for(size_t i = 0, offset = 0; i < WARMBOOT_RAM_SEGMENT_ID_MAX; i++) {
        warmboot_map_ram_segment(mmu_l3_tbl, WARMBOOT_RAM_SEGMENT_BASE + offset, pas[i], sizes[i], attribs[i]);
        offset += sizes[i];
    }
}

static void tzram_map_all_segments(uintptr_t *mmu_l3_tbl) {
    static const uintptr_t offs[]       =   { TUPLE_FOLD_LEFT_0(EVAL(TZRAM_SEGMENT_ID_MAX), _MMAPTZS, COMMA) };
    static const size_t sizes[]         =   { TUPLE_FOLD_LEFT_1(EVAL(TZRAM_SEGMENT_ID_MAX), _MMAPTZS, COMMA) };
    static const size_t increments[]    =   { TUPLE_FOLD_LEFT_2(EVAL(TZRAM_SEGMENT_ID_MAX), _MMAPTZS, COMMA) };
    static const bool is_executable[]   =   { TUPLE_FOLD_LEFT_3(EVAL(TZRAM_SEGMENT_ID_MAX), _MMAPTZS, COMMA) };

    for(size_t i = 0, offset = 0; i < TZRAM_SEGMENT_ID_MAX; i++) {
        tzram_map_segment(mmu_l3_tbl, TZRAM_SEGMENT_BASE, 0x7C010000ull + offs[i], sizes[i], is_executable[i]);
        offset += increments[i];
    }
}

static void configure_ttbls(void) {
    uintptr_t *mmu_l1_tbl = (uintptr_t *)(TZRAM_GET_SEGMENT_PA(TZRAM_SEGEMENT_ID_SECMON_EVT) + 0x800 - 64);
    uintptr_t *mmu_l2_tbl = (uintptr_t *)TZRAM_GET_SEGMENT_PA(TZRAM_SEGMENT_ID_L2_TRANSLATION_TABLE);
    uintptr_t *mmu_l3_tbl = (uintptr_t *)TZRAM_GET_SEGMENT_PA(TZRAM_SEGMENT_ID_L3_TRANSLATION_TABLE);

    mmu_init_table(mmu_l1_tbl, 64); /* 33-bit address space */
    mmu_init_table(mmu_l2_tbl, 4096);
    /*
        Nintendo uses the same L3 table for everything, but they make sure
        nothing clashes.
    */
    mmu_init_table(mmu_l3_tbl, 4096);

    mmu_map_table(1, mmu_l1_tbl, 0x40000000, mmu_l2_tbl, 0);
    mmu_map_table(1, mmu_l1_tbl, 0x1C0000000, mmu_l2_tbl, 0);

    mmu_map_table(2, mmu_l2_tbl, 0x40000000, mmu_l3_tbl, 0);
    mmu_map_table(2, mmu_l2_tbl, 0x7C000000, mmu_l3_tbl, 0);
    mmu_map_table(2, mmu_l2_tbl, 0x1F0000000ull, mmu_l3_tbl, 0);

    identity_map_all_mappings(mmu_l1_tbl, mmu_l3_tbl);
    mmio_map_all_devices(mmu_l3_tbl);
    lp0_entry_map_all_ram_segments(mmu_l3_tbl);
    warmboot_map_all_ram_segments(mmu_l3_tbl);
    tzram_map_all_segments(mmu_l3_tbl);
}

#if 0

static void copy_lma_to_vma(unsigned int segment_id, void *lma, size_t size, bool vma_is_pa) {
    uintptr_t vma = vma_is_pa ? TZRAM_GET_SEGMENT_PA(segment_id) : TZRAM_GET_SEGMENT_ADDRESS(segment_id);
    uintptr_t vma_offset = (uintptr_t)lma & 0xFFF;
    uint64_t *p_vma = (uint64_t *)vma;
    uint64_t *p_lma = (uint64_t *)lma;
    for (size_t i = 0; i < size / 8; i++) {
        p_vma[vma_offset / 8 + i] = p_lma[i];
    }
}

static void __libc_init_array(void) {
    for (size_t i = 0; i < __preinit_array_end - __preinit_array_start; i++)
        __preinit_array_start[i]();
    _init(); /* FIXME: do we have this gcc-provided symbol if we build with -nostartfiles? */
    for (size_t i = 0; i < __init_array_end - __init_array_start; i++)
        __init_array_start[i]();
}
#endif

uintptr_t get_coldboot_crt0_stack_address(void) {
    return TZRAM_GET_SEGMENT_PA(TZRAM_SEGMENT_ID_CORE3_STACK) + 0x800;
}

void coldboot_init(void) {
    /* TODO: Set NX BOOTLOADER clock time field */
    /*copy_lma_to_vma(TZRAM_SEGMENT_ID_WARMBOOT_CRT0_AND_MAIN, __warmboot_crt0_lma__, __warmboot_crt0_end__ - __warmboot_crt0_start__, true);*/
    /* TODO: set some mmio regs, etc. */
    /* TODO: initialize DMA controllers */
    configure_ttbls();
    /*copy_lma_to_vma(TZRAM_SEGMENT_ID_WARMBOOT_CRT0_AND_MAIN, __main_lma__, __main_end__ - __main_start__, false);
    copy_lma_to_vma(TZRAM_SEGMENT_ID_PK2LDR, __pk2ldr_lma__, __pk2ldr_end__ - __pk2ldr_start__, false);
    copy_lma_to_vma(TZRAM_SEGEMENT_ID_SECMON_EVT, __vectors_lma__, __vectors_end__ - __vectors_start__, false);*/
    /* TODO: set the MMU regs & tlbi & enable MMU */

    flush_dcache_all_tzram_pa();
    invalidate_icache_all_tzram_pa();
    /* TODO: zero-initialize the cpu context */
    /* Nintendo clears the (emtpy) pk2ldr's BSS section, but we embed it 0-filled in the binary */
    /*__libc_init_array();  construct global objects */
}
