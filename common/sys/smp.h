#ifndef SYS__SMP_H
#define SYS__SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/vmm.h>
#define LIMINE_NO_POINTERS
#include <limine.h>


extern bool smp_configure_apic;

struct limine_mp_info *init_smp(size_t   *cpu_count,
                                 uint32_t *_bsp_lapic_id,
                                 int       paging_mode,
                                 pagemap_t pagemap,
                                 bool      x2apic,
                                 bool      nx,
                                 uint64_t  hhdm,
                                 bool      wp);


#endif
