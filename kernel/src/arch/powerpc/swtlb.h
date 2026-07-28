/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     src/arch/powerpc/swtlb.h
 * Description:   
 *                
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *                
 * $Id$
 *                
 ********************************************************************/
#ifndef __ARCH__POWERPC__SWTLB_H__
#define __ARCH__POWERPC__SWTLB_H__

/* TLB entry 0 */
#define PPC_TLB_VALID		(0x80000000 >> 22)
#define PPC_TLB_SPACE0		(0)
#define PPC_TLB_SPACE1		(0x80000000 >> 23)
#define PPC_TLB_SIZE(x)		((1 << (x) -1) >> 24)
#define PPC_TLB_SIZE_1K		PPC_TLB_SIZE(0)
#define PPC_TLB_SIZE_4K		PPC_TLB_SIZE(1)
#define PPC_TLB_SIZE_16K	PPC_TLB_SIZE(2)
#define PPC_TLB_SIZE_64K	PPC_TLB_SIZE(3)
#define PPC_TLB_SIZE_256K	PPC_TLB_SIZE(4)
#define PPC_TLB_SIZE_1M		PPC_TLB_SIZE(5)
#define PPC_TLB_SIZE_16M	PPC_TLB_SIZE(7)
#define PPC_TLB_SIZE_256M	PPC_TLB_SIZE(9)
#define PPC_TLB_SIZE_1G		PPC_TLB_SIZE(10)

/* TLB entry 1 */
#define PPC_TLB_RPN(x)		(x & 0xfffff300)
#define PPC_TLB_ERPN(x)		(x & 0xf)

/* TLB entry 2 */
#define PPC_TLB_WL1		(0x80000000 >> 11)
#define PPC_TLB_IL1I		(0x80000000 >> 12)
#define PPC_TLB_IL1D		(0x80000000 >> 13)
#define PPC_TLB_IL2I		(0x80000000 >> 14)
#define PPC_TLB_IL2D		(0x80000000 >> 15)
#define PPC_TLB_U0		(0x80000000 >> 16)
#define PPC_TLB_U1		(0x80000000 >> 17)
#define PPC_TLB_U2		(0x80000000 >> 18)
#define PPC_TLB_U3		(0x80000000 >> 19)
#define PPC_TLB_WRITETHROUGH	(0x80000000 >> 20)
#define PPC_TLB_CACHEINHIBIT	(0x80000000 >> 21)
#define PPC_TLB_MEMCOHERENCY	(0x80000000 >> 22)
#define PPC_TLB_GUARDED		(0x80000000 >> 23)
#define PPC_TLB_ENDIAN		(0x80000000 >> 24)
#define PPC_TLB_UX		(0x80000000 >> 26)
#define PPC_TLB_UW		(0x80000000 >> 27)
#define PPC_TLB_UR		(0x80000000 >> 28)
#define PPC_TLB_SX		(0x80000000 >> 29)
#define PPC_TLB_SW		(0x80000000 >> 30)
#define PPC_TLB_SR		(0x80000000 >> 31)

#define PPC_MAX_TLB_ENTRIES	64

#ifndef ASSEMBLY

#include INC_ARCH(ppc_registers.h)

inline void ppc_tlbwe(word_t index, const word_t field, word_t value)
{
    asm volatile ("tlbwe %[value], %[index], %[field]\n"
		  : 
		  : [value]"b"(value), [index]"b"(index), 
		    [field]"i"(field)); 
}

inline word_t ppc_tlbre(word_t index, const word_t field)
{
    word_t value;
    asm volatile ("tlbre %[value], %[index], %[field]\n"
		  : [value]"=b"(value)
		  : [index]"b"(index), [field]"i"(field));
    return value;
}

/* index was a word_t& out-parameter. */
INLINE word_t ppc_tlbsx(word_t vaddr, word_t *index)
{
    word_t found;
    asm volatile ("tlbsx. %[index],0,%[vaddr]\n"
		  "beq	1f\n"
		  "li	%[found], 0\n"
		  "1:\n"
		  : [index] "=b"(index), [found] "=&b"(found)
		  : [vaddr] "b"(vaddr), "[found]"(true));
    return found;
}

inline word_t ppc_get_pid()
{
    return ppc_get_spr(SPR_PID);
}

inline void ppc_set_pid(word_t pid)
{
    ppc_set_spr(SPR_PID, pid);
}


struct ppc_tlb0_t {
    union {
	word_t raw;
	struct {
	    u32_t epn		: 22;
	    u32_t valid		: 1;
	    u32_t trans_space	: 1;
	    u32_t size		: 4;
	    u32_t parity	: 4;
	};
    };

};
typedef struct ppc_tlb0_t ppc_tlb0_t;

/* The C++ class had ppc_tlb0_t() and ppc_tlb0_t(vaddr, log2size, valid, space);
   the latter just forwarded to init_vaddr_size, which callers now use directly.
   Its valid/space arguments defaulted to true/0. */
INLINE void ppc_tlb0_init_vaddr_size (ppc_tlb0_t *self, word_t vaddr, word_t log2size,
				      bool valid, int space)
{
    self->raw = 0;
    self->epn = vaddr >> 10;
    self->size = (log2size - 10) / 2;
    self->trans_space = space;
    self->valid = valid;
}

INLINE bool ppc_tlb0_is_valid (ppc_tlb0_t *self)	{ return self->valid; }

INLINE ppc_tlb0_t ppc_tlb0_invalid (void)
{
    ppc_tlb0_t tmp;
    tmp.raw = 0;
    return tmp;
}

INLINE void   ppc_tlb0_set_vaddr (ppc_tlb0_t *self, word_t vaddr) { self->epn = vaddr >> 10; }
INLINE word_t ppc_tlb0_get_size (ppc_tlb0_t *self)	{ return (1024 << (self->size * 2)); }
INLINE word_t ppc_tlb0_get_log2size (ppc_tlb0_t *self)	{ return (self->size * 2) + 10; }
INLINE word_t ppc_tlb0_get_vaddr (ppc_tlb0_t *self)	{ return self->epn << 10; }

INLINE bool ppc_tlb0_is_vaddr_covered (ppc_tlb0_t *self, word_t vaddr)
{
    return (vaddr >= ppc_tlb0_get_vaddr (self) &&
	    vaddr <= ppc_tlb0_get_vaddr (self) + ppc_tlb0_get_size (self) - 1);
}

/* was operator += */
INLINE void ppc_tlb0_add_offset (ppc_tlb0_t *self, const word_t offset)
{ self->epn += (offset >> 10); }

INLINE bool ppc_tlb0_is_valid_pagesize (word_t log2size)
{
    return (KB(1) | KB(4) | KB(16) | KB(64) | KB(256) |
	    MB(1) | MB(16) | MB(256) | GB(1)) & (1 << log2size);
}

INLINE void ppc_tlb0_write (ppc_tlb0_t *self, int index) { ppc_tlbwe(index, 0, self->raw); }
INLINE void ppc_tlb0_read (ppc_tlb0_t *self, int index)  { self->raw = ppc_tlbre(index, 0); }



struct ppc_tlb1_t {
    union {
	u32_t raw;
	struct {
	    u32_t page		: 22;
	    u32_t parity	: 2;
	    u32_t __res		: 4;
	    u32_t extpage	: 4;
	};
    };

};
typedef struct ppc_tlb1_t ppc_tlb1_t;

INLINE void ppc_tlb1_set_paddr (ppc_tlb1_t *self, u64_t paddr)
{
    self->page = (paddr & ~0UL) >> 10;
    self->extpage = (paddr >> 32ULL);
}

INLINE u64_t ppc_tlb1_get_paddr (ppc_tlb1_t *self)
{ return ((u64_t)self->extpage << 32) | ((u64_t)self->page << 10); }

/* was the ppc_tlb1_t(u64_t) constructor */
INLINE void ppc_tlb1_init_paddr (ppc_tlb1_t *self, u64_t paddr)
{
    self->raw = 0;
    ppc_tlb1_set_paddr (self, paddr);
}

INLINE void ppc_tlb1_write (ppc_tlb1_t *self, word_t index) { ppc_tlbwe(index, 1, self->raw); }
INLINE void ppc_tlb1_read (ppc_tlb1_t *self, word_t index)  { self->raw = ppc_tlbre(index, 1); }

/* was operator += */
INLINE void ppc_tlb1_add_offset (ppc_tlb1_t *self, const u64_t offset)
{ ppc_tlb1_set_paddr (self, ppc_tlb1_get_paddr (self) + offset); }



struct ppc_tlb2_t {
    union {
	u32_t raw;
	struct {
	    u32_t parity	: 2;
	    u32_t __res1	: 9;
	    u32_t wt_l1		: 1;
	    u32_t inhibit_l1i	: 1;
	    u32_t inhibit_l1d	: 1;
	    u32_t inhibit_l2i	: 1;
	    u32_t inhibit_l2d	: 1;
	    u32_t user0		: 1;
	    u32_t user1		: 1;
	    u32_t user2		: 1;
	    u32_t user3		: 1;
	    u32_t write_through	: 1;
	    u32_t inhibit	: 1;
	    u32_t mem_coherency	: 1;
	    u32_t guarded	: 1;
	    u32_t endian	: 1;
	    u32_t __res2	: 1;
	    u32_t user_execute	: 1;
	    u32_t user_write	: 1;
	    u32_t user_read	: 1;
	    u32_t super_execute	: 1;
	    u32_t super_write	: 1;
	    u32_t super_read	: 1;
	};
    };

};
typedef struct ppc_tlb2_t ppc_tlb2_t;

INLINE void ppc_tlb2_write (ppc_tlb2_t *self, int index) { ppc_tlbwe(index, 2, self->raw); }
INLINE void ppc_tlb2_read (ppc_tlb2_t *self, int index)  { self->raw = ppc_tlbre(index, 2); }

INLINE void ppc_tlb2_init (ppc_tlb2_t *self) { self->raw = 0; }

INLINE void ppc_tlb2_init_shared_smp (ppc_tlb2_t *self)
{
    self->raw = 0;
    self->mem_coherency = 1;
#ifdef CONFIG_PPC_CACHE_L1_WRITETHROUGH
    self->wt_l1 = 1;
    self->user2 = 1;
#endif
}

INLINE void ppc_tlb2_init_guarded (ppc_tlb2_t *self)
{
    ppc_tlb2_init_shared_smp (self);
    self->guarded = 1;
}

INLINE void ppc_tlb2_init_cpu_local (ppc_tlb2_t *self) { self->raw = 0; }

INLINE void ppc_tlb2_init_device (ppc_tlb2_t *self)
{
    self->raw = 0;
    self->inhibit = 1;
    self->guarded = 1;
}

INLINE void ppc_tlb2_set_user_perms (ppc_tlb2_t *self, bool read, bool write, bool execute)
{
    self->user_read = read;
    self->user_write = write;
    self->user_execute = execute;
}

INLINE void ppc_tlb2_set_kernel_perms (ppc_tlb2_t *self, bool read, bool write, bool execute)
{
    self->super_read = read;
    self->super_write = write;
    self->super_execute = execute;
}

INLINE void ppc_tlb2_set_cache (ppc_tlb2_t *self, bool inhibit, bool write_through, bool guarded)
{
    self->inhibit = inhibit;
    self->write_through = write_through;
    self->guarded = guarded;
}

INLINE void ppc_tlb2_set_l1_cache (ppc_tlb2_t *self, bool inhibit_l1i, bool inhibit_l1d)
{
    self->inhibit_l1i = inhibit_l1i;
    self->inhibit_l1d = inhibit_l1d;
}

INLINE void ppc_tlb2_set_l2_cache (ppc_tlb2_t *self, bool inhibit_l2i, bool inhibit_l2d)
{
    self->inhibit_l2i = inhibit_l2i;
    self->inhibit_l2d = inhibit_l2d;
}

INLINE void ppc_tlb2_set_user0 (ppc_tlb2_t *self, bool u0) { self->user0 = u0; }
INLINE void ppc_tlb2_set_user1 (ppc_tlb2_t *self, bool u1) { self->user1 = u1; }
INLINE void ppc_tlb2_set_user2 (ppc_tlb2_t *self, bool u2) { self->user2 = u2; }
INLINE void ppc_tlb2_set_user3 (ppc_tlb2_t *self, bool u3) { self->user3 = u3; }
INLINE void ppc_tlb2_set_endian (ppc_tlb2_t *self, bool endian) { self->endian = endian; }

INLINE bool ppc_tlb2_is_user_accessible (ppc_tlb2_t *self)   { return self->raw & (7 << 3); }
INLINE bool ppc_tlb2_is_kernel_accessible (ppc_tlb2_t *self) { return self->raw & 7; }
INLINE bool ppc_tlb2_is_accessible (ppc_tlb2_t *self)	     { return self->raw & 0x3f; }



struct ppc_mmucr_t {
    union {
	word_t raw;
	struct {
	    word_t __res0			: 6;
	    word_t l2_store_without_allocate	: 1;
	    word_t store_without_allocate	: 1;
	    word_t __res1			: 1;
	    word_t u1_transient_enable		: 1;
	    word_t u2_store_without_allocate	: 1;
	    word_t u3_l2_store_without_allocate	: 1;
	    word_t dcache_unlock_exception	: 1;
	    word_t icache_unlock_exception	: 1;
	    word_t __res2			: 1;
	    word_t search_translation_space	: 1;
	    word_t __res3			: 8;
	    word_t search_id			: 8;
	};
    };

};
typedef struct ppc_mmucr_t ppc_mmucr_t;

INLINE void   ppc_mmucr_set_search_id (ppc_mmucr_t *self, word_t id) { self->search_id = id; }
INLINE word_t ppc_mmucr_get_search_id (ppc_mmucr_t *self)	     { return self->search_id; }

INLINE void ppc_mmucr_write (ppc_mmucr_t *self) { ppc_set_spr(SPR_MMUCR, self->raw); }

/* read() returned *this by value so it could be chained; kdb/arch/powerpc/regs.c
   does exactly that.  The C form mutates and the callers read the struct after. */
INLINE void ppc_mmucr_read (ppc_mmucr_t *self)	{ self->raw = ppc_get_spr(SPR_MMUCR); }

/* space defaulted to 0 */
INLINE void ppc_mmucr_write_search_id (word_t id, int space)
{
    ppc_mmucr_t mmucr;
    ppc_mmucr_read (&mmucr);
    ppc_mmucr_set_search_id (&mmucr, id);
    mmucr.search_translation_space = space;
    ppc_mmucr_write (&mmucr);
}



struct ppc_swtlb_t
{
    word_t current_index;
    word_t high_water;
    word_t mask[2]; // use hard-coded size for better code below
    
};
typedef struct ppc_swtlb_t ppc_swtlb_t;

INLINE void ppc_swtlb_set_used (ppc_swtlb_t *self, word_t index)
{ self->mask[index / BITS_WORD] &= ~(1 << (BITS_WORD - 1 - (index % BITS_WORD))); }

INLINE void ppc_swtlb_set_free (ppc_swtlb_t *self, word_t index)
{ self->mask[index / BITS_WORD] |=  (1 << (BITS_WORD - 1 - (index % BITS_WORD))); }

INLINE void ppc_swtlb_init (ppc_swtlb_t *self, word_t high_water)
{
    word_t idx;

    for (idx = 0; idx < high_water; idx++)
	ppc_swtlb_set_free (self, idx);
    for (idx = high_water; idx < sizeof(self->mask) * 8; idx++)
	ppc_swtlb_set_used (self, idx);
    self->current_index = 0;
    self->high_water = high_water;
}

INLINE word_t ppc_swtlb_get_replacement (ppc_swtlb_t *self)
{
    self->current_index = (self->current_index + 1) % self->high_water;
    return self->current_index;
}

INLINE word_t ppc_swtlb_allocate (ppc_swtlb_t *self)
{
    word_t idx;

    if ((idx = count_leading_zeros( self->mask[0] )) < sizeof(word_t) * 8)
    {
	ppc_swtlb_set_used (self, idx);
	return idx;
    }
    else if ( (idx += count_leading_zeros( self->mask[1] ) ) < 2 * sizeof(word_t) * 8)
    {
	ppc_swtlb_set_used (self, idx);
	return idx;
    }
    else
	return ppc_swtlb_get_replacement (self);
}

INLINE word_t ppc_swtlb_allocate_pinned (ppc_swtlb_t *self)
{
    self->high_water--;
    return self->high_water + 1;
}


#endif

#endif /* !__ARCH__POWERPC__SWTLB_H__ */
