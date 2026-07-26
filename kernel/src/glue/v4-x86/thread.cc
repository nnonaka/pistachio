/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006-2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/thread.cc
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

/* Have  naked function */
extern "C" void return_to_user();
void return_to_user_wrapper()
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


static inline void push(word_t * &stack, word_t val)
{
    *(--stack) = val;
}

#if defined(CONFIG_X_X86_HVM)
static void return_to_hvm()
{
    tcb_t *current = get_current_tcb();
    current->get_arch()->enter_hvm_loop();
}
#endif


/**
 * Setup TCB to execute a function when switched to
 * @param func pointer to function
 *
 * The old stack state of the TCB does not matter.
 */
void tcb_t::create_startup_stack(void (*func)())
{
    init_stack();

    word_t cs = X86_UCS;
    word_t flags = X86_USER_FLAGS;
    word_t return_ip = (word_t) return_to_user;
    
#if defined(CONFIG_X_X86_HVM)
    if (this->resource_bits.have_resource (HVM))
    {
	return_ip = (word_t) return_to_hvm;
	flags = X86_HVM_FLAGS;
    }
#endif    
#if defined(CONFIG_X86_COMPATIBILITY_MODE)
    if (resource_bits.have_resource(COMPATIBILITY_MODE))
        cs = X86_UCS32;       /* cs */
#endif   

    push(stack, X86_UDS);               /* ss (rpl = 3) */
    push(stack, 0x12345678);            /* sp */
    push(stack, flags);			/* flags */
    push(stack, cs);			/* cs */
    push(stack, 0x87654321);            /* ip */
    stack -= EXC_FRAME_SIZE;
    push(stack, return_ip);
    push(stack, (word_t)func);
}



/* arch/utcb accessor wrappers for api/v4/thread.c (declared in api/v4/tcb.h). */
BEGIN_DECLS
msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to, threadid_t from, timeout_t timeout)
					{ return self->do_ipc (to, from, timeout); }
void tcb_return_from_ipc (tcb_t *self)			{ self->return_from_ipc (); }
void tcb_return_from_user_interruption (tcb_t *self)	{ self->return_from_user_interruption (); }
addr_t tcb_get_user_ip (tcb_t *self)			{ return self->get_user_ip (); }
addr_t tcb_get_user_sp (tcb_t *self)			{ return self->get_user_sp (); }
void tcb_set_user_ip (tcb_t *self, addr_t ip)		{ self->set_user_ip (ip); }
void tcb_set_user_sp (tcb_t *self, addr_t sp)		{ self->set_user_sp (sp); }
word_t tcb_get_user_flags (tcb_t *self)			{ return self->get_user_flags (); }
void tcb_set_user_flags (tcb_t *self, word_t flags)	{ self->set_user_flags (flags); }
word_t tcb_get_user_handle (tcb_t *self)		{ return self->get_user_handle (); }
void tcb_set_user_handle (tcb_t *self, word_t handle)	{ self->set_user_handle (handle); }
void tcb_arch_init_root_server (tcb_t *self, space_t *space, word_t ip, word_t sp)
					{ self->arch_init_root_server (space, ip, sp); }
void tcb_init_stack (tcb_t *self)			{ self->init_stack (); }
void tcb_create_startup_stack (tcb_t *self, void (*func)(void)) { self->create_startup_stack (func); }
msg_tag_t tcb_get_tag (tcb_t *self)			{ return self->get_tag (); }
void tcb_set_tag (tcb_t *self, msg_tag_t tag)		{ self->set_tag (tag); }
word_t tcb_get_br (tcb_t *self, word_t index)		{ return self->get_br (index); }
void tcb_set_br (tcb_t *self, word_t index, word_t value) { self->set_br (index, value); }
threadid_t tcb_get_pager (tcb_t *self)			{ return self->get_pager (); }
void tcb_set_pager (tcb_t *self, threadid_t tid)	{ self->set_pager (tid); }
void tcb_set_exception_handler (tcb_t *self, threadid_t tid) { self->set_exception_handler (tid); }
threadid_t tcb_get_virtual_sender (tcb_t *self)		{ return self->get_virtual_sender (); }
void tcb_set_actual_sender (tcb_t *self, threadid_t tid)	{ self->set_actual_sender (tid); }
word_t tcb_get_utcb_location (tcb_t *self)		{ return self->get_utcb_location (); }
void tcb_set_global_id (tcb_t *self, threadid_t tid)	{ self->set_global_id (tid); }
word_t tcb_get_error_code (tcb_t *self)			{ return self->get_error_code (); }
void tcb_set_cpu (tcb_t *self, cpuid_t cpu)		{ self->set_cpu (cpu); }
void tcb_set_utcb_location (tcb_t *self, word_t loc)	{ self->set_utcb_location (loc); }
END_DECLS

BEGIN_DECLS
bool thread_control_interrupt_c (threadid_t irq_tid, threadid_t handler_tid)
{ return thread_control_interrupt (irq_tid, handler_tid); }
END_DECLS


/* C wrappers migrated from api/v4/thread.cc when it became C: they wrap tcb_t
   methods defined in glue headers, or free functions, for the C api/v4 files. */
BEGIN_DECLS
word_t tcb_get_mr (tcb_t *self, word_t index)			{ return self->get_mr (index); }
void   tcb_set_mr (tcb_t *self, word_t index, word_t value)	{ self->set_mr (index, value); }
void   tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg) { self->notify (func, arg); }
addr_t tcb_copy_area_real_address (tcb_t *self, addr_t addr)	{ return self->copy_area_real_address (addr); }
void   tcb_set_error_code (tcb_t *self, word_t err)		{ self->set_error_code (err); }
bool   tcb_is_local_cpu (tcb_t *self)				{ return self->is_local_cpu (); }
time_t tcb_get_xfer_timeout_snd (tcb_t *self)			{ return self->get_xfer_timeout_snd (); }
time_t tcb_get_xfer_timeout_rcv (tcb_t *self)			{ return self->get_xfer_timeout_rcv (); }
void   tcb_sched_set_timeout (tcb_t *self, time_t t)		{ self->sched_state.set_timeout (t); }
u64_t  time_get_microseconds (time_t *self)			{ return self->get_microseconds (); }
bool   time_lt (time_t a, time_t b)				{ return a < b; }
tcb_t * get_idle_tcb_c (void)					{ return get_idle_tcb (); }
tcb_t * get_dummy_tcb_c (void)					{ return get_dummy_tcb (); }
void   handle_ipc_timeout_c (word_t state)			{ handle_ipc_timeout (state); }
bool   is_privileged_space_c (space_t *space)			{ return is_privileged_space (space); }
void   spin_forever_c (int pos)					{ spin_forever (pos); }
void   arch_unmap_fpage_c (tcb_t *from, fpage_t fpage, bool flush) { arch_unmap_fpage (from, fpage, flush); }
END_DECLS

BEGIN_DECLS
void tcb_set_space (tcb_t *self, space_t *space)	{ self->set_space (space); }
void tcb_init_saved_state (tcb_t *self)			{ self->init_saved_state (); }
void tcb_dequeue_send (tcb_t *self, tcb_t *t)		{ self->dequeue_send (t); }
void tcb_enqueue_present (tcb_t *self)			{ self->enqueue_present (); }
void tcb_dequeue_present (tcb_t *self)			{ self->dequeue_present (); }
void tcb_lock_init (tcb_t *self)			{ self->tcb_lock.init (); }
void tcb_lock_state_init (tcb_t *self)
{
#if defined(CONFIG_SMP)
    self->lock_state.init (true);
#endif
}
void tcb_lock (tcb_t *self)				{ self->lock (); }
void tcb_unlock (tcb_t *self)				{ self->unlock (); }
END_DECLS

BEGIN_DECLS
void tcb_notify (tcb_t *self, void (*func)(void))	{ self->notify (func); }
void tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t a1, word_t a2)
							{ self->notify (func, a1, a2); }
END_DECLS

BEGIN_DECLS
void tcb_release_copy_area (tcb_t *self)		{ self->release_copy_area (); }
void migrate_interrupt_start_c (tcb_t *tcb)		{ migrate_interrupt_start (tcb); }
END_DECLS
