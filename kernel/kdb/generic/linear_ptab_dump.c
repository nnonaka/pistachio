/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007-2008, 2010, 2012,  Karlsruhe University
 *                
 * File path:     kdb/generic/linear_ptab_dump.c
 * Description:   Linear page table dump
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
 * $Id: linear_ptab_dump.cc,v 1.17 2006/10/19 22:57:36 ud3 Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/cmd.h>
#include <kdb/kdb.h>
#include <kdb/input.h>
#include <linear_ptab.h>

#include INC_ARCH(pgent.h)
#include INC_API(tcb.h)

/* defined in C (kdb/glue/v4-x86/x64/space.c); pgsize_e is a 4-byte enum, which
   is what the C side writes through its int* out-parameter. */
BEGIN_DECLS
void get_ptab_dump_ranges (addr_t * vaddr, word_t * num,
			   int * max_size);
END_DECLS

/* Non-x86 ports must define PGENT_SIZE_MAX (the C spelling of what was
   pgent_t::size_max) alongside their pgent_t C form. */
#if !defined(CONFIG_ARCH_X86)
void get_ptab_dump_ranges (addr_t * vaddr, word_t * num,
			   int * max_size)
{
    *vaddr = (addr_t) 0;
    *num = page_table_size (PGENT_SIZE_MAX);
    *max_size = PGENT_SIZE_MAX;
}
#endif


/**
 * cmd_dump_ptab: dump page table contents
 */
DECLARE_CMD (cmd_dump_ptab, root, 'p', "ptab", "dump page table");

CMD(cmd_dump_ptab, cg)
{
    static char spaces[] = "                                ";
    char * spcptr = spaces + sizeof (spaces) - 1;
    char * spcpad = spcptr - X86_PGSIZE_MAX * 2;

    space_t * space;
    addr_t vaddr;
    word_t num, count = 0;
    pgent_t * pg;
    int size, max_size;

    // Arrays to implement recursion
    pgent_t * r_pg[X86_PGSIZE_MAX];
    word_t r_num[X86_PGSIZE_MAX];

    // Get dump arguments
    space = get_space ("Space");
    size = X86_PGSIZE_MAX;
    
    word_t cpu = get_dec("CPU id", get_current_cpu(), NULL);
    if (cpu >= CONFIG_SMP_MAX_CPUS) cpu = get_current_cpu();
    
    get_ptab_dump_ranges (&vaddr, &num, &max_size);
    pg = space_pgent_cpu (space, page_table_index (X86_PGSIZE_MAX, vaddr), cpu);

    if (!pg)
    {
	printf ("No page table\n");
	return CMD_NOQUIT;
    }

    while (size != max_size)
    {
	if (!pgent_is_subtree (pg, space, size))
	{
	    printf ("No subtree");
	    return CMD_NOQUIT;
	}
	pg = pgent_subtree (pg, space, size--);
	pg = pgent_next (pg, space, size, page_table_index (size, vaddr));
   }

    
    while (num > 0)
    {
	if (((++count % 4000) == 0) && get_choice ("Continue", "y/n", 'y') == 'n')
	    break;

	if (pgent_is_valid (pg, space, size))
	{
	    if (pgent_is_subtree (pg, space, size))
	    {
		// Recurse into subtree
		printf ("%p [%p]:%s tree=%p\n", vaddr, pg->raw, spcptr,
			pgent_subtree (pg, space, size));

		size--;
		r_pg[size] = pgent_next (pg, space, size+1, 1);
		r_num[size] = num - 1;
		spcptr -= 2;
		spcpad += 2;

		pg = pgent_subtree (pg, space, size+1);
		num = page_table_size (size);
		continue;
	    }
	    else
	    {
		// Print valid mapping
		word_t pgsz = page_size (size);
		word_t rwx = pgent_reference_bits (pg, space, size, vaddr);


		printf ("%p [%p]:%s phys=%p map=%p %s%3d%cB %c%c%c "
			"(%c%c%c) %s",
			vaddr, pg->raw, spcptr, pgent_address (pg, space, size),
			pgent_mapnode (pg, space, size, vaddr), spcpad,
			(pgsz >= GB (1) ? pgsz >> 30 :
			 pgsz >= MB (1) ? pgsz >> 20 : pgsz >> 10),
			pgsz >= GB (1) ? 'G' : pgsz >= MB (1) ? 'M' : 'K',
			pgent_is_readable (pg, space, size)   ? 'r' : '~',
			pgent_is_writable (pg, space, size)   ? 'w' : '~',
			pgent_is_executable (pg, space, size) ? 'x' : '~',
			rwx & 4 ? 'R' : '~',
			rwx & 2 ? 'W' : '~',
			rwx & 1 ? 'X' : '~',
			pgent_is_kernel (pg, space, size) ? "kernel" : "user");
		pgent_dump_misc (pg, space, size);
		printf ("\n");
	    }
	}

	// Goto next ptab entry
	vaddr = addr_offset (vaddr, page_size (size));
	pg = pgent_next (pg, space, size, 1);
	num--;

	while (num == 0 && size < max_size)
	{
	    // Recurse up from subtree
	    pg = r_pg[size];
	    num = r_num[size];
	    size++;
	    spcptr += 2;
	    spcpad -= 2;
	}
    }

    return CMD_NOQUIT;
}

