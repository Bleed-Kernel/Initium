#ifndef SYS__CPU_H
#define SYS__CPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/misc.h>


static inline bool cpuid(uint32_t leaf, uint32_t subleaf,
          uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t cpuid_max;
    asm volatile ("cpuid"
                  : "=a" (cpuid_max)
                  : "a" (leaf & 0x80000000)
                  : "ebx", "ecx", "edx");
    if (leaf > cpuid_max)
        return false;
    asm volatile ("cpuid"
                  : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
                  : "a" (leaf), "c" (subleaf));
    return true;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %%al, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %%ax, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outd(uint16_t port, uint32_t value) {
    asm volatile ("outl %%eax, %1" : : "a" (value), "Nd" (port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile ("inb %1, %%al"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile ("inw %1, %%ax"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t value;
    asm volatile ("inl %1, %%eax" : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline void mmoutb(uintptr_t addr, uint8_t value) {
    asm volatile (
        "movb %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutw(uintptr_t addr, uint16_t value) {
    asm volatile (
        "movw %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutd(uintptr_t addr, uint32_t value) {
    asm volatile (
        "movl %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

#if defined(__x86_64__)
static inline void mmoutq(uintptr_t addr, uint64_t value) {
    asm volatile (
        "movq %1, (%0)"
        :
        : "r" (addr), "r" (value)
        : "memory"
    );
}
#endif

static inline uint8_t mminb(uintptr_t addr) {
    uint8_t ret;
    asm volatile (
        "movb (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint16_t mminw(uintptr_t addr) {
    uint16_t ret;
    asm volatile (
        "movw (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint32_t mmind(uintptr_t addr) {
    uint32_t ret;
    asm volatile (
        "movl (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

#if defined(__x86_64__)
static inline uint64_t mminq(uintptr_t addr) {
    uint64_t ret;
    asm volatile (
        "movq (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}
#endif

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t edx, eax;
    asm volatile ("rdmsr"
                  : "=a" (eax), "=d" (edx)
                  : "c" (msr)
                  : "memory");
    return ((uint64_t)edx << 32) | eax;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t edx = value >> 32;
    uint32_t eax = (uint32_t)value;
    asm volatile ("wrmsr"
                  :
                  : "a" (eax), "d" (edx), "c" (msr)
                  : "memory");
}

static inline bool disable_interrupts(void) {
    uintptr_t flags;
    asm volatile (
        "pushf\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r" (flags)
        :
        : "memory"
    );
    return !!(flags & ((uintptr_t)1 << 9));
}

static inline bool enable_interrupts(void) {
    uintptr_t flags;
    asm volatile (
        "pushf\n\t"
        "pop %0\n\t"
        "sti\n\t"
        : "=r" (flags)
        :
        : "memory"
    );
    return !!(flags & ((uintptr_t)1 << 9));
}

static inline uint64_t rdtsc(void) {
    uint32_t edx, eax;
    asm volatile ("rdtsc" : "=a" (eax), "=d" (edx) :: "memory");
    return ((uint64_t)edx << 32) | eax;
}

static inline uint64_t tsc_freq_arch(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x15, 0, &eax, &ebx, &ecx, &edx))
        return 0;
    if (eax == 0 || ebx == 0 || ecx == 0)
        return 0;
    return (uint64_t)ecx * ebx / eax;
}

#define rdrand(type, out) ({ \
    type rdrand__ret = 0; \
    bool rdrand__ok = false; \
    for (int rdrand__i = 0; rdrand__i < 10; rdrand__i++) { \
        asm volatile ( \
            "rdrand %0; setc %1" \
            : "=r" (rdrand__ret), "=qm" (rdrand__ok) \
        ); \
        if (rdrand__ok) break; \
    } \
    *(out) = rdrand__ret; \
    rdrand__ok; \
})

#define rdseed(type, out) ({ \
    type rdseed__ret = 0; \
    bool rdseed__ok = false; \
    for (int rdseed__i = 0; rdseed__i < 10; rdseed__i++) { \
        asm volatile ( \
            "rdseed %0; setc %1" \
            : "=r" (rdseed__ret), "=qm" (rdseed__ok) \
        ); \
        if (rdseed__ok) break; \
    } \
    *(out) = rdseed__ret; \
    rdseed__ok; \
})

#define write_cr(reg, val) do { \
    asm volatile ("mov %0, %%cr" reg :: "r" (val) : "memory"); \
} while (0)

#define read_cr(reg) ({ \
    size_t read_cr__cr; \
    asm volatile ("mov %%cr" reg ", %0" : "=r" (read_cr__cr) :: "memory"); \
    read_cr__cr; \
})

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret = 0; \
    asm volatile ( \
        "lock xadd %0, %1" \
        : "+r" (locked_read__ret), "+m" (*(var)) \
        :: "memory" \
    ); \
    locked_read__ret; \
})

#define locked_write(var, val) do { \
    __auto_type locked_write__ret = val; \
    asm volatile ( \
        "lock xchg %0, %1" \
        : "+r" ((locked_write__ret)), "+m" (*(var)) \
        :: "memory" \
    ); \
} while (0)

static inline void sync_icache_range(uintptr_t start, uintptr_t end) {
    (void)start; (void)end;
}


extern uint64_t tsc_freq;
void calibrate_tsc(void);

static inline uint64_t rdtsc_usec(void) {
    uint64_t exec_ticks = rdtsc();
    if (tsc_freq == 0) {
        return 0;
    }
    return exec_ticks / tsc_freq * 1000000
         + exec_ticks % tsc_freq * 1000000 / tsc_freq;
}

static inline uint64_t rdtsc_deadline(uint64_t us) {
    if (tsc_freq == 0) {
        return 0;
    }

    uint64_t seconds = us / 1000000;
    uint64_t remainder = us % 1000000;

    uint64_t ticks = CHECKED_MUL(seconds, tsc_freq, return UINT64_MAX);

    uint64_t remainder_ticks = CHECKED_MUL(remainder, tsc_freq / 1000000, return UINT64_MAX);
    remainder_ticks += remainder * (tsc_freq % 1000000) / 1000000;

    ticks = CHECKED_ADD(ticks, remainder_ticks, return UINT64_MAX);

    uint64_t now = rdtsc();
    return CHECKED_ADD(now, ticks, return UINT64_MAX);
}

static inline bool rdtsc_deadline_expired(uint64_t deadline) {
    return deadline != 0 && rdtsc() >= deadline;
}

static inline void stall(uint64_t us) {
    uint64_t ticks = (tsc_freq * us + 999999) / 1000000;
    uint64_t next_stop = rdtsc() + ticks;
    while (rdtsc() < next_stop);
}

static inline const char *current_arch(void) {
#if defined(__x86_64__)
    return "x86-64";
#elif defined(__i386__)
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
        return "ia-32";
    } else {
        return "x86-64";
    }
#else
#error "Unspecified architecture"
#endif
}

static inline const char *loader_arch(void) {
#if defined(__x86_64__)
    return "x86-64";
#elif defined(__i386__)
    return "ia-32";
#else
#error "Unspecified architecture"
#endif
}

#endif
