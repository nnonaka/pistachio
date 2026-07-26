/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007-2009, 2011-2012,  Karlsruhe University
 *                
 * File path:     api/v4/syscalls.h
 * Description:   declaration of system calls
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
 * $Id: syscalls.h,v 1.14 2004/04/23 04:58:46 cvansch Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__SYSCALLS_H__
#define __API__V4__SYSCALLS_H__

#include INC_API(types.h)
#include INC_API(fpage.h)
#include INC_API(thread.h)

/* XXX Should not include arch stuff here */
#include INC_GLUE(config.h)
#include INC_GLUE(syscalls.h)

BEGIN_DECLS

/**
 * the ipc system call
 * sys_ipc returns using the special return functions return_ipc and
 * return_ipc_error.  These have to be provided by the glue layer
 * @param to_tid destination thread id
 * @param from_tid from specifier
 * @param timeout ipc timeout
 */
SYS_IPC (threadid_t to_tid, threadid_t from_tid, timeout_t timeout);


/**
 * thread switch system call
 * @param dest_tid thread id to switch to
 */
SYS_THREAD_SWITCH (threadid_t dest_tid);


/**
 * thread control privileged system call
 * @param dest_tid thread id of the destination thread
 * @param space_tid space specifier
 * @param scheduler_tid thread id of the scheduler thread
 * @param pager_tid thread id of the pager thread
 * @param utcb_location location of the UTCB
 */
SYS_THREAD_CONTROL (threadid_t dest_tid, threadid_t space_tid, 
		    threadid_t scheduler_tid, threadid_t pager_tid, 
		    word_t utcb_location);

/**
 * exchange registers system call
 * @param dest_tid thread id of the destination thread (always global)
 * @param control control word specifying the operations to perform
 * @param usp user stack pointer
 * @param uip user instruction pointer
 * @param uflags user flags
 * @param pager_tid thread id of the pager
 * @param uhandle user defined handle
 * @param is_local true if the original dest_tid was local
 */
SYS_EXCHANGE_REGISTERS (threadid_t dest_tid, word_t control, 
			word_t usp, word_t uip, word_t uflags,
			word_t uhandle, threadid_t pager_tid,
			bool is_local);


/**
 * schedule system call
 * (note: the glue layer has to provide a return_schedule macro to load
 * the return values into the appropriate registers)
 * @param dest_tid thread id of the destination thread
 * @param time_control specifies total quantum and timeslice length
 * @param processor_control processor number the thread migrates to
 * @param prio_control new priority of the thread
 * @param preemption_control delayed preemption parameters
 */
SYS_SCHEDULE (threadid_t dest_tid, word_t time_control, 
	      word_t processor_control, word_t prio_control,
	      word_t preemption_control);



/**
 * unmap system call
 * @param control unmap control word specifying the number of 
 *  fpages to be unmapped
 */
SYS_UNMAP (word_t control);


/**
 * space control privileged system call
 * (note: the glue layer has to provide a return_space_control macro to 
 * load the return values into the appropriate registers)
 * @param space_tid address space specifier
 * @param control control parameter
 * @param kip_area kernel interface page area fpage
 * @param utcb_area user thread control block area fpage
 * @param redirector_tid thread id of the redirector
 */
SYS_SPACE_CONTROL (threadid_t space_tid, word_t control, fpage_t kip_area, 
		   fpage_t utcb_area, threadid_t redirector_tid);


/**
 * processor control privileged system call
 * (note: the glue layer has to provide a return_pocessor_control(result) 
 * macro to load the return values into the appropriate registers)
 * @param processor_no number of the processor
 * @param internal_frequency internal frequency of the processor in kHz
 * @param external_frequency external frequency of the processor in kHz
 * @param voltage voltage of the processor in mV
 */
SYS_PROCESSOR_CONTROL (word_t processor_no, word_t internal_frequency,
		       word_t external_frequency, word_t voltage);


/**
 * memory control privileged system call
 * @param control control word speciying the number of fpages in the message registers
 * @param attribute0 memory attribute 0
 * @param attribute1 memory attribute 1
 * @param attribute2 memory attribute 2
 * @param attribute3 memory attribute 3
 */
SYS_MEMORY_CONTROL (word_t control, 
		    word_t attribute0, word_t attribute1, 
		    word_t attribute2, word_t attribute3);

END_DECLS


/*********************************************************************
 *                 control register constants
 *********************************************************************/


/* Flag bit positions, named as macros so C can reference them; the C++
   flag_e enum below aliases these. */
#define EXREGS_CTRL_HALT_FLAG		0
#define EXREGS_CTRL_RECV_FLAG		1
#define EXREGS_CTRL_SEND_FLAG		2
#define EXREGS_CTRL_SP_FLAG		3
#define EXREGS_CTRL_IP_FLAG		4
#define EXREGS_CTRL_FLAGS_FLAG		5
#define EXREGS_CTRL_UHANDLE_FLAG	6
#define EXREGS_CTRL_PAGER_FLAG		7
#define EXREGS_CTRL_HALTFLAG_FLAG	8
#define EXREGS_CTRL_CTRLXFER_CONF_FLAG	9
#define EXREGS_CTRL_CTRLXFER_READ_FLAG	10
#define EXREGS_CTRL_CTRLXFER_WRITE_FLAG	11
#define EXREGS_CTRL_EXCHANDLER_FLAG	12
#define EXREGS_CTRL_SCHEDULER_FLAG	13

struct exregs_ctrl_t {
#if defined(__cplusplus)
    enum flag_e {
	halt_flag        	= EXREGS_CTRL_HALT_FLAG,
	recv_flag	 	= EXREGS_CTRL_RECV_FLAG,
	send_flag	 	= EXREGS_CTRL_SEND_FLAG,
	sp_flag	         	= EXREGS_CTRL_SP_FLAG,
	ip_flag	         	= EXREGS_CTRL_IP_FLAG,
	flags_flag	 	= EXREGS_CTRL_FLAGS_FLAG,
	uhandle_flag	 	= EXREGS_CTRL_UHANDLE_FLAG,
	pager_flag	 	= EXREGS_CTRL_PAGER_FLAG,
	haltflag_flag    	= EXREGS_CTRL_HALTFLAG_FLAG,
	ctrlxfer_conf_flag    	= EXREGS_CTRL_CTRLXFER_CONF_FLAG,
	ctrlxfer_read_flag    	= EXREGS_CTRL_CTRLXFER_READ_FLAG,
	ctrlxfer_write_flag    	= EXREGS_CTRL_CTRLXFER_WRITE_FLAG,
	exchandler_flag  	= EXREGS_CTRL_EXCHANDLER_FLAG,
	scheduler_flag   	= EXREGS_CTRL_SCHEDULER_FLAG,

    };
#endif

    union {
	struct {
	    word_t halt			: 1;
	    word_t recv			: 1;
	    word_t send			: 1;
	    word_t sp			: 1;
	    word_t ip			: 1;
	    word_t flags		: 1;
	    word_t uhandle		: 1;
	    word_t pager		: 1;
	    word_t haltflag		: 1;
	    word_t ctrlxfer_conf	: 1;
	    word_t ctrlxfer_read	: 1;
	    word_t ctrlxfer_write	: 1;
	    word_t exchandler		: 1;
	    word_t scheduler		: 1;
	    word_t __pad		: 18;
	};
	word_t raw;
    };

#if defined(__cplusplus)
    exregs_ctrl_t (void) {}
    exregs_ctrl_t (word_t r) { raw = r; }

    bool is_set(flag_e flag) { return raw  & (1UL << flag); }
    void set(flag_e flag) { raw  |= (1UL << flag); }

    char *string()
	{
	    static char s[] =  "~~~~~~~~~~~~~~";

	    s[0]  =  halt    		? 'h' : '~';
	    s[1]  =  recv    		? 'r' : '~';
	    s[2]  =  send    		? 's' : '~';
	    s[3]  =  sp      		? 's' : '~';
	    s[4]  =  ip      		? 'i' : '~';
	    s[5]  =  flags   		? 'f' : '~';
	    s[6]  =  uhandle 		? 'u' : '~';
	    s[7]  =  pager   		? 'p' : '~';
	    s[8]  =  haltflag		? 'h' : '~';
	    s[9]  =  ctrlxfer_conf	? 'C' : '~';
	    s[10] =  ctrlxfer_read	? 'R' : '~';
	    s[11] =  ctrlxfer_write	? 'W' : '~';
	    s[12] =  exchandler		? 'e' : '~';
	    s[13] =  scheduler		? 's' : '~';

	    return s;
	}
#endif /* __cplusplus */
};
typedef struct exregs_ctrl_t exregs_ctrl_t;

#if !defined(__cplusplus)
INLINE bool exregs_ctrl_is_set (const exregs_ctrl_t *self, word_t flag)
{ return (self->raw & (1UL << flag)) != 0; }
INLINE void exregs_ctrl_set (exregs_ctrl_t *self, word_t flag)
{ self->raw |= (1UL << flag); }
#endif /* !__cplusplus */

/* Remaining syscall-control types stay C++-only (matched by the #endif far
   below); only exregs_ctrl_t above needed a C form. */
#if defined(__cplusplus)

struct schedule_ctrl_t {
    union {
	struct {
	    u8_t	pad0[sizeof(word_t)-4];
	    time_t	timeslice;
	    time_t	total_quantum;
	};
	struct {	
	    BITFIELD4(long,
		      prio		:  9,
		      logid		:  7,
		      stride		:  16,
		      : BITS_WORD-32);
	};
	struct {
	    BITFIELD5(word_t,
		      max_delay		: 16,
		      sensitive_prio	:  8,
		      log_pm_msg	:  1,
		      hs_extended	:  1,
		      hs_extended_ctrl	:  6);
	}; 	
	struct {
	    BITFIELD3(word_t,
		      processor		: 16,
		      extended_migrate	: 1, 
		      : BITS_WORD-17);
	};
        threadid_t tid;
	word_t raw;

   } __attribute__((packed));

    inline void operator = (word_t raw) 
	{ this->raw = raw; }

    inline bool operator == (schedule_ctrl_t ctrl) 
	{ return (this->raw == ctrl.raw); }

    inline bool operator != (schedule_ctrl_t ctrl) 
	{ return (this->raw != ctrl.raw); }

    word_t get_raw()
	{ return this->raw; }

    static schedule_ctrl_t nilctrl()
	{
	    schedule_ctrl_t ctrl;
	    ctrl.raw = (~0UL);
	    return ctrl;
	}

};
#endif /* __cplusplus */

/*
 * Error code values
 */

#define EOK			(0)
#define ENO_PRIVILEGE		(1)
#define EINVALID_THREAD		(2)
#define EINVALID_SPACE		(3)
#define EINVALID_SCHEDULER	(4)
#define EINVALID_PARAM		(5)
#define EUTCB_AREA		(6)
#define EKIP_AREA		(7)
#define ENO_MEM			(8)


#endif /* !__API__V4__SYSCALLS_H__ */
