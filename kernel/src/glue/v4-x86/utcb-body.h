/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2006-2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/utcb-body.h
 * Description:   UTCB layout for IA32, as an includable body
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
 * $Id: utcb.h,v 1.15 2006/10/20 16:30:13 reichelt Exp $
 *                
 ********************************************************************/
/*
 * The UTCB layout, with its name taken from UTCB_NAME.  Split out of utcb.h so
 * that compatibility mode can emit it twice: once for the 64-bit UTCB
 * (x64_utcb_t) and once, with the API types renamed to their 32-bit twins, for
 * the 32-bit one (x32_utcb_t).  See x32comp/utcb.h.
 *
 * No include guard: this file is meant to be included more than once.
 */
#if !defined(UTCB_NAME)
#error UTCB_NAME must name the struct this expands to
#endif

struct UTCB_NAME
{
    /* do not delete this TCB_START_MARKER */

    word_t              compatibility_mode;     /* -256         */
    word_t		padding0[15];		/* -254 .. -200 */
    word_t		br[IPC_NUM_BR];		/* -196 .. -64	*/
    threadid_t		my_global_id;		/* -60		*/
    word_t		processor_no;		/* -56		*/
    word_t		user_defined_handle;	/* -52		*/
    threadid_t		pager;			/* -48		*/
    threadid_t		exception_handler;	/* -44		*/
    u8_t		preempt_flags;		/* -40		*/
    u8_t		cop_flags;
    u16_t		reserved0[sizeof(word_t)/2-1];
    word_t		error_code;		/* -36		*/
    timeout_t		xfer_timeout;		/* -32		*/
    threadid_t		intended_receiver;	/* -28		*/
    threadid_t		virtual_sender;		/* -24		*/
    word_t		reserved1[4];		/* -20 .. -4	*/
    word_t              word_size_mask;         /* - 4 ..  0 */
    word_t		mr[IPC_NUM_MR];		/* 0 .. 252	*/

    
    /* do not delete this TCB_END_MARKER */


} __attribute__((packed));
typedef struct UTCB_NAME UTCB_NAME;
