/*********************************************************************
 *
 * Copyright (C) 2026,  Karlsruhe University
 *
 * File path:     glue/v4-x86/x64/x32comp/x32-names.h
 * Description:   `namespace x32', in C
 *
 * @LICENSE@
 *
 * $Id$
 *
 ********************************************************************/

/*
 * Compatibility mode needs a second copy of the V4 API types, built with a
 * 32-bit word.  The original got it by re-including api/v4/{types,thread,
 * kernelinterface}.h -- with their include guards undefined and word_t
 * typedef'd to u32_t -- inside `namespace x32'.  C has no namespaces, so this
 * header supplies the same isolation with the preprocessor: it renames every
 * name those headers declare to an x32_-prefixed one, so the second copy can
 * be emitted alongside the first without colliding with it.
 *
 * Deliberately has no include guard.  Use it in pairs:
 *
 *     #include INC_GLUE_SA(x32comp/x32-names.h)
 *     ...re-include the API headers, with their guards undefined...
 *     #define X32_UNRENAME
 *     #include INC_GLUE_SA(x32comp/x32-names.h)
 *     #undef X32_UNRENAME
 *
 * The two lists below must stay in step.  They do so under pressure: a name
 * missing from the rename list collides on the second include, and one missing
 * from the unrename list leaves the 64-bit code compiling against x32_ names.
 * Both are compile errors, immediately.
 */

#if !defined(X32_UNRENAME)

/* the word size itself */
#define word_t x32_word_t
#define addr_t x32_addr_t

/* api/v4/types.h */
#define time_t          x32_time_t
#define time_is_never   x32_time_is_never
#define time_is_period  x32_time_is_period
#define time_is_zero    x32_time_is_zero
#define timeout_t       x32_timeout_t
#define timeout_never   x32_timeout_never
#define timeout_get_rcv x32_timeout_get_rcv
#define timeout_get_snd x32_timeout_get_snd
#define cpuid_t         x32_cpuid_t

/* api/v4/thread.h */
#define threadid_t                 x32_threadid_t
#define threadid_get_raw           x32_threadid_get_raw
#define threadid_set_raw           x32_threadid_set_raw
#define threadid_set               x32_threadid_set
#define threadid_anythread         x32_threadid_anythread
#define threadid_anylocalthread    x32_threadid_anylocalthread
#define threadid_nilthread         x32_threadid_nilthread
#define threadid_irqthread         x32_threadid_irqthread
#define threadid_idlethread        x32_threadid_idlethread
#define threadid_global            x32_threadid_global
#define threadid_from_raw          x32_threadid_from_raw
#define threadid_is_global         x32_threadid_is_global
#define threadid_is_local          x32_threadid_is_local
#define threadid_is_nilthread      x32_threadid_is_nilthread
#define threadid_is_anythread      x32_threadid_is_anythread
#define threadid_is_anylocalthread x32_threadid_is_anylocalthread
#define threadid_get_threadno      x32_threadid_get_threadno
#define threadid_get_version       x32_threadid_get_version
#define threadid_get_irqno         x32_threadid_get_irqno
#define threadid_is_interrupt      x32_threadid_is_interrupt
#define threadid_set_global_id     x32_threadid_set_global_id
#define threadid_equals            x32_threadid_equals
#define threadid_not_equals        x32_threadid_not_equals

/* generic/memregion.h */
#define mem_region_t        x32_mem_region_t
#define mem_region_get_size x32_mem_region_get_size
#define mem_region_set      x32_mem_region_set
#define mem_region_is_empty x32_mem_region_is_empty

/* api/v4/memdesc.h */
#define memdesc_t          x32_memdesc_t
#define memdesc_type       x32_memdesc_type
#define memdesc_subtype    x32_memdesc_subtype
#define memdesc_is_virtual x32_memdesc_is_virtual
#define memdesc_low        x32_memdesc_low
#define memdesc_high       x32_memdesc_high
#define memdesc_size       x32_memdesc_size
#define memdesc_set        x32_memdesc_set

/* api/v4/procdesc.h */
#define procdesc_t x32_procdesc_t

/* api/v4/kernelinterface.h */
#define kdebug_init_t                          x32_kdebug_init_t
#define kdebug_entry_t                         x32_kdebug_entry_t
#define kernel_descriptor_t                    x32_kernel_descriptor_t
#define root_server_t                          x32_root_server_t
#define memory_info_t                          x32_memory_info_t
#define memory_info_get_num_descriptors        x32_memory_info_get_num_descriptors
#define utcb_info_t                            x32_utcb_info_t
#define utcb_info_get_minimal_size             x32_utcb_info_get_minimal_size
#define utcb_info_get_utcb_alignment           x32_utcb_info_get_utcb_alignment
#define utcb_info_get_utcb_size                x32_utcb_info_get_utcb_size
#define utcb_info_is_valid_utcb_location       x32_utcb_info_is_valid_utcb_location
#define kip_area_info_t                        x32_kip_area_info_t
#define kip_area_info_get_size                 x32_kip_area_info_get_size
#define kip_area_info_get_size_log2            x32_kip_area_info_get_size_log2
#define clock_info_t                           x32_clock_info_t
#define clock_info_get_read_precision          x32_clock_info_get_read_precision
#define clock_info_get_schedule_precision      x32_clock_info_get_schedule_precision
#define thread_info_t                          x32_thread_info_t
#define thread_info_get_user_base              x32_thread_info_get_user_base
#define thread_info_get_system_base            x32_thread_info_get_system_base
#define thread_info_get_significant_threadbits x32_thread_info_get_significant_threadbits
#define thread_info_set_system_base            x32_thread_info_set_system_base
#define thread_info_set_user_base              x32_thread_info_set_user_base
#define page_info_t                            x32_page_info_t
#define page_info_get_access_rights            x32_page_info_get_access_rights
#define page_info_get_page_size_mask           x32_page_info_get_page_size_mask
#define processor_info_t                       x32_processor_info_t
#define processor_info_get_num_processors      x32_processor_info_get_num_processors
#define processor_info_get_procdesc_size       x32_processor_info_get_procdesc_size
#define api_flags_t                            x32_api_flags_t
#define api_version_t                          x32_api_version_t
#define api_version_get_version                x32_api_version_get_version
#define api_version_get_subversion             x32_api_version_get_subversion
#define api_flags_get_endian                   x32_api_flags_get_endian
#define api_flags_get_word_size                x32_api_flags_get_word_size
#define api_version_to_word                    x32_api_version_to_word
#define api_flags_to_word                      x32_api_flags_to_word
#define magic_word_t                           x32_magic_word_t
#define syscall_t                              x32_syscall_t
#define kernel_interface_page_t                x32_kernel_interface_page_t
#define get_kip                                x32_get_kip
#define kernel_id_t                            x32_kernel_id_t
#define kernel_id_get_subid                    x32_kernel_id_get_subid
#define kernel_id_get_id                       x32_kernel_id_get_id
#define kernel_id_get_raw                      x32_kernel_id_get_raw
#define kernel_gen_date_t                      x32_kernel_gen_date_t
#define kernel_gen_date_get_day                x32_kernel_gen_date_get_day
#define kernel_gen_date_get_month              x32_kernel_gen_date_get_month
#define kernel_gen_date_get_year               x32_kernel_gen_date_get_year
#define kernel_version_t                       x32_kernel_version_t
#define kernel_version_get_subsubver           x32_kernel_version_get_subsubver
#define kernel_version_get_subver              x32_kernel_version_get_subver
#define kernel_version_get_ver                 x32_kernel_version_get_ver
#define kernel_supplier_t                      x32_kernel_supplier_t
#define kernel_descriptor_get_version_string   x32_kernel_descriptor_get_version_string
#define processor_info_get_procdesc            x32_processor_info_get_procdesc
#define memory_info_get_memdesc                x32_memory_info_get_memdesc
#define memory_info_insert                     x32_memory_info_insert
#define kernel_interface_page_init             x32_kernel_interface_page_init

/* api/v4/kernelinterface.c (file-scope definitions) */
#define kdesc                 x32_kdesc
#define processor_descriptors x32_processor_descriptors
#define init_hello            x32_init_hello

#else /* defined(X32_UNRENAME) */

/* the word size itself */
#undef word_t
#undef addr_t

/* api/v4/types.h */
#undef time_t
#undef time_is_never
#undef time_is_period
#undef time_is_zero
#undef timeout_t
#undef timeout_never
#undef timeout_get_rcv
#undef timeout_get_snd
#undef cpuid_t

/* api/v4/thread.h */
#undef threadid_t
#undef threadid_get_raw
#undef threadid_set_raw
#undef threadid_set
#undef threadid_anythread
#undef threadid_anylocalthread
#undef threadid_nilthread
#undef threadid_irqthread
#undef threadid_idlethread
#undef threadid_global
#undef threadid_from_raw
#undef threadid_is_global
#undef threadid_is_local
#undef threadid_is_nilthread
#undef threadid_is_anythread
#undef threadid_is_anylocalthread
#undef threadid_get_threadno
#undef threadid_get_version
#undef threadid_get_irqno
#undef threadid_is_interrupt
#undef threadid_set_global_id
#undef threadid_equals
#undef threadid_not_equals

/* generic/memregion.h */
#undef mem_region_t
#undef mem_region_get_size
#undef mem_region_set
#undef mem_region_is_empty

/* api/v4/memdesc.h */
#undef memdesc_t
#undef memdesc_type
#undef memdesc_subtype
#undef memdesc_is_virtual
#undef memdesc_low
#undef memdesc_high
#undef memdesc_size
#undef memdesc_set

/* api/v4/procdesc.h */
#undef procdesc_t

/* api/v4/kernelinterface.h */
#undef kdebug_init_t
#undef kdebug_entry_t
#undef kernel_descriptor_t
#undef root_server_t
#undef memory_info_t
#undef memory_info_get_num_descriptors
#undef utcb_info_t
#undef utcb_info_get_minimal_size
#undef utcb_info_get_utcb_alignment
#undef utcb_info_get_utcb_size
#undef utcb_info_is_valid_utcb_location
#undef kip_area_info_t
#undef kip_area_info_get_size
#undef kip_area_info_get_size_log2
#undef clock_info_t
#undef clock_info_get_read_precision
#undef clock_info_get_schedule_precision
#undef thread_info_t
#undef thread_info_get_user_base
#undef thread_info_get_system_base
#undef thread_info_get_significant_threadbits
#undef thread_info_set_system_base
#undef thread_info_set_user_base
#undef page_info_t
#undef page_info_get_access_rights
#undef page_info_get_page_size_mask
#undef processor_info_t
#undef processor_info_get_num_processors
#undef processor_info_get_procdesc_size
#undef api_flags_t
#undef api_version_t
#undef api_version_get_version
#undef api_version_get_subversion
#undef api_flags_get_endian
#undef api_flags_get_word_size
#undef api_version_to_word
#undef api_flags_to_word
#undef magic_word_t
#undef syscall_t
#undef kernel_interface_page_t
#undef get_kip
#undef kernel_id_t
#undef kernel_id_get_subid
#undef kernel_id_get_id
#undef kernel_id_get_raw
#undef kernel_gen_date_t
#undef kernel_gen_date_get_day
#undef kernel_gen_date_get_month
#undef kernel_gen_date_get_year
#undef kernel_version_t
#undef kernel_version_get_subsubver
#undef kernel_version_get_subver
#undef kernel_version_get_ver
#undef kernel_supplier_t
#undef kernel_descriptor_get_version_string
#undef processor_info_get_procdesc
#undef memory_info_get_memdesc
#undef memory_info_insert
#undef kernel_interface_page_init

/* api/v4/kernelinterface.c (file-scope definitions) */
#undef kdesc
#undef processor_descriptors
#undef init_hello

#endif /* !defined(X32_UNRENAME) */
