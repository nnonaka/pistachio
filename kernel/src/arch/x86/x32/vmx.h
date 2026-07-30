/*********************************************************************
 *
 * Copyright (C) 2003-2005,  Karlsruhe University
 *
 * File path:     arch/x86/x32/vmx.h
 * Description:   Vanderpool Virtual Machine Extensions
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

#ifndef __ARCH__X86__X32__VMX_H__
#define __ARCH__X86__X32__VMX_H__

#include <kmemory.h>
#include <debug.h>

#include INC_API(fpage.h)
#include INC_ARCH(cpu.h)
#include INC_ARCH_SA(ptab.h)
#include INC_ARCH(pgent.h)
#include INC_ARCH(trapgate.h)
#include INC_ARCH(vmx.h)

/*
 * Was class x86_x32_vmx_t, all of whose members were static -- a namespace
 * rather than an object.  In C the members are free functions carrying the
 * class name as a prefix, and the private vmxon is file-static in the header
 * (its only caller, x86_x32_vmx_enable, is here too).
 */

INLINE bool x86_x32_vmx_is_available (void)
{
    if (!x86_x32_has_cpuid())
	return false;

    u32_t dummy, ecx=0;
    x86_cpuid(1, &dummy, &dummy, &ecx, &dummy);

    return ecx & X86_X32_FEAT2_VMX;
}


INLINE bool x86_x32_vmx_is_enabled (void)
{
    return x86_cr4_read() & X86_CR4_VMXE;
}


INLINE bool x86_x32_vmx_check_fixed_bits_cr0 (u64_t cr0)
{
    u64_t ncr0;
    u64_t FIXED0 = x86_rdmsr (X86_MSR_VMX_CR0_FIXED0);
    u64_t FIXED1 = x86_rdmsr (X86_MSR_VMX_CR0_FIXED1);

    // Fixed 0s.
    ncr0 = cr0 | FIXED0;
    // Fixed 1s.
    ncr0 = FIXED1 & ncr0;

    return cr0 == ncr0;
}

INLINE bool x86_x32_vmx_check_fixed_bits_cr4 (u64_t cr4)
{
    u64_t ncr4;
    u64_t FIXED0 = x86_rdmsr (X86_MSR_VMX_CR4_FIXED0);
    u64_t FIXED1 = x86_rdmsr (X86_MSR_VMX_CR4_FIXED1);

    // Fixed 0s.
    ncr4 = cr4 | FIXED0;
    // Fixed 1s.
    ncr4 = FIXED1 & ncr4;

    return cr4 == ncr4;
}


INLINE bool x86_x32_vmx_vmxon (vmcs_t *vmcs)
{
     ASSERT((x86_cr4_read() & X86_CR4_VMXE) != 0);
     ASSERT((x86_cr0_read() & X86_CR0_PE) != 0);

     x86_cr0_set(X86_CR0_NE);

     ASSERT((x86_cr0_read() & X86_CR0_NE) != 0);
     ASSERT((x86_rdmsr(X86_MSR_FEATURE_CONTROL) & X86_MSR_FEAT_CTR_LOCK) != 0);
     ASSERT((x86_rdmsr(X86_MSR_FEATURE_CONTROL) & X86_MSR_FEAT_CTR_ENABLE_VMXON) != 0);

     // Test CR0.
     if (!x86_x32_vmx_check_fixed_bits_cr0(x86_cr0_read()))
	 return false;

     // Test CR4.
     if (!x86_x32_vmx_check_fixed_bits_cr4(x86_cr4_read()))
	 return false;

     // Enter VMX-Root Mode.
     x86_vmxon((u64_t) (word_t) vmcs);

     return true;
}


INLINE bool x86_x32_vmx_enable (void)
{
    u64_t val;
    vmcs_t *vmcs;

    // Check VMXON Instruction.
    val = x86_rdmsr(X86_MSR_FEATURE_CONTROL);

    if (!(val & X86_MSR_FEAT_CTR_ENABLE_VMXON))
    {
	// VMXON is not enabled yet. Enable!
	if (val & X86_MSR_FEAT_CTR_LOCK)
	    panic("VMXON instruction is disabled. Could not enable it!");

	// Enable VMXON Intruction.
	val |= X86_MSR_FEAT_CTR_LOCK | X86_MSR_FEAT_CTR_ENABLE_VMXON;
	x86_wrmsr(X86_MSR_FEATURE_CONTROL, val);
    }

    // Set CR4.VMXE to 1.
    x86_cr4_set(X86_CR4_VMXE);

    // Initialize VMXON Region.
    vmcs = vmcs_alloc_vmcs();

    // VMXON.
    x86_x32_vmx_vmxon(vmcs);

    return true;
}


INLINE void x86_x32_vmx_disable (void)
{
    x86_vmxoff();
}


INLINE void NORETURN x86_x32_vmx_vmlaunch (x86_exceptionframe_t *frame)
{
    asm volatile (
	// Load VM's GPRs.
	"	mov 0x0c(%%eax), %%edi	       \n"
	"	mov 0x10(%%eax), %%esi	       \n"
	"	mov 0x14(%%eax), %%ebp	       \n"
	"	mov 0x1c(%%eax), %%ebx	       \n"
	"	mov 0x20(%%eax), %%edx	       \n"
	"	mov 0x24(%%eax), %%ecx	       \n"
	"	mov 0x28(%%eax), %%eax	       \n"
	// Other fields are read from VMCS.
	"	vmlaunch                       \n"
	:
	: "a" (&frame->__base.reason)
	: "ebx", "ecx", "edx", "edi", "esi", "ebp", "memory");

    // Failure on VM-Entry.
    panic("x86-hvm: vmlaunch failed\n");
}


INLINE NORETURN void x86_x32_vmx_vmresume (x86_exceptionframe_t *frame)
{
    asm volatile (
	// Load VM's GPRs.
	"	mov 0x0c(%%eax), %%edi	       \n"
	"	mov 0x10(%%eax), %%esi	       \n"
	"	mov 0x14(%%eax), %%ebp	       \n"
	"	mov 0x1c(%%eax), %%ebx	       \n"
	"	mov 0x20(%%eax), %%edx	       \n"
	"	mov 0x24(%%eax), %%ecx	       \n"
	"	mov 0x28(%%eax), %%eax	       \n"
	// Other fields are read from VMCS.
	"	vmresume                       \n"
	:
	: "a" (&frame->__base.reason)
	: "ecx", "edx", "edi", "esi", "ebp", "memory");

    // Failure on VM-Entry.
    panic("x86-hvm: vmresume failed\n");
}



#endif /* !__ARCH__X86__X32__VMX_H__ */
