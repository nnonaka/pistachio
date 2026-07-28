/*********************************************************************
 *                
 * Copyright (C) 2005,  Karlsruhe University
 *                
 * File path:     kdb/generic/mdb.c
 * Description:   Functions for debugging mapping databases
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
 * $Id: mdb.cc,v 1.6 2005/05/11 17:23:44 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/cmd.h>
#include <kdb/kdb.h>
#include <kdb/input.h>

#include <mdb.h>
#include <mdb_mem.h>
#include <linear_ptab.h>


/*
 * Command group for mapping database.
 */

DECLARE_CMD_GROUP (mdb);

/* were kdb_t::dump_table / dump_resource_table / dump_resource_map */
static void dump_table (mdb_t *mdb, mdb_table_t *t, word_t depth);
static void dump_resource_table (mdb_t *mdb, mdb_table_t *table, word_t addr,
				 word_t depth);
static void dump_resource_map (mdb_t *mdb, mdb_node_t *node, word_t addr,
			       word_t depth);


/**
 * Menu for mapping database
 */
DECLARE_CMD (cmd_mdb_menu, root, 'm', "mdb", "mapping database");

CMD (cmd_mdb_menu, cg)
{
    return cmd_group_interact (&mdb, cg, "mdb");
}


/**
 * Dump memory mappings for given page frame.
 */
DECLARE_CMD (cmd_mdb_mem_dump, mdb, 'm', "dumpmem",
	     "dump specific memory mappings");

CMD (cmd_mdb_mem_dump, cg)
{
    word_t addr;

    (void) cg;
    addr = get_hex ("Address", 0, NULL);
    if (addr == ABORT_MAGIC)
	return CMD_NOQUIT;

    dump_resource_map (&mdb_mem, sigma0_memnode, addr, 0);
    return CMD_NOQUIT;
}


/**
 * Dump all memory mappings.
 */
DECLARE_CMD (cmd_mdb_mem_dump_all, mdb, 'M', "dumpallmem",
	     "dump all memory mappings");

CMD (cmd_mdb_mem_dump_all, cg)
{
    (void) cg;
    dump_table (&mdb_mem, mdb_node_get_table (sigma0_memnode), 0);
    return CMD_NOQUIT;
}



/*
 * Helper functions
 */

static char * indent (word_t depth)
{
    static char spc[] = "                                              "
	"                                                              ";
    char * p = spc + sizeof (spc) - 1 - depth * 2;
    return p < spc || p >= spc + sizeof (spc) ? spc : p;
}

static word_t sz_num (word_t sz)
{
    return sz >= 30 ? (1UL << sz) >> 30 :
	sz >= 20 ? (1UL << sz) >> 20 :
	sz >= 10 ? (1UL << sz) >> 10 : (1UL << sz);
}

static const char * sz_suf (word_t sz)
{
    return sz >= 30 ? "G" : sz >= 20 ? "M" : sz >= 10 ? "K" : "";
}



/**
 * Dump mapping database table, including all subtrees and mapping
 * nodes.
 *
 * @param mdb		mapping databse
 * @param t		table to dump
 * @param depth		current recursion depth
 */
static void dump_table (mdb_t *mdb, mdb_table_t *t, word_t depth)
{
    word_t paddr;
    mdb_tableent_t *te;
    word_t k;

    if (t == NULL)
	return;

    paddr = t->prefix & ~(((1UL << t->objsize) << t->radix) - 1);

    printf ("%s%p table [objsize=%d%s  radix=%d  count=%d] (%p)\n",
	    indent (depth), paddr,
	    sz_num (t->objsize), sz_suf (t->objsize),
	    1UL << t->radix, t->count, t);

    te = mdb_table_get_entry (t, 0);

    for (k = 0;
	 k < (1UL << t->radix);
	 k++, te++, paddr += (1UL << t->objsize))
    {
	mdb_node_t *n;
	word_t start_depth;

	if (! mdb_tableent_is_valid (te))
	    continue;

	if (mdb_tableent_is_table (te))
	    dump_table (mdb, mdb_tableent_get_table (te), depth + 1);

	if (mdb_tableent_get_node (te))
	{
	    printf ("%s%p ", indent (depth + 1), paddr);

	    n = mdb_tableent_get_node (te);
	    if (n == NULL)
	    {
		printf ("[null node]\n");
		continue;
	    }
	    start_depth = mdb_node_get_depth (n);

	    mdb->ops->dump (mdb, n);
	    if (mdb_node_get_table (n))
		dump_table (mdb, mdb_node_get_table (n), depth + 2);

	    n = mdb_node_get_next (n);
	    while (n != NULL)
	    {
		printf ("%s%ws ", indent (depth + 1 + mdb_node_get_depth (n)
					  - start_depth), "");
		mdb->ops->dump (mdb, n);
		if (mdb_node_get_table (n))
		    dump_table (mdb, mdb_node_get_table (n),
				depth + 2 + mdb_node_get_depth (n) - start_depth);
		n = mdb_node_get_next (n);
	    }
	}
    }
}


/**
 * Dump part of mapping table that refers to object residing at a
 * particular address.
 *
 * @param mdb		mapping database
 * @param table		mapping table
 * @param addr		physical address
 * @param depth		current recursion depth
 */
static void dump_resource_table (mdb_t *mdb, mdb_table_t *table, word_t addr,
				 word_t depth)
{
    while (table && mdb_table_match_prefix (table, addr))
    {
	printf ("%stable %p [objsize=%d%s  radix=%d  count=%d] (%p)\n",
		indent (depth - 1), mdb_table_get_prefix (table),
		sz_num (table->objsize), sz_suf (table->objsize),
		1UL << table->radix, table->count, table);

	if (mdb_table_get_node_at (table, addr))
	    dump_resource_map (mdb, mdb_table_get_node_at (table, addr), addr,
			       depth + 1);

	table = mdb_table_get_table (table, addr);
	depth++;
    }
}


/**
 * Dump a mapping tree that refers to object resifing at a particular
 * address,
 *
 * @param mdb		mapping database
 * @param node		mapping node
 * @param addr		physical address
 * @param depth		current recursion depth
 */
static void dump_resource_map (mdb_t *mdb, mdb_node_t *node, word_t addr,
			       word_t depth)
{
    word_t start_depth = mdb_node_get_depth (node);

    while (node)
    {
	if (mdb_node_get_depth (node) > 0)
	{
	    printf ("%s", indent (depth - start_depth + mdb_node_get_depth (node) - 1));
	    mdb->ops->dump (mdb, node);
	}
	if (mdb_node_get_table (node))
	    dump_resource_table (mdb, mdb_node_get_table (node), addr,
				 mdb_node_get_depth (node) - start_depth + depth + 1);
	node = mdb_node_get_next (node);
    }
}
