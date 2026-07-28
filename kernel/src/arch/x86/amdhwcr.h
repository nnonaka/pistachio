/*********************************************************************
 *
 * Copyright (C) 2004, 2007,  Karlsruhe University
 *
 * File path:     arch/x86/amdhwcr.h
 * Description:   AMD K8 and later HWCR MSR
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
#ifndef __ARCH__X86__AMDHWCR_H__
#define __ARCH__X86__AMDHWCR_H__

#define X86_AMDHWCR_MSR                0xC0010015

/* AMDHWCR Bits features 26049 p289 */
#define X86_AMDHWCR_SMMLOCK        ( 1 <<  0)  /* SMM lock */
#define X86_AMDHWCR_SLOWFENCE      ( 1 <<  1)  /* Slow SFENCE enable */
#define X86_AMDHWCR_TLBCACHEDIS    ( 1 <<  3)  /* Cacheable memory disable*/
#define X86_AMDHWCR_INVD_WBINVD    ( 1 <<  4)  /* INVD to WBINVD conversion */
#define X86_AMDHWCR_FFDIS          ( 1 <<  6)  /* Flush filter disable*/
#define X86_AMDHWCR_DISLOCK        ( 1 <<  7)  /* x86 LOCK prefix disable  */
#define X86_AMDHWCR_IGNNE_EM       ( 1 <<  8)  /* IGNNE port emulation enable*/
#define X86_AMDHWCR_HLTXSPCYCEN    ( 1 << 12)  /* HLT special bus cyle enable  */
#define X86_AMDHWCR_SMISPCYCDIS    ( 1 << 13)  /* SMI special bus cyle disable */
#define X86_AMDHWCR_RSMSPCYCDIS    ( 1 << 14)  /* RSM special bus cyle disable */
#define X86_AMDHWCR_SSEDIS         ( 1 << 15)  /* SSE disable */
#define X86_AMDHWCR_WRAP32DIS      ( 1 << 17)  /* 32-bit address wrap disable  */
#define X86_AMDHWCR_MCIS_WREN      ( 1 << 18)  /* McI status write enable  */
#define X86_AMDHWCR_START_FID      (63 << 19)  /* startup FID status */


/*
 * C forms of the x86_amdhwcr_t static methods.  dump_hwcr was a header inline
 * whose only caller was kdb/arch/x86/x64/x86.cc, so it is re-implemented here
 * natively rather than asm-name bridged (a header inline is never emitted once
 * its last C++ caller goes away).  Note that seven of these predicates negate
 * a *DIS* bit -- transcribed from the bodies, not inferred from the names.
 */
INLINE bool amdhwcr_is_smm_locked (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_SMMLOCK) != 0; }
INLINE bool amdhwcr_is_slowfence_enabled (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_SLOWFENCE) != 0; }
INLINE bool amdhwcr_is_ptemem_cached (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_TLBCACHEDIS); }
INLINE bool amdhwcr_is_invd_wbinvd (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_INVD_WBINVD) != 0; }
INLINE bool amdhwcr_is_flushfilter_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_FFDIS); }

/*
 * The two flush-filter setters.  They were static members of
 * class x86_amdhwcr_t, removed by 4b5e3a0 with the rest of the C++ half; the C
 * half never had them because their only caller sits under
 * CONFIG_CPU_X86_K8, which the gate config does not set.  Notes §119.
 */
INLINE void amdhwcr_enable_flushfilter (void)
{
    u64_t hwcr = x86_rdmsr (X86_AMDHWCR_MSR);
    x86_wrmsr (X86_AMDHWCR_MSR, hwcr & ~(u64_t) X86_AMDHWCR_FFDIS);
}

INLINE void amdhwcr_disable_flushfilter (void)
{
    u64_t hwcr = x86_rdmsr (X86_AMDHWCR_MSR);
    x86_wrmsr (X86_AMDHWCR_MSR, hwcr | X86_AMDHWCR_FFDIS);
}
INLINE bool amdhwcr_is_lockprefix_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_DISLOCK); }
INLINE bool amdhwcr_is_ignne_emulation_enabled (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_IGNNE_EM) != 0; }
INLINE bool amdhwcr_is_hltx_spc_enabled (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_HLTXSPCYCEN) != 0; }
INLINE bool amdhwcr_is_smi_spc_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_SMISPCYCDIS); }
INLINE bool amdhwcr_is_rsm_spc_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_RSMSPCYCDIS); }
INLINE bool amdhwcr_is_sse_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_SSEDIS); }
INLINE bool amdhwcr_is_wrap32_enabled (void)
{ return !(x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_WRAP32DIS); }
INLINE bool amdhwcr_is_mci_status_write_enabled (void)
{ return (x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_MCIS_WREN) != 0; }
INLINE u8_t amdhwcr_get_startup_fid_status (void)
{ return (u8_t) ((x86_rdmsr (X86_AMDHWCR_MSR) & X86_AMDHWCR_START_FID) >> 19); }

INLINE void amdhwcr_dump_hwcr (void)
{
    printf("AMDHWCR register:\n");

    printf("\tsmmlock: %s\n"
           "\tslowfence: %s\n"
           "\ttlbcache: %s\n"
           "\tinvd_wbinvd: %s\n"
           "\tflush filter: %s\n"
           "\tlock prefix: %s\n"
           "\tignne emulation: %s\n"
           "\texit from hlt special bus cycle: %s\n"
           "\tsmi special bus cycle: %s\n"
           "\trsm special bus cycle: %s\n"
           "\tsse: %s\n"
           "\t32-bit address wrap: %s\n"
           "\tmci status write: %s\n"
           "\tstartup fid status: %x\n",
           (amdhwcr_is_smm_locked () ? "enabled" : "disabled" ),
           (amdhwcr_is_slowfence_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_ptemem_cached () ? "enabled" : "disabled" ),
           (amdhwcr_is_invd_wbinvd () ? "enabled" : "disabled" ),
           (amdhwcr_is_flushfilter_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_lockprefix_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_ignne_emulation_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_hltx_spc_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_smi_spc_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_rsm_spc_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_sse_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_wrap32_enabled () ? "enabled" : "disabled" ),
           (amdhwcr_is_mci_status_write_enabled () ? "enabled" : "disabled" ),
           amdhwcr_get_startup_fid_status ());
}




#endif /* !__ARCH__X86__AMDHWCR_H__ */
