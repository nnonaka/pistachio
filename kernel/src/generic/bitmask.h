/*********************************************************************
 *                
 * Copyright (C) 2003, 2007-2008,  Karlsruhe University
 *                
 * File path:     generic/bitmask.h
 * Description:   Generic bitmask class
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
 * $Id: bitmask.h,v 1.2 2003/09/24 19:04:24 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __BITMASK_H__
#define __BITMASK_H__



/*
 * Concrete instantiations of bitmask_t<T> used as struct members
 * (bitmask_t<word_t> in tcb_t / resources_t).  In C++ they alias the
 * template above -- identical layout, full operator set.  In C they are the
 * plain backing struct (a single T maskvalue).
 */
struct bitmask_word_t { word_t maskvalue; };
typedef struct bitmask_word_t bitmask_word_t;
struct bitmask_u32_t  { u32_t  maskvalue; };
typedef struct bitmask_u32_t bitmask_u32_t;
struct bitmask_u16_t  { u16_t  maskvalue; };
typedef struct bitmask_u16_t bitmask_u16_t;



#endif /* !__BITMASK_H__ */
