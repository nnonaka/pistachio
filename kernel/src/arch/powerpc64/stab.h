/****************************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:	arch/powerpc64/stab.h
 * Description:	PowerPC64 segment table hash abstractions.
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
 * $Id: stab.h,v 1.3 2004/06/04 02:14:26 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __ARCH__POWERPC64__STAB_H__
#define __ARCH__POWERPC64__STAB_H__

#if !defined(ASSEMBLY)

#include INC_GLUE(hwspace.h)

#define STAB_HASH_MASK		0x1f
#define STAB_STEG_BITS		7

struct ppc64_asr_t
{
    union {
	struct {
	    word_t staborg : 52;	/* Physical address of segment table */
	    word_t reserved : 11;
	    word_t valid : 1;		/* Power3 ASR valid bit */
	} x;
	u64_t raw;
    };
};
typedef struct ppc64_asr_t ppc64_asr_t;


struct ppc64_ste_t
{
    union {
	struct {
	    word_t esid : 36;		/* Effective segment ID */
	    word_t reserved1 : 20;
	    word_t v : 1;		/* Entry valid */
	    word_t t : 1;		/* T = 0 selects this format */
	    word_t ks : 1;		/* Supervisor protection key */
	    word_t kp : 1;		/* User protection key */
	    word_t n : 1;		/* No-execute bit */
	    word_t ste_class : 1;	/* Class */
	    word_t reserved2 : 2;
	    word_t vsid : 52;		/* Virtual Segment ID */
	    word_t reserved3 : 12;
	} x;
	struct {
	    u64_t word0;
	    u64_t word1;
	} raw;
    };
};
typedef struct ppc64_ste_t ppc64_ste_t;


struct ppc64_stab_t
{
    /* was private */
    ppc64_asr_t base;
};
typedef struct ppc64_stab_t ppc64_stab_t;

/* Out of line in arch/powerpc64/stab.c (built only for CONFIG_POWERPC64_STAB,
   i.e. platform ofpower3). */
BEGIN_DECLS
void ppc64_stab_init (ppc64_stab_t *self);
void ppc64_stab_free (ppc64_stab_t *self);
ppc64_ste_t * ppc64_stab_lookup_ste (ppc64_stab_t *self, word_t virt);
word_t ppc64_stab_reverse_hash (ppc64_stab_t *self, ppc64_ste_t *seghash_ste);
END_DECLS

INLINE word_t ppc64_stab_get_asr (ppc64_stab_t *self) { return self->base.raw; }
INLINE word_t ppc64_stab_get_stab (ppc64_stab_t *self)
{ return phys_to_virt(self->base.x.staborg << POWERPC64_PAGE_BITS); }

INLINE ppc64_ste_t * ppc64_stab_get_steg (ppc64_stab_t *self, word_t hash)
{
    return (ppc64_ste_t*)(ppc64_stab_get_stab (self) |
			  ((hash & STAB_HASH_MASK) << STAB_STEG_BITS));
}

/* were private */
INLINE word_t ppc64_stab_primary_hash( word_t esid )
{
    return (esid & STAB_HASH_MASK);
}

INLINE word_t ppc64_stab_secondary_hash( word_t esid )
{
    return ((~esid) & STAB_HASH_MASK);
}


INLINE void ppc64_ste_set_entry( ppc64_ste_t *self, word_t esid, word_t ks, word_t kp, word_t n, word_t vsid )
{
    self->raw.word1 = 0;
    self->x.vsid = vsid;
    /* Order VSID updte */
    __asm__ __volatile__ ("eieio" : : : "memory");
    self->raw.word0 = 0;
    self->x.esid = esid;
    self->x.v = 1;
    self->x.ks = ks;
    self->x.kp = kp;
    self->x.n = n;
    /* Order update     */
    __asm__ __volatile__ ("sync" : : : "memory");
}

INLINE void ppc64_ste_invalidate( ppc64_ste_t *self, bool sync )
{
    self->x.v = 0;

    if ( sync )
    {
	/* Order update     */
	__asm__ __volatile__ ("sync" : : : "memory");
    }
}

INLINE ppc64_ste_t *ppc64_stab_find_insertion( ppc64_stab_t *self, word_t vsid, word_t esid )
{
    word_t group, entry, random;
    ppc64_ste_t *ste = ppc64_stab_get_steg( self, ppc64_stab_primary_hash( esid ) );

    for( group = 0; group < 2; group ++ )
    {
	for( entry = 0; entry < 8; entry ++, ste++ )
	{
	    if (ste->x.v == 0)	    /* Invalid entry */
	    {
		return ste;	    /* Return the entry */
	    }
	}
	
	ste = ppc64_stab_get_steg( self, ppc64_stab_secondary_hash( esid ) );
    }

    /* No free entry found, need to evict one */

    do {
	__asm__ __volatile__ ("mftbl    %0;" : "=r" (random));


	if (random & 0x8)
	    ste = ppc64_stab_get_steg( self, ppc64_stab_primary_hash( esid ) );
	else
	    ste = ppc64_stab_get_steg( self, ppc64_stab_secondary_hash( esid ) );

	ste = &ste[random & 0x7];
    } while (((ste->x.esid >> 20) >= 0xfff0) && (!(random & 0x8))); /* Don't evict kernel entries */

    /* Force previous translations to complete. DRENG */
    __asm__ __volatile__ ("isync" : : : "memory" );

    ppc64_ste_invalidate( ste, true );

    return ste;
}


#endif	/* !ASSEMBLY */

#endif /* __ARCH__POWERPC64__STAB_H__ */
