/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/pghash.h
 * Description:	PowerPC64 page hash handler.
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
 * $Id: pghash.h,v 1.4 2004/06/04 02:52:57 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __GLUE__V4_POWERPC64__PGHASH_H__
#define __GLUE__V4_POWERPC64__PGHASH_H__

#include INC_ARCH(pghash.h)
#include INC_GLUE(pgent.h)

struct space_t;
typedef struct space_t space_t;

struct pghash_t
{
    ppc64_htab_t htab;
};
typedef struct pghash_t pghash_t;

/* Out of line in glue/v4-powerpc64/pghash.c.  try_location and finish_init
   were protected and are file-static there. */
BEGIN_DECLS
bool pghash_init( pghash_t *self, word_t tot_phys_mem );
void pghash_update_mapping( pghash_t *self, space_t *s, addr_t vaddr,
			    pgent_t *pgent, pgsize_e size );
/* The bolted argument defaulted to false; C has no defaults, so the two
   spellings are separate entry points. */
void pghash_insert_mapping_bolted( pghash_t *self, space_t *s, addr_t vaddr,
				   pgent_t *pgent, pgsize_e size, bool bolted );
void pghash_insert_mapping( pghash_t *self, space_t *s, addr_t vaddr,
			    pgent_t *pgent, pgsize_e size );
void pghash_flush_mapping( pghash_t *self, space_t *s, addr_t vaddr,
			   pgent_t *pgent, pgsize_e size );
END_DECLS

INLINE ppc64_htab_t *pghash_get_htab( pghash_t *self ) { return &self->htab; }

INLINE pghash_t *get_pghash(void)
{
    extern pghash_t pghash;
    return &pghash;
}

#endif	/* __GLUE__V4_POWERPC64__PGHASH_H__ */
