/*********************************************************************
 *                
 * Copyright (C) 2003, 2006,  Karlsruhe University
 *                
 * File path:     kdb/glue/v4-ia32/readmem.cc
 * Description:   disasm readmem
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
 * $Id: readmem.cc,v 1.3 2006/05/24 09:33:24 stoess Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>
#include <linear_ptab.h>
#include INC_API(tcb.h)

space_t *current_disas_space = NULL;

/* readmem<char> from generic/linear_ptab.h, specialised for the one byte the
   disassembler asks for (the template itself is C++-only). */
static bool readmem_char (space_t *space, addr_t vaddr, char *v)
{
    if (!space_is_user_area (vaddr))
    {
	/* We are not reading user memory.  Just access it directly */
	*v = *(char *) vaddr;
	return true;
    }

    word_t w;

    /* Check if memory is accessible */
    if (!space_readmem (space, vaddr, &w))
	return false;

    *v = (char) (w & 0xff);
    return true;
}

/* callback for the disassembler to access user mem */
int SECTION(".kdebug") kdb_disas_readmem(char * s, char * d)
{
    if (!current_disas_space)
	current_disas_space = get_kernel_space_c();

    return readmem_char(current_disas_space, s, d);
}


