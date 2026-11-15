#ifndef MM__VMM_H
#define MM__VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define VMM_FLAG_WRITE   ((uint64_t)1 << 1)
#define VMM_FLAG_NOEXEC  ((uint64_t)1 << 63)
#define VMM_FLAG_FB      (((uint64_t)1 << 3) | ((uint64_t)1 << 12))

#define VMM_MAX_LEVEL 3

#define PAGING_MODE_X86_64_4LVL 0
#define PAGING_MODE_X86_64_5LVL 1

#define PAGING_MODE_MIN PAGING_MODE_X86_64_4LVL
#define PAGING_MODE_MAX PAGING_MODE_X86_64_5LVL

#define paging_mode_va_bits(mode) ((mode) ? 57 : 48)

static inline uint64_t paging_mode_higher_half(int paging_mode) {
    if (paging_mode == PAGING_MODE_X86_64_5LVL) {
        return 0xff00000000000000;
    } else {
        return 0xffff800000000000;
    }
}

typedef struct {
    int   levels;
    void *top_level;
} pagemap_t;

#define VMM_PAGE_SIZE_4KIB ((size_t)0)
#define VMM_PAGE_SIZE_2MIB ((size_t)1)
#define VMM_PAGE_SIZE_1GIB ((size_t)2)

pagemap_t new_pagemap(int lv);
void map_page(
    pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, size_t page_size
);


int vmm_max_paging_mode(void);
void map_pages(pagemap_t pagemap, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t count);

#endif
