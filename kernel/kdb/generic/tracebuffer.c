/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2007-2010,  Karlsruhe University
 *                
 * File path:     kdb/generic/tracebuffer.c
 * Description:   Tracebuffer for PC99 platform
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

#if defined(CONFIG_TRACEBUFFER)
#include <debug.h>
#include <linear_ptab.h>
#include <generic/lib.h>
#include <kdb/kdb.h>
#include <kdb/input.h>
#include <kdb/tracepoints.h>
#include <kdb/tracebuffer.h>
#include INC_API(thread.h)
#include INC_API(tcb.h)
#include INC_GLUE(schedule.h)

#define TB_WRAP	80
extern void list_tp_choices (void);

#if defined(CONFIG_TBUF_PERFMON)
#define IF_PERFMON(a...) a
#else
#define IF_PERFMON(a...)
#endif

extern word_t local_apic_cpu_mhz;

static inline int SECTION(SEC_KDEBUG) strlen(const char* p) { int i=0; while (*(p++)) i++; return i; };
extern void putc(const char c);


/* Was a template; word_t and u64_t coincide on this subarch, so the two
   instantiations collapse to one.  A 32-bit port needs a second form. */
static void pmc_print(u64_t pmc)
{
    u64_t divisor = 0;
    int digits = 0, num = 0;

    const int width = 4;
    
    /* calculate number of digits */
    if (pmc == 0) 
	digits = 0;
    else
	for (divisor = 1, digits = 1; pmc/divisor >= 10; divisor *= 10, digits++);

    /* max() is a C++ template in generic/types.h; spelled out for C. */
    while (num < (width - digits > 0 ? width - digits : 0))
    {
	putc('0');
	num++;
    }
    
    while (num < width)
    {
	ASSERT(divisor);
	char d = (char) ((pmc/divisor) % 10);
	putc(d + '0');
	
	divisor /= 10;
	num++;
    }

    if (digits > width)
    {
        putc('e');
	putc((char) (digits-width + '0'));
    }
    else
    {
	putc(' ');
	putc(' ');
    }

    putc(' ');
    
}

static u64_t pmc_delta(u64_t cur, u64_t old)
{
    return (cur >= old) ? cur - old : cur + (u64_t) -1 - old;
}



DECLARE_CMD_GROUP (tracebuf);


#define TBUF_MAX_FILTERS 4

struct tbuf_handler_t
{
    word_t id[TBUF_MAX_FILTERS];
    tcb_t *tcb[TBUF_MAX_FILTERS];
    word_t typemask;
    word_t cpumask;
    u64_t tsc;
};
typedef struct tbuf_handler_t tbuf_handler_t;

/* Single instance; the free functions below name it directly, which also
   removes the member/parameter shadowing that the class needed `this->` for. */
static tbuf_handler_t tbuf_handler;

static bool tbuf_handler_cpu_pass (tracerecord_t *t)
	{
	    return ((tbuf_handler.cpumask & (1UL << t->cpu)) != 0);
	}

static bool tbuf_handler_type_pass (tracerecord_t *t)
	{
	    return (tbuf_handler.typemask & ((t->ktype << 16) | t->utype));
	}

static bool tbuf_handler_tsc_pass (tracerecord_t *t)
	{
	    if (tbuf_handler.tsc == 0) return true;
	    
	    u64_t ttsc = t->tsc;
#if defined(CONFIG_TBUF_PERFMON_ENERGY)
            if (get_tbuf_config().pmon_e)
                ttsc <<= X86_PMC_TSC_SHIFT;
#endif
	    return (ttsc >= tbuf_handler.tsc);
	}

    
static bool tbuf_handler_id_pass (tracerecord_t *t)
	{
	    if (tbuf_handler.id[0] == NULL || ((word_t) tbuf_handler.id[0] == t->id))
		return true;
	    
	    for (word_t i=1; i < TBUF_MAX_FILTERS; i++)
	    {
		if (tbuf_handler.id[i] == NULL)
		    return false;
		if ((word_t) tbuf_handler.id[i] == t->id)
		    return true;
	    }
	    
	    return false;
	}


static bool tbuf_handler_tcb_pass (tracerecord_t *t)
	{
	    tcb_t *rtcb = tracerecord_is_kernel_event (t) 
		? addr_to_tcb((addr_t) t->thread)
		: tcb_get_tcb(threadid_from_raw (t->thread));

	    if (tbuf_handler.tcb[0] == NULL || (tbuf_handler.tcb[0] == rtcb))
		return true;
	    
	    for (word_t i=1; i < TBUF_MAX_FILTERS; i++)
	    {
		if (tbuf_handler.tcb[i] == rtcb)
		    return true;
		if (tbuf_handler.tcb[i] == NULL)
		    return false;
	    }
	    return false;
	}

    
static void tbuf_handler_invalidate_filters (void)
	{
	    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	    {
		tbuf_handler.id[i] = NULL;
		tbuf_handler.tcb[i] = NULL;
	    }
	    tbuf_handler.cpumask = tbuf_handler.typemask = ~0UL;
	    tbuf_handler.tsc = 0;

	}
    
static void tbuf_handler_dump_filters (void)
	{
	    
	    printf("Record  filters:\n");
	    printf("\tTypemask: [%x]\n", get_tracebuffer()->mask);
	    
	    printf("Display filters:\n");
	    printf("\tCPU:      [%x]\n", tbuf_handler.cpumask);
	    printf("\tTypemask: [%x]\n", tbuf_handler.typemask);
	    printf("\tTSC:      [%x/%x]\n", (u32_t) (tbuf_handler.tsc >> 32), (u32_t) tbuf_handler.tsc);
	    printf("\tTracepoints: \n");
	    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	    {
		if (tbuf_handler.id[i] == NULL)
		    break;
		printf("\t\t%2d: %8d %s\n", i, tbuf_handler.id[i], tracepoint_list_get (&tp_list, tbuf_handler.id[i]-1)->name);
	    }    
	    
	    printf("\tTCBs: \n");
	    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	    {
		if (tbuf_handler.tcb[i] == NULL)
		    break;
		printf("\t\t%2d: %8t\n", i, (tcb_t *) tbuf_handler.tcb[i]);
	    }    
    


	}
    
static void tbuf_handler_set_cpumask (word_t mask) {  tbuf_handler.cpumask = mask; }
static word_t tbuf_handler_get_cpumask (void) { return tbuf_handler.cpumask; }
    
static void tbuf_handler_set_typemask (word_t mask) {  tbuf_handler.typemask = mask; }
static word_t tbuf_handler_get_typemask (void) { return tbuf_handler.typemask; }
    
static void tbuf_handler_set_tsc (u64_t t) {  tbuf_handler.tsc = t; }
static u64_t tbuf_handler_get_tsc (void) { return tbuf_handler.tsc; }

static void tbuf_handler_set_id (word_t idx, word_t id)
	{ 
	    ASSERT(idx < TBUF_MAX_FILTERS);
	    tbuf_handler.id[idx] = id;
	}
    
static word_t tbuf_handler_get_id (word_t idx)
	{ 
	    ASSERT(idx < TBUF_MAX_FILTERS);
	    return tbuf_handler.id[idx];
	}
    

static void tbuf_handler_set_tcb (word_t idx, tcb_t *t)
	{ 
	    ASSERT(idx < TBUF_MAX_FILTERS);
	    tbuf_handler.tcb[idx] = t;
	}
    
static bool tbuf_handler_pass (tracerecord_t *t)
	{ return tbuf_handler_cpu_pass (t) && tbuf_handler_type_pass (t) && tbuf_handler_id_pass (t) && tbuf_handler_tsc_pass (t) && tbuf_handler_tcb_pass (t); }

    
    /* Tbuf handling */
static void tbuf_handler_set_tbuf_typemask (word_t mask)
	{ get_tracebuffer()->mask = mask; }

static word_t tbuf_handler_get_tbuf_typemask (void)
	{ return get_tracebuffer()->mask; }

    traceconfig_t get_tbuf_config()
	{ return get_tracebuffer()->config; }

    
static word_t tbuf_handler_get_tbuf_max (void)
	{ return get_tracebuffer()->max; }
    
static word_t tbuf_handler_get_tbuf_current (void)
	{ return atomic_read (&get_tracebuffer()->current); }

static void tbuf_handler_reset_tbuf (void)
	{ 
	    memset (get_tracebuffer ()->tracerecords, 0,
		    TRACEBUFFER_SIZE - sizeof (tracerecord_t));
	    atomic_set (&get_tracebuffer ()->current, 0);

	}
    
static void tbuf_handler_reset_tbuf_counters (void)
	{ 
	    tracebuffer_t * tracebuffer = get_tracebuffer ();
	    
	    for (word_t i = 0; i < 8; i++)
		tracebuffer->counters[i] = 0;

	}
    
static void tbuf_handler_dump_tbuf_counters (void)
	{
	    
	    tracebuffer_t * tracebuffer = get_tracebuffer ();
	    
	    for (word_t i = 0; i < 8; i++)
		printf ("Counter %d = %10d\n", i, tracebuffer->counters[i]);
	    
	}


static word_t tbuf_handler_find_tbuf_start (word_t end, word_t count, word_t size)
	{ 
    
	    word_t start, num;
	    
	    for (start = end, num = 0; num < size && count; start--, num++)
	    {
		if (start > size) start = size;
		
		tracerecord_t *rec = get_tracebuffer()->tracerecords + start;
		if (!tbuf_handler_pass (rec))
		{
		    //if (rec->tsc && !tbuf_handler_tsc_pass (rec))
		    //break;
		    //else
		    continue;
		}		
		count--;
	    }
	    return start;
	}
    
static word_t tbuf_handler_find_tbuf_end (word_t start, word_t count, word_t size)
	{ 
	    word_t end, num;
		
	    for (end = start, num = 0; num < size && count; end++, num++)
	    {
		if (end > size) end = 0;
		
		if (!tbuf_handler_pass (get_tracebuffer()->tracerecords + end))
		    continue;
		
		count--;
	    }
	    return end;
	}
    
static bool tbuf_handler_is_tbuf_valid (void)
	{
	    tracebuffer_t * tracebuffer = get_tracebuffer ();

	    if (! tracebuffer_is_valid (tracebuffer))
	    {
		printf("Bad tracebuffer signature at %p [%p]\n",
		       (word_t) (&tracebuffer->magic), tracebuffer->magic);
		return false;
	    }  
	    
	    if (atomic_read (&tracebuffer->current) == 0)
	    {
		printf ("No records\n");
		return false;
	    }
	    
	    return true;

	}
    

    
static void tbuf_handler_dump_tbuf (word_t start, word_t count, word_t size, bool header)
	{
	    word_t num, index;
	    tracerecord_t * rec;
	    tracebuffer_t * tracebuffer = get_tracebuffer ();
	    bool printed = false;
	    space_t * space = get_current_space_c ();

	    struct {
		word_t tsc;
		u64_t pmc0;
		u64_t pmc1;
	    } old[CONFIG_SMP_MAX_CPUS], sum = { 0, 0, 0 };

	    for (word_t cpu = 0; cpu < CONFIG_SMP_MAX_CPUS; cpu++)
		old[cpu].tsc = old[cpu].pmc0 = old[cpu].pmc1 = 0;

	    if (header)
		printf ("\nRecord P Type     TP %ws   TSC " IF_PERFMON (" PMC0  PMC1 ") "  Event\n", 
			"Thread");
	    
	    bool current_reached = false;
	    for (num = 1, index = start; count--; index++)
	    {
		if (index >= size) index = 0;
		rec = tracebuffer->tracerecords + index;

		if (!tbuf_handler_pass (rec))
		    continue;
		
		word_t cpu = rec->cpu;
		
		if (((++num % 4000) == 0) && get_choice ("Continue", "y/n", 'y') == 'n')
		    break;
		
		if (header && !current_reached && (index >= tbuf_handler_get_tbuf_current () + 1))
		{
		    current_reached = true;
		    printf ("------------------- Current ---------------"
			    IF_PERFMON ("--------------------") "\n");  
		}
		
		if (!old[cpu].tsc)
		{
		    old[cpu].tsc = rec->tsc;
		    IF_PERFMON (old[cpu].pmc0 = rec->pmc0);
		    IF_PERFMON (old[cpu].pmc1 = rec->pmc1);
		}

		u64_t c_delta = pmc_delta(rec->tsc, old[cpu].tsc);

		if (!header && !c_delta)
		    continue;
			
		printed = true;
	
		tcb_t * tcb;
		threadid_t tid;
		
		if (tracerecord_is_kernel_event (rec))
		{
		    tcb = addr_to_tcb((addr_t) rec->thread);
		    tid = tcb_get_global_id (tcb);
		    
		    printf ("%6d %01d %04x %c %4d %wt ", index, rec->cpu, tracerecord_get_type (rec), tracerecord_is_kernel_event (rec) ? 'k' : 'u', rec->id, tcb);
		}
		else
		{
		    tid = threadid_from_raw (rec->thread);
		    tcb = tcb_get_tcb (tid);
		    printf ("%6d %01d %04x %c %4d %wt ", index, rec->cpu, tracerecord_get_type (rec), 
			    tracerecord_is_kernel_event (rec) ? 'k' : 'u', rec->id, tid.raw);

		}


                if (get_tbuf_config().pmon)
                {
                    u64_t pmcdelta0;		
                    u64_t pmcdelta1;
#if defined(CONFIG_TBUF_PERFMON_ENERGY)
                    if (get_tbuf_config().pmon_e)
                    {                
                        // Energy mix
                        c_delta <<= X86_PMC_TSC_SHIFT;
				    
                        u64_t e_cur = ((u64_t) (rec->pmc1) << 32) | (u64_t) rec->pmc0;
                        u64_t e_old = ((u64_t) (old[cpu].pmc1) << 32) | (u64_t) old[cpu].pmc0;
		
                        u64_t e_delta = pmc_delta(e_cur, e_old);
			
                        u64_t p_freq_mhz = get_timer()->get_proc_freq() / 1000;
                        word_t n = (c_delta / 100) < (p_freq_mhz) ? 100 : 1;

                        u64_t t_delta = (n * c_delta) / p_freq_mhz;
                        u64_t p_delta = (t_delta ? ((n * e_delta) / t_delta) : 0);
		
                        pmcdelta0 = e_delta / 1000;		
                        pmcdelta1 = p_delta / 1000;
                    }
                    else
#endif
                    {
                        // User and kernel instructions
                        pmcdelta0 = pmc_delta(rec->pmc0, (word_t) old[cpu].pmc0);
                        pmcdelta1 = pmc_delta(rec->pmc1, (word_t) old[cpu].pmc1);
                    }
                		
                    pmc_print(c_delta);
                    pmc_print(pmcdelta0);
                    pmc_print(pmcdelta1);
		
                    sum.pmc0 += pmcdelta0;
                    sum.pmc1 += pmcdelta1;
		
                    old[cpu].pmc0 = rec->pmc0;
                    old[cpu].pmc1 = rec->pmc1;

                }
                else
                {
                    pmc_print(c_delta);
                }
            		
		sum.tsc  += (rec->tsc - old[cpu].tsc);

		old[cpu].tsc = rec->tsc;

		static char tb_str[256];
		word_t idx = 0;
		char *src = (char*) rec->str, *dst = tb_str;
		bool mapped = true;
		
		if (tracerecord_is_kernel_event (rec))
		{
		    space = get_kernel_space_c ();
		}
		else
		{
		    // For user strings we attempt to look up the string in
		    // the space of the thread.  We don't really bother too
		    // much if this does not work.

		    // Check if we seem to have a valid space and string pointer

		    if (tcb_get_global_id (tcb).raw != tid.raw ||
			space_is_user_area ((addr_t) tcb_get_space (tcb)) ||
			! space_is_user_area ((addr_t) rec->str))
		    {
	 		printf ("%p (%p, %p, %p, %p)\n", rec->str,
				rec->arg[0], rec->arg[1],
				rec->arg[2], rec->arg[3],
				rec->arg[4], rec->arg[5],
				rec->arg[6], rec->arg[7],
				rec->arg[8]);
			continue;
		    }
		    space = tcb_get_space (tcb);

		}
		addr_t p = (addr_t) src;
		char c;

		while ((mapped = readmem_u8 (space, p, &c)) && (c != 0) && idx++ < (sizeof (tb_str) - 1))
		{
		    *dst++ = c;
		    p = addr_offset (p, 1);
		    if (idx % TB_WRAP == 0)
		    {
			bool fid = (*(dst-1) == '%');
			if (fid) dst-=1;
			*dst++ = '\n'; *dst++ = '\t';
			*dst++ = '\t'; *dst++ = '\t';
			*dst++ = '\t'; *dst++ = '\t';
			*dst++ = '\t'; *dst++ = ' ';
			*dst++ = ' ' ; *dst++ = ' ';
			idx+=10;
			if (fid) *dst++ = '%';
		    }
		    // Turn '%s' into '%p' (i.e., avoid printing arbitrary
		    // user strings).
		    if (*dst == 's' &&
			( *(dst-1) == '%' || 	
			  ( *(dst-2) == '%' &&		      
			    ((*(dst-1) >= '0' && *(dst-1) <= '9') 
			     || *(dst-1) == 'w' || *(dst-1) == 'l' || *(dst-1) == '.'))))
			*dst = 'p';
		    
		    if (!mapped) *dst++ = 0;
		    
		}	    
		
		tb_str[idx] = 0;
		
		printf (tb_str, rec->arg[0], 
			rec->arg[1], rec->arg[2], 
			rec->arg[3], rec->arg[4], 
			rec->arg[5], rec->arg[6], 
			rec->arg[7], rec->arg[8]);	

		if (!mapped)
		    printf("[###]");

		// Append a newline and zero if needed and possible
		idx = strlen(tb_str);
		if( (idx < 1) || (tb_str[idx-1] != '\n' && tb_str[idx-1] != '\r') )
		    printf("\n"); 

	    }
    
	    if (header)
	    {
		printf ("-------------------------------------------"
			IF_PERFMON ("--------------------") "\n");  
		printf ("Mask %08x                 ", tracebuffer->mask);
		
		pmc_print(sum.tsc);
		IF_PERFMON (pmc_print(sum.pmc0));
		IF_PERFMON (pmc_print(sum.pmc1));
		
		printf(" %d entries", num-1);
	    }
	    
	    if (header || printed)
		printf("\n");

	}



 



void tbuf_dump (word_t count, word_t usec, word_t tp_id, word_t cpumask)
{
    word_t start, end, max;
    word_t old_tp_id[TBUF_MAX_FILTERS];
    word_t old_cpumask = tbuf_handler_get_cpumask ();
    word_t old_typemask = tbuf_handler_get_typemask ();
    u64_t old_tsc = tbuf_handler_get_tsc ();
    word_t old_tbuf_typemask = tbuf_handler_get_tbuf_typemask ();
    
    tbuf_handler_set_cpumask (cpumask);
    tbuf_handler_set_tbuf_typemask ((word_t) ~0ULL);
    tbuf_handler_set_typemask ((word_t )~0ULL);
    
    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
    {
	old_tp_id[i] = tbuf_handler_get_id (i);
	tbuf_handler_set_id (i, 0);
    }
    tbuf_handler_set_id (0, tp_id);
    
    max  =  tbuf_handler_get_tbuf_max ();
    end   = tbuf_handler_get_tbuf_current ();
    
    if (usec)
    {
        procdesc_t * pdesc = processor_info_get_procdesc (&get_kip()->processor_info, get_current_cpu());
        ASSERT (pdesc);
        word_t freq = pdesc->internal_freq + 1;
        u64_t tsc = get_cpu_cycles() - ((u64_t) usec * (u64_t) (freq / 1000));
	count = max;
	tbuf_handler_set_tsc (tsc);
    }
    else if (count == 0)
	count = max;
    
    start = tbuf_handler_find_tbuf_start (end, count, max);
    count = (end >= start) ? end - start : end + max - start;
    tbuf_handler_dump_tbuf (start, count, max, false);

    if (tp_id)
    {
	for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	    tbuf_handler_set_id (i, old_tp_id[i]);
    }
    tbuf_handler_set_cpumask (old_cpumask);
    tbuf_handler_set_typemask (old_typemask);
    tbuf_handler_set_tsc (old_tsc);
    tbuf_handler_set_tbuf_typemask (old_tbuf_typemask);
 
}



/*
 * Submenu for tracebuffer related commands.
 */

DECLARE_CMD (cmd_tracebuffer, root, 'y', "tracebuffer",
	     "Dump/manipulate tracebuffer");

CMD (cmd_tracebuffer, cg)
{
    return cmd_group_interact (&tracebuf, cg, "tracebuffer");
}


/*
 * Reset tracebuffer.
 */

DECLARE_CMD (cmd_tb_reset, tracebuf, 'r', "reset", "Reset buffer");

CMD (cmd_tb_reset, cg)
{	
    tbuf_handler_reset_tbuf ();
    return CMD_NOQUIT;
}


/*
 * Reset counters.
 */

DECLARE_CMD (cmd_tb_reset_ctr, tracebuf, 'R', "resetctr", "Reset counters");

CMD (cmd_tb_reset_ctr, cg)
{
    tbuf_handler_reset_tbuf_counters ();
    return CMD_NOQUIT;
}


/*
 * Dump counters.
 */

DECLARE_CMD (cmd_tb_dump_ctr, tracebuf, 'c', "counters", "Dump counters");

CMD (cmd_tb_dump_ctr, cg)
{
    tbuf_handler_dump_tbuf_counters ();
    return CMD_NOQUIT;
}


/*
 * Apply filter for tracebuffer events.
 */

word_t get_typemask()
{
    word_t mask = 0;
    
    switch (get_choice ("Keep which events",
			"All/Kernel/No tracept/no tpDetails/User/Mask", 'a'))
    {
    case 'a':
	mask = 0xffffffff;
	break;
    case 'k':
	mask = 0xffff0000;
	break;
    case 'n':
	mask = 0xfffcffff;
	break;
    case 'd':
	mask = 0x00010001;
	break;
    case 'u':
	mask = 0x0000ffff;
	break;
    case 'm':
	mask = get_hex ("Mask", 0xffffffff, NULL);
	break;
    }
    
    return mask;
    
}

DECLARE_CMD (cmd_tb_type_filter, tracebuf, 'f', "filter", "Record filter");

CMD (cmd_tb_type_filter, cg)
{	
    tbuf_handler_set_tbuf_typemask (get_typemask());
    return CMD_NOQUIT;
}


/*
 * Dump current tracebuffer.
 */

DECLARE_CMD (cmd_tb_dump, tracebuf, 'd', "dump", "Dump tracebuffer");

CMD (cmd_tb_dump, cg)
{ 
    word_t start, end, max, count;
   
    if (!tbuf_handler_is_tbuf_valid ())
	return CMD_NOQUIT;
    
    max  = tbuf_handler_get_tbuf_max ();
    count = 32;
    start = 0;
    end   = tbuf_handler_get_tbuf_current ();
    
    switch (get_choice ("Dump tracebuffer", "All/Region/Top/Bottom", 'b'))
    {
    case 'a': 
	count = max;
	break;
    case 'r': 
	start = get_dec ("From record",  0, NULL);
	// Fall through
    case 't': 
	count = get_dec ("Record count", count, NULL);
	end = tbuf_handler_find_tbuf_end (start, count, max);
	if (count > max)  count = max; 
	break;
    case 'b':
    default: 
	count = get_dec ("Record count", count, NULL);
	if (count > max) count = max;
	start = tbuf_handler_find_tbuf_start (end, count, max);
	break;
    } 

    count = (end >= start) ? end - start : end + max - start;
    tbuf_handler_dump_tbuf (start, count, max, true);
	
    return CMD_NOQUIT;
}


/*
 * Dump current tracebuffer (default values).
 */

DECLARE_CMD (cmd_tb_dump_def, root, 'Y', "tracebuffer dump",
	     "Dump tracebuffer (default values");


CMD (cmd_tb_dump_def, cg)
{
    word_t start, end, max, count;
   
    if (!tbuf_handler_is_tbuf_valid ())
	return CMD_NOQUIT;
    
    
    max  = tbuf_handler_get_tbuf_max ();
    count = 64;
    end   = tbuf_handler_get_tbuf_current ();
    start = tbuf_handler_find_tbuf_start (end, count, max);
    count = (end >= start) ? end - start : end + max - start;
    tbuf_handler_dump_tbuf (start, count, max, true);

    return CMD_NOQUIT;
}

/*
 * Submenu for tracebuffer display filters.
 */

#if defined(CONFIG_SMP)

DECLARE_CMD (cmd_tb_cpu, tracebuf, 'C', "cpufilter", "CPU display filter");
CMD(cmd_tb_cpu, cg) 
{
    tbuf_handler_set_cpumask (get_hex("Processor Filter", ~0UL, "all"));
    return CMD_NOQUIT;
}

#endif	    

/*
 * Apply filter for tracebuffer events.
 */

DECLARE_CMD (cmd_tb_events, tracebuf, 'F', "filter", "Record display filter");

CMD (cmd_tb_events, cg)
{	
    tbuf_handler_set_typemask (get_typemask());
    return CMD_NOQUIT;
}


DECLARE_CMD (cmd_tb_evt, tracebuf, 't', "tpfilter", "TP display filter");
CMD(cmd_tb_evt, cg) 
{
    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	tbuf_handler_set_id (i, 0);
    
    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
    {
	for (;;)
	{
	    tbuf_handler_set_id (i, 0);
	    word_t id = get_dec ("Select TP", 0, "list");
	    if (id == 0)
	    {
		list_tp_choices ();
		continue;
	    }
	    else if (id == ABORT_MAGIC)
		return CMD_NOQUIT;
	    else if (id <= tracepoint_list_size (&tp_list))
		tbuf_handler_set_id (i, id);
	    else if (id >= TB_USERID_START)
		tbuf_handler_set_id (i, id);
	    break;
	}
	if (get_choice ("More events", "y/n", 'n') == 'n')
	    break;

    }
    return CMD_NOQUIT;
}


DECLARE_CMD (cmd_tb_tcb, tracebuf, 'T', "tcbfilter", "TCB display filter");
CMD(cmd_tb_tcb, cg) 
{
    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
	tbuf_handler_set_tcb (i, NULL);
    
    for (word_t i=0; i < TBUF_MAX_FILTERS; i++)
    {
	tbuf_handler_set_tcb (i, get_thread ("tcb/tid/name"));
	if (get_choice ("More threads", "y/n", 'n') == 'n')
	    break;

    }
    return CMD_NOQUIT;
	
}

DECLARE_CMD (cmd_tb_showfilters, tracebuf, 's', "showfilters", "Show filters");
CMD(cmd_tb_showfilters, cg) 
{
    tbuf_handler_dump_filters ();
    return CMD_NOQUIT;
	
}

DECLARE_CMD (cmd_tb_zero, tracebuf, 'z', "zerofilter", "Invalidate filters");
CMD(cmd_tb_zero, cg) 
{
    tbuf_handler_invalidate_filters ();
    return CMD_NOQUIT;
}



#endif /* CONFIG_TRACEBUFFER */
