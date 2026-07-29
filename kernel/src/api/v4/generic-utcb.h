/*********************************************************************
 *                
 * Copyright (C) 2006,  Karlsruhe University
 *                
 * File path:     api/v4/generic-utcb.h
 * Description:   Generic V4 UTCB access functions
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
 * $Id: generic-utcb.h,v 1.2 2006/10/20 16:29:11 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__GENERIC_UTCB_H__
#define __API__V4__GENERIC_UTCB_H__

/*
 * Generic UTCB accessors.  The fields are C-visible, so on every configuration
 * but one these are literally the field access spelled out.  The exception is
 * x86-x64 compatibility mode, where utcb_t is a union of a 64-bit and a 32-bit
 * UTCB and the accessors have to pick between them -- see
 * glue/v4-x86/x64/x32comp/utcb.h, which renames this file's definitions out of
 * the way and supplies its own dispatching set under these names.
 *
 * Call sites therefore go through the accessors rather than touching the
 * fields, so that the compatibility-mode dispatch has somewhere to live.
 */

INLINE void utcb_set_my_global_id (utcb_t *self, threadid_t tid)
{
    self->my_global_id = tid;
}

INLINE void utcb_set_processor_no (utcb_t *self, word_t cpu)
{
    self->processor_no = cpu;
}

INLINE word_t utcb_get_user_defined_handle (utcb_t *self)
{
    return self->user_defined_handle;
}

INLINE void utcb_set_user_defined_handle (utcb_t *self, word_t handle)
{
    self->user_defined_handle = handle;
}

INLINE threadid_t utcb_get_pager (utcb_t *self)
{
    return self->pager;
}

INLINE void utcb_set_pager (utcb_t *self, threadid_t tid)
{
    self->pager = tid;
}

INLINE threadid_t utcb_get_exception_handler (utcb_t *self)
{
    return self->exception_handler;
}

INLINE void utcb_set_exception_handler (utcb_t *self, threadid_t tid)
{
    self->exception_handler = tid;
}

INLINE u8_t utcb_get_preempt_flags (utcb_t *self)
{
    return self->preempt_flags;
}

INLINE void utcb_set_preempt_flags (utcb_t *self, u8_t flags)
{
    self->preempt_flags = flags;
}

INLINE u8_t utcb_get_cop_flags (utcb_t *self)
{
    return self->cop_flags;
}

INLINE word_t utcb_get_error_code (utcb_t *self)
{
    return self->error_code;
}

INLINE void utcb_set_error_code (utcb_t *self, word_t err)
{
    self->error_code = err;
}

INLINE timeout_t utcb_get_xfer_timeout (utcb_t *self)
{
    return self->xfer_timeout;
}

INLINE threadid_t utcb_get_intended_receiver (utcb_t *self)
{
    return self->intended_receiver;
}

INLINE threadid_t utcb_get_virtual_sender (utcb_t *self)
{
    return self->virtual_sender;
}

INLINE void utcb_set_virtual_sender (utcb_t *self, threadid_t tid)
{
    self->virtual_sender = tid;
}

/* Message and buffer registers.  The index is the raw slot in the array;
   the br[32 - n] convention stays at the call sites, as it always was. */

INLINE word_t utcb_get_mr (utcb_t *self, word_t index)
{
    return self->mr[index];
}

INLINE void utcb_set_mr (utcb_t *self, word_t index, word_t value)
{
    self->mr[index] = value;
}

INLINE word_t utcb_get_br (utcb_t *self, word_t slot)
{
    return self->br[slot];
}

INLINE void utcb_set_br (utcb_t *self, word_t slot, word_t value)
{
    self->br[slot] = value;
}


#endif /* !__API__V4__GENERIC_UTCB_H__ */
