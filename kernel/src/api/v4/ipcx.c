/*********************************************************************
 *
 * Copyright (C) 2002, 2004-2005, 2007-2008,  Karlsruhe University
 *
 * File path:     api/v4/ipcx.c
 * Description:   Extended transfer of IPC
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
 * $Id: ipcx.cc,v 1.26 2006/10/19 22:57:38 ud3 Exp $
 *
 ********************************************************************/
#include <debug.h>
#include <kdb/tracepoints.h>
#include INC_API(tcb.h)
#include INC_API(ipc.h)
#include INC_API(fpage.h)
#include INC_API(schedule.h)

#define CHECK_BR_IDX(idx) if (idx > IPC_NUM_BR) goto message_overflow
/* "total" is a count of message registers, so the highest index that may be
   accessed is total - 1.  Every caller passes the index it is about to
   dereference, hence >= rather than >. */
#define CHECK_MR_IDX(idx, total) if (idx >= total) goto message_overflow


DECLARE_TRACEPOINT_DETAIL(IPC_STRING_COPY);
DECLARE_TRACEPOINT_DETAIL(IPC_STRING_ITEM);
DECLARE_TRACEPOINT_DETAIL(IPC_MAPGRANT_ITEM);
DECLARE_TRACEPOINT_DETAIL(IPC_MESSAGE_OVERFLOW);
DECLARE_TRACEPOINT_DETAIL(IPC_EXT_TRANSFER);
#if defined(CONFIG_X_CTRLXFER_MSG)
DECLARE_TRACEPOINT_DETAIL(IPC_CTRLXFER_ITEM);
DECLARE_TRACEPOINT_DETAIL(IPC_CTRLXFER_ITEM_DETAILS);
#endif

#if !defined(IPC_STRING_COPY)
void * memcpy (void * dst, const void * src, word_t len);
#define IPC_STRING_COPY memcpy
#endif

word_t ipc_copy (tcb_t * src, addr_t src_addr,
		 tcb_t * dst, addr_t dst_addr, word_t len)
{
    TRACEPOINT (IPC_STRING_COPY, "IPC string copy: %t @ %p -> %t @ %p, len=0x%x\n",
		src, src_addr, dst, dst_addr, len);

    space_t * src_space = tcb_get_space (src);
    space_t * dst_space = tcb_get_space (dst);

    // Check limits of source and destination string.
    word_t src_len = space_get_copy_limit (src_space, src_addr, len);
    word_t dst_len = space_get_copy_limit (dst_space, dst_addr, len);

    // String might need to be clipped.
    if (src_len < len) len = src_len;
    if (dst_len < len) len = dst_len;

    // For inter-space string copy we need to use a copy area.
    if (src_space != dst_space)
	tcb_adjust_for_copy_area (src, dst, &src_addr, &dst_addr);

    src->misc.ipc_copy.copy_start_src =
	src->misc.ipc_copy.copy_fault = src_addr;
    src->misc.ipc_copy.copy_start_dst = dst_addr;

    // we may cause a nested pagefault ipc, therefore unlock the receiver tcb
    tcb_unlock (dst);
    IPC_STRING_COPY (dst_addr, src_addr, len);
    tcb_lock (dst);

    src->misc.ipc_copy.copy_length += len;

    return len;
}

static void copy_mr(tcb_t * dst, tcb_t * src, word_t index)
{
    ASSERT(index < IPC_NUM_MR);
    tcb_set_mr (dst, index, tcb_get_mr (src, index));
}

msg_tag_t extended_transfer(tcb_t * src, tcb_t * dst, msg_tag_t msgtag)
{
    msg_item_t src_item;
    acceptor_t acceptor;
    word_t br_idx = 1;
    word_t total_mrs = msg_tag_get_untyped (&msgtag) + msg_tag_get_typed (&msgtag) + 1;
    word_t total_len = 0;
#if defined(CONFIG_X_CTRLXFER_MSG)
    word_t total_cxfer_mrs = 0;
#endif
    bool accept_strings;

//    ENABLE_TRACEPOINT (IPC_STRING_COPY, ~0, 0);
//    ENABLE_TRACEPOINT (IPC_STRING_ITEM, ~0, 0);
//    ENABLE_TRACEPOINT (IPC_MESSAGE_OVERFLOW, ~0, 0);
#undef TRACEF
#define TRACEF(args...)

    if (total_mrs > IPC_NUM_MR)
    {
	printf("message exceeds MR's (untyped=%d, typed=%d)\n",
	       msg_tag_get_untyped (&msgtag), msg_tag_get_typed (&msgtag));
	enter_kdebug("message exceeds MR's");
	goto message_overflow;
    }

    tcb_set_state (src, THREAD_STATE_LOCKED_RUNNING);
    tcb_set_state (dst, THREAD_STATE_LOCKED_WAITING);

    tcb_set_partner (src, tcb_get_global_id (dst));
    src->misc.ipc_copy.copy_length = 0;

    acceptor.raw = tcb_get_br (dst, 0);

    /* does the receiver (still) accepts strings? */
    accept_strings = acceptor_accept_strings (&acceptor);

    TRACEPOINT(IPC_EXT_TRANSFER, "tag=%p, untyped: %d, typed: %d, acceptor: %x\n",
	       msgtag.raw, msg_tag_get_untyped (&msgtag), msg_tag_get_typed (&msgtag), acceptor.raw);

    for (word_t src_idx = msg_tag_get_untyped (&msgtag) + 1; src_idx < total_mrs; )
    {
	src_item.raw = tcb_get_mr (src, src_idx);

#if defined(CONFIG_X_CTRLXFER_MSG)
	if (msg_item_is_ctrlxfer_item (&src_item))
	{
	    bool src_mr = !tcb_flags_is_set (src, TCB_FLAG_KERNEL_CTRLXFER_MSG);
	    bool dst_mr = !acceptor_accept_ctrlxfer (&acceptor);
	    word_t cxfer_regs;

	    TRACEPOINT(IPC_CTRLXFER_ITEM,
		       "ctrlxfer item: ipc %t->%t id=%d, mask=%x %c->%c",
		       src, dst, msg_item_get_ctrlxfer_id (&src_item), msg_item_get_ctrlxfer_mask (&src_item),
		       src_mr ? 'm' : 'f', dst_mr ? 'm' : 'f');

	    cxfer_regs = tcb_ctrlxfer (src, dst, src_item, src_idx,
				       msgtag.x.untyped+1+total_cxfer_mrs, src_mr, dst_mr);

	    src_idx += src_mr ? cxfer_regs : 1;
	    msgtag.x.typed += src_mr ? 0 : cxfer_regs - 1;
	    total_cxfer_mrs += dst_mr ? 0 : cxfer_regs;

	}
	else
#endif
	if (msg_item_is_map_item (&src_item) || msg_item_is_grant_item (&src_item))
	{
	    /* is the descriptor beyond the valid range? */
	    CHECK_MR_IDX(src_idx + 1, total_mrs);

	    fpage_t snd_fpage, rcv_fpage;
	    snd_fpage.raw = tcb_get_mr (src, src_idx + 1);

	    if (fpage_is_mempage (&snd_fpage))
		rcv_fpage.raw = acceptor_get_rcv_window (&acceptor);
	    else if (fpage_is_archpage (&snd_fpage))
		rcv_fpage = acceptor_get_arch_specific_rcvwindow (&acceptor, dst);
	    else
		// Unknown fpage type
		goto message_overflow;


	    TRACEPOINT(IPC_MAPGRANT_ITEM, "%s item: snd_base=%p, fpage=%p\n",
		       msg_item_is_map_item (&src_item) ? "map" : "grant",
		       msg_item_get_snd_base (&src_item), snd_fpage.raw);

	    /* does the receiver accept mappings */
	    if (EXPECT_FALSE( fpage_is_nil_fpage (&rcv_fpage) ))
		goto message_overflow;

	    copy_mr(dst, src, src_idx++);
	    copy_mr(dst, src, src_idx++);

	    if (fpage_is_mempage (&snd_fpage))
	    {
		space_map_fpage
		    (tcb_get_space (src), snd_fpage, msg_item_get_snd_base (&src_item),
		     tcb_get_space (dst), rcv_fpage, msg_item_is_grant_item (&src_item));
	    }
	    else if (fpage_is_archpage (&snd_fpage))
		arch_map_fpage_c (src, snd_fpage, msg_item_get_snd_base (&src_item),
			       dst, rcv_fpage, msg_item_is_grant_item (&src_item));

	    if (EXPECT_FALSE (msg_item_is_grant_item (&src_item) &&
			      fpage_get_size_log2 (&snd_fpage) >
			      fpage_get_size_log2 (&rcv_fpage)))
	    {
		/*
		 * We are granting an fpage that is larger than
		 * receive window.  Make sure that the whole fpage of
		 * the sender is flushed.  We don't call unmap from
		 * the map function itself to avoid deep function
		 * recursion.
		 */
		if (fpage_is_mempage (&snd_fpage))
		    space_unmap_fpage (tcb_get_space (src), snd_fpage, true, false);
		else if (fpage_is_archpage (&snd_fpage))
		    arch_unmap_fpage_c (src, snd_fpage, true);
	    }
	}
	else if (msg_item_is_string_item (&src_item))
	{
	    /*
	     * Copy the MR at the very beginning to make sure the
	     * receiver can deal with cut message situations.
	     */
	    copy_mr (dst, src, src_idx);

	    if (! accept_strings)
		goto message_overflow;

	    // We might have overflow on MRs
	    CHECK_MR_IDX (src_idx + msg_item_get_string_ptr_count (&src_item),
			  total_mrs);

	    msg_item_t dst_item;
	    word_t src_addr, src_len, src_ptridx;
	    word_t dst_addr, dst_len, dst_ptridx;

	    src_addr = tcb_get_mr (src, src_idx + 1);
	    src_len  = msg_item_get_string_length (&src_item);
	    src_ptridx = 1;

	    // Check for overflow on BRs.  br_idx is always guaranteed
	    // to be in range.
	    dst_item.raw = tcb_get_br (dst, br_idx);
	    CHECK_BR_IDX (br_idx + msg_item_get_string_ptr_count (&dst_item));

	    dst_addr = tcb_get_br (dst, br_idx + 1);
	    dst_len  = msg_item_get_string_length (&dst_item);
	    dst_ptridx = 1;

	    TRACEPOINT (IPC_STRING_ITEM,
			"IPC string item: src:%p (sub=%d idx=%d len=%p %s)"
			"dst:%p (sub=%d idx=%d len=%p %s)",
			src_item.raw,
			msg_item_get_string_ptr_count (&src_item),
			src_ptridx, src_len,
			msg_item_is_string_compound (&src_item) ?
			"c" : "",
			dst_item.raw,
			msg_item_get_string_ptr_count (&dst_item),
			dst_ptridx, dst_len,
			msg_item_is_string_compound (&dst_item) ?
			"c" : "");

	    // Sanity checking
	    if (! msg_item_is_string_item (&dst_item))
		goto message_overflow;

	    // Check if there are more receive buffers after this one
	    accept_strings = msg_item_more_strings (&dst_item);

	    bool end_of_send_string = false;

	    while (! end_of_send_string)
	    {
		TRACEPOINT (IPC_STRING_ITEM,
			    "  src: addr=%p len=%p (idx=%d)\n"
			    "  dst: addr=%p len=%p (idx=%d)\n",
			    src_addr, src_len, src_ptridx,
			    dst_addr, dst_len, dst_ptridx);

		copy_mr (dst, src, src_idx + src_ptridx);

		word_t copy_length = dst_len < src_len ? dst_len : src_len;
		word_t cpy_len = ipc_copy (src, (addr_t) src_addr,
					   dst, (addr_t) dst_addr,
					   copy_length);

		total_len += cpy_len;

		// Copy operation might have been clipped.
		if (cpy_len < copy_length)
		    goto message_overflow;

		src_len  -= cpy_len;
		dst_len  -= cpy_len;
		src_addr += cpy_len;
		dst_addr += cpy_len;

		if (src_len == 0)
		{
		    // Current part of send string exhausted.  Check
		    // if there are more substrings or compund strings
		    // in the current send string.

		    if (src_ptridx < msg_item_get_string_ptr_count (&src_item))
		    {
			// More substrings to send
			src_ptridx++;
			src_addr = tcb_get_mr (src, src_idx + src_ptridx);
			src_len  = msg_item_get_string_length (&src_item);
		    }
		    else if (msg_item_is_string_compound (&src_item))
		    {
			// Send string is compund
			src_idx += src_ptridx + 1;
			src_ptridx = 1;
			CHECK_MR_IDX (src_idx + 1, total_mrs);

			src_item.raw = tcb_get_mr (src, src_idx);
			src_addr = tcb_get_mr (src, src_idx + src_ptridx);
			src_len  = msg_item_get_string_length (&src_item);

			CHECK_MR_IDX (src_idx +
				      msg_item_get_string_ptr_count (&src_item),
				      total_mrs);

			TRACEPOINT (IPC_STRING_ITEM,
				    "  src: substrings=%d (idx=%d) len=%p %s\n",
					    msg_item_get_string_ptr_count (&src_item),
					    src_ptridx, src_len,
					    msg_item_is_string_compound (&src_item) ?
					    "compound" : "");
		    }
		    else
		    {
			// End of send string
			end_of_send_string = true;
			src_idx += src_ptridx + 1;
		    }
		}

		if (end_of_send_string)
		{
		    // No more data in the current send string.  Skip
		    // past the current receive buffer.

		    TRACEF ("Skip receive buffer\n");
		    bool compound;
		    do {
			compound = msg_item_is_string_compound (&dst_item);
			TRACEF ("compund=%d  more=%d\n",
				msg_item_is_string_compound (&dst_item),
				dst_item.more_strings ());

			// Calculate position of next string item
			br_idx += msg_item_get_string_ptr_count (&dst_item) + 1;

			if (br_idx >= IPC_NUM_BR)
			{
			    // BR register overflow
			    TRACEF ("Last receive buffer\n");
			    accept_strings = false;
			    break;
			}

			dst_item.raw = tcb_get_br (dst, br_idx);
		    } while (compound);
		}
		else
		{
		    // More data in send string.  Check if we have
		    // room in the receive buffers.

		    if (dst_len == 0)
		    {
			if (dst_ptridx < msg_item_get_string_ptr_count (&dst_item))
			{
			    // More substrings in receive buffer
			    dst_ptridx++;
			    dst_addr = tcb_get_br (dst, br_idx + dst_ptridx);
			    dst_len  = msg_item_get_string_length (&dst_item);
			}
			else if (msg_item_is_string_compound (&dst_item))
			{
			    // Receive buffer is compund
			    br_idx += dst_ptridx + 1;
			    dst_ptridx = 1;
			    CHECK_BR_IDX (br_idx + 1);

			    dst_item.raw = tcb_get_br (dst, br_idx);
			    dst_addr = tcb_get_br (dst, br_idx + dst_ptridx);
			    dst_len  = msg_item_get_string_length (&dst_item);

			    CHECK_BR_IDX (br_idx +
					  msg_item_get_string_ptr_count (&dst_item));

			    TRACEPOINT (
				IPC_STRING_ITEM,
				"  dst: substrings=%d (idx=%d)"
					"  len=%p %s\n",
					msg_item_get_string_ptr_count (&dst_item),
					dst_ptridx, src_len,
					msg_item_is_string_compound (&dst_item) ?
					"compound" : "");
			}
			else
			{
			    // No more receive buffers
			    TRACEF ("No more receive buffers\n");
			    goto message_overflow;
			}
		    }
		}

	    } // while (! end_of_send_string)
	}
       	else
	{
	    TRACEF("unknown item type\n");
	    goto message_overflow;
	}
    }

    // Release copy area
    tcb_release_copy_area (src);

    // Cancel any pending XFER timeouts.
    sched_ktcb_cancel_timeout (&src->sched_state);
    tcb_flags_remove (src, TCB_FLAG_HAS_XFER_TIMEOUT);
    return msgtag;

message_overflow:

    // Release copy area
    tcb_release_copy_area (src);

    // Cancel any pending XFER timeouts.
    sched_ktcb_cancel_timeout (&src->sched_state);

    TRACEPOINT (IPC_MESSAGE_OVERFLOW, "IPC message overflow (%t->%t), len=0x%x\n",
		src, dst, total_len);

    // Report message overflow error
    tcb_set_error_code (dst, IPC_RCV_ERROR (ERR_IPC_MSG_OVERFLOW (total_len)));
    tcb_set_error_code (src, IPC_SND_ERROR (ERR_IPC_MSG_OVERFLOW (total_len)));
    msg_tag_set_error (&msgtag);
    return msgtag;
}
