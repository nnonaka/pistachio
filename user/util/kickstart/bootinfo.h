/*********************************************************************
 *                
 * Copyright (C) 2004-2006,  Karlsruhe University
 *                
 * File path:     bootinfo.h
 * Description:   generic bootinfo creation functions
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
 * $Id: bootinfo.h,v 1.2 2006/10/22 19:38:02 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __KICKSTART__BOOTINFO_H__
#define __KICKSTART__BOOTINFO_H__

#include <config.h>

#include "mbi.h"

#if defined(__L4__BOOTINFO_H__)
#error do not include <l4/bootinfo.h> before including this file
#endif

/* Upstream included <l4/bootinfo.h> twice, once inside `namespace BI32' and
   once inside `namespace BI64', each with L4_Word_t locally typedef'd to the
   width it wanted -- so one header generated two complete sets of bootinfo
   types.  C has no namespaces, so the same double inclusion is done with the
   preprocessor: every type and inline function the header declares is renamed
   to a BI32_/BI64_ prefixed one for the duration, then undefined again.

   The six #defined constants are deliberately NOT renamed: a #define cannot
   rename itself, and their bodies would in any case refer to an L4_Word_t that
   is no longer in scope by the time they expand.  They are width-independent
   at every use here -- each is assigned to a struct field that does the
   conversion.  The name list must be kept in step with l4/bootinfo.h.
   See doc/notes/cpp-to-c-migration.md §173. */

/* ---- BI32: <l4/bootinfo.h> with L4_Word_t = L4_Word32_t ---- */
typedef L4_Word32_t BI32_L4_Word_t;

#define L4_Word_t	L4_Word32_t
#define L4_BootInfo_Entries	BI32_L4_BootInfo_Entries
#define L4_BootInfo_FirstEntry	BI32_L4_BootInfo_FirstEntry
#define L4_BootInfo_Size	BI32_L4_BootInfo_Size
#define L4_BootInfo_Valid	BI32_L4_BootInfo_Valid
#define L4_BootInfo_t	BI32_L4_BootInfo_t
#define L4_BootRec_Next	BI32_L4_BootRec_Next
#define L4_BootRec_Type	BI32_L4_BootRec_Type
#define L4_BootRec_t	BI32_L4_BootRec_t
#define L4_Boot_EFI_t	BI32_L4_Boot_EFI_t
#define L4_Boot_MBI_t	BI32_L4_Boot_MBI_t
#define L4_Boot_Module_t	BI32_L4_Boot_Module_t
#define L4_Boot_SimpleExec_t	BI32_L4_Boot_SimpleExec_t
#define L4_EFI_MemdescSize	BI32_L4_EFI_MemdescSize
#define L4_EFI_MemdescVersion	BI32_L4_EFI_MemdescVersion
#define L4_EFI_Memmap	BI32_L4_EFI_Memmap
#define L4_EFI_MemmapSize	BI32_L4_EFI_MemmapSize
#define L4_EFI_Systab	BI32_L4_EFI_Systab
#define L4_MBI_Address	BI32_L4_MBI_Address
#define L4_Module_Cmdline	BI32_L4_Module_Cmdline
#define L4_Module_Size	BI32_L4_Module_Size
#define L4_Module_Start	BI32_L4_Module_Start
#define L4_Next	BI32_L4_Next
#define L4_SimpleExec_BssPstart	BI32_L4_SimpleExec_BssPstart
#define L4_SimpleExec_BssSize	BI32_L4_SimpleExec_BssSize
#define L4_SimpleExec_BssVstart	BI32_L4_SimpleExec_BssVstart
#define L4_SimpleExec_Cmdline	BI32_L4_SimpleExec_Cmdline
#define L4_SimpleExec_DataPstart	BI32_L4_SimpleExec_DataPstart
#define L4_SimpleExec_DataSize	BI32_L4_SimpleExec_DataSize
#define L4_SimpleExec_DataVstart	BI32_L4_SimpleExec_DataVstart
#define L4_SimpleExec_Flags	BI32_L4_SimpleExec_Flags
#define L4_SimpleExec_InitialIP	BI32_L4_SimpleExec_InitialIP
#define L4_SimpleExec_Label	BI32_L4_SimpleExec_Label
#define L4_SimpleExec_Set_Flags	BI32_L4_SimpleExec_Set_Flags
#define L4_SimpleExec_Set_Label	BI32_L4_SimpleExec_Set_Label
#define L4_SimpleExec_TextPstart	BI32_L4_SimpleExec_TextPstart
#define L4_SimpleExec_TextSize	BI32_L4_SimpleExec_TextSize
#define L4_SimpleExec_TextVstart	BI32_L4_SimpleExec_TextVstart
#define L4_Type	BI32_L4_Type

#include <l4/bootinfo.h>
#undef __L4__BOOTINFO_H__

#undef L4_Word_t
#undef L4_BootInfo_Entries
#undef L4_BootInfo_FirstEntry
#undef L4_BootInfo_Size
#undef L4_BootInfo_Valid
#undef L4_BootInfo_t
#undef L4_BootRec_Next
#undef L4_BootRec_Type
#undef L4_BootRec_t
#undef L4_Boot_EFI_t
#undef L4_Boot_MBI_t
#undef L4_Boot_Module_t
#undef L4_Boot_SimpleExec_t
#undef L4_EFI_MemdescSize
#undef L4_EFI_MemdescVersion
#undef L4_EFI_Memmap
#undef L4_EFI_MemmapSize
#undef L4_EFI_Systab
#undef L4_MBI_Address
#undef L4_Module_Cmdline
#undef L4_Module_Size
#undef L4_Module_Start
#undef L4_Next
#undef L4_SimpleExec_BssPstart
#undef L4_SimpleExec_BssSize
#undef L4_SimpleExec_BssVstart
#undef L4_SimpleExec_Cmdline
#undef L4_SimpleExec_DataPstart
#undef L4_SimpleExec_DataSize
#undef L4_SimpleExec_DataVstart
#undef L4_SimpleExec_Flags
#undef L4_SimpleExec_InitialIP
#undef L4_SimpleExec_Label
#undef L4_SimpleExec_Set_Flags
#undef L4_SimpleExec_Set_Label
#undef L4_SimpleExec_TextPstart
#undef L4_SimpleExec_TextSize
#undef L4_SimpleExec_TextVstart
#undef L4_Type

BI32_L4_BootRec_t * BI32_init_bootinfo (BI32_L4_BootInfo_t * bi);

BI32_L4_BootRec_t * BI32_record_bootinfo_modules (BI32_L4_BootInfo_t * bi,
						BI32_L4_BootRec_t * rec,
						mbi_t * mbi,
						mbi_module_t orig_mbi_modules[],
						unsigned int decode_count);

BI32_L4_BootRec_t * BI32_record_bootinfo_mbi (BI32_L4_BootInfo_t * bi,
					    BI32_L4_BootRec_t * rec,
					    mbi_t * mbi);

/* ---- BI64: <l4/bootinfo.h> with L4_Word_t = L4_Word64_t ---- */
typedef L4_Word64_t BI64_L4_Word_t;

#define L4_Word_t	L4_Word64_t
#define L4_BootInfo_Entries	BI64_L4_BootInfo_Entries
#define L4_BootInfo_FirstEntry	BI64_L4_BootInfo_FirstEntry
#define L4_BootInfo_Size	BI64_L4_BootInfo_Size
#define L4_BootInfo_Valid	BI64_L4_BootInfo_Valid
#define L4_BootInfo_t	BI64_L4_BootInfo_t
#define L4_BootRec_Next	BI64_L4_BootRec_Next
#define L4_BootRec_Type	BI64_L4_BootRec_Type
#define L4_BootRec_t	BI64_L4_BootRec_t
#define L4_Boot_EFI_t	BI64_L4_Boot_EFI_t
#define L4_Boot_MBI_t	BI64_L4_Boot_MBI_t
#define L4_Boot_Module_t	BI64_L4_Boot_Module_t
#define L4_Boot_SimpleExec_t	BI64_L4_Boot_SimpleExec_t
#define L4_EFI_MemdescSize	BI64_L4_EFI_MemdescSize
#define L4_EFI_MemdescVersion	BI64_L4_EFI_MemdescVersion
#define L4_EFI_Memmap	BI64_L4_EFI_Memmap
#define L4_EFI_MemmapSize	BI64_L4_EFI_MemmapSize
#define L4_EFI_Systab	BI64_L4_EFI_Systab
#define L4_MBI_Address	BI64_L4_MBI_Address
#define L4_Module_Cmdline	BI64_L4_Module_Cmdline
#define L4_Module_Size	BI64_L4_Module_Size
#define L4_Module_Start	BI64_L4_Module_Start
#define L4_Next	BI64_L4_Next
#define L4_SimpleExec_BssPstart	BI64_L4_SimpleExec_BssPstart
#define L4_SimpleExec_BssSize	BI64_L4_SimpleExec_BssSize
#define L4_SimpleExec_BssVstart	BI64_L4_SimpleExec_BssVstart
#define L4_SimpleExec_Cmdline	BI64_L4_SimpleExec_Cmdline
#define L4_SimpleExec_DataPstart	BI64_L4_SimpleExec_DataPstart
#define L4_SimpleExec_DataSize	BI64_L4_SimpleExec_DataSize
#define L4_SimpleExec_DataVstart	BI64_L4_SimpleExec_DataVstart
#define L4_SimpleExec_Flags	BI64_L4_SimpleExec_Flags
#define L4_SimpleExec_InitialIP	BI64_L4_SimpleExec_InitialIP
#define L4_SimpleExec_Label	BI64_L4_SimpleExec_Label
#define L4_SimpleExec_Set_Flags	BI64_L4_SimpleExec_Set_Flags
#define L4_SimpleExec_Set_Label	BI64_L4_SimpleExec_Set_Label
#define L4_SimpleExec_TextPstart	BI64_L4_SimpleExec_TextPstart
#define L4_SimpleExec_TextSize	BI64_L4_SimpleExec_TextSize
#define L4_SimpleExec_TextVstart	BI64_L4_SimpleExec_TextVstart
#define L4_Type	BI64_L4_Type

#include <l4/bootinfo.h>
#undef __L4__BOOTINFO_H__

#undef L4_Word_t
#undef L4_BootInfo_Entries
#undef L4_BootInfo_FirstEntry
#undef L4_BootInfo_Size
#undef L4_BootInfo_Valid
#undef L4_BootInfo_t
#undef L4_BootRec_Next
#undef L4_BootRec_Type
#undef L4_BootRec_t
#undef L4_Boot_EFI_t
#undef L4_Boot_MBI_t
#undef L4_Boot_Module_t
#undef L4_Boot_SimpleExec_t
#undef L4_EFI_MemdescSize
#undef L4_EFI_MemdescVersion
#undef L4_EFI_Memmap
#undef L4_EFI_MemmapSize
#undef L4_EFI_Systab
#undef L4_MBI_Address
#undef L4_Module_Cmdline
#undef L4_Module_Size
#undef L4_Module_Start
#undef L4_Next
#undef L4_SimpleExec_BssPstart
#undef L4_SimpleExec_BssSize
#undef L4_SimpleExec_BssVstart
#undef L4_SimpleExec_Cmdline
#undef L4_SimpleExec_DataPstart
#undef L4_SimpleExec_DataSize
#undef L4_SimpleExec_DataVstart
#undef L4_SimpleExec_Flags
#undef L4_SimpleExec_InitialIP
#undef L4_SimpleExec_Label
#undef L4_SimpleExec_Set_Flags
#undef L4_SimpleExec_Set_Label
#undef L4_SimpleExec_TextPstart
#undef L4_SimpleExec_TextSize
#undef L4_SimpleExec_TextVstart
#undef L4_Type

BI64_L4_BootRec_t * BI64_init_bootinfo (BI64_L4_BootInfo_t * bi);

BI64_L4_BootRec_t * BI64_record_bootinfo_modules (BI64_L4_BootInfo_t * bi,
						BI64_L4_BootRec_t * rec,
						mbi_t * mbi,
						mbi_module_t orig_mbi_modules[],
						unsigned int decode_count);

BI64_L4_BootRec_t * BI64_record_bootinfo_mbi (BI64_L4_BootInfo_t * bi,
					    BI64_L4_BootRec_t * rec,
					    mbi_t * mbi);

#endif /* !__KICKSTART__BOOTINFO_H__ */
