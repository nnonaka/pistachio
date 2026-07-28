/*********************************************************************
 *                
 * Copyright (C) 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/tcb.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__TCB_H__
#define __GLUE__V4_X86__TCB_H__

#ifndef __API__V4__TCB_H__
#error not for stand-alone inclusion
#endif

#include INC_API(syscalls.h)			/* for sys_ipc */
#include INC_GLUE(resource_functions.h)		/* for thread_resources_t */
#include INC_GLUE_SA(tcb.h)

/* forward declaration */
tcb_t * get_idle_tcb();

/**********************************************************************
 * 
 *            generic tcb functions
 *
 **********************************************************************/


#endif /* !__GLUE__V4_X86__TCB_H__ */
