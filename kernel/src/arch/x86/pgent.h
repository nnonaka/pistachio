/*********************************************************************
 *                
 * Copyright (C) 2007-2008, 2012,  Karlsruhe University
 *                
 * File path:     arch/x86/pgent.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __ARCH__X86__PGENT_H__
#define __ARCH__X86__PGENT_H__

#include <kmemory.h>
#include <debug.h>
#include INC_GLUE(hwspace.h)
#include INC_ARCH(mmu.h)
#include INC_ARCH_SA(ptab.h)

#if defined(CONFIG_X86_SMALL_SPACES)
#include INC_GLUE_SA(smallspaces.h)
#endif

EXTERN_KMEM_GROUP (kmem_pgtab);

#if defined(CONFIG_NEW_MDB)
#define mapnode_t mdb_node_t
#endif


struct mapnode_t;
struct space_t;

struct pgent_t
{
    union {
	x86_pgent_t    pgent;
	word_t		raw;
    };

};
typedef struct pgent_t pgent_t;

/* C wrappers for the pgent_t methods above (defined in glue/v4-x86/space.cc)
   so generic/linear_ptab_walker.c can drive the page tables. pgsize is a
   word_t holding an X86_PGSIZE_* value; the wrappers cast it to pgsize_e. */
BEGIN_DECLS
bool      pgent_is_valid       (pgent_t *self, struct space_t *s, word_t pgsize);
bool      pgent_is_writable    (pgent_t *self, struct space_t *s, word_t pgsize);
bool      pgent_is_readable    (pgent_t *self, struct space_t *s, word_t pgsize);
bool      pgent_is_executable  (pgent_t *self, struct space_t *s, word_t pgsize);
bool      pgent_is_subtree     (pgent_t *self, struct space_t *s, word_t pgsize);
addr_t    pgent_address        (pgent_t *self, struct space_t *s, word_t pgsize);
word_t    pgent_attributes     (pgent_t *self, struct space_t *s, word_t pgsize);
pgent_t * pgent_subtree        (pgent_t *self, struct space_t *s, word_t pgsize);
pgent_t * pgent_next           (pgent_t *self, struct space_t *s, word_t pgsize, word_t num);
struct mapnode_t * pgent_mapnode      (pgent_t *self, struct space_t *s, word_t pgsize, addr_t vaddr);
void      pgent_clear          (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel, addr_t vaddr);
void      pgent_make_subtree   (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel);
void      pgent_remove_subtree (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel);
void      pgent_set_entry      (pgent_t *self, struct space_t *s, word_t pgsize, paddr_t paddr, word_t rwx, word_t attrib, bool kernel);
void      pgent_set_linknode   (pgent_t *self, struct space_t *s, word_t pgsize, struct mapnode_t *map, addr_t vaddr);
void      pgent_update_rights  (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx);
/* Additional entries used by the mapping database (mapping.c). */
addr_t    pgent_vaddr          (pgent_t *self, struct space_t *s, word_t pgsize, struct mapnode_t *map);
word_t    pgent_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize, addr_t vaddr);
void      pgent_reset_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize);
void      pgent_update_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx);
void      pgent_revoke_rights  (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx);
/* Entries used by the new mapping database (generic/mdb_mem.c). */
word_t    pgent_rights         (pgent_t *self, struct space_t *s, word_t pgsize);
void      pgent_set_rights     (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx);
void      pgent_set_attributes (pgent_t *self, struct space_t *s, word_t pgsize, word_t attrib);
void      pgent_flush          (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel, addr_t vaddr);
/* Entries used by the AMD64 SMP page-table sync (glue/v4-x86/x64/space.c). */
word_t    pgent_idx            (pgent_t *self);
bool      pgent_is_cpulocal    (pgent_t *self, struct space_t *s, word_t pgsize);
void      pgent_smp_sync       (pgent_t *self, struct space_t *s, word_t pgsize);
word_t    pgent_smp_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize, addr_t vaddr);
void      pgent_make_cpu_subtree (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel);
END_DECLS

/* C forms of the pgent_t modifier methods used only by glue/v4-x86/space.c:
   bit-twiddling on the C-visible x86_pgent_t union (pgsize is X86_PGSIZE_*).
   pgent_sync mirrors pgent_t::sync -> smp_sync. */
INLINE void pgent_sync (pgent_t *self, struct space_t *s, word_t pgsize)
{
#if defined(CONFIG_SMP)
    if (pgsize >= X86_PGSIZE_SYNC)
	pgent_smp_sync (self, s, pgsize);
#else
    /* pgent_t::sync was `{ }' in the non-SMP branch (notes §118). */
    (void) self; (void) s; (void) pgsize;
#endif
}

INLINE void pgent_set_global (pgent_t *self, struct space_t *s, word_t pgsize, bool global)
{
#if defined(CONFIG_X86_PGE)
    x86_pgent_set_global (&self->pgent, global);
    pgent_sync (self, s, pgsize);
#endif
}

INLINE void pgent_set_cpulocal (pgent_t *self, struct space_t *s, word_t pgsize, bool local)
{
    (void) s; (void) pgsize;
    x86_pgent_set_cpulocal (&self->pgent, local);
}

INLINE bool pgent_is_kernel (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return x86_pgent_is_kernel (&self->pgent); }

/* pgent_t::dump_misc (kdb) -- native C, not a bridge: every accessor it needs
   already has an x86_pgent_* C form. */
INLINE void pgent_dump_misc (pgent_t *self, struct space_t *s, word_t pgsize)
{
    (void) s;
    if (x86_pgent_is_global (&self->pgent))
	printf ("global ");

    if (x86_pgent_is_cpulocal (&self->pgent))
	printf ("local ");

#if defined(CONFIG_X86_PAT)
    if (x86_pgent_is_pat (&self->pgent, pgsize))
	printf (x86_pgent_is_cache_disabled (&self->pgent) ? "WP" :
		x86_pgent_is_write_through (&self->pgent)  ? "WT" : "WC");
    else
#endif
	printf (x86_pgent_is_cache_disabled (&self->pgent) ? "UC" :
		x86_pgent_is_write_through (&self->pgent)  ? "WT" : "WB");
}

INLINE void pgent_set_cacheability (pgent_t *self, struct space_t *s, word_t pgsize, bool cacheable)
{
    x86_pgent_set_cacheability (&self->pgent, cacheable, pgsize);
    pgent_sync (self, s, pgsize);
}

#if defined(CONFIG_NEW_MDB)
#undef mapnode_t
#endif

#endif /* !__ARCH__X86__PGENT_H__ */
