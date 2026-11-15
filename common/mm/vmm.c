#include <stdint.h>
#include <stddef.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <sys/cpu.h>

#define PT_SIZE ((uint64_t)0x1000)

typedef uint64_t pt_entry_t;

// Maps level indexes to the page size for that level.
_Static_assert(VMM_MAX_LEVEL <= 5, "6-level paging not supported");
static uint64_t page_sizes[5] = {
    0x1000,
    0x200000,
    0x40000000,
    0x8000000000,
    0x1000000000000,
};

static pt_entry_t *get_next_level(
    pagemap_t pagemap, pt_entry_t *current_level, uint64_t virt, size_t desired_sz,
    size_t level_idx, size_t entry
);

// Being naturally aligned, only a large page based at 0 can span the fixed
// MTRRs, which give the first MiB of physical memory several memory types.
// Such a page has an undefined type on AMD and costs a split back to 4KiB TLB
// entries on Intel. Intel SDM 325462-092, 14.11.9;
// AMD APM 40332 rev 4.10, Large Page Sizes.
#define LARGE_PAGE_PHYS_OK(phys) ((phys) != 0)

void map_pages(pagemap_t pagemap, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t count) {
    if (virt % 0x1000 != 0 || phys % 0x1000 != 0 || count % 0x1000 != 0) {
        panic(true, "vmm: Misaligned call to map_pages()");
    }

    for (uint64_t i = 0; i < count; ) {
        if (LARGE_PAGE_PHYS_OK(phys + i)
         && ((phys + i) & (0x40000000 - 1)) == 0 && ((virt + i) & (0x40000000 - 1)) == 0 && count - i >= 0x40000000) {
            map_page(pagemap, virt + i, phys + i, flags, VMM_PAGE_SIZE_1GIB);
            i += 0x40000000;
            continue;
        }
        if (LARGE_PAGE_PHYS_OK(phys + i)
         && ((phys + i) & (0x200000 - 1)) == 0 && ((virt + i) & (0x200000 - 1)) == 0 && count - i >= 0x200000) {
            map_page(pagemap, virt + i, phys + i, flags, VMM_PAGE_SIZE_2MIB);
            i += 0x200000;
            continue;
        }
        map_page(pagemap, virt + i, phys + i, flags, VMM_PAGE_SIZE_4KIB);
        i += 0x1000;
    }
}


#define PT_FLAG_VALID    ((uint64_t)1 << 0)
#define PT_FLAG_WRITE    ((uint64_t)1 << 1)
#define PT_FLAG_USER     ((uint64_t)1 << 2)
#define PT_FLAG_LARGE    ((uint64_t)1 << 7)
#define PT_FLAG_NX       ((uint64_t)1 << 63)
#define PT_PADDR_MASK    ((uint64_t)0x0000FFFFFFFFF000)

#define PT_TABLE_FLAGS   (PT_FLAG_VALID | PT_FLAG_WRITE | PT_FLAG_USER)
#define PT_IS_TABLE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_LARGE)) == PT_FLAG_VALID)
#define PT_IS_LARGE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_LARGE)) == (PT_FLAG_VALID | PT_FLAG_LARGE))
#define PT_TO_VMM_FLAGS(x) ((x) & (PT_FLAG_WRITE | PT_FLAG_NX | VMM_FLAG_FB))

#define pte_new(addr, flags)    ((pt_entry_t)(addr) | (flags))
#define pte_addr(pte)           ((pte) & PT_PADDR_MASK)

pagemap_t new_pagemap(int paging_mode) {
    pagemap_t pagemap;
    pagemap.levels    = paging_mode == PAGING_MODE_X86_64_5LVL ? 5 : 4;
    pagemap.top_level = ext_mem_alloc(PT_SIZE);
    return pagemap;
}

static bool is_1gib_page_supported(void) {
    // Cache the cpuid result :^)
    static bool CACHE_INIT = false;
    static bool CACHE = false;

    if (!CACHE_INIT) {
        // Check if 1GiB pages are supported:
        uint32_t eax, ebx, ecx, edx;

        CACHE = cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) && ((edx & 1 << 26) == 1 << 26);
        CACHE_INIT = true;

        printv("paging: 1GiB pages are %s!\n", CACHE ? "supported" : "not supported");
    }

    return CACHE;
}

void map_page(
    pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, size_t pg_size
) {
    // Calculate the indices in the various tables using the virtual address
    size_t pml5_entry = (virt_addr & ((uint64_t)0x1ff << 48)) >> 48;
    size_t pml4_entry = (virt_addr & ((uint64_t)0x1ff << 39)) >> 39;
    size_t pml3_entry = (virt_addr & ((uint64_t)0x1ff << 30)) >> 30;
    size_t pml2_entry = (virt_addr & ((uint64_t)0x1ff << 21)) >> 21;
    size_t pml1_entry = (virt_addr & ((uint64_t)0x1ff << 12)) >> 12;

    pt_entry_t *pml5, *pml4, *pml3, *pml2, *pml1;

    static bool pat_supported = false, pat_supported_got = false;
    if (!pat_supported_got) {
        uint32_t eax, ebx, ecx, edx;
        if (cpuid(1, 0, &eax, &ebx, &ecx, &edx) && (edx & (1 << 16))) {
            pat_supported = true;
        }
        pat_supported_got = true;
    }

    flags |= PT_FLAG_VALID; // Always present
    if ((flags & VMM_FLAG_FB) && !pat_supported) {
        flags &= ~(uint64_t)VMM_FLAG_FB;
    }

    // Paging levels
    switch (pagemap.levels) {
        case 5:
            pml5 = pagemap.top_level;
            goto level5;
        case 4:
            pml4 = pagemap.top_level;
            goto level4;
        default:
            __builtin_unreachable();
    }

level5:
    pml4 = get_next_level(pagemap, pml5, virt_addr, pg_size, 4, pml5_entry);
level4:
    pml3 = get_next_level(pagemap, pml4, virt_addr, pg_size, 3, pml4_entry);

    if (pg_size == VMM_PAGE_SIZE_1GIB) {
        // Check if 1GiB pages are available.
        if (is_1gib_page_supported()) {
            pml3[pml3_entry] = (pt_entry_t)(phys_addr | flags | PT_FLAG_LARGE);
        } else {
            // If 1GiB pages are not supported then emulate it by splitting them into
            // 2MiB pages.
            for (uint64_t i = 0; i < 0x40000000; i += 0x200000) {
                map_page(pagemap, virt_addr + i, phys_addr + i, flags, VMM_PAGE_SIZE_2MIB);
            }
        }

        return;
    }

    pml2 = get_next_level(pagemap, pml3, virt_addr, pg_size, 2, pml3_entry);

    if (pg_size == VMM_PAGE_SIZE_2MIB) {
        pml2[pml2_entry] = (pt_entry_t)(phys_addr | flags | PT_FLAG_LARGE);
        return;
    }

    pml1 = get_next_level(pagemap, pml2, virt_addr, pg_size, 1, pml2_entry);

    // PML1 wants PAT bit at 7 instead of 12
    if (flags & ((uint64_t)1 << 12)) {
        flags &= ~((uint64_t)1 << 12);
        flags |= ((uint64_t)1 << 7);
    }

    pml1[pml1_entry] = (pt_entry_t)(phys_addr | flags);
}


static pt_entry_t *get_next_level(
    pagemap_t pagemap, pt_entry_t *current_level, uint64_t virt, size_t desired_sz,
    size_t level_idx, size_t entry
) {
    pt_entry_t *ret;

    if (PT_IS_TABLE(current_level[entry])) {
        ret = (pt_entry_t *)(size_t)pte_addr(current_level[entry]);
    } else {
        if (PT_IS_LARGE(current_level[entry])) {
            // We are replacing an existing large page with a smaller page.
            // Split the previous mapping into mappings of the newly requested size
            // before performing the requested map operation.


            if ((level_idx >= VMM_MAX_LEVEL) || (level_idx == 0))
                panic(false, "Unexpected level in get_next_level");
            if (desired_sz >= VMM_MAX_LEVEL)
                panic(false, "Unexpected page size in get_next_level");

            uint64_t old_page_size = page_sizes[level_idx];
            uint64_t new_page_size = page_sizes[desired_sz];

            // Save all the information from the old entry at this level
            uint64_t old_flags = PT_TO_VMM_FLAGS(current_level[entry]);
            uint64_t old_phys = pte_addr(current_level[entry]);
            uint64_t old_virt = virt & ~(old_page_size - 1);

            if (old_phys & (old_page_size - 1))
                panic(false, "Unexpected page table entry address in get_next_level");

            // Allocate a table for the next level
            ret = ext_mem_alloc(PT_SIZE);
            current_level[entry] = pte_new((size_t)ret, PT_TABLE_FLAGS);

            // Recreate the old mapping with smaller pages
            for (uint64_t i = 0; i < old_page_size; i += new_page_size) {
                map_page(pagemap, old_virt + i, old_phys + i, old_flags, desired_sz);
            }
        } else {
            // Allocate a table for the next level
            ret = ext_mem_alloc(PT_SIZE);
            current_level[entry] = pte_new((size_t)ret, PT_TABLE_FLAGS);
        }
    }

    return ret;
}
