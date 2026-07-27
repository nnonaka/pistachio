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


#if defined(__cplusplus)
class mapnode_t;
class space_t;
#else
struct mapnode_t;
struct space_t;
#endif

struct pgent_t
{
    union {
	x86_pgent_t    pgent;
	word_t		raw;
    };

#if defined(__cplusplus)
    enum pgsize_e X86_PGSIZES;

private:

    // Linknode access
#if !defined(CONFIG_SMP)
    word_t * __linknode_ptr(pgsize_e pgsize)
	{
	    if (pgsize == size_superpage)
		return (word_t *) ((word_t) this - X86_PTAB_BYTES);
	    else
		return (word_t *) ((word_t) this + X86_PTAB_BYTES);
	}
public:
    void sync (space_t * s, pgsize_e pgsize) { }

#else /* CONFIG_SMP */

private:
   
    //
    word_t * __linknode_ptr(pgsize_e pgsize)
	{
	    if (pgsize == size_max) 
	    {
		word_t space = ((word_t*)(word_t(this) & X86_PAGE_MASK))[SPACE_BACKLINK >> X86_TOP_PDIR_BITS];
		return (word_t*)(space + (word_t(this) & ~X86_PAGE_MASK));
	    } else
		return (word_t *) ((word_t) this + X86_PAGE_SIZE);
	}

    /* Defined in C (glue/v4-x86/x64/space.c); the pgsize_e args widen to
       word_t at the C++ call sites so the C word_t params match by ABI. */
    void smp_sync(space_t * s, word_t pgsize) __asm__ ("pgent_smp_sync");
    word_t smp_reference_bits(space_t * s, word_t pgsize, addr_t vaddr) __asm__ ("pgent_smp_reference_bits");

public:

    void sync (space_t * s, pgsize_e pgsize)
	{ if (pgsize >= size_sync) smp_sync(s, pgsize); }
    
    
    // creates a CPU local subtree for CPU local data
    void make_cpu_subtree (space_t * s, pgsize_e pgsize, bool kernel)
	{
	    ASSERT(kernel); // cpu-local subtrees are _always_ kernel
	    pgent.set_ptab_entry
		(virt_to_phys(kmem_alloc(&kmem, kmem_pgtab, X86_PTAB_BYTES)), X86_PAGE_USER | X86_PAGE_WRITABLE);

	}

#endif /* defined(CONFIG_SMP) */

public:
    
    word_t get_linknode (pgsize_e pgsize)
	{ return *__linknode_ptr(pgsize); }

    bool set_linknode_atomic (pgsize_e pgsize, word_t val, word_t old_val)
	{
	    bool result;
	    word_t * ptr = __linknode_ptr(pgsize);
	    asm("lock; cmpxchg %2, %3\n"
		"setz %0\n"
		: "=r"(result)
		: "a"(old_val), "r"(val), "m"(*ptr));
	    return result;
	}


    void set_linknode (pgsize_e pgsize, word_t val)
	{ 
	    *__linknode_ptr(pgsize) = val;
	}

    void set_linknode (space_t * s, pgsize_e pgsize,
		       mapnode_t * map, addr_t vaddr)
	{ set_linknode (pgsize, (word_t) map ^ (word_t) vaddr); }



    // Predicates

    bool is_valid (space_t * s, pgsize_e pgsize)
	{ return pgent.is_valid (); }
    
    bool is_writable (space_t * s, pgsize_e pgsize)
	{ return pgent.is_writable (); }
    
    bool is_readable (space_t * s, pgsize_e pgsize)
	{ return pgent.is_valid(); }
    
    bool is_executable (space_t * s, pgsize_e pgsize)
	{ return !pgent.is_executable(); }

    bool is_subtree (space_t * s, pgsize_e pgsize)
	{ return !(pgsize == size_4k || (pgsize == size_superpage && pgent.is_superpage())); }

    bool is_kernel (space_t * s, pgsize_e pgsize)
	{ return pgent.is_kernel(); }

    bool is_global (space_t * s, pgsize_e pgsize)
	{ return pgent.is_global(); }

    bool is_cpulocal (space_t * s, pgsize_e pgsize)
	{ return pgent.is_cpulocal (); }

    // Retrieval

    
    addr_t address (space_t * s, pgsize_e pgsize)
	{ return pgent.get_address ((x86_pgent_t::pagesize_e) pgsize); }

    pgent_t * subtree (space_t * s, pgsize_e pgsize)
	{ return (pgent_t *) phys_to_virt (pgent.get_ptab ()); }

    mapnode_t * mapnode (space_t * s, pgsize_e pgsize, addr_t vaddr) 
	{ return (mapnode_t *) (get_linknode (pgsize) ^ (word_t) vaddr); }

    addr_t vaddr (space_t * s, pgsize_e pgsize, mapnode_t * map) 
	{ return (addr_t) (get_linknode(pgsize) ^ (word_t) map); }

    word_t rights (space_t * s, pgsize_e pgsize)
	{ return ((1<<2) | 
		  (pgent.is_writable() ? (1<<1) : 0) | 
		  (pgent.is_executable() ? (1<<0) : 0));
	}

    word_t reference_bits (space_t * s, pgsize_e pgsize, addr_t vaddr)
	{
#if defined(CONFIG_SMP)
	    if (pgsize == size_superpage && !pgent.is_cpulocal()) 
		return smp_reference_bits(s, pgsize, vaddr);
#endif
	    word_t rwx = 0;
	    if (pgent.is_accessed ()) { rwx |= 5; }
	    if (pgent.is_dirty ()) { rwx |= 6; }
	    return rwx;
	}


    word_t attributes (space_t * s, pgsize_e pgsize)
	{
	    return (pgent.is_write_through() ? 1 : 0) |
		(pgent.is_cache_disabled() ? 2 : 0) |
		(pgent.is_pat((x86_pgent_t::pagesize_e) pgsize) ? 4 : 0);
	}
    
    word_t idx()
        { return (((word_t)this & (word_t)(X86_PTAB_BYTES - 1)) / sizeof(pgent_t)); }


    
    // Modification
    
    void clear (space_t * s, pgsize_e pgsize, bool kernel, addr_t vaddr)
	{
	    pgent.clear ();
	    sync (s, pgsize);

	    if (! kernel)
		set_linknode (pgsize, 0);

#if defined(CONFIG_X86_SMALL_SPACES)
	    if (pgsize == size_4m && is_smallspace(s))
		enter_kdebug ("Removing 4M mapping from small space");
#endif
	}


    void flush (space_t * s, pgsize_e pgsize, bool kernel, addr_t vaddr)
	{
	}

    void make_subtree (space_t * s, pgsize_e pgsize, bool kernel)
	{
	    word_t size = 
		(!kernel && (pgsize == size_4k || pgsize == size_superpage))
		? 2 * X86_PTAB_BYTES : X86_PTAB_BYTES;
	    
	    pgent.set_ptab_entry
		(virt_to_phys
		 (kmem_alloc(&kmem, kmem_pgtab, size)), X86_PAGE_USER | X86_PAGE_WRITABLE);

	    sync(s, pgsize);
	}


        
    void remove_subtree (space_t * s, pgsize_e pgsize, bool kernel)
	{
	    addr_t ptab = pgent.get_ptab ();
	    pgent.clear();
	    sync(s, pgsize);
	    word_t size = 
		(!kernel && (pgsize == size_4k || pgsize == size_superpage)) 
		? 2 * X86_PTAB_BYTES : X86_PTAB_BYTES;

	    kmem_free(&kmem, kmem_pgtab, phys_to_virt (ptab), size);

#if defined(CONFIG_X86_SMALL_SPACES)
	    if (is_smallspace (s))
		enter_kdebug ("Removing subtree from small space");
#endif
	}

    void set_entry (space_t * s, pgsize_e pgsize, paddr_t paddr,
		    word_t rwx, word_t attrib, bool kernel)
	{
	    pgent.set_entry (paddr,
			     (x86_pgent_t::pagesize_e) pgsize,
			     (kernel ? X86_PAGE_KERNEL : X86_PAGE_USER) |
#if defined(CONFIG_X86_PGE)
			     (kernel ? X86_PAGE_GLOBAL : 0) |
#endif
			     (attrib & 1 ? X86_PAGE_WRITE_THROUGH : 0) |
			     (attrib & 2 ? X86_PAGE_CACHE_DISABLE : 0) |
#if defined(CONFIG_X86_PAT)
			     (attrib & 4 ?
			      (1UL << (pgsize == size_4k ? 7 : 12)) : 0) |
#endif
			     (rwx & 2 ? X86_PAGE_WRITABLE : 0) |
#if defined(CONFIG_X86_NX)
			     (rwx & 1 ? 0 : X86_PAGE_NX) |
#endif
			     X86_PAGE_VALID);

#if defined(CONFIG_X86_SMALL_SPACES_GLOBAL)
	    if ((! kernel) && is_smallspace(s))
		pgent.set_global (true);
#endif
	    sync(s, pgsize);

	} 
    
    
    void set_entry (space_t * s, pgsize_e pgsize, pgent_t pgent)
	{
	    this->pgent = pgent.pgent;
	    sync(s, pgsize);
	}
	    
	    
    void set_cacheability (space_t * s, pgsize_e pgsize, bool cacheable)
	{
	    this->pgent.set_cacheability(cacheable, 
					 (x86_pgent_t::pagesize_e) pgsize);
	    sync(s, pgsize);
	}

    void set_attributes (space_t * s, pgsize_e pgsize, word_t attrib)
	{
	    this->pgent.set_pat(attrib, (x86_pgent_t::pagesize_e) pgsize);
	    sync(s, pgsize);
	}

    void set_global (space_t * s, pgsize_e pgsize, bool global)
	{
#if defined(CONFIG_X86_PGE)
	    pgent.set_global (global);
	    sync (s, pgsize);
#endif
	}

    void set_cpulocal (space_t * s, pgsize_e pgsize, bool local)
	{ pgent.set_cpulocal (local); }

    
    // Rights

    void revoke_rights (space_t * s, pgsize_e pgsize, word_t rwx)
	{ 
	    if (rwx & 2) raw &= ~X86_PAGE_WRITABLE; 
#if defined(CONFIG_X86_NX)
	    if (rwx & 1) raw |= X86_PAGE_NX; 
#endif
	    sync(s, pgsize); 
	}

    void update_rights (space_t * s, pgsize_e pgsize, word_t rwx)
	{ 
	    if (rwx & 2) raw |= X86_PAGE_WRITABLE;
#if defined(CONFIG_X86_NX)
	    if (rwx & 1) raw &= ~X86_PAGE_NX;
#endif
	    sync(s, pgsize);
	}
    
    void set_rights (space_t * s, pgsize_e pgsize, word_t rwx)
	{
	    bool mod = false;
	    if ((rwx & 2) && ! (raw & X86_PAGE_WRITABLE))
	    { raw |= X86_PAGE_WRITABLE; mod = true; }
	    else if (! (rwx & 2) && (raw & X86_PAGE_WRITABLE))
	    { raw &= ~X86_PAGE_WRITABLE; mod = true; }
#if defined(CONFIG_X86_NX)
	    if ((rwx & 1) && (raw | X86_PAGE_NX))
	    { raw &= ~X86_PAGE_NX; mod = true; }  
	    else if (! (rwx & 1) && ! (raw & X86_PAGE_NX))
	    { raw |= X86_PAGE_NX; mod = true; }
#endif
	    if (mod) sync (s, pgsize);

	}

    
    void reset_reference_bits (space_t * s, pgsize_e pgsize)
	{ raw &= ~(X86_PAGE_ACCESSED | X86_PAGE_DIRTY); sync(s, pgsize); }
    
    void update_reference_bits (space_t * s, pgsize_e pgsize, word_t rwx)
	{ raw |= ((rwx >> 1) & 0x3) << 5; }
    
    // Movement
    
    pgent_t * next (space_t * s, pgsize_e pgsize, word_t num)
	{ return this + num; }

        // Debug

    void dump_misc (space_t * s, pgsize_e pgsize)
	{
	    if (pgent.is_global())
		printf ("global ");
	    
	    if (pgent.is_cpulocal())
		printf ("local ");

#if defined(CONFIG_X86_PAT)
	    if (pgent.is_pat((x86_pgent_t::pagesize_e) pgsize))
		printf (pgent.is_cache_disabled() ? "WP" :
			pgent.is_write_through()  ? "WT" : "WC");
	    else
#endif
		printf (pgent.is_cache_disabled() ? "UC" :
			pgent.is_write_through()  ? "WT" : "WB");
	}
#endif /* __cplusplus */
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
void      pgent_flush          (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel, addr_t vaddr);
/* Entries used by the AMD64 SMP page-table sync (glue/v4-x86/x64/space.c). */
word_t    pgent_idx            (pgent_t *self);
bool      pgent_is_cpulocal    (pgent_t *self, struct space_t *s, word_t pgsize);
void      pgent_smp_sync       (pgent_t *self, struct space_t *s, word_t pgsize);
END_DECLS

#if !defined(__cplusplus)
/* C forms of the pgent_t modifier methods used only by glue/v4-x86/space.c:
   bit-twiddling on the C-visible x86_pgent_t union (pgsize is X86_PGSIZE_*).
   pgent_sync mirrors pgent_t::sync -> smp_sync. */
INLINE void pgent_sync (pgent_t *self, struct space_t *s, word_t pgsize)
{
    if (pgsize >= X86_PGSIZE_SYNC)
	pgent_smp_sync (self, s, pgsize);
}

INLINE void pgent_set_global (pgent_t *self, struct space_t *s, word_t pgsize, bool global)
{
#if defined(CONFIG_X86_PGE)
    self->pgent.pg4k.global = global;
    pgent_sync (self, s, pgsize);
#endif
}

INLINE void pgent_set_cpulocal (pgent_t *self, struct space_t *s, word_t pgsize, bool local)
{
    (void) s; (void) pgsize;
    self->pgent.pg4k.cpulocal = local;
}

INLINE void pgent_set_cacheability (pgent_t *self, struct space_t *s, word_t pgsize, bool cacheable)
{
    self->pgent.pg4k.cache_disabled = !cacheable;
    if (pgsize == X86_PGSIZE_4K)
	self->pgent.pg4k.pat = 0;
    else
	self->pgent.pg2m.pat = 0;
    pgent_sync (self, s, pgsize);
}
#endif /* !__cplusplus */

#if defined(CONFIG_NEW_MDB)
#undef mapnode_t
#endif

#endif /* !__ARCH__X86__PGENT_H__ */
