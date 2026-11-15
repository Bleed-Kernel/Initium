#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/libc.h>
#include <lib/acpi.h>
#include <sys/cpu.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <sys/smp.h>
#include <sys/lapic.h>
#include <sys/gdt.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mtrr.h>
#define LIMINE_NO_POINTERS
#include <limine.h>

extern symbol smp_trampoline_start;
extern size_t smp_trampoline_size;



struct trampoline_passed_info {
    uint8_t  smp_tpl_booted_flag;
    uint8_t  smp_tpl_target_mode;
    uint32_t smp_tpl_pagemap;
    uint32_t smp_tpl_info_struct;
    struct gdtr smp_tpl_gdt;
    uint64_t smp_tpl_hhdm;
    uint64_t smp_tpl_bsp_apic_addr_msr;
    uint64_t smp_tpl_mtrr_restore;
    uint64_t smp_tpl_temp_stack;
    uint64_t smp_tpl_lapic_setup;
} __attribute__((packed));

bool smp_configure_apic = false;

static bool smp_start_ap(uint32_t lapic_id, struct gdtr *gdtr,
                         struct limine_mp_info *info_struct,
                         int paging_mode, uint32_t pagemap,
                         bool x2apic, bool nx, uint64_t hhdm, bool wp,
                         bool ext_apic_id) {
    // Prepare the trampoline
    static void *trampoline = NULL;
    if (trampoline == NULL) {
        trampoline = conv_mem_alloc(smp_trampoline_size);

        memcpy(trampoline, smp_trampoline_start, smp_trampoline_size);
    }

    static void *temp_stack = NULL;
    if (temp_stack == NULL) {
        temp_stack = ext_mem_alloc(8192);
    }

    static struct trampoline_passed_info *passed_info = NULL;
    if (passed_info == NULL) {
        passed_info = (void *)(((uintptr_t)trampoline + smp_trampoline_size)
                               - sizeof(struct trampoline_passed_info));
    }

    passed_info->smp_tpl_info_struct = (uint32_t)(uintptr_t)info_struct;
    passed_info->smp_tpl_booted_flag = 0;
    passed_info->smp_tpl_pagemap     = pagemap;
    passed_info->smp_tpl_target_mode = ((uint32_t)(paging_mode == PAGING_MODE_X86_64_5LVL) << 1)
                                     | ((uint32_t)nx << 3)
                                     | ((uint32_t)wp << 4)
                                     | ((uint32_t)ext_apic_id << 5);
    passed_info->smp_tpl_gdt = *gdtr;
    passed_info->smp_tpl_hhdm = hhdm;
    passed_info->smp_tpl_bsp_apic_addr_msr = rdmsr(0x1b);
    passed_info->smp_tpl_mtrr_restore = (uint64_t)(uintptr_t)mtrr_restore;
    passed_info->smp_tpl_temp_stack = (uint64_t)(uintptr_t)temp_stack + 8192;
    passed_info->smp_tpl_lapic_setup = smp_configure_apic
        ? (uint64_t)(uintptr_t)lapic_configure_handoff_state : 0;

    asm volatile ("" ::: "memory");

    // Send the INIT IPI
    if (x2apic) {
        x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) | 0x4500);
    } else {
        lapic_icr_wait();
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, 0x4500);
    }
    stall(10000);

    // Send two Startup IPIs per Intel SDM recommendation (Vol 3, 8.4.4.1)
    for (int j = 0; j < 2; j++) {
        if (x2apic) {
            x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) |
                                         ((size_t)trampoline / 4096) | 0x4600);
        } else {
            lapic_icr_wait();
            lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
            lapic_write(LAPIC_REG_ICR0, ((size_t)trampoline / 4096) | 0x4600);
        }
        if (j == 0) {
            stall(200);
        }
    }

    if (!x2apic) {
        lapic_icr_wait();
    }

    for (int i = 0; i < 100; i++) {
        if (locked_read(&passed_info->smp_tpl_booted_flag) == 1) {
            return true;
        }
        stall(10000);
    }

    return false;
}

// A processor described by both structures is one CPU, and the x2APIC one is
// what describes it: its ACPI UID field is 32 bits wide rather than 8.
static bool madt_has_x2apic_entry(struct madt *madt, uint32_t apic_id) {
    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        if (*madt_ptr != 9 || *(madt_ptr + 1) < sizeof(struct madt_x2apic)) {
            continue;
        }

        struct madt_x2apic *x2lapic = (void *)madt_ptr;

        if (x2lapic->x2apic_id == apic_id
         && (x2lapic->flags & MADT_LAPIC_ENABLED)) {
            return true;
        }
    }

    return false;
}

static bool mp_info_has_lapic_id(struct limine_mp_info *mp_info, size_t count,
                                 uint32_t lapic_id) {
    for (size_t i = 0; i < count; i++) {
        if (mp_info[i].lapic_id == lapic_id) {
            return true;
        }
    }

    return false;
}

struct limine_mp_info *init_smp(size_t   *cpu_count,
                                 uint32_t *_bsp_lapic_id,
                                 int       paging_mode,
                                 pagemap_t pagemap,
                                 bool      x2apic,
                                 bool      nx,
                                 uint64_t  hhdm,
                                 bool      wp) {
    if (!lapic_check())
        return NULL;

    // An x2APIC mode x2apic_check() rejects is reverted just as when the
    // kernel does not support x2APIC, unless it cannot be: a kernel that
    // supports it then keeps it.
    bool x2apic_supported = x2apic;
    x2apic = x2apic && x2apic_check();

    // PROTOCOL.md promises this revert whenever the MP request is present, so
    // it precedes the MADT search, which can return without it.
    if (rdmsr(0x1b) & (1 << 10)) {
        if (!x2apic) {
            if (x2apic_disable()) {
                printv("smp: Firmware had x2APIC enabled, reverted to xAPIC mode\n");
            } else if (x2apic_supported) {
                x2apic = true;
            } else {
                panic(false, "smp: Kernel does not support x2APIC and x2APIC cannot be disabled");
            }
        }
    }

    // Search for MADT table
    struct madt *madt = acpi_get_table("APIC", 0);

    if (madt == NULL)
        return NULL;

    struct gdtr gdtr = gdt;

    uint32_t bsp_lapic_id;

    x2apic = x2apic && x2apic_enable();

    if (x2apic) {
        bsp_lapic_id = x2apic_read(LAPIC_REG_ID);
    } else {
        bsp_lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
    }

    *_bsp_lapic_id = bsp_lapic_id;

    *cpu_count = 0;

    // Count the MAX of startable APs and allocate accordingly
    size_t max_cpus = 0;
    uint32_t max_apic_id = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        // Skip zero-length or out-of-bounds MADT entries
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic))
                    continue;

                struct madt_lapic *lapic = (void *)madt_ptr;

                if (lapic->lapic_id != 0xff && lapic->lapic_id > max_apic_id) {
                    max_apic_id = lapic->lapic_id;
                }

                // Check if we can actually try to start the AP
                if (lapic->flags & MADT_LAPIC_ENABLED)
                    max_cpus++;

                continue;
            }
            case 9: {
                // Processor local x2APIC
                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic))
                    continue;

                struct madt_x2apic *x2lapic = (void *)madt_ptr;

                if (!x2apic && x2lapic->x2apic_id >= 0xff) {
                    continue;
                }

                if (x2lapic->x2apic_id > max_apic_id) {
                    max_apic_id = x2lapic->x2apic_id;
                }

                // Check if we can actually try to start the AP
                if (x2lapic->flags & MADT_LAPIC_ENABLED)
                    max_cpus++;

                continue;
            }
        }
    }

    if (max_cpus == 0) {
        return NULL;
    }

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));
    *cpu_count = 0;

    // Without 8-bit IDs, AMD local APICs match the low 4 bits of a physical
    // destination and take 0xf there for a broadcast. AMD PPR 56569 rev.
    // 3.03, APICx410. APs set theirs with paging off, so below 4GiB.
    bool ext_apic_id = !x2apic && max_apic_id >= 0xf && (rdmsr(0x1b) >> 32) == 0
                    && lapic_enable_ext_apic_id();

    // Try to start all APs
    mtrr_save();

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        // Skip zero-length or out-of-bounds MADT entries
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic))
                    continue;

                struct madt_lapic *lapic = (void *)madt_ptr;

                // An entry carrying the broadcast ID describes no CPU, and
                // INIT-SIPI to it would reset every CPU, this one included.
                if (lapic->lapic_id == 0xff) {
                    continue;
                }

                // Check if we can actually try to start the AP
                if (!(lapic->flags & MADT_LAPIC_ENABLED))
                    continue;

                if (madt_has_x2apic_entry(madt, lapic->lapic_id)
                 || mp_info_has_lapic_id(ret, *cpu_count, lapic->lapic_id)) {
                    continue;
                }

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = lapic->acpi_processor_uid;
                info_struct->lapic_id = lapic->lapic_id;

                // Do not try to restart the BSP
                if (lapic->lapic_id == bsp_lapic_id) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: [xAPIC] Found candidate AP for bring-up. LAPIC ID: %u\n", lapic->lapic_id);

                // Set up per-AP LINT values before starting
                if (smp_configure_apic) {
                    lapic_prep_lint(madt, lapic->acpi_processor_uid);
                }

                // Try to start the AP
                if (!smp_start_ap(lapic->lapic_id, &gdtr, info_struct,
                                  paging_mode, (uintptr_t)pagemap.top_level,
                                  x2apic, nx, hhdm, wp, ext_apic_id)) {
                    print("smp: FAILED to bring-up AP\n");
                    continue;
                }

                printv("smp: Successfully brought up AP\n");

                (*cpu_count)++;
                continue;
            }
            case 9: {
                // Processor local x2APIC
                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic))
                    continue;

                struct madt_x2apic *x2lapic = (void *)madt_ptr;

                // The x2APIC broadcast ID, as above
                if (x2lapic->x2apic_id == 0xffffffff) {
                    continue;
                }

                // 0xff is the broadcast destination of an ICR that only
                // carries 8 bits of one, and firmware parks the processors
                // above it. Intel SDM 325462-092, 13.12.8.1.
                if (!x2apic && x2lapic->x2apic_id >= 0xff) {
                    continue;
                }

                // Check if we can actually try to start the AP
                if (!(x2lapic->flags & MADT_LAPIC_ENABLED))
                    continue;

                if (mp_info_has_lapic_id(ret, *cpu_count, x2lapic->x2apic_id)) {
                    continue;
                }

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = x2lapic->acpi_processor_uid;
                info_struct->lapic_id = x2lapic->x2apic_id;

                // Do not try to restart the BSP
                if (x2lapic->x2apic_id == bsp_lapic_id) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: [x2APIC] Found candidate AP for bring-up. LAPIC ID: %u\n", x2lapic->x2apic_id);

                // Set up per-AP LINT values before starting
                if (smp_configure_apic) {
                    lapic_prep_lint(madt, x2lapic->acpi_processor_uid);
                }

                // Try to start the AP
                if (!smp_start_ap(x2lapic->x2apic_id, &gdtr, info_struct,
                                  paging_mode, (uintptr_t)pagemap.top_level,
                                  x2apic, nx, hhdm, wp, ext_apic_id)) {
                    print("smp: FAILED to bring-up AP\n");
                    continue;
                }

                printv("smp: Successfully brought up AP\n");

                (*cpu_count)++;
                continue;
            }
        }
    }

    if (*cpu_count == 0) {
        pmm_free(ret, max_cpus * sizeof(struct limine_mp_info));
        return NULL;
    }

    return ret;
}

