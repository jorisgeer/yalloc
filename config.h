/* config.h - compile-time configuration constants

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// --- diagnostics / error handling ---

  // output fds, -1 for file except Err_fd
  static int Yal_log_fd = -1; // trace and diag
  static int Yal_err_fd = -1; // errors
  static int Yal_Err_fd = 2; // aux errors
  static int Yal_stats_fd = -1; // stats

  // thread id appended
  static const char *Yal_log_file[] = { "yal-log-heap",".log" };
  static const char *Yal_err_file[] = { "yal-err-heap",".log" };
  static const char *Yal_stats_file[] = { "yal-stats-heap",".log" };

  #define Yal_log_utf8 1 // allow unicode in logs in utf-8

  /* control error handling - as bit mask
   1 - detect, count and ignore. Format diagnostic for yal_stats()
   2 - In addition to above print diagnostic
   4 - exit via _Exit(1)
  */
  #define Yal_check_envvar "Yalloc_check"
  #define Yal_check_default 7

  /* Enable statistics:
  0 - disabled. Yal_stats() is not defined
  1 - minimal. Yal_stats(() is defined and a few basic tallies are maintained
  2 - full
   */
  #define Yal_enable_stats 2

  /* If enabled above, control statistics printing at exit - as bit mask
    1 - summary per heap
    2 - detailed per region
    4 - totals over all heaps
    8 - add state
    32 - add config
   */
  #define Yal_stats_envvar "Yalloc_stats"

  #define Yal_trigger_stats 0x11223344 // compatible hack - make calloc(0,trigger) invoke Yal_stats()
  #define Yal_trigger_stats_threads 0x11223345

  #define Yal_enable_trace 1 // incurs minor overhead, unless enabled at run time
  #define Yal_trace_default 0

  /* control tracing bitmask
   1 - basic - one line per call
   2 - extended
   4 - use yal-diag.cfg for suppressions
   8 - include callsite
  */
  #define Yal_trace_envvar "Yalloc_trace"

  #define Yal_trace_ctl "yal_diag.cfg"

  // Store callsite tag. Adds minor overhead
  #define Yal_enable_tag 0

  // Enables various internal checks aka assertions. Adds minor overhead. Advised to enable for alpha and beta versions.
  #define Yal_enable_check 1

  // enable semi stack trace. Adds minor overhead
  #define Yal_enable_stack 0
  #define Yal_stack_len 32

  #define Yal_log_level 5 // 1 assert 2 error 3 warn 4 info 5 trace 6 vrb 7 dbg

  #define Yal_dbg_level 0 // enable detailed debug logging. Levels 1..3

  // Uses valgrind's client requests to set memory blocks as memcheck would have done. See ./vg_mc.sh and vg_drd.h
  // Useful when not using vg_replace_malloc(). Adds minimal overhead.
  #define Yal_enable_valgrind 0

  // If set, let sigsegv and sigbus print an error, statistics and exit
  #define Yal_signal 1

// -- portability --

  #define Yal_errno 1 // Whether to set errno to ENOMEM on out-of-memory

// --- virtual memory, in bits ---

#define Vmbits64 48 // -rerun configure- 256 TB for 64-bit systems, 32 for 32-bit systems

#define Minregion 16

#define Mmap_threshold 16u // mmap threshold for unpopular blocks
#define Mmap_max_threshold 22u // -rerun configure-  mmap threshold for all blocks

#define Xclas_threshold 4
#define Clas_threshold 128 // popularity measure

#define Smalclas 1024 // -rerun configure- use tabled class below this len

// --- memory usage ---

// How many free() calls between a region age step. Must be pwr2 - 1
static const unsigned int regfree_interval = 0xff;

#define Trim_scan 64 // number of regions to scan at a time. 0 to disable

// --- safety ---
#define Realloc_clear 0

// ageing thresholds
static unsigned int Trim_ages[3] = {
  2, // recycle
  6, // remove from dir
  12}; // release mem
static unsigned int Trim_Ages[3] = {3,6,9}; // idem, larger blocks

static const unsigned int Region_interval = 0xff; // pwr2 - 1
static const unsigned int Region_alloc = 32; // allow #alloc releases per interval
static const unsigned long Mmap_retainlimit = 1ul << 30; // directly release memory

// --- slab ---
#define Cel_nolen 1023 // Store user aka net length per cell above this len

#define Rbinbuf 64 // Initial remote freelist
#define Buffer_flush 256 // Item threshold to flush remote freelist

// -- bump region (within heap) --
#define Bumplen 0x4000
#define Bumpmax 256
#define Bumpregions 4

// -- mini bump region (outside heap) --
#define Minilen 1024
#define Minimax 64

// --- slab regions ---
#define Regmem_inc 32
#define Xregmem_inc (64 * 4)

#define Rmeminc 0x4000

// number of directories
#define Dirmem_init 8
#define Dirmem 16

// --- threading ---

#define Yal_enable_private 1 // private heap for main thread
static const unsigned int Private_drop_threshold = 1024;
static const unsigned int Private_interval = 0xff; // pwr2 - 1

/* Thread support mode
  0 - none
  1 - TLS
  2 - pthread_self()
*/
#define Yal_tidmode 1
#define Maxtid 65536

#define L1line 128

// Install thread exit handler - nonportable
#define Yal_thread_exit 0

#define Contention 6 // create per-thread heap if contended * 1 << contention exceeds uncontended

// Set to prep TLS with a before-main function. gcc on darwin aka macos call malloc() at TLS init...
#ifdef __linux__
 #define Yal_prep_TLS 0
#elif defined __APPLE__ && defined __MACH__ && defined __aarch64__ && ! defined __clang__
 #define Yal_prep_TLS 1
#else
 #define Yal_prep_TLS 0
#endif

// --- preallocated ---

#define Bootmem (0x1000 - 32)

// --- align ---
#define Basealign 8u // Expected alignof(max_align_t)
#define Stdalign 16u // Alignment of blocks > 8

// --- extensions / compatibility ---
#define Yal_enable_extensions 1

#define Yal_psx_memalign 1 // 2 to include valloc

#define Yal_enable_c23 1 // free_sized
#define Yal_glibc_mtrace 0

#define Yal_malloc_stats 1

#define Yal_mallopt 0
#define Yal_mallinfo 0
#define Yal_glibc_malloc_stats 1
