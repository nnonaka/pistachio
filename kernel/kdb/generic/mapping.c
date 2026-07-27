/*********************************************************************
 *                
 * Copyright (C) 2002, 2009,  Karlsruhe University
 *                
 * File path:     kdb/generic/mapping.c
 * Description:   Mapping database dumping
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
 * $Id: mapping.cc,v 1.2 2003/09/24 19:05:11 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/cmd.h>
#include <kdb/kdb.h>
#include <kdb/input.h>
#include <mapping.h>
#include <linear_ptab.h>

static void dump_mdbmaps (mapnode_t * map, addr_t paddr,
			  word_t size,
			  rootnode_t * proot, char * spc);

static void dump_mdbroot (rootnode_t * root, addr_t paddr,
			  word_t size, char * spc);


/*
 * Helper functions
 */

INLINE word_t mdb_arraysize (word_t pgsize)
{
    return 1 << (mdb_pgshifts[pgsize+1] - mdb_pgshifts[pgsize]);
}

INLINE word_t mdb_get_index (word_t size, addr_t addr)
{
    return ((word_t) addr >> mdb_pgshifts[size]) & (mdb_arraysize(size) - 1);
}

INLINE rootnode_t * mdb_index_root (word_t size, rootnode_t * r,
				    addr_t addr)
{
    return r + mdb_get_index (size, addr);
}

INLINE word_t hw_pgsize (word_t mdb_pgsize)
{
    word_t s = 0;
    while (hw_pgshifts[s] < mdb_pgshifts[mdb_pgsize])
	s++;
    return s;
}


/**
 * cmd_dump_mdb: dump mapping database
 */
DECLARE_CMD (cmd_dump_mdb, root, 'm', "mdb", "dump mapping database");

CMD (cmd_dump_mdb, cg)
{
    static char spaces[] = "                                                ";

    addr_t paddr = (addr_t) get_hex ("Address", 0, NULL);
    if ((word_t) paddr == ABORT_MAGIC)
	return CMD_NOQUIT;
    
    dump_mdbroot (mdb_index_root (MDB_PGSIZE_MAX, 
				  mapnode_get_nextroot (sigma0_mapnode), paddr),
		  paddr, MDB_PGSIZE_MAX, spaces + sizeof (spaces)-1);

    return CMD_NOQUIT;
}

static void dump_mdbmaps (mapnode_t * map, addr_t paddr,
			  word_t size,
			  rootnode_t * proot, char * spc)
{
    mapnode_t * pmap = NULL;

    while (map)
    {
	space_t * space = mapnode_get_space (map);
	word_t hwsize = hw_pgsize (size);

	printf ("%s[%d] space=%p  vaddr=%p  pgent=%p  (%p)\n",
		spc - mapnode_get_depth (map) * 2, mapnode_get_depth (map), space,
		(pmap ?
		 pgent_vaddr (mapnode_get_pgent (map, pmap), space, hwsize, map) :
		 pgent_vaddr (mapnode_get_pgent (map, proot), space, hwsize, map)),
		pmap ? mapnode_get_pgent (map, pmap) : mapnode_get_pgent (map, proot),
		map);
	
	pmap = map;
	if (mapnode_is_next_root (map) || (mapnode_is_next_both (map) &&
                                     mapnode_get_nextroot (map) != NULL))
	{
	    dump_mdbroot (mdb_index_root (size-1, mapnode_get_nextroot (map), paddr),
			  paddr, size-1, spc - 2 - mapnode_get_depth (map) * 2);
	}
	map = mapnode_get_nextmap (map);
    }
}

static void dump_mdbroot (rootnode_t * root, addr_t paddr,
			  word_t size, char * spc)
{
    printf ("%s%p: %d%cB %s (%p)\n",
	    spc, addr_mask (paddr,  ~((1 << mdb_pgshifts[size]) - 1)),
	    ((mdb_pgshifts[size] >= 30) ? 1 << (mdb_pgshifts[size] - 30) :
	     (mdb_pgshifts[size] >= 20) ? 1 << (mdb_pgshifts[size] - 20) :
	     1 << (mdb_pgshifts[size] - 10)),
	    ((mdb_pgshifts[size] >= 30) ? 'G' :
	     (mdb_pgshifts[size] >= 20) ? 'M' : 'K'),
	    rootnode_is_next_both (root) ? "[root/map]" :
	    rootnode_is_next_root (root) ? "[root]" : "[map]", root);

    if (rootnode_is_next_map (root) || rootnode_is_next_both (root))
	dump_mdbmaps (rootnode_get_map (root), paddr, size, root, spc - 2);

    if (rootnode_is_next_root (root) || rootnode_is_next_both (root))
	dump_mdbroot (mdb_index_root (size-1, rootnode_get_root (root), paddr), paddr,
		      size-1,  spc - 2);
}

