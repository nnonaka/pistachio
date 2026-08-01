/*********************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:     arch/powerpc64/stab.c
 * Description:   segment table management
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
 * $Id: stab.cc,v 1.4 2004/06/04 03:40:20 cvansch Exp $
 *                
 ********************************************************************/

#include <debug.h>
#include <kmemory.h>
#include <kdb/tracepoints.h>
#include INC_ARCH(segment.h)
#include INC_GLUE(hwspace.h)

DECLARE_KMEM_GROUP (kmem_stab);

void ppc64_stab_init( ppc64_stab_t *self )
{
    word_t _stab;
    ppc64_stab_t *stab;
    word_t vsid, esid;
    ppc64_ste_t *ste;
    addr_t page = kmem_alloc(&kmem,  kmem_stab, POWERPC64_STAB_SIZE );
    //TRACEF( "created segement table at %p\n", page );

    self->base.raw = (word_t)virt_to_phys( page );
    self->base.x.valid = 1;

    _stab = (word_t)virt_to_phys( page );
    /* Get the segment table */
    stab = (ppc64_stab_t *)&_stab;

    vsid = space_get_vsid( get_kernel_space(), (addr_t)KERNEL_OFFSET );
    esid = ESID( KERNEL_OFFSET );

    ste = ppc64_stab_find_insertion( stab, vsid, esid );
    ppc64_ste_set_entry( ste, esid, 0, 1, 0, vsid );
 
    /* XXX hack hack - were should this happen */
    vsid = space_get_vsid( get_kernel_space(), (addr_t)KTCB_AREA_START );
    esid = ESID( KTCB_AREA_START );
    ste = ppc64_stab_find_insertion( stab, vsid, esid );
    ppc64_ste_set_entry( ste, esid, 0, 1, 0, vsid );

    /* XXX hack hack - were should this happen */
    vsid = space_get_vsid( get_kernel_space(), (addr_t)CPU_AREA_START );
    esid = ESID( CPU_AREA_START );
    ste = ppc64_stab_find_insertion( stab, vsid, esid );
    ppc64_ste_set_entry( ste, esid, 0, 1, 0, vsid );

    /* XXX hack hack - were should this happen */
    vsid = space_get_vsid( get_kernel_space(), (addr_t)0xfffd0000f80003fdul );
    esid = ESID( 0xfffd0000f80003fdul );
    ste = ppc64_stab_find_insertion( stab, vsid, esid );
    ppc64_ste_set_entry( ste, esid, 0, 1, 0, vsid );
}

void ppc64_stab_free( ppc64_stab_t *self )
{
    addr_t page = (addr_t)ppc64_stab_get_stab( self );
    kmem_free(&kmem,  kmem_stab, page, POWERPC64_STAB_SIZE );
}

