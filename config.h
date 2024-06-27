/* config.h - compile-time configurable constants

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// -- portability --

/* threading model
    0 - none, use a single static heap
    1 - thread local store aka TLS
    2 - pthreads : pthread_self() and hash
    3 - C11 threads and hash
 */
#define Yal_thread_model 1

// Support free(p) and realloc(p) with 'p' allocated in another thread
#define Yal_inter_thread_free 1

/*
  Operating system locking
  0 - none
  1 - Linux (futex)
  4 - macOS aka Darwin
  5 - Windows
 */
#define Yal_locking 4

// Set to prep TLS with a before-main function. gcc on darwin aka macos needs this
#define Yal_prep_TLS 0

#define Lockspin 100

// --- behaviour ---

#define Free_failed_realloc 0 // if realloc(p,n) fails, free p.

// --- virtual memory ---

// #define Page_override 12

#define Vmsize 40 // 1 TB

#define Minregion 16
#define Maxregion 32

#define Accel_cnt 3

#define Minorder 2  // smallest block

#define Mmap_initial_threshold 16u

// --- slab ---
//#define Maxsizclas 16
#define Clasbits 2 // size class granularity. steps between powers of two
#define Maxclass Mmap_initial_threshold
#define Clas_threshold 0u

#define Metaguard 0 // # guard pages around metadata blocks

#define Multilen 16 // runs of multi-cell blocks

// --- regions ---
#define Regions 0x10000
#define Iniregs 256

// #define Region 14

#define Regmem_inc 32
#define Xregmem_inc (64 * 4)

#define Regfree_trim 4u

#define Regbin 4
#define Regbinsum (1ul << 22)

// --- region directory ---
//  vm:40 - pg:14 = 26 = 8 + 7 + leaf:11
#define Dir1 8
#define Dir2 7
#define Dir3 11  // 4 KB leaf dir

//  40 - 20 = 20 = 2 x 10

#define Dirlimit 33

// number of directories
#define Dirmem_init 4
#define Dirmem 16

// threading
#define Hash_order 32

// --- preallocated ---
#define Bumplen 0x8000u
//#define Bumpmax 256u
#define Bumpmax 1u

// #define Initdir 8
#define Bootmem 0x1000
#define Heap_del_threshhold 16

// --- buddy ---
#define Min2order 4  // smallest block
#define Maxorder 26
#define Addorder 4

// --- recycling bin ---
#define Bin 64 // #binned items per size. Even number
#define Binful 16

// --- align ---
// #define Basealign _Alignof(max_align_t)
#define Basealign2 3
#define Stdalign 16u

// --- diag ---
#define Yal_enable_log 1
#define Yal_enable_error 1
#define Yal_enable_trace 1
#define Yal_enable_dbg 1

#define Yal_log_level 5 // 1 assert 2 error 3 warn 4 info 5 trace 6 dbg

#define Diag_counts 256
#define Diag_buf 256
#define Yal_diag_init "yal_diag.cfg"

#define Yal_enable_stats 1
#define Yal_enable_check 1
#define Yal_enable_valgrind 0

#define Yal_trigger_stats 0x11223344 // calloc(0,trigger)
#define Yal_trigger_stats_clear 0x11223345

#define Yal_log_fd 1
#define Yal_error_fd 1
#define Yal_stats_buf (1ul << 18)

#define Yal_stats_envvar "Yal_stats"

#define Yal_enable_bist 1
#define Bistlen (1ul << 20)
#define Bistregs 2048

#define Yal_enable_test 0

// --- extensions / compatibility ---
#define Yal_glibc_mtrace 1
#define Yal_enable_extensions 1
#define Yal_enable_boot_malloc 0
#define Yal_enable_mallopt 0
#define Yal_enable_mallinfo 0
#define Yal_enable_maltrim 0
#define Yal_enable_glibc_malloc_stats 0
