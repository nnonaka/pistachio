/*********************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:     glue/v4-powerpc64/hwspace.h
 * Description:   Conversion between kernel addrs and physical addrs 
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
 * $Id: hwspace.h,v 1.3 2004/06/04 02:52:57 cvansch Exp $
 *                
 ********************************************************************/

#ifndef __GLUE__V4_POWERPC64__HWSPACE_H__
#define __GLUE__V4_POWERPC64__HWSPACE_H__

#include <debug.h>	/* for UNIMPLMENTED() */

#include INC_GLUE(offsets.h)

/* Was a template on T, and the return type is T, not void*: callers pass a
   pointer, a word_t and an addr_t, and each expects its own type back.  Fixing
   the parameter at void* would make the word_t callers implicit int/pointer
   conversions.  __typeof__ restores the template, exactly as the 32-bit port's
   glue/v4-powerpc/hwspace.h already does for the same pair.  Notes §140.

   The `+ 0' is the array-to-pointer decay that binding an array to the
   template's by-value `T x' used to perform; __typeof__ alone keeps the array
   type, which is not castable to. */
#define virt_to_phys(x)	((__typeof__((x) + 0)) ((u64_t) (x) - KERNEL_OFFSET))
#define phys_to_virt(x)	((__typeof__((x) + 0)) ((u64_t) (x) + KERNEL_OFFSET))


#endif /* __GLUE__V4_POWERPC64__HWSPACE_H__ */
