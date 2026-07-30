/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 *                
 * File path:     kdb/glue/v4-x86/addrtranslation.c
 * Description:   
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
 * $Id$
 *                
 ********************************************************************/

#include <debug.h>
#include <kdb/kdb.h>
#include <kdb/cmd.h>
#include <kdb/input.h>

#include <linear_ptab.h>
#include INC_GLUE(hwspace.h)
#include INC_GLUE(space.h)
#include INC_ARCH(pgent.h)
#include INC_API(tcb.h)


DECLARE_CMD( cmd_virt_to_phys, root, 'i', "virt_to_phys", "Translate virtual address to physical address");

CMD( cmd_virt_to_phys, cg )
{
    threadid_t space_id;
    threadid_set_raw (&space_id,  get_hex("Space:", 0, NULL ) );
    addr_t vaddr = (addr_t)get_hex("Virtual Address", 0, NULL );
    cpuid_t cpu = (cpuid_t)get_dec("CPU", 0, NULL );
    
    space_t * space;
    if ( threadid_get_raw (&space_id) == 0x0 )
         space = get_kernel_space_c();
    else
        space = tcb_get_space (tcb_get_tcb (space_id));
    
    word_t size = X86_PGSIZE_MAX;
    word_t offset = 0;

#if defined(CONFIG_X_X86_HVM)
    if (space_is_hvm_space (space))
    {
	x86_hvm_space_t *hvm_space = space_get_hvm_space (space);
	tcb_t *hvm_tcb = x86_hvm_space_get_tcb_list (hvm_space);
	addr_t gpaddr;

	arch_hvm_ktcb_dump_hvm_ptab_entry (&hvm_tcb->arch.hvm, vaddr);

	if (x86_hvm_space_lookup_gphys_addr (hvm_space, vaddr, &gpaddr))
	    vaddr = gpaddr;
    }
#endif    

    pgent_t * pgent = space_pgent_cpu (space, page_table_index (size, vaddr), cpu);
    printf( "PDIR @ %p\n", (void *) space_get_top_pdir_phys (space, cpu) );
    
    if ( pgent_is_subtree (pgent, space, size) )
    {
        size--;
        printf( "PTAB @ %p\n", x86_pgent_get_ptab (&pgent->pgent) );
        pgent = pgent_subtree (pgent, space, size);
        pgent = pgent_next (pgent, space, size, page_table_index (size, vaddr));
        offset = (word_t)vaddr & ~X86_PAGE_MASK;
    }
    else
    {
        offset = (word_t)vaddr & ~X86_SUPERPAGE_MASK;
    }
        
    addr_t paddr = pgent_address (pgent, space, size);
    paddr = (addr_t)((word_t)paddr + offset);
    
    printf("[virt] %p -> [phys] %p ", vaddr, paddr );

    word_t pgsz = page_size (size);
    word_t rwx = pgent_reference_bits (pgent, space, size, vaddr);
    printf("%3d%cB %c%c%c (%c%c%c) %s ",
            (pgsz >= GB (1) ? pgsz >> 30 :
             pgsz >= MB (1) ? pgsz >> 20 : pgsz >> 10),
            pgsz >= GB (1) ? 'G' : pgsz >= MB (1) ? 'M' : 'K',
            pgent_is_readable (pgent, space, size)   ? 'r' : '~',
            pgent_is_writable (pgent, space, size)   ? 'w' : '~',
            pgent_is_executable (pgent, space, size) ? 'x' : '~',
            rwx & 4 ? 'R' : '~',
            rwx & 2 ? 'W' : '~',
            rwx & 1 ? 'X' : '~',
            pgent_is_kernel (pgent, space, size) ? "kernel" : "user");
    pgent_dump_misc (pgent, space, size);
    printf ("\n");
    
    return CMD_NOQUIT;
}

