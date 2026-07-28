/*********************************************************************
 *                
 * Copyright (C) 2002-2005,  Karlsruhe University
 *                
 * File path:     generic/fpage.h
 * Description:   dummy architecture specific fpage declaration, for use by 
 *		  architectures without such pages
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
 * $Id: generic-archfpage.h,v 1.1 2005/05/19 08:34:59 stoess Exp $
 *                
 ********************************************************************/
#ifndef __GENERIC__FPAGE_H__
#define __GENERIC__FPAGE_H__

#include INC_API(config.h)

struct fpage_t;
struct tcb_t;


/**
 * Flexpages are size-aligned memory objects and can cover multiple hardware
 * pages. arch_fpage_t implements the architecture-specific flexpage type,
 * having read, write and execute bits.
 */
struct arch_fpage_t
{
    /* data members */
    word_t raw;
    /* member functions */

};
typedef struct arch_fpage_t arch_fpage_t;


#endif /* !__GENERIC__FPAGE_H__ */
