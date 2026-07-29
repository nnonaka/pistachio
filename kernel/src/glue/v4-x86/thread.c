/*********************************************************************
 *
 * Copyright (C) 2002-2004, 2006-2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/thread.c
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
 ********************************************************************/
#include <debug.h>
#include INC_GLUE(config.h)
#include INC_API(thread.h)
#include INC_API(tcb.h)
#include INC_API(interrupt.h)
#include INC_API(space.h)
#include INC_API(generic-archmap.h)
#include INC_API(schedule.h)
#include INC_ARCH_SA(tss.h)

#if defined(CONFIG_IS_64BIT)
# define EXC_FRAME_SIZE ((sizeof(x86_exceptionframe_t)/BYTES_WORD) - 5)
# else
# define EXC_FRAME_SIZE ((sizeof(x86_exceptionframe_t)/BYTES_WORD) - 4)
#endif

/* sys_ipc (the IPC syscall handler) is defined in C in api/v4/ipc.c.  x64's
   tcb_do_ipc calls it directly; x32's reaches it from inline asm, because its
   calling convention hands back MR1/MR2 in registers. */
#if defined(CONFIG_IS_64BIT)
x86_x64_sysret_t sys_ipc (timeout_t timeout, threadid_t to, threadid_t from);
#else
extern void notify_trampoline (void);
#endif

/* These were declared only in the C++ branches of x64/tcb.h (notify_prologue),
   space.h (active_cpu_space_set) and tcb.h (the present-list globals).  Those
   branches went with the guard collapse, so these are now the only
   declarations -- they belong back in their headers. */
extern void notify_prologue (void);
extern void active_cpu_space_set (cpuid_t cpu, space_t *s);
#if defined(CONFIG_DEBUG)
extern tcb_t *global_present_list;
extern spinlock_t present_list_lock;
#endif

/* Have  naked function */
void return_to_user (void);
void return_to_user_wrapper (void)
{
    /*
     * TODO: perhaps setting ds, es, ss to 0
     * is not needed
     */
    __asm__ (
        ".globl return_to_user          \n"
        ".type return_to_user,@function \n"
        "return_to_user:                \n"
        "    mov %0, %%eax              \n"
        "    mov %%eax, %%ds            \n"
        "    mov %%eax, %%es            \n"
        "    mov %%eax, %%fs            \n"

#if defined(CONFIG_IS_64BIT)
        "    add %1, %%rsp              \n"
        "    iretq                      \n"
#else
#if defined(CONFIG_X_CTRLXFER_MSG)
	"     addl   $16, %%esp		\n"
	"     popa			\n"
	"     addl   $4, %%esp		\n"
#else
	"     add %1, %%esp		\n"
#endif
	"     iret			\n"
#endif
        :
        : "i"(X86_UDS), "i"(EXC_FRAME_SIZE * BYTES_WORD)
        );
}


#if defined(CONFIG_X_X86_HVM)
static void return_to_hvm (void)
{
    tcb_t *current = get_current_tcb ();
    current->get_arch()->enter_hvm_loop();
}
#endif


/**
 * Setup TCB to execute a function when switched to
 * @param func pointer to function
 *
 * The old stack state of the TCB does not matter.
 */
void tcb_create_startup_stack (tcb_t *self, void (*func)(void))
{
    /* init_stack */
    self->stack = tcb_get_stack_top (self);

    word_t cs = X86_UCS;
    word_t flags = X86_USER_FLAGS;
    word_t return_ip = (word_t) return_to_user;

    /* CONFIG_X_X86_HVM / CONFIG_X86_COMPATIBILITY_MODE are off in this config. */

    *(--self->stack) = X86_UDS;			/* ss (rpl = 3) */
    *(--self->stack) = 0x12345678;		/* sp */
    *(--self->stack) = flags;			/* flags */
    *(--self->stack) = cs;			/* cs */
    *(--self->stack) = 0x87654321;		/* ip */
    self->stack -= EXC_FRAME_SIZE;
    *(--self->stack) = return_ip;
    *(--self->stack) = (word_t) func;
}


/* arch/utcb accessor wrappers for the C api/v4 files (declared in api/v4/tcb.h).
   The utcb-delegating ones go through api/v4/generic-utcb.h's accessors, which
   compatibility mode replaces with a 32/64-bit dispatch; the stack-based ones
   index tcb_get_stack_top. */
word_t tcb_get_mr (tcb_t *self, word_t index)		{ ASSERT (index < IPC_NUM_MR); return utcb_get_mr (self->utcb, index); }
void   tcb_set_mr (tcb_t *self, word_t index, word_t value) { ASSERT (index < IPC_NUM_MR); utcb_set_mr (self->utcb, index, value); }
word_t tcb_get_br (tcb_t *self, word_t index)		{ return utcb_get_br (self->utcb, 32U - index); }
void   tcb_set_br (tcb_t *self, word_t index, word_t value) { utcb_set_br (self->utcb, 32U - index, value); }

addr_t tcb_get_user_ip (tcb_t *self)			{ return (addr_t) tcb_get_stack_top (self)[KSTACK_UIP]; }
addr_t tcb_get_user_sp (tcb_t *self)			{ return (addr_t) tcb_get_stack_top (self)[KSTACK_USP]; }
word_t tcb_get_user_flags (tcb_t *self)			{ return tcb_get_stack_top (self)[KSTACK_UFLAGS]; }
void   tcb_set_user_ip (tcb_t *self, addr_t ip)		{ tcb_get_stack_top (self)[KSTACK_UIP] = (word_t) ip; }
void   tcb_set_user_sp (tcb_t *self, addr_t sp)		{ tcb_get_stack_top (self)[KSTACK_USP] = (word_t) sp; }
void   tcb_set_user_flags (tcb_t *self, word_t flags)
{ tcb_get_stack_top (self)[KSTACK_UFLAGS] = (tcb_get_user_flags (self) & (~X86_USER_FLAGMASK)) | (flags & X86_USER_FLAGMASK); }

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
/*
 * With compatibility mode the UTCB is a union of a 32- and a 64-bit UTCB and
 * has no mr[] of its own, so the mr0 offset is spelled out.
 *
 * srXXX: This is rather ugly!  To create a 32-bit thread in a new address
 * space, an application must call ThreadControl without a pager, then
 * SpaceControl with appropriate flags, then ThreadControl with a pager.  The
 * ThreadControl implementation calls space_allocate_utcb, which is implemented
 * in glue and calls this function -- so the resource bits are updated here.
 * The correct fix is to keep a list of TCBs per address space and update all
 * of them in SpaceControl.
 */
#define UTCB_MR0_OFFSET	0x200

word_t tcb_get_utcb_location (tcb_t *self)
{
    if (tcb_get_space (self) && space_is_compatibility_mode (tcb_get_space (self)))
	resource_bits_add (&self->resource_bits, COMPATIBILITY_MODE);
    return threadid_get_raw (&self->myself_local) - UTCB_MR0_OFFSET;
}
void   tcb_set_utcb_location (tcb_t *self, word_t loc)
{ threadid_set_raw (&self->myself_local, loc + UTCB_MR0_OFFSET); }
#else
word_t tcb_get_utcb_location (tcb_t *self)
{ utcb_t *dummy = (utcb_t *) 0; return threadid_get_raw (&self->myself_local) - ((word_t) &dummy->mr[0]); }
void   tcb_set_utcb_location (tcb_t *self, word_t loc)
{ utcb_t *dummy = (utcb_t *) 0; threadid_set_raw (&self->myself_local, loc + ((word_t) &dummy->mr[0])); }
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */


void   tcb_set_cpu (tcb_t *self, cpuid_t cpu)
{
    /* update the pdir cache on migration */
    if (self->space && !self->pdir_cache)
    {
	self->pdir_cache = (word_t) space_get_top_pdir_phys (self->space, cpu);
	ASSERT (self->pdir_cache);
    }
    /* only update UTCB if there is one */
    if (self->utcb)
	utcb_set_processor_no (self->utcb, cpu);
    self->cpu = cpu;
}

void   tcb_set_space (tcb_t *self, space_t *space)
{
    self->space = space;
    self->pdir_cache = space ? (word_t) space_get_top_pdir_phys (space, tcb_get_cpu (self)) : (word_t) 0;
#if defined(CONFIG_X86_COMPATIBILITY_MODE)
    if (space && space_is_compatibility_mode (space))
	resource_bits_add (&self->resource_bits, COMPATIBILITY_MODE);
#endif
}

void   tcb_init_stack (tcb_t *self)			{ self->stack = tcb_get_stack_top (self); }
#if !defined(CONFIG_STATIC_TCBS)
/* Dynamic KTCBs: nothing to do.  The CONFIG_STATIC_TCBS form lives in
   api/v4/thread.c, next to the tcb_array it initialises. */
void   tcb_init_tcbs (void)				{ /* Nothing to do (CONFIG_STATIC_TCBS off). */ }
#endif

word_t * tcb_get_stack_top (tcb_t *self)		{ return (word_t *) addr_offset (self, KTCB_SIZE); }


void   tcb_arch_init_root_server (tcb_t *self, space_t *space, word_t ip, word_t sp)
{ (void) self; (void) ip; space_space_control (space, sp, fpage_nilpage (), fpage_nilpage (), threadid_nilthread ()); }


void   tcb_release_copy_area (tcb_t *self)		{ tcb_resources_release_copy_area (&self->resources, self, true); }


void tcb_lock_state_init (tcb_t *self)
{
#if defined(CONFIG_SMP)
    self->lock_state.flags.raw = 0;
    self->lock_state.flags.X.enabled = true;
#endif
}


/* IPC / thread-switch / notify -- the arch bodies translated from the
   subarchitectures' tcb.h.  These are register-level code; the two
   subarchitectures share their shape and nothing else. */
#if defined(CONFIG_IS_64BIT)

msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to, threadid_t from, timeout_t timeout)
{
    msg_tag_t tag;
    sys_ipc (timeout, to, from);
    tag.raw = utcb_get_mr (self->utcb, 0);
    return tag;
}

void tcb_return_from_ipc (tcb_t *self)
{
    __asm__ ("movq %0, %%rsp\n"
	     "movq %1, %%r11\n"
	     "retq\n"
	     :
	     : "r" (&tcb_get_stack_top (self)[KSTACK_RET_IPC]),
	       "r" (self),
	       "d" (utcb_get_mr (self->utcb, 0)));
    while (1);
}

void tcb_return_from_user_interruption (tcb_t *self)
{
    __asm__ ("movq %0, %%rsp\n"
	     "retq\n"
	     :
	     : "r" (&tcb_get_stack_top (self)[- (word_t) sizeof (x86_exceptionframe_t) / 8 - 1]));
}

#else /* !defined(CONFIG_IS_64BIT) */

msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to, threadid_t from, timeout_t timeout)
{
    msg_tag_t tag;
    word_t mr1, mr2, dummy;
    __asm__ __volatile__("pushl	%%ebp		\n"
			 "pushl	%%ecx		\n"
			 "call	sys_ipc		\n"
			 "addl	$4, %%esp	\n"
			 "movl	%%ebp, %%ecx	\n"
			 "popl	%%ebp		\n"
			 : "=S"(tag.raw),
			   "=b"(mr1),
			   "=c"(mr2),
			   "=a"(dummy),
			   "=d"(dummy)
			 : "a"(threadid_get_raw (&to)),
			   "d"(threadid_get_raw (&from)),
			   "c"(timeout.raw)
			 : "edi", "memory");
    tcb_set_mr (self, 1, mr1);
    tcb_set_mr (self, 2, mr2);
    return tag;
}

void tcb_return_from_ipc (tcb_t *self)
{
    threadid_t local = tcb_get_local_id (self);
    msg_tag_t tag = tcb_get_tag (self);

    __asm__ ("movl %0, %%esp\n"
	     "mov  %3, %%ebp\n"
	     "ret\n"
	     :
	     : "r" (&tcb_get_stack_top (self)[KSTACK_RET_IPC]),
	       "S" (tag.raw),
	       "b" (tcb_get_mr (self, 1)),
	       "r" (tcb_get_mr (self, 2)),
	       "D" (threadid_get_raw (&local)));
}

void tcb_return_from_user_interruption (tcb_t *self)
{
    __asm__ ("movl %0, %%esp\n"
	     "ret\n"
	     :
	     : "r" (&tcb_get_stack_top (self)[- (word_t) sizeof (x86_exceptionframe_t) / 4 - 2]));
}

#endif /* defined(CONFIG_IS_64BIT) */

void tcb_switch_to (tcb_t *self, tcb_t *dest)
{
    word_t dummy;

    ASSERT (dest->stack);
    ASSERT (dest != self);
    ASSERT (tcb_get_cpu (self) == tcb_get_cpu (dest));

    if (EXPECT_FALSE (resource_bits_have_resources (&self->resource_bits)))
	tcb_resources_save (&self->resources, self);

    /* modify stack in tss */
#if defined(CONFIG_IS_64BIT)
    tss.rsp[0] = (u64_t) tcb_get_stack_top (dest);
#else
    x86_tss_set_esp0 (&tss, (u32_t) (word_t) tcb_get_stack_top (dest));
#endif

    tbuf_record_event (TP_DETAIL, 0, "switch %t => %t", (word_t) self, (word_t) dest);

#if defined(CONFIG_SMP)
    active_cpu_space_set (tcb_get_cpu (self), dest->space);
#endif
#if defined(CONFIG_IS_64BIT)
    __asm__ __volatile__ (
	"/* switch_to_thread */			\n\t"
	"movq	%[dtcb], %%r11			\n\t"	/* save dest			*/
	"pushq	%%rbp				\n\t"	/* save rbp			*/

	"pushq	$3f				\n\t"	/* store return address		*/

	"movq	%%rsp, %c[stack](%[stcb])	\n\t"	/* switch stacks		*/
	"movq	%c[stack](%[dtcb]), %%rsp	\n\t"

	"cmpq	%[spdir], %[dpdir]		\n\t"	/* same pdir_cache?		*/
	"je	2f				\n\t"

	"cmpq	$0, %c[space](%[dtcb])		\n\t"	/* kernel thread (space==NULL)?	*/
	"jnz	1f				\n\t"
	"movq	%[spdir], %c[pdir](%[dtcb])	\n\t"	/* yes: update dest->pdir_cache */
	"jmp	2f				\n\t"

	"1:					\n\t"
	"movq	%[dpdir], %%cr3			\n\t"	/* no:  reload pagedir		*/
	"2:					\n\t"
	"popq	%%rdx				\n\t"	/* load (new) return address	*/
	"movq   %[utcb], %%gs:0		        \n\t"   /* update current UTCB		*/
	"jmpq	*%%rdx				\n\t"	/* jump to new return address 	*/

	"3:					\n\t"
	"movq   %%r11, %[stcb]			\n\t"   /* restore this			*/
	"popq	%%rbp				\n\t"	/* restore rbp			*/
	"/* switch_to_thread */			\n\t"
	: /* output */
	  "=a" (dummy),						/* %0 RAX */
	  "=c" (dummy)						/* %1 RCX */
	: /* input */
	  [dtcb]	"D" (dest),				/* %2 RDI */
	  [stcb]	"S" (self),				/* %3 RSI */
	  [stack]	"i" (OFS_TCB_STACK),			/* %4 IMM */
	  [space]	"i" (OFS_TCB_SPACE),			/* %5 IMM */
	  [pdir]	"i" (OFS_TCB_PDIR_CACHE),		/* %6 IMM */
	  [dpdir]	"0" (dest->pdir_cache),			/* %7 RAX */
	  [spdir]	"1" (self->pdir_cache),			/* %8 RCX */
	  [utcb]	"b" (threadid_get_raw (&dest->myself_local))	/* %9 RBX */

	: /* clobber - trash global registers */
	  "memory", "rdx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
	);
#else /* !defined(CONFIG_IS_64BIT) */
    __asm__ __volatile__ (
	"/* switch_to_thread */	\n\t"
	"pushl	%%ebp		\n\t"

	"pushl	$3f		\n\t"	/* store return address	*/

	"movl	%%esp, %c4(%1)	\n\t"	/* switch stacks	*/
	"movl	%c4(%2), %%esp	\n\t"
#if !defined(CONFIG_CPU_X86_P4)
	"movl	%%cr3, %8	\n\t"	/* load current ptab */
	"cmpl	$0, %c5(%2)	\n\t"	/* if kernel thread-> use current */
	"je	2f		\n\t"
#endif
	"cmpl	%7, %8		\n\t"	/* same page dir?	*/
	"je	2f		\n\t"
#if defined(CONFIG_CPU_X86_P4)
	"cmpl	$0, %c5(%2)	\n\t"	/* kernel thread (space==NULL)?	*/
	"jne	1f		\n\t"
	"movl	%8, %c6(%2)	\n\t"	/* rewrite dest->pdir_cache */
	"jmp	2f		\n\t"

	"1:			\n\t"
#endif
	"movl	%7, %%cr3	\n\t"	/* reload pagedir */
	"2:			\n\t"
	"popl	%%edx		\n\t"	/* load activation addr */
	"movl	%3, %%gs:0	\n\t"	/* update current UTCB */

	"jmp	*%%edx		\n\t"
	"3:			\n\t"
	"movl	%2, %1		\n\t"	/* restore this */
	"popl	%%ebp		\n\t"
	"/* switch_to_thread */	\n\t"
	: /* trash everything */
	  "=a" (dummy)				/* 0 */
	:
	"b" (self),				/* 1 */
	"S" (dest),				/* 2 */
	"D" (threadid_get_raw (&dest->myself_local)),	/* 3 */
	"i" (OFS_TCB_STACK),			/* 4 */
	"i" (OFS_TCB_SPACE),			/* 5 */
	"i" (OFS_TCB_PDIR_CACHE),		/* 6 */
	"a" (dest->pdir_cache),			/* 7 */
#if defined(CONFIG_CPU_X86_P4)
	"c" (self->pdir_cache)			/* 8 */
#else
	"c" (dest->pdir_cache)			/* 8 -- dummy */
#endif
	: "edx", "memory"
	);
#endif /* defined(CONFIG_IS_64BIT) */

    if (EXPECT_FALSE (resource_bits_have_resources (&self->resource_bits)))
	tcb_resources_load (&self->resources, self);
}

#if !defined(CONFIG_X86_COMPATIBILITY_MODE)
void tcb_copy_mrs (tcb_t *self, tcb_t *dest, word_t start, word_t count)
{
    ASSERT (start + count <= IPC_NUM_MR);
    ASSERT (count > 0);
    word_t dummy;

    /* use optimized IA32 copy loop -- uses complete cacheline transfers */
    __asm__ __volatile__ (
	"cld\n"
#if defined(CONFIG_IS_64BIT)
	"rep  movsq (%0), (%1)\n"
#else
	"rep  movsl (%0), (%1)\n"
#endif
	: "=S" (dummy), "=D" (dummy), "=c" (dummy)
	: "c" (count), "S" (&self->utcb->mr[start]),
	  "D" (&dest->utcb->mr[start]));
}
#else /* defined(CONFIG_X86_COMPATIBILITY_MODE) */
void tcb_copy_mrs (tcb_t *self, tcb_t *dest, word_t start, word_t count)
{
    ASSERT (start + count <= IPC_NUM_MR);
    ASSERT (count > 0);

    utcb_t *this_utcb = self->utcb;
    utcb_t *dest_utcb = dest->utcb;

    /* Optimized copy loops for 32/32 and 64/64 transfers */
    if (EXPECT_FALSE (resource_bits_have_resource (&self->resource_bits, COMPATIBILITY_MODE)))
    {
	if (EXPECT_FALSE (resource_bits_have_resource (&dest->resource_bits, COMPATIBILITY_MODE)))
	{
	    word_t dummy;
	    __asm__ __volatile__ (
		"repnz movsl (%%rsi), (%%rdi)\n"
		: /* output */
		"=S" (dummy), "=D" (dummy), "=c" (dummy)
		: /* input */
		"c" (count), "S" (&this_utcb->x32.mr[start]),
		"D" (&dest_utcb->x32.mr[start]));
	}
	else
	{
	    if (start == 0)
	    {
		/* Sign-extend the label (for page fault protocol). */
		dest_utcb->x64.mr[0] = (s32_t) this_utcb->x32.mr[0];
		count--;
		start++;
	    }
	    for (; count > 0; count--, start++)
		dest_utcb->x64.mr[start] = this_utcb->x32.mr[start];
	}
    }
    else
    {
	if (EXPECT_FALSE (resource_bits_have_resource (&dest->resource_bits, COMPATIBILITY_MODE)))
	{
	    for (; count > 0; count--, start++)
		dest_utcb->x32.mr[start] = this_utcb->x64.mr[start];
	}
	else
	{
	    word_t dummy;
	    __asm__ __volatile__ (
		"repnz movsq (%%rsi), (%%rdi)\n"
		: /* output */
		"=S" (dummy), "=D" (dummy), "=c" (dummy)
		: /* input */
		"c" (count), "S" (&this_utcb->x64.mr[start]),
		"D" (&dest_utcb->x64.mr[start]));
	}
    }
}
#endif /* !defined(CONFIG_X86_COMPATIBILITY_MODE) */

#if defined(CONFIG_IS_64BIT)

void tcb_notify (tcb_t *self, void (*func)(void))
{
    *(--self->stack) = (word_t) func;
    self->stack -= 2;
    *(--self->stack) = (word_t) notify_prologue;
}
void tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg1)
{
    *(--self->stack) = (word_t) func;
    self->stack--;
    *(--self->stack) = arg1;
    *(--self->stack) = (word_t) notify_prologue;
}
void tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t arg1, word_t arg2)
{
    *(--self->stack) = (word_t) func;
    *(--self->stack) = arg2;
    *(--self->stack) = arg1;
    *(--self->stack) = (word_t) notify_prologue;
}

#else /* !defined(CONFIG_IS_64BIT) */

/* x32 returns into func directly for the no-argument form, and through
   notify_trampoline -- which always removes two parameters -- otherwise. */
void tcb_notify (tcb_t *self, void (*func)(void))
{
    *(--self->stack) = (word_t) func;
}
void tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg1)
{
    self->stack--;
    *(--self->stack) = arg1;
    *(--self->stack) = (word_t) notify_trampoline;
    *(--self->stack) = (word_t) func;
}
void tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t arg1, word_t arg2)
{
    *(--self->stack) = arg2;
    *(--self->stack) = arg1;
    *(--self->stack) = (word_t) notify_trampoline;
    *(--self->stack) = (word_t) func;
}

#endif /* defined(CONFIG_IS_64BIT) */




void   initial_switch_to_c (tcb_t *tcb)
{
#if defined(CONFIG_IS_64BIT)
    __asm__ ("movq %0, %%rsp\n"
	     "retq\n"
	     :
	     : "r" (tcb->stack));
#else
    __asm__ ("movl %0, %%esp\n"
	     "ret\n"
	     :
	     : "r" (tcb->stack));
#endif
    while (1);
}


/* migrate_interrupt_start is declared and defined only under CONFIG_SMP
   (api/v4/interrupt.h/.c), and its only caller -- xcpu_release_thread -- is
   inside the same guard, so the wrapper must be too.  Notes §118. */
#if defined(CONFIG_SMP)
void   migrate_interrupt_start_c (tcb_t *tcb)	{ migrate_interrupt_start (tcb); }
#endif


