/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2008,  Karlsruhe University
 *                
 * File path:     generic/linear_ptab.h
 * Description:   Helper functions for generic linear page table
 *                manipulation.
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
 * $Id: linear_ptab.h,v 1.11 2004/04/28 16:37:45 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __LINEAR_PTAB_H__
#define __LINEAR_PTAB_H__

#include INC_ARCH(pgent.h)
#include INC_GLUE(space.h)


/**
 * Array containing the actual page sizes (as bit-shifts) for the
 * various page table numbers.  Array is indexed by page size number
 * (i.e., word_t).  Last entry must be the bit-shift for the
 * complete address space.
 */
extern word_t hw_pgshifts[];



/* C forms of the readmem<T> template above.  C has no templates, so one
   function per width actually used in the tree; each mirrors the template
   body exactly (direct access outside the user area, otherwise a checked
   space_readmem plus a mask).  is_user_area is static in C++, hence no
   space argument. */
INLINE bool readmem_u8 (space_t * space, addr_t vaddr, u8_t * v)
{
    word_t w;

    if (! space_is_user_area (vaddr))
    {
	*v = *(u8_t *) vaddr;
	return true;
    }
    if (! space_readmem (space, vaddr, &w))
	return false;
    *v = (u8_t) (w & 0xff);
    return true;
}

INLINE bool readmem_u32 (space_t * space, addr_t vaddr, u32_t * v)
{
    word_t w;

    if (! space_is_user_area (vaddr))
    {
	*v = *(u32_t *) vaddr;
	return true;
    }
    if (! space_readmem (space, vaddr, &w))
	return false;
    *v = (u32_t) (w & 0xffffffff);
    return true;
}

INLINE bool readmem_s32 (space_t * space, addr_t vaddr, s32_t * v)
{
    word_t w;

    if (! space_is_user_area (vaddr))
    {
	*v = *(s32_t *) vaddr;
	return true;
    }
    if (! space_readmem (space, vaddr, &w))
	return false;
    *v = (s32_t) (w & 0xffffffff);
    return true;
}

INLINE bool readmem_word (space_t * space, addr_t vaddr, word_t * v)
{
    word_t w;

    if (! space_is_user_area (vaddr))
    {
	*v = *(word_t *) vaddr;
	return true;
    }
    if (! space_readmem (space, vaddr, &w))
	return false;
    *v = w;
    return true;
}

/* readmem<u64_t>, used only by the HVM GDT/IDT dump.  The template's `case 8'
   assigned the word_t it read straight through, so on a 32-bit word only the
   low half of the descriptor comes back from user memory; kept as it was. */
INLINE bool readmem_u64 (space_t * space, addr_t vaddr, u64_t * v)
{
    word_t w;

    if (! space_is_user_area (vaddr))
    {
	*v = *(u64_t *) vaddr;
	return true;
    }
    if (! space_readmem (space, vaddr, &w))
	return false;
    *v = (u64_t) w;
    return true;
}

/* C reimplementations of the page-geometry helpers. The C++ versions above
   take word_t; C passes a word_t holding an X86_PGSIZE_* value. */
INLINE word_t page_size (word_t pgsize)
{ return 1UL << hw_pgshifts[pgsize]; }

INLINE word_t page_shift (word_t pgsize)
{ return hw_pgshifts[pgsize]; }

INLINE word_t page_mask (word_t pgsize)
{ return (1UL << hw_pgshifts[pgsize]) - 1; }

INLINE word_t page_table_size (word_t pgsize)
{ return 1UL << (hw_pgshifts[pgsize+1] - hw_pgshifts[pgsize]); }

INLINE word_t page_table_index (word_t pgsize, addr_t vaddr)
{ return ((word_t) vaddr >> hw_pgshifts[pgsize]) & (page_table_size (pgsize) - 1); }

INLINE bool is_page_size_valid (word_t pgsize)
{ return ((1UL << hw_pgshifts[pgsize]) & HW_VALID_PGSIZES) != 0; }



#endif /* !__LINEAR_PTAB_H__ */
