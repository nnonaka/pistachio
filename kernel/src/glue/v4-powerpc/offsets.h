/*********************************************************************
 *                
 * Copyright (C) 2002,  Karlsruhe University
 *                
 * File path:     glue/v4-powerpc/offsets.h
 * Description:   Addresses used for C++, asm AND linker scripts
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
 * $Id: offsets.h,v 1.5 2003/09/24 19:04:51 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __GLUE__V4_POWERPC__OFFSETS_H__
#define __GLUE__V4_POWERPC__OFFSETS_H__


/* DON'T USE 0x........UL HERE. THE LINKER WILL NOT UNDERSTAND THAT */

/* The offset of the .text section's virtual address.
 * Must be a multiple of the largest page hash table size, 32MB.
 */
#define KERNEL_OFFSET	0xC0000000

/* The offset of the per-cpu data area.  Must be a multiple of the BAT page
 * size used to map the region.
 *
 * The two MMU variants place it differently, and only because d52a5e2 moved
 * something else.  Before that commit the page hash sat at DEVICE_AREA_END,
 * 0xD2000000, and its 32MB ended exactly where the cpu area began; the commit
 * inserted the 16MB pinned and 16MB console areas into that span and repointed
 * PGHASH_AREA_START at CONSOLE_AREA_END, which is 0xD4000000 -- the cpu area's
 * own base.  config.h has asserted the overlap ever since, so no segment-MMU
 * configuration has compiled since 2010.
 *
 * Nothing can go back where it was: there is no free 32MB-aligned 32MB slot
 * left below 0xD4000000, and shrinking PGHASH_AREA_SIZE would cap the largest
 * hash pghash_init is allowed to choose at run time.  Moving the cpu area up
 * is what remains, and 0xD6000000 is forced -- it must clear the hash ending
 * there, stay 128KB-aligned for the BAT that maps it, and leave CPU_AREA_END
 * below KTCB_AREA_START at 0xE0000000, which the 160MB gap above it does.
 *
 * ppc44x keeps 0xD4000000 exactly, so the one PowerPC kernel that runs today
 * is untouched.  This has been verified to build and link, not to boot: there
 * is no ofppc hardware or emulator here.  Notes §144.
 */
#if defined(CONFIG_PPC_MMU_SEGMENTS)
#define KERNEL_CPU_OFFSET	0xD6000000
#else
#define KERNEL_CPU_OFFSET	0xD4000000
#endif

#endif /* !__GLUE__V4_POWERPC__OFFSETS_H__ */
