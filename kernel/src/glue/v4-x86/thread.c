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

/* sys_ipc (the IPC syscall handler) is defined in C in api/v4/ipc.c; tcb_do_ipc
   below calls it directly. */
x86_x64_sysret_t sys_ipc (timeout_t timeout, threadid_t to, threadid_t from);

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
   utcb_t is a plain C-visible struct, so the utcb-delegating accessors read
   self->utcb->field directly; the stack-based ones index tcb_get_stack_top. */
word_t tcb_get_mr (tcb_t *self, word_t index)		{ ASSERT (index < IPC_NUM_MR); return self->utcb->mr[index]; }
void   tcb_set_mr (tcb_t *self, word_t index, word_t value) { ASSERT (index < IPC_NUM_MR); self->utcb->mr[index] = value; }
word_t tcb_get_br (tcb_t *self, word_t index)		{ return self->utcb->br[32U - index]; }
void   tcb_set_br (tcb_t *self, word_t index, word_t value) { self->utcb->br[32U - index] = value; }

addr_t tcb_get_user_ip (tcb_t *self)			{ return (addr_t) tcb_get_stack_top (self)[KSTACK_UIP]; }
addr_t tcb_get_user_sp (tcb_t *self)			{ return (addr_t) tcb_get_stack_top (self)[KSTACK_USP]; }
word_t tcb_get_user_flags (tcb_t *self)			{ return tcb_get_stack_top (self)[KSTACK_UFLAGS]; }
void   tcb_set_user_ip (tcb_t *self, addr_t ip)		{ tcb_get_stack_top (self)[KSTACK_UIP] = (word_t) ip; }
void   tcb_set_user_sp (tcb_t *self, addr_t sp)		{ tcb_get_stack_top (self)[KSTACK_USP] = (word_t) sp; }
void   tcb_set_user_flags (tcb_t *self, word_t flags)
{ tcb_get_stack_top (self)[KSTACK_UFLAGS] = (tcb_get_user_flags (self) & (~X86_USER_FLAGMASK)) | (flags & X86_USER_FLAGMASK); }

word_t tcb_get_utcb_location (tcb_t *self)
{ utcb_t *dummy = (utcb_t *) 0; return threadid_get_raw (&self->myself_local) - ((word_t) &dummy->mr[0]); }
void   tcb_set_utcb_location (tcb_t *self, word_t loc)
{ utcb_t *dummy = (utcb_t *) 0; threadid_set_raw (&self->myself_local, loc + ((word_t) &dummy->mr[0])); }


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
	self->utcb->processor_no = cpu;
    self->cpu = cpu;
}

void   tcb_set_space (tcb_t *self, space_t *space)
{
    self->space = space;
    self->pdir_cache = space ? (word_t) space_get_top_pdir_phys (space, tcb_get_cpu (self)) : (word_t) 0;
}

void   tcb_init_stack (tcb_t *self)			{ self->stack = tcb_get_stack_top (self); }
void   tcb_init_tcbs (void)				{ /* Nothing to do (CONFIG_STATIC_TCBS off). */ }

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


/* IPC / thread-switch / notify -- the arch bodies translated from x64/tcb.h. */
msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to, threadid_t from, timeout_t timeout)
{
    msg_tag_t tag;
    sys_ipc (timeout, to, from);
    tag.raw = self->utcb->mr[0];
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
	       "d" (self->utcb->mr[0]));
    while (1);
}

void tcb_return_from_user_interruption (tcb_t *self)
{
    __asm__ ("movq %0, %%rsp\n"
	     "retq\n"
	     :
	     : "r" (&tcb_get_stack_top (self)[- (word_t) sizeof (x86_exceptionframe_t) / 8 - 1]));
}

void tcb_switch_to (tcb_t *self, tcb_t *dest)
{
    word_t dummy;

    ASSERT (dest->stack);
    ASSERT (dest != self);
    ASSERT (tcb_get_cpu (self) == tcb_get_cpu (dest));

    if (EXPECT_FALSE (resource_bits_have_resources (&self->resource_bits)))
	tcb_resources_save (&self->resources, self);

    /* modify stack in tss */
    tss.rsp[0] = (u64_t) tcb_get_stack_top (dest);

    tbuf_record_event (TP_DETAIL, 0, "switch %t => %t", (word_t) self, (word_t) dest);

#if defined(CONFIG_SMP)
    active_cpu_space_set (tcb_get_cpu (self), dest->space);
#endif
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

    if (EXPECT_FALSE (resource_bits_have_resources (&self->resource_bits)))
	tcb_resources_load (&self->resources, self);
}

void tcb_copy_mrs (tcb_t *self, tcb_t *dest, word_t start, word_t count)
{
    ASSERT (start + count <= IPC_NUM_MR);
    ASSERT (count > 0);
    word_t dummy;

    /* use optimized IA32 copy loop -- uses complete cacheline transfers */
    __asm__ __volatile__ (
	"cld\n"
	"rep  movsq (%0), (%1)\n"
	: "=S" (dummy), "=D" (dummy), "=c" (dummy)
	: "c" (count), "S" (&self->utcb->mr[start]),
	  "D" (&dest->utcb->mr[start]));
}

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


tcb_t * get_dummy_tcb_c (void)			{ extern tcb_t *__dummy_tcb; return __dummy_tcb; }


void   initial_switch_to_c (tcb_t *tcb)
{
    __asm__ ("movq %0, %%rsp\n"
	     "retq\n"
	     :
	     : "r" (tcb->stack));
    while (1);
}


void   migrate_interrupt_start_c (tcb_t *tcb)	{ migrate_interrupt_start (tcb); }


