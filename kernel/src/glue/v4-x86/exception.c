/*********************************************************************
 *
 * Copyright (C) 2007-2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/exception.c
 * Description:
 *
 * @LICENSE@
 *
 * $Id:$
 *
 ********************************************************************/

#include <debug.h>
#include <kdb/tracepoints.h>
#include <linear_ptab.h>
#include INC_ARCH(traps.h)
#include INC_ARCH(trapgate.h)
#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_API(schedule.h)
#include INC_API(kernelinterface.h)
#include INC_GLUE(traphandler.h)

DECLARE_TRACEPOINT_DETAIL (EXCEPTION_IPC);
DECLARE_TRACEPOINT (X86_NOMATH);
DECLARE_TRACEPOINT (X86_GP);
DECLARE_TRACEPOINT (X86_SEGRELOAD);
DECLARE_TRACEPOINT (X86_UD);
DECLARE_TRACEPOINT (X86_HLT);

#if defined(CONFIG_X86_IO_FLEXPAGES)
#include INC_GLUE(io_space.h)
#endif

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
#include INC_GLUE_SA(x32comp/kernelinterface.h)
#endif

/* asm-named methods (their C++ decls are invisible to C). */
bool space_readmem (space_t *self, addr_t vaddr, word_t *contents);
void tcb_resources_x86_no_math_exception (thread_resources_t *self, tcb_t *tcb);
void tcb_save_state (tcb_t *self);
void tcb_restore_state (tcb_t *self);

/* C form of get_kernel_descriptor()->kernel_id.raw (the KIP-read path). */
static word_t kip_get_kernel_id_raw (kernel_interface_page_t *kip)
{
    kernel_descriptor_t *kd =
	(kernel_descriptor_t *) ((addr_word_t) kip + kip->kernel_desc_ptr);
    return (kd->kernel_id.id << 24) | (kd->kernel_id.subid << 16);
}

/* readmem_u8 is now shared, in generic/linear_ptab.h. */


bool send_exception_ipc(x86_exceptionframe_t * frame, word_t exception)
{
    tcb_t * current = get_current_tcb();
    threadid_t handler = tcb_get_exception_handler (current);
    if (threadid_is_nilthread (&handler))
	return false;

    TRACEPOINT (EXCEPTION_IPC, "exception ipc at %x, %T (%p) -> %T \n",
		frame->__base.regs[X86_EXC_IPREG], tcb_get_global_id (current).raw,
		current, tcb_get_exception_handler (current).raw);

    /* setup exception IPC */
    word_t saved_mr[NUM_EXC_REGS-IPC_NUM_SAVED_MRS];
    msg_tag_t tag;

    msg_tag_set (&tag, 0, 2, (word_t) (-5 << 4));

#if defined(CONFIG_X_CTRLXFER_MSG)
    tag.x.typed += current->append_ctrlxfer_item(tag, 3);
    bool ctrlxfer = (tag.x.typed != 0);
#else
    bool ctrlxfer = false;
#endif

    tcb_save_state (current);

    if (ctrlxfer)
    {
	tcb_set_mr (current, 1, exception);
	tcb_set_mr (current, 2, frame->__base.error);
	acceptor_t acceptor;
	acceptor.raw = 0;
	acceptor_set_rcv_window (&acceptor, fpage_complete_mem ());
	acceptor.x.ctrlxfer = 1;
	tcb_set_br (current, 0, acceptor.raw);
    }
    else
    {
	tag.x.untyped = (tag.x.untyped + NUM_EXC_REGS - 3) & 0x3f;

	for (word_t i = 0; i < NUM_EXC_REGS-IPC_NUM_SAVED_MRS; i++)
	    saved_mr[i] = tcb_get_mr (current, i+IPC_NUM_SAVED_MRS);

	tcb_set_mr (current, x86_exc_reg_mr(0), exception);
	for (word_t num=1; num < NUM_EXC_REGS; num++)
	    tcb_set_mr (current, x86_exc_reg_mr(num), frame->__base.regs[x86_exc_reg_reg(num)]);
    }

    tcb_set_mr (current, 0, tag.raw);
    tag = tcb_do_ipc (current, tcb_get_exception_handler (current),
		      tcb_get_exception_handler (current),
		      timeout_never ());

    if (msg_tag_is_error (&tag))
    {
	printf("exception delivery error tag=%x, error code=%x\n",
	       tag.raw, tcb_get_error_code (current));

	enter_kdebug("exception delivery error");
    }



    if (!ctrlxfer)
    {
	word_t flags = tcb_get_user_flags (current);

	for (word_t num=0; num < NUM_EXC_REGS; num++)
	    frame->__base.regs[x86_exc_reg_reg(num)] = tcb_get_mr (current, x86_exc_reg_mr(num));

	/* mask eflags appropriately */
	tcb_get_stack_top (current)[KSTACK_UFLAGS] &= X86_USER_FLAGMASK;
	tcb_get_stack_top (current)[KSTACK_UFLAGS] |= (flags & ~X86_USER_FLAGMASK);

	for (word_t i = 0; i < NUM_EXC_REGS-IPC_NUM_SAVED_MRS; i++)
	    tcb_set_mr (current, i+IPC_NUM_SAVED_MRS, saved_mr[i]);
    }
    tcb_restore_state (current);

    return !msg_tag_is_error (&tag);
}

/**
 * Try handling the faulting instruction by decoding the instruction
 * stream.  If we are able to handle the fault in the kernel (e.g., by
 * reloading segment registers), we do so without involving the
 * user-level exception handler.
 *
 * @param frame		exception frame
 *
 * @return true if kernel handled the fault, false otherwise
 */
static bool handle_faulting_instruction (x86_exceptionframe_t * frame)
{
    tcb_t * current = get_current_tcb ();
    space_t * space = tcb_get_space (current);
    addr_t instr = (addr_t) frame->__base.regs[X86_EXC_IPREG];
    u8_t i[4];

    if (!readmem_u8 (space, instr, &i[0]))
	return false;

    switch (i[0])
    {
#if defined(CONFIG_X86_SMALL_SPACES)
    case 0xcf:
    {
	/*
	 * CF		iret
	 *
	 * When returning to user-level the instruction pointer
	 * might happen to be outside the small space.  If so, we
	 * must promote the space to a large one.
	 */
	if (! space->is_user_area (instr) &&
	    space->is_small () &&
	    (word_t) current->get_user_ip () > space->smallid ()->size ())
	{
	    space->make_large ();
	    return true;
	}
	break;
    }
#endif

#if defined(CONFIG_X86_IO_FLEXPAGES)

    case 0xe4:  /* in  %al,      port imm8  (byte)  */
    case 0xe6:  /* out %al,      port imm8  (byte)  */
    {
	if (!readmem (space, addr_offset(instr, 1), &i[1]))
	    return false;
	return handle_io_pagefault(current, i[1], 0, instr);
    }
    case 0xe5:  /* in  %eax, port imm8  (dword) */
    case 0xe7:  /* out %eax, port imm8  (dword) */
    {
	if (!readmem (space, addr_offset(instr, 1), &i[1]))
	    return false;
	return handle_io_pagefault(current, i[1], 2, instr);
    }
    case 0xec:  /* in  %al,        port %dx (byte)  */
    case 0xee:  /* out %al,        port %dx (byte)  */
    case 0x6c:  /* insb		   port %dx (byte)  */
    case 0x6e:  /* outsb           port %dx (byte)  */
	return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 0, instr);
    case 0xed:  /* in  %eax,   port %dx (dword) */
    case 0xef:  /* out %eax,   port %dx (dword) */
    case 0x6d:  /* insd	       port %dx (dword) */
    case 0x6f:  /* outsd       port %dx (dword) */
	return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 2, instr);
    case 0x66:
    {
	if (!readmem (space, addr_offset(instr, 1), &i[1]))
	    return false;
	/* operand size override prefix */
	switch (i[1])
	{
	case 0xe5:  /* in  %ax, port imm8  (word) */
	case 0xe7:  /* out %ax, port imm8  (word) */
	{
	    if (!readmem (space, addr_offset(instr, 2), &i[2]))
                return false;
	    return handle_io_pagefault(current, i[2], 1, instr);
	}
	case 0xed:  /* in  %ax, port %dx  (word) */
	case 0xef:  /* out %ax, port %dx  (word) */
	case 0x6d:  /* insw     port %dx  (word) */
	case 0x6f:  /* outsw    port %dx  (word) */
	    return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 1, instr);
	}
    }
    case 0xf3:
    {
	/* rep instruction */
	if (!readmem (space, addr_offset(instr, 1), &i[1]))
	    return false;
        switch (i[1])
	{
        case 0xe4:  /* in  %al,  port imm8  (byte)  */
        case 0xe6:  /* out %al,  port imm8  (byte)  */
        {
	    if (!readmem (space, addr_offset(instr, 2), &i[2]))
		return false;
	    return handle_io_pagefault(current, i[2], 0, instr);
        }
        case 0xe5:  /* in  %eax, port imm8  (dword) */
        case 0xe7:  /* out %eax, port imm8  (dword) */
        {
	    if (!readmem (space, addr_offset(instr, 2), &i[2]))
		return false;
	    return handle_io_pagefault(current, i[2], 2, instr);
        }
        case 0xec:  /* in  %al,    port %dx (byte)  */
        case 0xee:  /* out %al,    port %dx (byte)  */
        case 0x6c:  /* insb        port %dx (byte)  */
        case 0x6e:  /* outsb       port %dx (byte)  */
	    return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 0, instr);
        case 0xed:  /* in  %eax,   port %dx (dword) */
        case 0xef:  /* out %eax,   port %dx (dword) */
        case 0x6d:  /* insd        port %dx (dword) */
        case 0x6f:  /* outsd       port %dx (dword) */
	    return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 2, instr);
        case 0x66:
	{
            /* operand size override prefix */
	    if (!readmem (space, addr_offset(instr, 2), &i[2]))
		return false;
            switch (i[2])
            {
            case 0xe5:  /* in  %ax, port imm8  (word) */
            case 0xe7:  /* out %ax, port imm8  (word) */
            {
		if (!readmem (space, addr_offset(instr, 3), &i[3]))
		    return false;
		return handle_io_pagefault(current, i[3], 1, instr);
            }
            case 0xed:  /* in  %ax, port %dx  (word) */
            case 0xef:  /* out %ax, port %dx  (word) */
            case 0x6d:  /* insw	    port %dx  (word) */
            case 0x6f:  /* outsw    port %dx  (word) */
		return handle_io_pagefault(current, frame->__base.regs[X86_EXC_RDXREG] & 0xFFFF, 1, instr);
	    }
	}
	}
    }
#endif

    case 0x0f: /* two-byte instruction prefix */
    {
	if (!readmem_u8 (space, addr_offset(instr, 1), &i[1]))
	    return false;
	switch( i[1] )
	{
	case 0x30:
    	    /* wrmsr */
	    if ( is_privileged_space_c (space) ) {
		/* the MSR index is taken from ECX only, so truncating is correct */
		x86_wrmsr ((u32_t) frame->__base.regs[X86_EXC_RCXREG],
			   ((u64_t)(frame->__base.regs[X86_EXC_RAXREG])) |
			   ((u64_t)(frame->__base.regs[X86_EXC_RDXREG])) << 32);
		frame->__base.regs[X86_EXC_IPREG] += 2;
		return true;
	    } break;

	case 0x32:
	    /* rdmsr */
	    if ( is_privileged_space_c (space) ) {
		/* the MSR index is taken from ECX only, so truncating is correct */
		u64_t val = x86_rdmsr ((u32_t) frame->__base.regs[X86_EXC_RCXREG]);
		frame->__base.regs[X86_EXC_RAXREG] = (u32_t) val;
		frame->__base.regs[X86_EXC_RDXREG] = (u32_t)(val >> 32);
		frame->__base.regs[X86_EXC_IPREG] += 2;
		return true;
	    } break;

	case 0xa1:
	case 0xa9:
	    goto pop_seg;
	}
	break;
    } /* two-byte prefix */

    case 0x8e:
    case 0x07:
    case 0x17:
    case 0x1f:
    pop_seg:
	/*
	 * 8E /r	mov %reg, %segreg
	 * 07		pop %es
	 * 17		pop %ss
	 * 1F		pop %ds
	 * 0F A1	pop %fs
	 * 0F A9	pop %gs
	 *
	 * Segment descriptor is being written with an invalid value.
	 * Reset all descriptors with the correct value and skip the
	 * instruction.
	 */

	if (! space_is_user_area (instr))
	    // Assume that kernel knows what it is doing.
	    break;

	TRACEPOINT (X86_SEGRELOAD, "segment register reload");
	reload_user_segregs_c ();
#if defined(CONFIG_SUBARCH_X32)
	frame->ds = frame->es = X86_UDS;
#endif
	frame->__base.regs[X86_EXC_IPREG]++;

	if (i[0] == 0x8e || i[0] == 0x0f)
	    frame->__base.regs[X86_EXC_IPREG]++;

	return true;
    case 0xf4:
	/* HLT */
        if (sched_idle_hlt ())
        {
	    frame->__base.regs[X86_EXC_IPREG]++;
            return true;
        }
    }

#if defined(CONFIG_X86_SMALL_SPACES)
    /*
     * A GP(0) or SS(0) might indicate that a small address space
     * tries to access memory outside of the small space boundary.
     * Try to promote space to a large one instead of sending
     * exception IPC.
     */
    if ((frame->reason == X86_EXC_STACKSEG_FAULT ||
         frame->reason == X86_EXC_GENERAL_PROTECTION) &&
        frame->__base.error == 0 && space->is_small ())
    {
        space->make_large ();
        return true;
    }
#endif

    return false;
}

EXTERN_C void sysexit_tramp (void);
EXTERN_C void sysexit_tramp_end (void);
EXTERN_C void reenter_sysexit (void);

X86_EXCWITH_ERRORCODE(exc_gp, X86_EXC_GENERAL_PROTECTION)
{
#if defined(CONFIG_DEBUG)
    if (kdebug_check_interrupt())
        return;
#endif

    TRACEPOINT (X86_GP, "general protection fault @ %p, error: %x\n",
                frame->__base.regs[X86_EXC_IPREG], frame->__base.error);

#if defined(CONFIG_X86_SMALL_SPACES) && defined(CONFIG_X86_SYSENTER)
    /*
     * Check if we caught an exception in the sysexit trampoline.
     */
    tcb_t * current = get_current_tcb ();
    addr_t user_eip = current->get_user_ip ();

    if (user_eip >= (addr_t) sysexit_tramp &&
        user_eip <  (addr_t) sysexit_tramp_end)

    {
        /*
         * If we faulted at the LRET instruction or otherwise was
         * interrupted during the sysexit trampoline (i.e., still in
         * user level) we must IRET to the kernel due to the user
         * space code segment limitation.  We must also disable
         * interrupts since we can not be allowed to be preempted in
         * the reenter-trampoline.
         */
        frame->cs = X86_KCS;
        frame->eflags &= ~X86_FLAGS_IF;
        frame->ecx = (word_t) current->get_user_sp ();
        frame->eip = (word_t) reenter_sysexit;
        return;
    }
#endif

#if defined(CONFIG_SUBARCH_X32)
    /*
     * A GP(0) could mean that we have a segment register with zero
     * contents.  If so, just reload all segment register selectors
     * with appropriate values.
     */

    if (frame->__base.error == 0)
    {
        word_t fs, gs;
        asm ("	mov	%%fs, %w0	\n"
             "	mov	%%gs, %w1	\n"
             :"=r"(fs), "=r"(gs));

        if ((frame->ds & 0xffff) == 0 || (frame->es & 0xffff) == 0 ||
            fs == 0 || gs == 0 )
        {
            printf ("segment register reload\n");

            TRACEPOINT (X86_SEGRELOAD, "segment register reload");
            reload_user_segregs_c ();
            frame->ds = frame->es =
                (frame->cs & 0xffff) == X86_UCS ? X86_UDS : X86_KDS;
            return;
        }
    }
#endif

    /*
     * In some cases we handle the faulting instruction without
     * involving the user-level exception handler.
     */
    if (handle_faulting_instruction (frame))
        return;


    if (send_exception_ipc(frame, X86_EXC_GENERAL_PROTECTION))
        return;

#ifdef CONFIG_KDB
    frame->dump();
    word_t ds = 0 , es = 0, fs = 0, gs = 0;

    __asm__ (
        "mov %%ds, %0   \n"
        "mov %%es, %1	\n"
        "mov %%fs, %2   \n"
        "mov %%gs, %3   \n"
        :
        : "r"(ds), "r"(es), "r"(fs),"r"(gs)
        );

    enter_kdebug("#GP");
#endif

    tcb_set_state (get_current_tcb(), THREAD_STATE_HALTED);
    sched_schedule (get_idle_tcb_c(), sched_handoff);
}




X86_EXCNO_ERRORCODE(exc_invalid_opcode, X86_EXC_INVALIDOPCODE)
{
    tcb_t * current = get_current_tcb();
    space_t * space = tcb_get_space (current);
    addr_t addr = (addr_t) frame->__base.regs[X86_EXC_IPREG];

    TRACEPOINT (X86_UD, "x86_ud at %x (%x) (current=%x)", addr, space_get_from_user (space, addr), current);

    /* instruction emulation */
    switch( (u8_t) space_get_from_user (space, addr))
    {
    case 0xf0: /* lock prefix */
        if ( (u8_t) space_get_from_user (space, addr_offset(addr, 1)) == 0x90)
        {
            /* lock; nop */
	    fpage_t kip_area = space_get_kip_page_area (space);
            frame->__base.regs[X86_EXC_RAXREG] = (word_t) fpage_get_base (&kip_area);
            frame->__base.regs[X86_EXC_RCXREG] = api_version_to_word (&get_kip()->api_version);
            frame->__base.regs[X86_EXC_RSIREG] = kip_get_kernel_id_raw (get_kip());
#if defined(CONFIG_X86_COMPATIBILITY_MODE)
            if (space->is_compatibility_mode())
            {
                /* srXXX: Hack: Update system and user base in 32-bit KIP.
                   This is necessary because they are not set in the initialization phase. */
                x32::get_kip()->thread_info.set_system_base(get_kip()->thread_info.get_system_base());
                x32::get_kip()->thread_info.set_user_base(get_kip()->thread_info.get_user_base());
                frame->__base.regs[X86_EXC_RDXREG] = x32::get_kip()->api_flags;
                frame->__base.regs[X86_EXC_IPREG] += 2;
                return;
            }
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */
            frame->__base.regs[X86_EXC_RDXREG] = api_flags_to_word (&get_kip()->api_flags);
            frame->__base.regs[X86_EXC_IPREG] += 2;
            return;
        }

    default:
        printf ("%p: invalid opcode at IP %p\n", current, addr);
        enter_kdebug("invalid opcode");
    }

    if (send_exception_ipc(frame, X86_EXC_INVALIDOPCODE))
        return;

    tcb_set_state (get_current_tcb(), THREAD_STATE_HALTED);
    sched_schedule (get_idle_tcb_c(), sched_handoff);

}



X86_EXCNO_ERRORCODE(exc_nomath_coproc, X86_EXC_NOMATH_COPROC)
{
    tcb_t * current = get_current_tcb();

    TRACEPOINT(X86_NOMATH, "X86_NOMATH %t @ %p\n",
               current, frame->__base.regs[X86_EXC_IPREG]);

    tcb_resources_x86_no_math_exception (&current->resources, current);
}


u64_t exc_catch_all[IDT_SIZE] UNIT("x86.exc_all");
EXTERN_C void exc_catch_common_handler(x86_exceptionframe_t *frame)
{

    word_t exc  = (frame->__base.error - 5 - (word_t) exc_catch_all) / 8;
    if (send_exception_ipc(frame, exc))
        return;


#if defined(CONFIG_IOAPIC)
    if (exc == 15)
    {
        TRACE("Ignoring spurious APIC interrupt\n");
        return;
    }
#endif
    printf("Unhandled exception %d\n", exc);

    enter_kdebug("Exception caught");

    tcb_set_state (get_current_tcb(), THREAD_STATE_HALTED);
    sched_schedule (get_idle_tcb_c(), sched_handoff);

}
