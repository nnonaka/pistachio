/****************************************************************************
 *
 * Copyright (C) 2003-2004,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/exception.c
 * Description:	PowerPC64 exception handlers.
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
 * $Id: exception.cc,v 1.18 2006/11/17 17:04:18 skoglund Exp $
 *
 ***************************************************************************/

#include <debug.h>
#include <kdb/tracepoints.h>
#if defined(CONFIG_DEBUG)
#include <kdb/console.h>
#endif
#include INC_ARCH(msr.h)
#include INC_ARCH(except.h)

#include INC_PLAT(prom.h)

#include INC_API(tcb.h)
#include INC_API(schedule.h)	/* for sched_handoff */
#include INC_API(kernelinterface.h)

#include INC_GLUE(syscalls.h)
#include INC_GLUE(pghash.h)
#include INC_GLUE(pgent_inline.h)
#include INC_GLUE(exception.h)

#if CONFIG_PLAT_OFPOWER3
#include INC_ARCH(segment.h)
#endif

DECLARE_TRACEPOINT(except_dsi_cnt);
DECLARE_TRACEPOINT(except_isi_cnt);


#define GENERIC_SAVED_REGISTERS (EXCEPT_IPC_GEN_MR_NUM_ADDRESS+1)

static bool send_exception_ipc( word_t exc_no, word_t exc_code, bool with_address, word_t address )
{
    tcb_t *current = get_current_tcb();
    if( ({ threadid_t __h = tcb_get_exception_handler (current); threadid_is_nilthread (&__h); }) )
    {
	printf( "Unable to deliver user exception: no exception handler.\n" );
	return false;
    }

    // Save message registers on the stack
    word_t saved_mr[GENERIC_SAVED_REGISTERS];
    msg_tag_t tag;
    int i;

    // Save message registers.
    for( i = 0; i < GENERIC_SAVED_REGISTERS; i++ )
	saved_mr[i] = tcb_get_mr (current, i);
    tcb_set_saved_partner (current, tcb_get_partner (current));
    tcb_set_saved_state (current, tcb_get_state (current));

    // Create the message tag.
    msg_tag_set (&tag, 0, with_address ? EXCEPT_IPC_GEN_MR_NUM_ADDRESS :  EXCEPT_IPC_GEN_MR_NUM,
		    EXCEPT_IPC_GEN_LABEL);
    tcb_set_tag (current, tag);

    // Create the message.
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_IP, (word_t)tcb_get_user_ip (current));
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_SP, (word_t)tcb_get_user_sp (current));
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_FLAGS, (word_t)tcb_get_user_flags (current));
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_EXCEPTNO, exc_no);
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_ERRORCODE, exc_code);
    tcb_set_mr (current, EXCEPT_IPC_GEN_MR_LOCALID, ({ threadid_t __l = tcb_get_local_id (current); threadid_get_raw (&__l); }));
    if (with_address)
	tcb_set_mr (current, EXCEPT_IPC_GEN_MR_ERRORADDRESS, address);

    // Deliver the exception IPC.
    tag = tcb_do_ipc (current, tcb_get_exception_handler (current),
	    tcb_get_exception_handler (current), timeout_never() );

    // Alter the user context if necessary.
    if( !msg_tag_is_error (&tag) )
    {
	tcb_set_user_ip (current, (addr_t)tcb_get_mr (current, EXCEPT_IPC_GEN_MR_IP));
	tcb_set_user_sp (current, (addr_t)tcb_get_mr (current, EXCEPT_IPC_GEN_MR_SP));
	tcb_set_user_flags (current, tcb_get_mr (current, EXCEPT_IPC_GEN_MR_FLAGS));
    }
    else
	printf( "Unable to deliver user exception: IPC error.\n" );

    // Clean-up.
    for( i = 0; i < GENERIC_SAVED_REGISTERS; i++ )
	tcb_set_mr (current, i, saved_mr[i]);

    tcb_set_partner (current, tcb_get_saved_partner (current));
    tcb_set_saved_partner (current, NILTHREAD);
    tcb_set_state (current, tcb_get_saved_state (current));
    tcb_set_saved_state (current, THREAD_STATE_ABORTED);

    return !msg_tag_is_error (&tag);
}

static void halt_user_thread( void )
{
    tcb_t *current = get_current_tcb();

    tcb_set_state (current, THREAD_STATE_HALTED);
    sched_schedule (get_idle_tcb_c (), sched_handoff);
}

/* Was tcb_t *get_kdebug_tcb() in the C++ header set; api/v4/smp.c compares
   against it.  Same sentinel as glue/v4-powerpc/except_handlers.c. */
tcb_t *get_kdebug_tcb (void) { return (tcb_t*)~0UL; }

/* except_return() short circuits the C code return path.
 * We declare the exception handlers as noreturn, to avoid
 * the C prolog (which redundantly spills registers which the assembler
 * path already spills).
 */
#define except_return()			\
do {					\
    asm volatile (			\
	"mtlr %0 ;"			\
	"ld %%r1, 0(%1) ;"		\
	"blr ;"				\
	:				\
	: "r" (__builtin_return_address(0)), \
	  "b" (__builtin_frame_address(0)) \
    );					\
    while(1);				\
} while(0)

void ppc64_except_unhandled( word_t vect, powerpc64_irq_context_t *context )
{
    const char * vector;
    bool with_address = false, deliver = false;
    word_t address = 0, exc_code = 0;

    switch(vect)
    {
    case 0x0100: vector = "System Reset"; break;
    case 0x0200: vector = "Machine Check"; break;
    case 0x0300: vector = "Data Access";
		 with_address = true;
		 address = context->dar;
		 exc_code = context->dsisr;
		 deliver = true;
		 break;
    case 0x0400: vector = "Instruction Access"; deliver = true; break;
    case 0x0500: vector = "Hardware Int"; break;
    case 0x0600: vector = "Alignment";
		 with_address = true;
		 address = context->dar;
		 exc_code = context->dsisr;
		 deliver = true;
		 break;
    case 0x0700: vector = "Program Check"; deliver = true; break;
    case 0x0800: vector = "FPU Unavailable"; break;
    case 0x0900: vector = "Decrementer"; break;
    case 0x0c00: vector = "System Call"; break;
    case 0x0d00: vector = "Trace"; deliver = true; break;
    case 0x0f00: vector = "Performance"; deliver = true; break;
    case 0x1300: vector = "Instruction Break"; deliver = true; break;
    case 0x1500: vector = "Soft Patch"; deliver = true; break;
    case 0x1600: vector = "Maintenance"; deliver = true; break;
    case 0x2000: vector = "Instrumentation"; deliver = true; break;
    default    : vector = "Unknown"; break;
    }

    if ( !deliver || !send_exception_ipc( vect, exc_code, with_address, address ) )
    {
	printf( "\n-- KD# %s exception --\n", vector );

	if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
	    get_kip()->kdebug_entry(context);

	halt_user_thread();
    }

    except_return();
}

/* Data storage interrupt (Data TLB miss) */
void dsi_handler( word_t dar, word_t dsisr, powerpc64_irq_context_t *context )
{
    tcb_t *tcb = get_current_tcb();

    bool is_kernel = ppc64_is_kernel_mode(context->srr1);

    TRACEPOINT( except_dsi_cnt, "[%p%s] Data exception @ %p from %p\n", tcb,
		is_kernel ? " (kernel)" : "", dar, context->srr0 );

#if defined(CONFIG_DEBUG)
    // Do we have a DABR hit?
    if( EXPECT_FALSE( EXCEPT_IS_DSI_DABR_MATCH(dsisr) ) )
    {
	printf( "Data Address Break Point @ %p\n", dar );
	get_kip()->kdebug_entry( context );
	except_return();
    }
#endif

    // If fault is in the kernel area, just map in a kernel page
    if ( EXPECT_FALSE( is_kernel && space_is_kernel_area ((addr_t)dar) ))
    {
	//TRACEF( "kernel fault\n" );
	pgent_t pg;
        space_t *space = get_kernel_space();
#ifdef CONFIG_POWERPC64_LARGE_PAGES
	pgsize_e size = size_16m;
#else
	pgsize_e size = size_4k;
#endif

	/* Create a dummy page table entry */
	pgent_set_entry( &pg, space, size, virt_to_phys((addr_t)dar),
		      7, l4default, true );

	/* Insert the kernel mapping, bolted */
	pghash_insert_mapping_bolted( get_pghash(), space, (addr_t)dar, &pg, size, true );

	//TRACEF( "kernel fault done\n" );
	except_return();
    }

    space_t *space = tcb_get_space (tcb);
    if (!space) space = get_kernel_space();

    // Use kernel space if we have kernel fault in TCB area or
    // when no space is set (e.g., running on the idle thread)
    if ( EXPECT_FALSE( is_kernel && space_is_tcb_area ((addr_t)dar) )
			    || space == NULL)
    {
        space = get_kernel_space();
	//TRACEF( "kernel space\n" );
    }

    ASSERT( !(is_kernel && space_is_cpu_area ((addr_t)dar)) );

    // Do we have a page hash miss?
    if( EXPECT_TRUE( EXCEPT_IS_DSI_MISS(dsisr) ) )
    {

	// Is the page hash miss in the copy area?
	if( EXPECT_FALSE(space_is_copy_area ((addr_t)dar)) )
	{
	    enter_kdebug( "Page table needs to be fixed for copy area" );
	    // Resolve the fault using the partner's address space!
	    tcb_t *partner = tcb_get_partner_tcb (tcb);
	    if( partner )
	    {
		addr_t real_fault = tcb_copy_area_real_address (tcb, (addr_t)dar );
		if( space_handle_hash_miss (tcb_get_space (partner), real_fault) )
	    	    except_return();
	    }
	}

	// Normal page hash miss.
	if( EXPECT_TRUE(space_handle_hash_miss (space, (addr_t)dar)) )
	{
	    //TRACEF("found - returning\n");
	    except_return();
	}
    }
    else if( EXCEPT_IS_DSI_FAULT(dsisr) )
    {
	// Page found, but access denied
	if( EXPECT_TRUE(space_handle_protection_fault (space, (addr_t)dar, true )) )
	{
	    //TRACEF("handled - returning\n");
	    except_return();
	}
    } else {
	if ( !send_exception_ipc( 0x300, dsisr, true, dar) )
	{
	    printf( "\n-- KD# data address exception --\n" );
	    if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
		get_kip()->kdebug_entry(context);
	}
    }

    //TRACEF("handle pagefault\n");
    space_handle_pagefault (space, (addr_t)dar, (addr_t)context->srr0,
		    EXCEPT_IS_DSI_WRITE(dsisr) ?  SPACE_ACCESS_WRITE : SPACE_ACCESS_READ,
		    ppc64_is_kernel_mode(context->srr1) );

    //TRACEF("handled - returning\n");
    except_return();
}

void program_check_handler( word_t vect, powerpc64_irq_context_t *context )
{
#if defined(CONFIG_DEBUG)
    if ( *(u32_t *)context->srr0 == KDEBUG_EXCEPT_INSTR )
    {
	switch( context->r0 ) {
	case L4_TRAP64_KDEBUG:
	    printf( "-- DEBUG: %s --\n", (char *)(context->srr0+8) );
	    kdebug_entry( context );
	    break;
	case L4_TRAP64_KPUTC:
	    putc( (char)context->r3 );
	    break;
	case L4_TRAP64_KGETC:
	    context->r3 = getc(true);
	    break;
	case L4_TRAP64_KGETC_NB:
	    context->r3 = getc(false);
	    break;
	default:
	    goto exception;
	}
	context->srr0 += 4;	/* Skip the trap instruction */
	except_return();
    }
#endif

    if ( *(u32_t *)context->srr0 == KIP_EXCEPT_INSTR )
    {
	//TRACEF( "KernelInterface() at %p (%p)\n", context->srr0, get_current_tcb() );

	context->srr0 += 4;
        space_t * space = get_current_space ();

	{ fpage_t kip_area = space_get_kip_page_area (space);
	  context->r3 = (u64_t) fpage_get_base (&kip_area); }
	context->r4 = api_version_to_word (&get_kip ()->api_version);
	context->r5 = api_flags_to_word (&get_kip ()->api_flags);
	context->r6 = (NULL != get_kip()->kernel_desc_ptr) ?
		    *(word_t *)((word_t)get_kip() + get_kip()->kernel_desc_ptr) : 0;

	except_return();
    }

exception:
    if ( !send_exception_ipc( vect, 0, 0, 0) )
    {
	printf( "\n-- KD# Program check --\n" );
	if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
	    get_kip()->kdebug_entry(context);

	halt_user_thread();
    }

    except_return();
}

/* Instruction storage interrupt (Instruction TLB miss) */
void isi_handler( powerpc64_irq_context_t *context )
{
    word_t srr0 = context->srr0;
    word_t srr1 = context->srr1;

    tcb_t *tcb = get_current_tcb();
    space_t *space = tcb_get_space (tcb);
    if (!space) space = get_kernel_space();

    bool is_kernel = ppc64_is_kernel_mode(srr1);

    TRACEPOINT( except_isi_cnt, "[%p%s] Instruction fault @ %p\n", tcb,
		is_kernel ? " (kernel)" : "", srr0 );

    // If fault is in the kernel area, just map in a kernel page
    if ( EXPECT_FALSE( is_kernel && space_is_kernel_area ((addr_t)srr0) ))
    {
	TRACEF( "kernel execute @ %p\n", srr0 );
	pgent_t pg;
        space = get_kernel_space();
#ifdef CONFIG_POWERPC64_LARGE_PAGES
	pgsize_e size = size_16m;
#else
	pgsize_e size = size_4k;
#endif

	/* Create a dummy page table entry */
	pgent_set_entry( &pg, space, size, virt_to_phys((addr_t)srr0),
		      7, l4default, true );

	/* Insert the kernel mapping, bolted */
	pghash_insert_mapping_bolted( get_pghash(), space, (addr_t)srr0, &pg, size, true );

	//TRACEF( "kernel fault done\n" );
	except_return();
    }

    // Use kernel space if we have kernel fault in TCB area or
    // when no space is set (e.g., running on the idle thread)
    if ( EXPECT_FALSE( is_kernel && space_is_tcb_area ((addr_t)srr0) )
			    || space == NULL)
    {
        space = get_kernel_space();
	//TRACEF( "kernel space\n" );
    }

    if ( EXPECT_FALSE( is_kernel ))
    {
	if( srr0 < USER_AREA_END )
	    TRACEF( "kernel execution in user area: %p\n", srr0 );
	else if( (srr0 < KERNEL_AREA_START) || (srr0 >= KERNEL_AREA_END) )
	    TRACEF( "kernel execution in data area: %p\n", srr0 );
    }

    if( EXPECT_TRUE( EXCEPT_IS_ISI_MISS(srr1) ) ) 
    {
	if( EXPECT_TRUE(space_handle_hash_miss (space, (addr_t)srr0)) ) 
	{
	    //TRACEF("found - returning\n");
	    except_return();
	}
    }
    else if( EXCEPT_IS_FAULT(srr1) )
    {
	// Page found, but access denied
	if( EXPECT_TRUE(space_handle_protection_fault (space, (addr_t)srr0, false )) )
	{
	    //TRACEF("handled - returning\n");
	    except_return();
	}
    } else {
	printf( "-- KD# Unknown instruction fault --\n");
	if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
	    get_kip()->kdebug_entry(context);
    }

    //TRACEF("handle pagefault\n");
    space_handle_pagefault (space, (addr_t)srr0, (addr_t)srr0,
	    SPACE_ACCESS_EXECUTE, ppc64_is_kernel_mode(srr1) );

    // Try to reload the hash table after pagefault
    space_handle_hash_miss (space, (addr_t)srr0);

    //TRACEF("done pagefault\n");
    except_return();
}

/* FIXME - check if the kernel debugger is waiting for this */
void ppc64_except_trace( word_t vect, powerpc64_irq_context_t *context )
{
    printf( "--KD# Trace Point: IP=%p, SRR1=%p --\n", context->srr0, context->srr1 );
    if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
	get_kip()->kdebug_entry(context);
    except_return();
}

void fpu_unavailable_handler( word_t vect, powerpc64_irq_context_t *context )
{
    tcb_t *current = get_current_tcb();

    ASSERT(!ppc64_is_kernel_mode(context->srr1));

    tcb_resources_powerpc64_fpu_unavail_exception (&current->resources, current);

    except_return();
}

#define SYSCALL_SAVED_REGISTERS (EXCEPT_IPC_SYS_MR_NUM+1)

static bool send_syscall_ipc( powerpc64_irq_context_t *context )
{
    tcb_t *current = get_current_tcb();
    if( ({ threadid_t __h = tcb_get_exception_handler (current); threadid_is_nilthread (&__h); }) )
    {
	printf( "Unable to deliver user exception: no exception handler.\n" );
	return false;
    }

    // Save message registers on the stack
    word_t saved_mr[SYSCALL_SAVED_REGISTERS];
    msg_tag_t tag;
    int i;

    // Save message registers.
    for( i = 0; i < SYSCALL_SAVED_REGISTERS; i++ )
	saved_mr[i] = tcb_get_mr (current, i);
    tcb_set_saved_partner (current, tcb_get_partner (current));
    tcb_set_saved_state (current, tcb_get_state (current));

    // Create the message tag.
    msg_tag_set (&tag, 0, EXCEPT_IPC_SYS_MR_NUM, EXCEPT_IPC_SYS_LABEL);
    tcb_set_tag (current, tag);

    // Create the message.
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R3, context->r3);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R4, context->r4);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R5, context->r5);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R6, context->r6);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R7, context->r7);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R8, context->r8);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R9, context->r9);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R10, context->r10);
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_R0, context->r0);

    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_IP, (word_t)tcb_get_user_ip (current));
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_SP, (word_t)tcb_get_user_sp (current));
    tcb_set_mr (current, EXCEPT_IPC_SYS_MR_FLAGS, (word_t)tcb_get_user_flags (current));

    // Deliver the exception IPC.
    tag = tcb_do_ipc (current, tcb_get_exception_handler (current),
	    tcb_get_exception_handler (current), timeout_never() );

    // Alter the user context if necessary.
    if( !msg_tag_is_error (&tag) )
    {
	tcb_set_user_ip (current, (addr_t)tcb_get_mr (current, EXCEPT_IPC_SYS_MR_IP));
	tcb_set_user_sp (current, (addr_t)tcb_get_mr (current, EXCEPT_IPC_SYS_MR_SP));
	tcb_set_user_flags (current, tcb_get_mr (current, EXCEPT_IPC_SYS_MR_FLAGS));
    }
    else
	printf( "Unable to deliver user exception: IPC error.\n" );

    // Results
    context->r3 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R3);
    context->r4 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R4);
    context->r5 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R5);
    context->r6 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R6);
    context->r7 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R7);
    context->r8 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R8);
    context->r9 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R0);
    context->r10 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R10);
    context->r0 = tcb_get_mr (current, EXCEPT_IPC_SYS_MR_R0);

    // Clean-up.
    for( i = 0; i < SYSCALL_SAVED_REGISTERS; i++ )
	tcb_set_mr (current, i, saved_mr[i]);

    tcb_set_partner (current, tcb_get_saved_partner (current));
    tcb_set_saved_partner (current, NILTHREAD);
    tcb_set_state (current, tcb_get_saved_state (current));
    tcb_set_saved_state (current, THREAD_STATE_ABORTED);

    return !msg_tag_is_error (&tag);
}

void ppc64_except_syscall( word_t vect, powerpc64_irq_context_t *context )
{
    if ( !send_syscall_ipc( context ) )
    {
	printf("-- KD# Unhandled user system call --\n");
	if( EXPECT_FALSE(get_kip()->kdebug_entry != NULL) )
	    get_kip()->kdebug_entry(context);

	halt_user_thread();
    }
    except_return();
}

