/* config.h - compile-time configuration constants

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// --- diagnostics / error handling ---

  // output fds, -1 for file
  static int Yal_log_fd = 1; // trace and diag
  static int Yal_err_fd = 1; // errors
  static int Yal_Err_fd = 2; // aux errors
  static int Yal_stats_fd = 1; // stats

  // thread id appended
  static const char *Yal_log_file[] =  { "yal-log-heap",".log" };
  static const char *Yal_err_file[] =  { "yal-err-heap",".log" };
  static const char *Yal_stats_file[] =  { "yal-stats-heap",".log" };

  #define Yal_log_utf8 1 // allow unicode in logs in utf-8

  /* control error handling - as bit mask
   0 - detect, count and ignore. Format diagnostic for yal_stats()
   1 - In addition to above print diagnostic
   2 - exit via _Exit(1)
  */
  #define Yal_check_envvar "Yalloc_check"
  #define Yal_check_default 3

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


  /* control tracing
   1 - enable
   2 - use yal-diag.cfg for suppressions
  */
  #define Yal_enable_trace 1 // incurs minor overhead, unless enabled at run time
  #define Yal_trace_default 0

  #define Yal_trace_envvar "Yalloc_trace"

  #define Yal_trace_ctl "yal_diag.cfg"

  // Store callsite tag. Adds minor overhead
  #define Yal_enable_tag 0

  // Enables various internal checks aka assertions. Adds minor overhead. Advised to enable for alpha and beta versions.
  #define Yal_enable_check 1

  // enable detailed debug logging
  #define Yal_enable_dbg 0

  // enable semi stack trace
  #define Yal_enable_stack 0

  // Uses valgrind's client requests to set memory blocks as memcheck would have done. See ./vg.sh
  // Useful when not using vg_replace_malloc(). Adds minimal overhead.
  #define Yal_enable_valgrind 0

  // If set, let sigsegv and sigbus print an error, statistics and exit
  #define Yal_signal 1

// -- portability --

  #define Yal_errno 1 // Whether to set errno to ENOMEM on out-of-memory

// --- virtual memory ---

#define Vmbits64 48 // 256 TB for 64-bit systems, 32 for 32-bit systems

#define Minregion 16

#define Mmap_threshold 16u
#define Mmap_max_threshold 22u

#define Xclas_threshold 4
#define Clas_threshold 256 // popularity measure

// --- memory usage ---

// How many free() calls between a region age step. Must be pwr2 - 1
static const unsigned int regfree_interval = 0x7f;

#define Trim_scan 64 // number of regions to scan at a time

// aging thresholds
static unsigned int Trim_ages[3] = {
  2, // recycle
  6, // remove from dir
  12}; // release mem
static unsigned int Trim_Ages[3] = {3,6,9}; // idem, larger blocks

// --- slab ---
#define Cel_nolen 64 // Do not store user aka net length per cell below this block len

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

// remote free
#define Rembatch 32
#define Rembkt 64
#define Ovflen 256
#define Ovfxlen 64
#define Ovfmax 65536

// number of directories
#define Dirmem_init 8
#define Dirmem 16

// --- threading ---

#define L1line 128

// Support free(p) and realloc(p) with 'p' allocated in another thread
#define Yal_inter_thread_free 1

// Install thread exit handler - nonportable
#define Yal_thread_exit 0

// #define Minheaps 4
#define Contention 6 // create per-thread heap if contended * 1 << contention exceeds uncontended

#define Segment 4u // freelist shards

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
#define Basealign2 3
#define Stdalign 16u

// --- diag ---
#define Yal_log_level 5 // 1 assert 2 error 3 warn 4 info 5 trace 6 vrb 7 dbg
#define Yal_dbg_level 1

// --- extensions / compatibility ---
#define Yal_enable_extensions 1

#define Yal_psx_memalign 1 // 2 to include valloc

#define Yal_enable_c23 1 // free_sized
#define Yal_glibc_mtrace 0

#define Yal_malloc_stats 1

#define Yal_mallopt 0
#define Yal_mallinfo 0
#define Yal_glibc_malloc_stats 1
