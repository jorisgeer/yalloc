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

#define Version "0.0.1"

#include <limits.h>

#include <stdarg.h>
#include <stddef.h> // size_t,max_align_t
#include <stdint.h> // SIZE_MAX
#include <string.h> // memset

extern void exit(int);
extern char *getenv(const char *name);
extern int atexit(void (*function)(void));

#include "stdlib.h"

#include "config_gen.h"

#include "config.h"

#if Yal_enable_trace
 #undef Yal_enable_stats
 #define Yal_enable_stats 1
#endif

#include "malloc.h"

#if Yal_enable_valgrind
 #include <valgrind/valgrind.h>
 #include <valgrind/memcheck.h>
 #define vg_mem_noaccess(p,n) VALGRIND_MAKE_MEM_NOACCESS((p),(n));
 #define vg_mem_undef(p,n) VALGRIND_MAKE_MEM_UNDEFINED((p),(n));
 #define vg_mem_def(p,n) VALGRIND_MAKE_MEM_DEFINED((p),(n));
 #define vg_mem_maydef(p,n,c) if (c) VALGRIND_MAKE_MEM_DEFINED((p),(n)); else VALGRIND_MAKE_MEM_UNDEFINED((p),(n));

static bool vg_mem_isaccess(void *p,size_t n) // accessible when not expected
{
  if (RUNNING_ON_VALGRIND == 0) return 0;

  return (VALGRIND_CHECK_MEM_IS_ADDRESSABLE(p,n) > p
  || VALGRIND_CHECK_MEM_IS_ADDRESSABLE( (char *) + n - 1,1) == 0);
}

static bool vg_mem_isnoaccess(void *p,size_t n) // inaccessible when not expected
{
  if (RUNNING_ON_VALGRIND == 0) return 0;

  return VALGRIND_CHECK_MEM_IS_ADDRESSABLE(p,n) != 0
}

#else
 #define vg_mem_noaccess(p,n)
 #define vg_mem_undef(p,n)
 #define vg_mem_def(p,n)
 #define vg_mem_maydef(p,n,c)
#endif

#include "base.h"
#include "yalloc.h"

enum Packed8 Rtype { Rnone,Rbuddy,Rslab,Rmmap,Rmmap_free };

#define Full hi64

#define Dir1len (1u << Dir1)
#define Dir3len (1u << Dir3)

#define Clascnt (Maxclass * Clasbits * 2 + 16)
#define Maxclasslen (1u << Maxclass)

#define Regionchunks (Regions / Regmem_inc)
#define Xregionchunks (Regions / Xregmem_inc)

#if defined Page_override && Page_override > 4 // from config.h
 #define Page Page_override
#else
 #define Page _Sys_Page // as determined by ./configure
#endif

#if Page >= 32
 #error "Page needs to be power of two of Pagesize"
#endif
#define Pagesize (1u << Page)

// -- end derived config --

typedef unsigned short reg_t;

#define Clasregs 32 // (Vmbits - Maxregion)

#include "os.h"
#include "printf.h"

// static unsigned int printf_dot = Printf_dot; // comnpatibility hack to get %'x and %'b

enum Packed8 Status { St_ok, St_oom,St_tmo,St_intr,St_error,St_nolock,St_trim };

#include "diag.h"

#include "lock.h"

#undef Logfile
#define Logfile Fyalloc
#ifndef Logfile
#endif

struct st_bregion; // bist

// thread heap base including starter kit. page-aligned
struct st_heap {

  _Atomic ylock_t lock;
  _Atomic int lockmode; // 0 - none  1 request  2 active

  // slabs
  // ub2 clascnts[Clascnt];

  // slab
  ub4 claslens[Clascnt];

  struct st_region *clasregs[Clascnt * Clasregs];
  ub2 claspos[Clascnt]; // currently used
  ub2 clastop[Clascnt];

  struct st_region *prvallreg;
  struct st_xregion *mrufrereg;

  // buddy
  struct st_region *buddies[32 - Minorder]; // for each order
  ub4 buddycnt;
  ub4 buddyreg_f;

  uint32_t buddymask;

  // region bases
  struct st_region *regmem;
  struct st_xregion *xregmem;
  ub4 regmem_pos;
  ub4 xregmem_pos;
  reg_t regchkpos,xregchkpos;
  reg_t regchks[Regionchunks];
  reg_t xregchks[Xregionchunks];

  ub4 allocregcnt,freeregcnt;
  _Atomic reg_t freeregs; // mrf list of freed regions
  _Atomic reg_t freexregs; // mrf list of freed xregions

  // page dir root
  reg_t ** rootdir[Dir1len];

  _Atomic size_t dirversion;

  struct st_xregion ** regs; // rid to reg
  ub4 regmaplen;

  // starter mem for dir pages
  char *dirmem;
  ub4 dirmem_pos;
  ub4 dirmem_top;

  // region bin
  ub2 regbinpos[Maxregion];
  reg_t regbins[Maxregion * Regbin];
  size_t regbinsum;

  struct st_heap *nxt; // chai of all heaps
  struct st_heap *free; // chain of free heaps
  struct st_heap *prvxheap; // mru remote

  void *hash;
  enum Status status;

  struct yal_stats stats;

  ub4 id; // ident
  size_t tid;

  size_t mmap_threshold;

  char errmsg[Diag_buf];
  ub4 errfln;

#if Yal_enable_bist
  struct st_bregion *bistregs;
  // struct bistentry *bist;
  size_t bist_all;
#endif

  Align(16) ub4 bistpos;
  ub4 bistprv;
  ub4 delcnt;
  ub4 baselen;
};
typedef struct st_heap heap;

struct st_mheap {
  ub4 pos;
  ub4 id;
  struct st_mheap *nxt;
  ub2 meta[Bumplen / Stdalign];
  Align(Stdalign) char mem[Bumplen];
};
typedef struct st_mheap mheap;

struct regstat {
#if Yal_enable_stats
  size_t allocs,callocs,reallocles,reallocgts,binallocs,preallocs,fastregs;
  size_t frees,binned;
  size_t remotefrees;
  size_t free2s;
#endif
  size_t invalidfrees;
  size_t locks,oslocks,oslocktmos;
  ub4 minlen,maxlen;
};

struct st_xregion { // mini region for mmap blocks
  void *user; // user aka client block
  ub8 *meta;  // used for alioc_align > pagesize
  size_t len; // rounded up mmap len

  enum Rtype typ;
  ub1 filler1;
  ub2 nxt; // next free
  ub2 dirid;
  ub2 filler2;
};
typedef struct st_xregion xregion;

// main region structure
struct st_region {
  void *user;
  ub8 *meta; // metadata aka admin. separate block
  size_t len; // main region len

  enum Rtype typ;
  ub1 filler1;
  ub2 nxt; // next free
  ub2 dirid;
  ub2 filler2;

// end xregion

  ub4 frecnt;
  ub4 cnt;

  size_t metacnt; // number of line entries

  bool hasrun,userun;
  enum Status status;
  ub4 errfln;

  // recycling bin
  ub2 binpos;

  ub4 preofs,hiofs; // line offset for below
  ub8 premsk; // preallocated run. bit set if free

  // ? struct st_region *regbin; // recycled regions

  uint32_t smask; // buddy
  ub4 id; // dirid + heap id
  ub4 uid; // unique for each creation

  ub4 ucellen; // user aka net cel length for slab
  ub4 cellen; // gross aka aligned cel len
  ub4 clen,clrlen; // length to clear
  ub4 celcnt;
  ub4 runcnt; // #complete runs

  ub2 clas;
  ub2 claspos;
  ub1 minorder; // buddy: granularity
  ub2 celord;  //   slab: cel len if pwr2
  ub1 cntord;
  ub1 maxorder; // buddy
  ub2 order; // region len = 1 << order

  ub4 apos,bpos,cpos,rpos,lpos;
  ub8 dmsk; // = accd[1]
  ub8 accc[64];

  struct regstat stats,accstats;

  Align(16) ub4 bin[Bin]; // cel within user block. checked for cel, not for unalloc / double free
};
typedef struct st_region region;

static region dummyreg;
static xregion dummyxreg;

static void *oom(heap *hb,ub4 fln,enum Loc loc,size_t n1,size_t n2)
{
  if (hb) { *hb->errmsg = 0; error2(hb->errmsg,loc,fln,"heap %u out of memory allocating %zu` * %zu`b",hb->id,n1,n2) }
  else do_ylog(__COUNTER__,loc,fln,Error,nil,"out of memory allocating %zu` * %zu`b",n1,n2);
  return nil;
}

static void free2(heap *hb,ub4 fln,enum Loc loc,ub4 rid,void *p,size_t len,cchar *msg)
{
  *hb->errmsg = 0;
  error2(hb->errmsg,loc,fln,"double free of ptr %p len %zu`b region %x %s ",p,len,rid,msg);
}

#if Yal_enable_bist
 #include "bist.h"
 #define Bist_add(h,reg,p,len,loc) if (bist_add(h,reg,p,len,loc)) { hb->errfln = Fln; hb->status = St_error; return nil; }
 #define Bist_del(h,reg,p,loc) if (bist_del(h,reg,p,loc)) { hb->errfln = Fln; hb->status = St_error; return nil; }
  struct bistentry {
    size_t p;
    size_t len;
  };

#else
 #define Bist_add(rv,h,r,p,n,l)
 #define Bist_del(rv,h,r,p,l)
ub4 bist_check(unsigned int id) { return id; }
#endif

#if Yal_inter_thread_free && Yal_locking > 0

static enum Status do_lock(heap *hb,enum Loc loc,region *reg,ub4 fln,ub4 tmo)
{
  enum Status rv = St_ok;
  ylock_t one = 1;
  ylock_t two = 2;
  ub4 iter = 50;
  ub4 intr;
  bool didcas;

  reg->stats.locks++;
  do {
    if (trylock2(&hb->lock) == 0) return St_ok;

    /* contended */
    didcas = Cas(hb->lock,one,2);
    reg->stats.oslocks++;
    intr = 10;
    do {
      rv = oslock(loc,reg->dirid,&hb->lock,2,tmo,hb->errmsg);
     } while (rv == St_intr && --intr);

    if (rv == St_tmo) {
      ylog2(loc,fln,"val %zu",(size_t)atomic_load(&hb->lock))
      break;
    } else if (rv == St_error) {
      break;
    }
  } while (--iter);
  if (iter == 0) rv = St_tmo;
  if (rv != St_ok) {
    if (didcas) Cas(hb->lock,two,1);
  }
  return rv;
}

static void do_unlock(heap *hb,region *reg,enum Loc loc,ub4 fln,cchar *desc)
{
  enum Status lockrv;
  ylock_t one = 1;
  ylock_t two = 2;

  // do_ylog(fln,Info,"< unlock for region %x.%zx %s val %zu",reg->id,tid,desc,(size_t)atomic_load(&reg->lock));
  if (Cas(hb->lock,two,0)) lockrv = osunlock(loc,reg->dirid,fln,&hb->lock,hb->errmsg);
  else if (Cas(hb->lock,one,0) == 0) lockrv = St_error;
  else lockrv = St_ok;
  if (lockrv != St_ok) {
    ylog2(loc,fln,"yalloc.c:%u region %x %s no os unlock (%zu)",__LINE__,reg->id,desc,(size_t)atomic_load(&hb->lock))
  } else {
    // ylock_t loc = atomic_load(&reg->lock);
    // if (loc) do_ylog(fln,Info,"reg %x lockvar %zu",reg->id,(size_t)loc);
  }
}

static _Atomic size_t glob_locs,glob_unlocs;

#define Lock(hb,reg,tmo,loc,fln,rv,desc,arg) \
\
  if (unlikely(trylock(&hb->lock) == 0)) { \
    enum Status lockrv; \
  \
    lockrv = do_lock(hb,loc,reg,fln,(tmo)); \
    if (lockrv == St_tmo) { \
      size_t locs = atomic_load(&glob_locs); \
      size_t unlocs = atomic_load(&glob_unlocs); \
      *hb->errmsg = 0; \
      errorctx(loc,hb->errmsg,"heap %u",hb->id); \
      error2(hb->errmsg,loc,fln,"region %x %s (%u) no os lock in %u us +%zu -%zu",reg->dirid,(desc),(arg),(tmo),locs,unlocs) \
      reg->stats.oslocktmos++; \
      hb->status = St_tmo; \
      return rv; \
    } \
  }

 #define Unlock(hb,reg,loc,fln,desc) \
\
  if (unlikely(Cas(hb->lock,one,0) == 0)) { \
    do_unlock(hb,reg,loc,fln,desc); \
  }

#else

 #define Lock(hb,reg,tmo,loc,fln,rv,msg,arg)
 #define Unlock(hb,reg,loc,fln,msg)

#endif // Yal_inter_thread_free

static _Atomic unsigned int global_mapcnt = 1;

#if Yal_prep_TLS
 static bool yal_tls_inited; // set by accessing TLS from a 'constructor' aka .ini section function before main()
#endif

static ub1 mapshifts[32] = {
  0,0,0,0,
  1,1,1,1,
  2,2,2,2,
  3,3,4,4,
  5,5,6,6,
  7,7,8,8,
  9,10,11,
  12,13,14,15 };

static char zeroblock; // malloc(0)

static bool osunmem(ub4 fln,heap *hb,void *p,size_t len,cchar *desc)
{
  ub4 hid = hb->id;

#if Yal_enable_log
  ylog2(Lnone,fln,"yalloc.c:%u heap %u osunmem %zu`b for %s = %p",__LINE__,hid,len,desc,p)
#endif
    hb->stats.munmaps++;
  if (osmunmap(p,len)) {
    error2(nil,Lnone,fln,"invalid munmap of %p for %s in heap %u - %m",p,desc,hid)
    return 1;
  }
  return 0;
}

#include "heap.h"

#undef Logfile
#define Logfile Fyalloc

static void ytrim(void)
{
  heap *hb = getheap();

  if (hb == nil) return;
}

// Get chunk of memory from the O.S. Trim heap if needed
static void *osmem(ub4 fln,heap *hb,size_t len,cchar *desc)
{
  void *p;

  if (len < 4096) { ylog2(Lnone,fln,"heap %u osmem len %u %s",hb->id,(ub4)len,desc) }

  p = osmmap(len);
  hb->stats.mmaps++;
  // do_ylog(line,file,"osmem %zu`b for %s = %p",len,desc,p);
  if (p) return p;
  p = osmmap(len);
  if (p) return p;
  *hb->errmsg = 0;
  errorctx(Lnone,hb->errmsg,"heap %u %s",hb->id,desc)
  oom(hb,fln,Lnone,len,1);
  return p;
}

#include "region.h"

#include "buddy.h"
#include "slab.h"

#include "alloc.h"
#include "free.h"
#include "realloc.h"

#if Yal_enable_boot_malloc || Yal_prep_TLS || Yal_locking == 3

#undef Logfile
#define Logfile Fyalloc

// bump allocator from canned pool
void *__je_bootstrap_malloc(size_t len)
{
  static ub1 bootmem[Bootmem];
  static _Atomic ub4 boot_pos;
  size_t alen;
  ub4 pos;

  alen = doalign(len,Stdalign);
  pos = atomic_fetch_add(&boot_pos,alen);

  ylog(Lalloc,"boot alloc %zu",len)
  if (len == 0) return bootmem + pos;

  atomic_fetch_and(&boot_pos,hi24);
   if (pos + alen <= Bootmem) {
     return bootmem + pos;
  }
  return osmmap(alen); // last resort when full
}

void *__je_bootstrap_calloc(size_t num, size_t size)
{
  return __je_bootstrap_malloc(num * size); // bootmem is zeroed
}

void __je_bootstrap_free(void *p) {} // no metadata or length

#endif

#include "std.h"

#undef Logfile
#define Logfile Fyalloc
#ifndef Logfile
#endif

// --- optional ---

#if Yal_enable_extensions

void yal_options(enum Yal_options opt,size_t arg)
{
  switch (opt) {
    case Yal_log: ylog_mask = (ub4)arg; break;
  }
}

#endif

size_t malloc_usable_size(void * ptr)
{
  return yalloc_getsize(ptr);
}

#if Yal_enable_mallopt

int mallopt(int param, int value)
{
  switch (param) {
  case M_MMAP_THRESHOLD:
  default: break;
  }
  return 0;
}
#endif

#if Yal_enable_mallinfo
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

#if Yal_enable_maltrim
int malloc_trim(size_t pad)
{
  ytrim();
  return 0;
}
#endif

#if Yal_enable_glibc_malloc_stats
void malloc_stats(void)
{
}
#endif

#if Yal_glibc_mtrace
void mtrace(void)
{
  ylog(Lnone,"region %zu`b heap %zu`b",sizeof(struct st_region),sizeof(struct st_heap))
}

void muntrace(void) {}
#endif
