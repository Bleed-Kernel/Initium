#ifndef STATE_H
#define STATE_H

// Slots of the register snapshot each entry stub takes, shared with the
// assembly, which is why they are plain numbers rather than a structure.

#if defined(__x86_64__)
#define ST_RAX 0
#define ST_RBX 1
#define ST_RCX 2
#define ST_RDX 3
#define ST_RSI 4
#define ST_RDI 5
#define ST_RBP 6
#define ST_RSP 7
#define ST_R8 8
#define ST_R15 15
#define ST_RFLAGS 16
#define ST_CS 17
#define ST_DS 18
#define ST_ES 19
#define ST_SS 20
#define ST_FS 21
#define ST_GS 22
#define ST_CR0 23
#define ST_CR2 24
#define ST_CR3 25
#define ST_CR4 26
#define ST_CR8 27
#define ST_EFER 28
#define ST_FS_BASE 29
#define ST_GS_BASE 30
#define ST_KGS_BASE 31
#define ST_PAT 32
#define ST_APIC_BASE 33
#define ST_GDTR 34
#define ST_IDTR 36
#define ST_TR 38
#define ST_LDTR 39
#define ST_RET 40
#define ST_COUNT 48
#endif

#define MAX_APS 32

// Which symbol the executable was entered through.
#define ENTRY_ELF 1
#define ENTRY_REQUEST 2

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint64_t bsp_state[ST_COUNT];
extern uint64_t ap_states[MAX_APS][ST_COUNT];
extern volatile uint64_t entry_id;
#endif

#endif
