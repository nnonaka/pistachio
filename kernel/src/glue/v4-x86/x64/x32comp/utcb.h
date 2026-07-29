/*********************************************************************
 *                
 * Copyright (C) 2006, 2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/x32comp/utcb.h
 * Description:   UTCB spanning the 32- and 64-bit layouts
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
 * $Id: types.h,v 1.2 2006/10/20 16:18:38 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X64__X32__UTCB_H__
#define __GLUE_V4_X86__X64__X32__UTCB_H__

#include INC_GLUE_SA(x32comp/types.h)
#include INC_GLUE_SA(x32comp/thread.h)

/*
 * The 32-bit UTCB: the same layout as the 64-bit one, in 32-bit words.  Only
 * the API types need renaming here -- the struct takes its name from
 * UTCB_NAME -- but the rename is the whole x32 set, since that is what stands
 * in for `namespace x32'.
 */
#include INC_GLUE_SA(x32comp/x32-names.h)

#define UTCB_NAME x32_utcb_t
#include INC_GLUE(utcb-body.h)
#undef UTCB_NAME

#define X32_UNRENAME
#include INC_GLUE_SA(x32comp/x32-names.h)
#undef X32_UNRENAME

/* Parts of the padding around the 32-bit UTCB, used by
   user_exchange_registers_32. */
struct utcb_exreg32_t
{
    word_t	   compatibility_mode;	/* -512 */
    x32_word_t	   is_local;		/* -504 */
    x32_threadid_t pager;		/* -500 */
    x32_word_t	   control;		/* -496 */
} __attribute__((packed));
typedef struct utcb_exreg32_t utcb_exreg32_t;

/* The UTCB proper: whichever of the two the thread's space selected. */
struct utcb_t
{
    union {
	x64_utcb_t x64;
	struct {
	    word_t padding[32];
	    x32_utcb_t x32;
	};
	bool compatibility_mode;
	utcb_exreg32_t exreg32;
    };
};
typedef struct utcb_t utcb_t;

/*
 * The api/v4/generic-utcb.h accessor set, dispatching on the mode.  Everything
 * outside this file reaches the UTCB through these, which is what lets the
 * two layouts stay confined to compatibility mode.
 */

INLINE bool utcb_is_compatibility_mode (utcb_t *self)
{
    return EXPECT_FALSE (self->compatibility_mode);
}

INLINE void utcb_set_compatibility_mode (utcb_t *self, bool cm)
{
    self->compatibility_mode = cm;
}

INLINE void utcb_set_my_global_id (utcb_t *self, threadid_t tid)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.my_global_id = threadid_32 (tid);
    else
	self->x64.my_global_id = tid;
}

INLINE void utcb_set_processor_no (utcb_t *self, word_t cpu)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.processor_no = (x32_word_t) cpu;
    else
	self->x64.processor_no = cpu;
}

INLINE word_t utcb_get_user_defined_handle (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return self->x32.user_defined_handle;
    else
	return self->x64.user_defined_handle;
}

INLINE void utcb_set_user_defined_handle (utcb_t *self, word_t handle)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.user_defined_handle = (x32_word_t) handle;
    else
	self->x64.user_defined_handle = handle;
}

INLINE threadid_t utcb_get_pager (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return threadid_64 (self->x32.pager);
    else
	return self->x64.pager;
}

INLINE void utcb_set_pager (utcb_t *self, threadid_t tid)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.pager = threadid_32 (tid);
    else
	self->x64.pager = tid;
}

INLINE threadid_t utcb_get_exception_handler (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return threadid_64 (self->x32.exception_handler);
    else
	return self->x64.exception_handler;
}

INLINE void utcb_set_exception_handler (utcb_t *self, threadid_t tid)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.exception_handler = threadid_32 (tid);
    else
	self->x64.exception_handler = tid;
}

INLINE u8_t utcb_get_preempt_flags (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return self->x32.preempt_flags;
    else
	return self->x64.preempt_flags;
}

INLINE void utcb_set_preempt_flags (utcb_t *self, u8_t flags)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.preempt_flags = flags;
    else
	self->x64.preempt_flags = flags;
}

INLINE u8_t utcb_get_cop_flags (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return self->x32.cop_flags;
    else
	return self->x64.cop_flags;
}

INLINE word_t utcb_get_error_code (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return self->x32.error_code;
    else
	return self->x64.error_code;
}

INLINE void utcb_set_error_code (utcb_t *self, word_t err)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.error_code = (x32_word_t) err;
    else
	self->x64.error_code = err;
}

INLINE timeout_t utcb_get_xfer_timeout (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return timeout_64 (self->x32.xfer_timeout);
    else
	return self->x64.xfer_timeout;
}

INLINE threadid_t utcb_get_intended_receiver (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return threadid_64 (self->x32.intended_receiver);
    else
	return self->x64.intended_receiver;
}

INLINE threadid_t utcb_get_virtual_sender (utcb_t *self)
{
    if (utcb_is_compatibility_mode (self))
	return threadid_64 (self->x32.virtual_sender);
    else
	return self->x64.virtual_sender;
}

INLINE void utcb_set_virtual_sender (utcb_t *self, threadid_t tid)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.virtual_sender = threadid_32 (tid);
    else
	self->x64.virtual_sender = tid;
}

/*
 * Message and buffer registers.  MR0 carries the message tag, whose label the
 * page-fault protocol expects sign-extended; the other MRs must not be, or
 * page faults past 2G could not be described.
 */

INLINE word_t utcb_get_mr (utcb_t *self, word_t index)
{
    if (utcb_is_compatibility_mode (self))
    {
	x32_word_t result = self->x32.mr[index];
	if (index == 0)
	    return (word_t) (s64_t) (s32_t) result;
	else
	    return result;
    }
    else
	return self->x64.mr[index];
}

INLINE void utcb_set_mr (utcb_t *self, word_t index, word_t value)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.mr[index] = (x32_word_t) value;
    else
	self->x64.mr[index] = value;
}

INLINE word_t utcb_get_br (utcb_t *self, word_t slot)
{
    if (utcb_is_compatibility_mode (self))
	return self->x32.br[slot];
    else
	return self->x64.br[slot];
}

INLINE void utcb_set_br (utcb_t *self, word_t slot, word_t value)
{
    if (utcb_is_compatibility_mode (self))
	self->x32.br[slot] = (x32_word_t) value;
    else
	self->x64.br[slot] = value;
}


#endif /* !__GLUE_V4_X86__X64__X32__UTCB_H__ */
