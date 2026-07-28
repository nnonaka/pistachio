/*********************************************************************
 *                
 * Copyright (C) 2002-2005, 2007-2010,  Karlsruhe University
 *                
 * File path:     api/v4/ipc.h
 * Description:   IPC declarations
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
 * $Id: ipc.h,v 1.24 2006/10/19 22:57:34 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__IPC_H__
#define __API__V4__IPC_H__

#include INC_API(fpage.h)
#include INC_GLUE(ipc.h)
#include <debug.h>
#include <kdb/tracepoints.h>

struct tcb_t;
typedef struct tcb_t tcb_t;


/**
 * Error codes
 */
#define ERR_IPC_TIMEOUT				(1)
#define ERR_IPC_NON_EXISTING			(2)
#define ERR_IPC_CANCELED			(3)
#define ERR_IPC_MSG_OVERFLOW(off)		(4 + ((off) << 3))
#define ERR_IPC_XFER_TIMEOUT_CURRENT(off)	(5 + ((off) << 3))
#define ERR_IPC_XFER_TIMEOUT_PARTNER(off)	(6 + ((off) << 3))
#define ERR_IPC_ABORTED(off)			(7 + ((off) << 3))

/**
 * Error encoding
 */
#define IPC_SND_ERROR(err)		((err << 1) | 0)
#define IPC_RCV_ERROR(err)		((err << 1) | 1)

/**
 * MR0 values
 */
#define IPC_MR0_PROPAGATED		(1 << 12)
#define IPC_MR0_REDIRECTED		(1 << 13)
#define IPC_MR0_XCPU			(1 << 14)
#define IPC_MR0_ERROR			(1 << 15)
#define IPC_MR0_PAGEFAULT		((-2UL) << 4)

#define IPC_NESTING_LEVEL	1	


struct msg_tag_t
{

    union {
	word_t raw;
	struct {
	    BITFIELD7(word_t,
		      untyped		: 6,
		      typed		: 6,
		      propagated	: 1,
		      redirected	: 1,
		      xcpu	       	: 1,
		      error		: 1,
		      label		: BITS_WORD - 16);
	} x;
    };
};
typedef struct msg_tag_t msg_tag_t;

/* C forms of the msg_tag_t methods (the raw/x union is C-visible); mirror the
   like-named C++ methods for api/v4/thread.c. */
INLINE word_t msg_tag_get_untyped (const msg_tag_t *self)	{ return self->x.untyped; }
INLINE word_t msg_tag_get_typed (const msg_tag_t *self)		{ return self->x.typed; }
INLINE bool   msg_tag_is_error (const msg_tag_t *self)		{ return self->x.error; }
INLINE void   msg_tag_set_error (msg_tag_t *self)		{ self->x.error = 1; }
INLINE word_t msg_tag_get_label (const msg_tag_t *self)		{ return self->x.label; }
INLINE bool   msg_tag_is_propagated (const msg_tag_t *self)	{ return self->x.propagated; }
INLINE bool   msg_tag_is_redirected (const msg_tag_t *self)	{ return self->x.redirected; }
INLINE bool   msg_tag_is_xcpu (const msg_tag_t *self)		{ return self->x.xcpu; }
INLINE void   msg_tag_set_propagated (msg_tag_t *self, bool val){ self->x.propagated = val; }
INLINE void   msg_tag_set_xcpu (msg_tag_t *self)		{ self->x.xcpu = 1; }
INLINE void   msg_tag_clear_receive_flags (msg_tag_t *self)	{ self->raw &= ~(0xeUL << 12); }
INLINE void   msg_tag_set (msg_tag_t *self, word_t typed, word_t untyped, word_t label)
{
    self->raw = 0;
    self->x.typed = typed & 0x3f;
    self->x.untyped = untyped & 0x3f;
    self->x.label = label & (~0UL >> 16);
}
INLINE msg_tag_t msg_tag_error_tag (void)
{ msg_tag_t t; t.raw = 0; t.x.error = 1; return t; }
INLINE msg_tag_t msg_tag_preemption_tag (void)
{ msg_tag_t t; msg_tag_set (&t, 0, 2, (-3UL << 4)); return t; }
INLINE msg_tag_t msg_tag_irq_tag (void)
{ msg_tag_t t; msg_tag_set (&t, 0, 0, (-1UL << 4)); return t; }

struct msg_item_t
{
    union {
	word_t raw;
	union {
	    struct{
		BITFIELD3(word_t,
			  continued		: 1,
			  type			: 3,
						: (sizeof(word_t)*8) - 4);
		
	    };
	    struct{
		BITFIELD4(word_t,
						: 4,
			  num_ptrs		: 5,
			  continuation		: 1,
			  length		: (sizeof(word_t)*8) - 10);
	    };
	    struct{
		BITFIELD3(word_t,
			  			: 4,	
			  id			: 8,
			  mask			: (sizeof(word_t)*8) - 12);
	    };    
	} __attribute__((packed));
    };
};
typedef struct msg_item_t msg_item_t;

/* C forms of the msg_item_t methods (the anonymous bitfield union is
   C-visible; mirror the C++ inline bodies exactly). */
INLINE bool   msg_item_is_map_item (const msg_item_t *self)	{ return self->type == 4; }
INLINE bool   msg_item_is_grant_item (const msg_item_t *self)	{ return self->type == 5; }
INLINE bool   msg_item_is_string_item (const msg_item_t *self)	{ return (self->type & 4) == 0; }
INLINE bool   msg_item_more_strings (const msg_item_t *self)	{ return self->continued; }
INLINE word_t msg_item_get_string_length (const msg_item_t *self)   { return self->length; }
INLINE word_t msg_item_get_string_ptr_count (const msg_item_t *self){ return self->num_ptrs + 1; }
INLINE bool   msg_item_is_string_compound (const msg_item_t *self)  { return self->continuation; }
INLINE word_t msg_item_get_string_cache_hints (const msg_item_t *self) { return self->type & 3; }
INLINE word_t msg_item_get_snd_base (const msg_item_t *self)	{ return self->raw & (~0x3ffUL); }

struct acceptor_t
{
    union {
	word_t raw;
	struct {
	    BITFIELD4 (word_t,
		       strings		: 1,
		       ctrlxfer		: 1,
		       reserved		: 2,
		       rcv_window	: (sizeof(word_t)*8) - 4);
	} x;
    };
    
};
typedef struct acceptor_t acceptor_t;

/* C forms of the acceptor_t methods (the raw/x union is C-visible). */
INLINE void acceptor_set_rcv_window (acceptor_t *self, fpage_t fpage)
{ word_t window = fpage.raw >> 4; self->x.rcv_window = window & (~0UL >> 4); }
INLINE bool   acceptor_accept_strings (const acceptor_t *self)	{ return self->x.strings; }
INLINE word_t acceptor_get_rcv_window (const acceptor_t *self)	{ return self->x.rcv_window << 4; }

/* get_arch_specific_rcvwindow calls into the arch mapping layer, so it is a
   real wrapper (defined in glue thread.cc with the map.h chain in scope). */
BEGIN_DECLS
fpage_t acceptor_get_arch_specific_rcvwindow (acceptor_t *self, struct tcb_t *dest);
END_DECLS

#if !defined(CONFIG_X_CTRLXFER_MSG)
#define IPC_NUM_SAVED_MRS	3
#else

#define IPC_NUM_SAVED_MRS	4
#define IPC_CTRLXFER_STDFAULTS	4

class ctrlxfer_item_t : public arch_ctrlxfer_item_t
{ 

public:
    /* members */
    msg_item_t item;
    union
    {
	word_t regs[];
    };

    static msg_item_t kernel_fault_item(word_t fault)
	{
	    msg_item_t item;
	    item.raw = 0;
	    item.continued = 0;
	    item.type = 6;
	    item.mask = 0x3ff;	
	    item.id = fault; // we operate with 0-based fault IDs
	    return item;
	}

    static msg_item_t fault_item(id_e id)
	{
	    msg_item_t item;
	    item.raw = 0;
	    item.continued = 0;
	    item.type = 6;
	    item.mask = (1 << num_hwregs[id]) - 1;	
	    item.id = id;
	    return item;	
	}

    static const void mask_hwregs(const word_t  id, word_t &val)
	{ val &= (1UL << num_hwregs[id])-1; }

#if defined(CONFIG_DEBUG)
    static const char* get_idname(const word_t id);
    static const char* get_hwregname(const word_t id, const word_t reg);
#endif

    static const word_t num_hwregs[id_max];
    static const word_t * const hwregs[id_max];
};

#endif


/**
 * Implements all non-untyped transfers
 *
 * @param src		Source tcb the message is sent from
 * @param dst		Destination tcb the message is sent to
 * @param msgtag	Message tag of the message being transferred
 *
 * @returns the message tag
 */
/* api/v4/ipcx.c is C; keep C linkage so the C++ caller (ipc.cc) agrees. */
BEGIN_DECLS
msg_tag_t extended_transfer(tcb_t * src, tcb_t * dst, msg_tag_t msgtag);
END_DECLS

#endif /* !__API__V4__IPC_H__ */
