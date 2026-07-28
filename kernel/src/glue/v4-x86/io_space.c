/*********************************************************************
 *                
 * Copyright (C) 2004-2009,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/io_space.cc
 * Description:   IO-Fpage implementation for IA-32
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
 * $Id: io_space.cc,v 1.4 2006/12/05 15:23:16 skoglund Exp $
 *                
 ********************************************************************/
#include <linear_ptab.h>
#include <kdb/tracepoints.h>
#include INC_ARCH_SA(ptab.h)
#include INC_ARCH_SA(tss.h)
#include INC_API(fpage.h)
#include INC_API(kernelinterface.h)
#include INC_API(tcb.h)
#include INC_API(thread.h)
#include INC_GLUE(space.h)
#include INC_GLUE(io_space.h)


DECLARE_TRACEPOINT (X86_IO_PORT_SPACE);

/* generic/lib.h's min() is min(int,int); these operands are word_t and the
   C++ used a template, so a word-sized form avoids narrowing (notes §116). */
INLINE word_t min_word (word_t a, word_t b) { return a < b ? a : b; }

/* asm-named entry points defined in glue/v4-x86/thread.c */
void tcb_save_state (tcb_t *);
void tcb_restore_state (tcb_t *);

FEATURESTRING ("ioflexpages");

/*
 * was the INLINE acceptor_t::get_arch_specific_rcvwindow specialisation in
 * io_space.h; api/v4/accessors.c has the generic one under the opposite guard.
 */
fpage_t acceptor_get_arch_specific_rcvwindow (acceptor_t *self, struct tcb_t *dest)
{
    (void) self; (void) dest;
    return fpage_complete_arch ();
}

/*
 * void zero_io_bitmap()
 */

void zero_io_bitmap (space_t *space, word_t port, word_t log2size)
{
    word_t bits;
    word_t size = (1UL << log2size);
    word_t *w;

    /*
     * If the task had no mappings before, it has the default IOPBM (which is
     * set to ~0). Create a new one, set it to 0 between port and port + size,
     * and to ~0 else
     */

    if (space_get_io_bitmap (space, current_cpu) == x86_tss_get_io_bitmap (&tss))
    {
	word_t *new_iopbm = (word_t *) space_install_io_bitmap (space, true);

	TRACEPOINT (X86_IO_PORT_SPACE, "installed IO bitmap %x", new_iopbm);
	ASSERT (new_iopbm);
	w = new_iopbm;

	/* until we reach the port, all bits are set to 1 (word-wise) */
	for (; w < new_iopbm + (port / BITS_WORD); w++)
	    *w = ~(0UL);

	/* Set the bits before port, zero the others */
	TRACEPOINT (X86_IO_PORT_SPACE, "clear IO bitmap %x port %d size %d -> %p",
		    new_iopbm, port, log2size, w);

	bits = min_word (size, BITS_WORD - (port & (BITS_WORD - 1)));
	*w++ = ~((~(0UL) >> (BITS_WORD - bits)) << (port & (BITS_WORD - 1)));

	/* Wordwise zero between bit port and bit port+size */
	for (size -= bits; size >= BITS_WORD; size -= BITS_WORD)
	    *w++ = 0;

	/* Zero the word hosting bit port+size */
	if (size)
	    *w++ = ~(~(0UL) >> (BITS_WORD - size));

	/* The remaining words are set to 1 */
	for (; w < new_iopbm + IOPERMBITMAP_SIZE / sizeof (word_t); w++)
	    *w = ~0UL;

	return;
    }

    w = (word_t *) space_get_io_bitmap (space, current_cpu);

    /* Jump to the right word */
    w += (port / BITS_WORD);

    /* Zero the bits after bit port */
    TRACEPOINT (X86_IO_PORT_SPACE, "clear IO bitmap %x port %d size %d -> %p",
		space_get_io_bitmap (space, current_cpu), port, log2size, w);

    bits = min_word (size, BITS_WORD - (port & (BITS_WORD - 1)));

    *w++ &= ~((~(0U) >> (BITS_WORD - bits)) << (port & (BITS_WORD - 1)));

    /* Wordwise zero between bit port and bit port+size */
    for (size -= bits; size >= BITS_WORD; size -= BITS_WORD)
	*w++ = 0;

    /* Zero the bits before bit port+size */
    if (size)
	*w++ &= ~(~(0U) >> (BITS_WORD - size));
}

/*
 * void set_io_bitmap()
 */

void set_io_bitmap (space_t *space, word_t port, word_t log2size)
{
    word_t bits;
    word_t size = (1UL << log2size);
    word_t *w = (word_t *) space_get_io_bitmap (space, current_cpu);

    /* Jump to the right word */
    w += port / BITS_WORD;

    /* Set the bits after bit port */
    TRACEPOINT (X86_IO_PORT_SPACE, "set IO bitmap %x port %d size %d -> %p",
		space_get_io_bitmap (space, current_cpu), port, log2size, w);

    bits = min_word (size, BITS_WORD - (port & (BITS_WORD - 1)));
    *w++ |= ((~(0U) >> (BITS_WORD - bits)) << (port & (BITS_WORD - 1)));

    /* Wordwise set between bit port and bit port+size */
    for (size -= bits; size >= BITS_WORD; size -= BITS_WORD)
	*w++ = ~0UL;

    /* Set the bits before bit port+size */
    if (size)
	*w++ |= (~(0UL) >> (BITS_WORD - size));
}


/*
 * init_io_space()
 *
 * initializes the default IO bitmap
 */

void init_io_space (void)
{
    /* Set the default IOPBM Bits to 1 */
    word_t *p = (word_t *) x86_tss_get_io_bitmap (&tss);
    u32_t i;

    for (i = 0; i < IOPERMBITMAP_SIZE / sizeof (word_t); i++)
	*(p + i) = ~0UL;

#if defined(CONFIG_X86_PVI)
    /* Enable PVI Bit */
#warning Setting PVI bit in CR4 will not work with vmware
    x86_cr4_set (X86_CR4_PVI);
#endif
}

/*
 * arch_map_fpage()
 *
 * maps an IO-Fpage
 */

void arch_map_fpage (tcb_t * src, fpage_t snd_fpage,
		     word_t snd_base,
		     tcb_t * dst, fpage_t rcv_fpage,
		     bool grant)
{
    space_t *sspace = tcb_get_space (src);
    space_t *dspace = tcb_get_space (dst);

    (void) snd_base;
    TRACEPOINT (X86_IO_PORT_SPACE, "map %t, raw = %x, port = %x, size %x, base = %x, from = %p, to = %p",
		dst, snd_fpage.raw, arch_fpage_get_base (&snd_fpage.arch),
		arch_fpage_get_size (&snd_fpage.arch), snd_base, sspace, dspace);

    if (space_get_io_space (sspace))
    {
	if (! space_get_io_space (dspace))
	    space_set_io_space (dspace, vrt_io_alloc ());

	vrt_map_fpage (&space_get_io_space (sspace)->base, snd_fpage,
		       (word_t) arch_fpage_get_base (&snd_fpage.arch),
		       &space_get_io_space (dspace)->base, rcv_fpage, grant);
    }
}


/*
 * arch_unmap_fpage()
 *
 * revokes an IO-Fpage
 */

void arch_unmap_fpage (tcb_t * from, fpage_t fpage, bool flush)
{
    mdb_ctrl_t ctrl;

    ctrl.raw = 0;
    ctrl.unmap = true;
    ctrl.mapctrl_self = flush;
    vrt_mapctrl (&space_get_io_space (tcb_get_space (from))->base, fpage, ctrl, 0, 0);
}


/*
 * handle_io_pagefault()
 *
 * handle an IO pagefault exception
 */

bool handle_io_pagefault (tcb_t *tcb, u16_t port, u16_t log2size, addr_t ip)
{
    space_t *space = tcb_get_space (tcb);
    msg_tag_t tag;
    acceptor_t acceptor;
    fpage_t iofp;

    TRACEPOINT (X86_IO_PORT_SPACE, "IO-Pagefault @ %x [size %x] (current=%T)",
		(word_t) port, (word_t) log2size, TID (tcb_get_global_id (tcb)));

    if (space == sigma0_space)
    {
	zero_io_bitmap (space, port, log2size);
	return true;
    }

    if (space_sync_io_bitmap (space))
	return true;

    tcb_save_state (tcb);

    /* generate pagefault message (rw) */
    msg_tag_set (&tag, 0, 2, IPC_MR0_IO_PAGEFAULT | (1 << 2) | (1 << 1));

    /* create acceptor for whole address space */
    acceptor.raw = 0;
    acceptor_set_rcv_window (&acceptor, fpage_complete_arch ());

    iofp.raw = 0;
    arch_fpage_set (&iofp.arch, port, log2size, 0, 0, 0);

    tcb_set_tag (tcb, tag);
    tcb_set_mr (tcb, 1, iofp.raw);
    tcb_set_mr (tcb, 2, (word_t) ip);
    tcb_set_br (tcb, 0, acceptor.raw);

    tag = tcb_do_ipc (tcb, tcb_get_pager (tcb), tcb_get_pager (tcb), timeout_never ());
    if (msg_tag_is_error (&tag))
    {
	printf ("result tag = %p, ip = %p, port = %x, size = %x", tag.raw, ip, port, log2size);
	enter_kdebug ("IO-pagefault IPC error");
    }

    tcb_restore_state (tcb);

    return true;
}
