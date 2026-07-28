/*********************************************************************
 *                
 * Copyright (C) 2005,  Karlsruhe University
 *                
 * File path:     kdb/generic/vrt.cc
 * Description:   Functions for dumping VRTs
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
 * $Id: vrt.cc,v 1.2 2005/05/12 17:42:49 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <vrt.h>
#include <kdb/kdb.h>

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
 * Dump VRT table.  Was kdb_t::dump_vrt_table.
 *
 * @param vrt		VRT object
 * @param table		table to dump
 * @param depth		current recursion depth
 */
void dump_vrt_table (vrt_t *vrt, vrt_table_t *t, word_t depth)
{
    word_t paddr = t->prefix & ~((1UL << (t->objsize + t->radix)) - 1);
    vrt_node_t *node;
    word_t k;

    printf ("%s%p table [objsize=%d%s  radix=%d] (%p)\n",
	    indent (depth), paddr,
	    sz_num (t->objsize), sz_suf (t->objsize),
	    1UL << t->radix, t);

    node = vrt_table_get_node (t, 0);

    for (k = 0;
	 k < (1UL << vrt_table_get_radix (t));
	 k++, node++, paddr += (1UL << vrt_table_get_objsize (t)))
    {
	if (! vrt_node_is_valid (node))
	    continue;

	if (vrt_node_is_table (node))
	    dump_vrt_table (vrt, vrt_node_get_table (node), depth + 1);
	else
	{
	    printf ("%s%p ", indent (depth + 1), paddr);
	    vrt->ops->dump (vrt, node);
	}
    }
}

/**
 * Dump VRT.  Was kdb_t::dump_vrt.
 * @param vrt		VRT object
 */
void dump_vrt (vrt_t *vrt)
{
    dump_vrt_table (vrt, vrt_get_table (vrt), 0);
}
