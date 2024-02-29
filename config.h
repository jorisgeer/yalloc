/* config.h - compile-time configurable constants

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// behaviour
#define FREE_FAIL_REALLOC 0

// vm
#define Maxvm 40
#define Maxvmsiz (1ul << Maxvm)

#define Minregion 16

#define Accel_cnt 3

// 64k for 48/42-bit vm and 4/3 accels
#define Maxregion (Maxvm - Minorder - 6 - (Accel_cnt * 6))

// slab
//#define Maxsizclas 16
#define Maxclasslen 4096u
#define Clas_threshold 0u
// #define Maxclas_len (1U << Maxsizclas)
// #define Maxclas_cnt (1U << (Maxsizclas + Sizestep))
#define Maxtclass 2048u
#define Maxclass 256u

#define Regstep 4
#define Regclas ?

// regions
#define Region 14

#define Regmem_inc 1024u

#define Region_cnt (1u << Region) // e.g. Linux vm.max_map_count = 65530
#define Regfree_trim 4u

// region directory

// 8 : avg span = 20
#define Dir 8

#define Dirmem 8192u

// 42 - 18 = 24
#if (Maxvm - Minregion) <= Dir
 #error "VM directory too coarse"
#endif

#if (Maxvm - Minregion) > 3 * Dir
  #define Dirlvl 4
#elif (Maxvm - Minregion) > 2 * Dir
  #define Dirlvl 3
#else
  #define Dirlvl 2
#endif

#define Mmap_limit 22

// preallocated
#define  Iniheap 0x20000u
#define Inimem 0x400u
// #define Initdir 8
// #define Bootmem 0x1000
#define Heap_del_threshhold 16

// buddy

#define Minorder 3  // smallest block
#define Maxorder 26
#define Orderrange 16

// recycling bin
//#define Binbit 16 // Recycle blocks below this size
#define Bin 8 // #binned items per size
#define Binmask ((1u << Bin) - 1)

// align
// #define Basealign _Alignof(max_align_t)
#define Basealign 8u

#define Page 4096u

// diag
#define Yal_enable_log 1
#define Yal_enable_stats 1

#define Mmap_threshold (1ul << 24)

#define Yal_glibc_mtrace 1

// Dynamic config vars with initial value

static unsigned int inireg = 16; // Initial region size
static unsigned int inidir = 8; // preallocated directory entries

// static unsigned int mmap_threshold_bit = 24;
static size_t mmap_threshold = Mmap_threshold;

static unsigned int safe_mode = 1;
static unsigned int guardbit = 0;
