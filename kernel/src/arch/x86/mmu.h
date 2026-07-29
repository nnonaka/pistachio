/*********************************************************************
 *                
 * Copyright (C) 2003-2005, 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     arch/x86/mmu.h
 * Description:   X86 specific MMU Stuff
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
#ifndef __ARCH__X86__MMU_H__
#define __ARCH__X86__MMU_H__

#include INC_ARCH(cpu.h)

/*
 * x86_mmu_t is a static-methods-only holder (no data members, no instances
 * anywhere -- only x86_mmu_t::flush_tlb() etc.).  There is nothing to
 * represent as a C struct, so the whole class and its out-of-line inline
 * methods are simply guarded out of the C path; a C consumer would use
 * free-function equivalents instead.
 */

/* C mirror of the x86_mmu_t static methods used by resources.c / init.c. */
INLINE void x86_mmu_set_active_pagetable (word_t root)
{
    __asm__ __volatile__ ("mov %0, %%cr3 \n" : : "r"(root));
}

INLINE void x86_mmu_enable_global_pages (void)
{
    x86_cr4_set (X86_CR4_PGE);
}

/* C mirrors of the long-mode bring-up statics used by arch/x86/x64/init32.c. */
INLINE void x86_mmu_enable_paging (void)
{
    x86_cr0_set (X86_CR0_PG | X86_CR0_WP | X86_CR0_PE);
    __asm__ __volatile__ ("jmp penabled; penabled:");
}

INLINE void x86_mmu_disable_paging (void)
{
    x86_cr0_mask (X86_CR0_PG);
}

/* was x86_mmu_t::enable_super_pages -- x32 turns on 4M pages through CR4.PSE
   (x64's superpages come with PAE, which init32 enables instead). */
INLINE void x86_mmu_enable_super_pages (void)
{
    x86_cr4_set (X86_CR4_PSE);
}

INLINE void x86_mmu_enable_pae_mode (void)
{
    x86_cr4_set (X86_CR4_PAE);
}

/* Long mode: the CPUID_* constants these need live in x64/cpu.h, and there is
   nothing for a 32-bit kernel to ask.  The callers (x64/init32.c) are x64-only
   too. */
#if defined(CONFIG_SUBARCH_X64)
INLINE __attribute__((always_inline)) bool x86_mmu_has_long_mode (void)
{
    if (!(x86_x64_has_cpuid ()))
	return false;

    u32_t features, lfn, dummy;

    x86_cpuid (CPUID_MAX_EXT_FN_NR, &lfn, &dummy, &dummy, &dummy);

    if (lfn < CPUID_AMD_FEATURES)
	return false;

    x86_cpuid (CPUID_AMD_FEATURES, &dummy, &dummy, &dummy, &features);

    return (features & CPUID_AMD_HAS_LONGMODE);
}

INLINE void x86_mmu_enable_long_mode (void)
{
    u64_t efer = x86_rdmsr (X86_MSR_EFER);
    efer |= X86_MSR_EFER_LME;
    x86_wrmsr (X86_MSR_EFER, efer);
}
#endif /* defined(CONFIG_SUBARCH_X64) */

#if defined(CONFIG_SUBARCH_X64)
INLINE bool x86_mmu_long_mode_active (void)
{
    u64_t efer = x86_rdmsr (X86_MSR_EFER);
    return (efer & X86_MSR_EFER_LMA);
}
#endif


INLINE void x86_mmu_flush_tlb (bool global)
{
    word_t dummy1;
#if defined(CONFIG_X86_PGE)
    if (!global)
    {
	__asm__ __volatile__(
		"mov    %%cr3, %0   \n\t"
		"mov    %0, %%cr3   \n\t"
		: "=r" (dummy1));
    }
    else
    {
	word_t dummy2;
	__asm__ __volatile__(
		"mov    %%cr4, %0       \n"
		"and    %2, %0          \n"
		"mov    %0, %%cr4       \n"
		"mov    %%cr3, %1       \n"
		"mov    %1, %%cr3       \n"
		"or     %3, %0          \n"
		"mov    %0, %%cr4       \n"
		: "=r"(dummy1), "=r"(dummy2)
		: "i" (~X86_CR4_PGE), "i" (X86_CR4_PGE));
    }
#else
    __asm__ __volatile__(
	    "mov    %%cr3, %0   \n\t"
	    "mov    %0, %%cr3   \n\t"
	    : "=r" (dummy1));
#endif
}

INLINE word_t x86_mmu_get_active_pagetable (void)
{
    word_t pgm;
    __asm__ __volatile__ ("mov %%cr3, %0\n" : "=a" (pgm));
    return pgm;
}

INLINE word_t x86_mmu_get_pagefault_address (void)
{
    word_t tmp;
    __asm__ ("mov %%cr2, %0\n" : "=r" (tmp));
    return tmp;
}

INLINE void x86_mmu_flush_tlbent (word_t addr)
{
    __asm__ __volatile__ ("invlpg (%0)\n" : : "r" (addr));
}

#endif /* !__ARCH__X86__MMU_H__ */
