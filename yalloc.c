/* yalloc.c - yet another memory allocator

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Memory ranges are obtained from the os as large power-of-two sized regions. Each region has separatly mmap()ed user data and metadata.
  User blocks above a given size are mmap()ed directly, described by a virtual region. Initial regions are of a given size, subsequent regions may be larger dependent on overall usage.

  Regions are described by a region descriptor table, similar to multi-level page tables describe virtual memory. A top-level directory holds 256 entries to mid-level table of 256 entries each.
  The leaf tables hold region entries. free() uses these to locae an entry, given the minimum region size

  Within a region, user data is kept separate from admin aka metadata. This protects metadata from being overwriitten
  User blocks have no header or trailer. Consecutively allocated blocks are adjacent without a gap. This helps cache and TLB efficiency.
  Once a region becomes fully free, it is returned to the os.
  Within a regular region, buddy allocation is done to serve malloc() requests. Size is represented as a power of two bitshift aka 'order'
  requested block sizes are rounded up to the next power of two. Thus, internal fragemantiation is between 0% -best case  and 50% -worst case-

  Blocks below a given size are binned into size classes. Above a certain usage threshold, a fixed-side slab region is created and used for subsequent requests.

  Blocks are aligned at their rounded-up size following 'weak alignment' as in https://www.open-std.org/JTC1/SC22/WG14/www/docs/n2293.htm
  A 4-byte block is aligned 4.

  Freed blocks are held in a recycling bin per order, genuinely freeing a LRU item. malloc() uses these on an MRU basis if available.

  Multiple threads are supported by having a per-thread heap containing all of the above parts.
  */

#include <limits.h>

#include <stdarg.h>
#include <stddef.h> // size_t,max_align_t
#include <stdint.h> // SIZE_MAX
#include <string.h> // memset

#include "stdlib.h"
#include "config.h"
#include "malloc.h"

#ifdef VALGRIND
 #include <valgrind/valgrind.h>
 #include <valgrind/memcheck.h>
 #define vg_mem_noaccess(p,n) VALGRIND_MAKE_MEM_NOACCESS((p),(n));
 #define vg_mem_undef(p,n) VALGRIND_MAKE_MEM_UNDEFINED((p),(n));
 #define vg_mem_def(p,n) VALGRIND_MAKE_MEM_DEFINED((p),(n));
#else
 #define vg_mem_noaccess(p,n)
 #define vg_mem_undef(p,n)
 #define vg_mem_def(p,n)
#endif

#include "base.h"

enum Packed8 Rtype { Rnil,Rbuddy,Rxbuddy,Rslab,Rmmap };

#define Full 0xffffffffffffffff // 64 bits
#define Noclass 0xffff

struct st_region { // 5b
  void *user;
  ub8 *meta;  // metadata aka admin. separate block

  struct st_region *prv;
  struct st_region *nxt; // free slab/buddy chain

  struct st_region *bin; // recycled regions

  size_t len; // user len for mmap block, net cell len for slab
  size_t metalen;
  ub8 linmask;
  ub4 linofs;
  ub4 frecnt;
  ub4 cnt;
  ub4 alloccelcnt,freecelcnt;
  uint32_t smask;
  ub4 id;
  enum Rtype typ;
  ub4 cellen; // gross cel length for slab
  ub4 celcnt;

  ub4 ofs;

  ub2 clas;
  ub1 minorder; // buddy: granularity
  ub1 celord;  //   slab: cel len if pwr2
  ub1 cntord;
  ub1 maxorder; // buddy
  ub1 order; // region size = 1 << order
};
typedef struct st_region region;

// region directory
struct direntry {
  struct direntry *dir;
  region *reg;
};

struct binentry { // slab recycling bin
  void *p;
  region *reg;
};

struct binentry2 { // idem, buddy
  void *p;
  region *reg;
  size_t len;
};

// main thread heap base including starter kit
struct st_heap { // 4.5k

  // slabs
  ub1 ssizecount[16];
  ub1 sizecount[Maxtclass];

  ub2 len2tclas[Maxclasslen];
  ub2 tclas2len[Maxtclass];
  ub2 tclascnt;

  ub2 tclas2clas[Maxtclass];
  ub2 clas2len[Maxclass];
  ub2 clascnt;

  region *clasreg[Maxclass];

  // recycling bin
  ub2 binmasks[Maxclass]; // bit set for bin slot occupied
  struct binentry bins[Maxclass * Bin]; // 4 + 2 + 8 = 14b

  // buddy
  region *buddies[32 - Minorder]; // for each order
  ub4 buddycnt;
  ub4 buddyreg_f;

  struct binentry2 bins2[Maxorder * Bin];
  uint32_t buddymask;

  // region bases
  region *regmem;
  ub4 regmem_pos,regmem_top;
  ub4 allocregcnt,freeregcnt;
  region *freereg;
  region *nxtregs;

  // starter mem for dir pages
  struct direntry rootdir[Dir]; // 4+8b .4k

  struct direntry *dirmem; // initdir at start
  ub4 dirmem_len;
  ub4 dirmem_pos;
  ub4 dirmem_top;

  // boot mem
  ub4 inipos;
  char  *inimem;

  region *lastreg;
  void *lastptr;
  size_t lastlen;

  bool iniheap;

  // preserve state
  ub4 delcnt;
  ub4 baselen;

  ub4 id; // ident
};
typedef struct st_heap heap;

// per-thread heap base
// delcnt if bit 0 set
static _Thread_local heap *thread_heap = nil;

static _Atomic unsigned int global_mapcnt = 1;
static _Atomic unsigned int heap_gid;

static ub4 get_align(ub4 len)
{
  static ub1 aligns[16] = { 1,1,2,4,4,8,8,8,8,8,8,8,8,8,8,8 };

  if (len < 16) return aligns[len];
  else return Basealign;
}

static ub1 mapshifts[32] = {
  0,0,0,1,
  1,1,1,2,
  2,2,2,3,
  3,4,4,5,
  5,6,6,7,
  7,8,8,9,
  9,10,11,
  12,13,14,15 };

#include "os.h"

static int diag_fd = 2;

#include "printf.h"
#include "diag.h"

static void trimbin(heap *hb,bool full);

static void ytrim(void)
{
  heap *hb = thread_heap;

  if (hb) trimbin(hb,1);
}

// Get chunk of memory from the O.S. Trim heap if needed
static void *osmem(ub4 line,enum File file,heap *hb,size_t len,cchar *desc)
{
    void *p = osmmap(len);

    do_ylog(line,file,"heap %u",hb->id);
    ylog(Fyalloc,"osmem %zu`b for %s = %p",len,desc,p);
    if (p) return p;
    trimbin(hb,0);
    p = osmmap(len);
    if (p) return p;
    error(line,file,"heap %u oom for %zu`b",hb->id,len);
    return p;
}

static void osunmem(ub4 line,enum File file,heap *hb,void *p,size_t len,cchar *desc)
{
  do_ylog(line,file,"heap %u",hb->id);
  ylog(Fyalloc,"osunmem %zu`b for %s = %p",len,desc,p);
  osmunmap(p,len);
}

#include "heap.h"

#include "region.h"

#include "buddy.h"
#include "slab.h"

#include "alloc.h"
#include "free.h"
#include "realloc.h"

#include "std.h"

// --- optional ---

#ifdef Y_enable_boot_malloc
void * __je_bootstrap_malloc(size_t len)
{
  static ub1 bootmem[Bootmem];
  static _Atomic ub4 boot_pos;
  ub4 pos;

  if (len == 0) return bootmem + pos;

  len = align(len,Stdalign);
  pos = atomic_fetch_add(&boot_pos,len);
  atomic_fetch_and(&boot_pos,0xff);
   if (pos + len <= Bootheap) {
     return bootmem + pos;
  }
  return osmmap(len);
}

void __je_bootstrap_free(void *p) {} // no metadata or length

void *bootstrap_calloc(size_t num, size_t size)
{
  return __je_bootstrap_malloc(num * size); // bootmem is zeroed
}
#endif

#ifdef Y_enable_mallopt
int mallopt(int param, int value)
{
  switch (param) {
  case M_MMAP_THRESHOLD:
  default: break;
  }
  return 0;
}
#endif

#ifdef Y_enable_mallinfo
struct mallinfo mallinfo(void)
{
  static struct mallinfo mi;

  return &mi;
}

struct mallinfo2 mallinfo2(void)
{
  static struct mallinfo2 mi;

  return &mi;
}

int malloc_info(int options, void *stream)
{
  return 0;
}
#endif

#ifdef Y_enable_maltrim
int malloc_trim(size_t pad)
{
  ytrim();
  return 0;
}
#endif

#ifdef Y_enable_glibc_malloc_stats
void malloc_stats(void)
{
}
#endif

#if Yal_glibc_mtrace
void mtrace(void)
{
  ylog(Fyalloc,"region %zu`b heap %zu`b",sizeof(struct st_region),sizeof(struct st_heap));
}

void muntrace(void) {}
#endif

#ifdef Test

#include <stdio.h>

int main(int argc,char *argv[])
{
  ub4 i;
  ub8 n;

  for (i = 16; i < 32; i++) {
    n = admsiz(i);
    printf("bit %-3u  %lx %10luK %luM\n", i, n,n >>10,n >> 20);
  }
  for (i = 32; i < 48; i++) {
    n = admsiz(i);
    printf("bit %-3u  %lx %12luM %luG\n", i, n,n >>20,n >> 30);
  }
  for (i = 48; i < 60; i++) {
    n = admsiz(i);
    printf("bit %-3u  %6luT %12lx %12luP %luE\n", i, n,1ul << i,n >>40,n >> 50);
  }

  heap *h = newheap(0);
}
#endif
