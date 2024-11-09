/* yalloc.c - yet another memory allocator

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  A 'heap' is the toplevel structure to hold all admin.
  Memory ranges are obtained from the OS as large power-of-two sized regions. Each region has separately mmap()ed user data and metadata.
  User blocks above a given size are mmap()ed directly, described by a virtual region.
  Initial regions are of a given size, subsequent regions may be larger dependent on overall usage.

  Regions are described by a region descriptor table, similar to how multi-level page tables describe virtual memory.
  A single top-level directory holds entries to mid-level tables.
  These in turn hold entries to leaf tables. The latter holds region pointers per OS memory page.
  free() and realloc() uses these to locate an entry, given the minimum region size. Valid pointers are guaranteed by leading to a region and being at a valid cell start.

  Within a region, user data is kept separate from admin aka metadata. This protects metadata from being overwriitten
  The user data is a single block, consisting of fixed-size cells. The metadata contains entries per cell.
  User blocks have no header or trailer. Consecutively allocated blocks are adjacent without a gap. This helps cache and TLB efficiency.
  Once a region becomes fully free, it is returned to the os.

  Blocks are aligned following 'weak alignment' as in https://www.open-std.org/JTC1/SC22/WG14/www/docs/n2293.htm
  Thus, small blocks follow the alignment of the largest type that fits in. 2=2 3=4 4=4 5=8 ...

  Freed blocks are held in a recycling bin aka freelist, genuinely freeing a LRU item. malloc() uses these on an MRU basis if available.
  In additon, each cell has a 'free' marker used to detect double free.

  Multiple threads are supported by giving each thread a full private heap. Yet these heaps are created on demand, and shared as long as no contention would happen.
  Otherwise a new heap is created.
  Synchronization is done by opportunistic 'try' locks using atomic compare-swap.

  If free / realloc cannot locate a block [in the local heap], a global region descriptor tabel is consulted. This table holds a cumulative region table and is updated atomically.
  Each region contains a local and remote freelist.
  A free or realloc from the same thread is taken from the local freelist without atomics (except double-free detect) or locking.
  Free or realloc from a different thread is handled by adding it to the owner region's remote freelist.
  This is handled by a set of atomic compare-swap. A subsequent alloc request will periodically inspect this list and movess it to the local freelist.
  If empty, the remote freelist is checked and a nonblocking 'trylock' is used to remove the entry.
  For realloc(), the size is obtained first. If a change is needed, a new block is allocated from the local heap, and the free of the original block is handled
  as with a free().

  Double-free detection is done using atomic compare-swap, to detect double or concurrent free / realloc in the presence of multiple threads.
  This is independent from the freelist binning described above, yet must be done before adding a block to a bin, as otherwise a subsequent alloc
  would hand out the same block twice.

  Regions are created on-demand when no existing region has space.  After a certain number of free() calls, a trimming scan runs to check for empty regions.
  These are first cleaned back to initial. In a next round and stull unused, they are moved to a region recycling state.
  New regions can reuse these. If not reused after a certain number of scans, the underying user memory is freed.
  */

// Note: after headers with declarations and macros only, all modules with function bodies are included as .h

#include "config_gen.h" // generated by configure.c

#include "config.h" // user config

#if Yal_signal
 #define _POSIX_C_SOURCE 199309L // needs to be at first system header
 #include <signal.h>
#endif

#include <stddef.h> // size_t
#include <limits.h> // UINT_MAX

#include <stdarg.h> // va_list (diag.h)
#include <string.h> // memset memcpy

extern char *getenv(const char *name); // no <stdlib.h> as its malloc() defines may not be compatible

#ifdef Yal_stats_envvar
  extern int atexit(void (*function)(void));
#endif

#include "stdlib.h" // our own

#if Yal_errno
 #include <errno.h>
 #define Enomem errno = ENOMEM;
 #define Einval errno = EINVAL;
#else
 #define Enomem // empty
 #define Einval
#endif

/* Support using Valgrind without replacing malloc as if replaced.
 * using vg client requests to emulate memcheck's checks
 * These calls add minimal overhead when not running in vg
 * typical usage: valgrind --tool=memcheck--soname-synonyms=somalloc=nouserintercept
 */
#if Yal_enable_valgrind
 #include <valgrind/valgrind.h>
 #include <valgrind/memcheck.h>
 #include <valgrind/drd.h>
 #define vg_mem_noaccess(p,n) VALGRIND_MAKE_MEM_NOACCESS( (char *)(p),(n));
 #define vg_mem_undef(p,n) VALGRIND_MAKE_MEM_UNDEFINED( (char *)(p),(n));
 #define vg_mem_def(p,n) VALGRIND_MAKE_MEM_DEFINED( (char *)(p),(n));
 #define vg_mem_name(p,n,d) VALGRIND_CREATE_BLOCK((p),(n),(d));

 #define vg_atom_before(adr) ANNOTATE_HAPPENS_BEFORE((adr));
 #define vg_atom_after(adr) ANNOTATE_HAPPENS_AFTER((adr));

 #define vg_drd_rwlock_init(p) ANNOTATE_RWLOCK_CREATE((p));
 #define vg_drd_wlock_acq(p) ANNOTATE_WRITERLOCK_ACQUIRED((p));
 #define vg_drd_wlock_rel(p) ANNOTATE_WRITERLOCK_RELEASED((p));

static size_t vg_mem_isaccess(void *p,size_t n) // accessible when not expected
{
  size_t adr;
  unsigned int tid;

  if (RUNNING_ON_VALGRIND == 0) return (size_t)p;
  tid = DRD_GET_DRD_THREADID;
  if (tid) return (size_t)p;
  adr = VALGRIND_CHECK_MEM_IS_ADDRESSABLE(p,n);
  return adr;
}

static size_t vg_mem_isdef(void *p,size_t n) // defined when not expected
{
  size_t adr = VALGRIND_CHECK_MEM_IS_DEFINED(p,n);
  return adr;
}

#else
 #define vg_mem_noaccess(p,n)
 #define vg_mem_undef(p,n)
 #define vg_mem_def(p,n)
 #define vg_mem_name(p,n,d)

 #define vg_mem_isaccess(p,n) (size_t)((p))
 #define vg_mem_isdef(p,n) 0

 // #define vg_atom_before(adr)
 // #define vg_atom_after(adr)

 #define vg_drd_rwlock_init(p)
 #define vg_drd_wlock_acq(p)
 #define vg_drd_wlock_rel(p)

#endif

#include "base.h"

#include "malloc.h" // nonstandard, common extensions

typedef struct yal_stats yalstats;

// todo posix only
extern Noret void _Exit(int status);

#include "yalloc.h"

#include "util.h"

#include "atom.h"

// -- start derived config --

#define Dir1len (1u << Dir1)
#define Dir2len (1u << Dir2)
#define Dir3len (1u << Dir3)

#define Xclascnt (32 * 4)

#define Regorder 36

static const unsigned long mmap_max_limit = (1ul << min(Mmap_max_threshold,Hi30));
static const unsigned long mmap_limit = (1ul << Mmap_threshold);

#if defined Page_override && Page_override > 4 // from config.h
 #define Page Page_override
#else
 #define Page Sys_page // as determined by ./configure
#endif

#if Page >= 32
 #error "Page needs to be power of two of Pagesize"
#endif

#define Pagesize (1u << Page)
static const ub4 Pagesize1 = Pagesize - 1;

#define Stdalign1 (Stdalign - 1)

#if Page > 16
  #undef Minilen
  #undef Bumplen
  #define Minilen 0
  #define Bumplen 0
#endif

#define Clasregs 32 // ~Vmbits - Maxregion, ub4 clasmsk

#if Yal_enable_tag
  #define Tagargt(t) ,ub4 t
  #define Tagarg(t) ,t
#else
  #define Tagargt(t)
  #define Tagarg(t)
#endif

// -- end derived config --

// local config

#define Remhid 64


#ifdef Inc_os
  #include "os.c"
#else
  #include "os.h"
#endif

#include "printf.h"

// -- diagnostics --

static int newlogfile(cchar *name[],cchar *suffix,ub4 id,unsigned long pid)
{
  char fname[256];
  int fd;

  snprintf_mini(fname,0,255,"%.32s%.32s-%u-%lu%.32s",name[0] ? name[0] : "",suffix,id,pid,name[1] ? name[1] : "");
  fd = oscreate(fname);
  if (fd == -1) fd = 2;
  return fd;
}

enum File { Falloc,Fatom,Fbist,Fboot,Fbump,Fdbg,Fdiag,Ffree,Fheap,Fmini,Frealloc,Fregion,Fsize,Fslab,Fstat,Fstd,Fyalloc,Fcount };
static cchar * const filenames[Fcount] = {
  "alloc.h","atom","bist.h","boot.h","bump.h","dbg.h","diag.h","free.h","heap.h","mini.h","realloc.h","region.h","size","slab.h","stats.h","std.h","yalloc.c"
};

#define Trcnames 256

static cchar * trcnames[Trcnames] ;

enum Loglvl { Fatal,Assert,Error,Warn,Info,Trace,Vrb,Debug,Nolvl };
static cchar * const lvlnames[Nolvl + 1] = { "Fatal","Assert","Error","Warn","Info","Trace","Vrb","Debug"," " };

enum Loc { Lnone,
  Lreal = 1,Lfree = 2,Lsize = 3,Lalloc = 4,Lallocal = 5,Lcalloc = 6,Lstats = 7,Ltest = 8,Lsig = 9,
  Lmask = 15,Lremote = 16,
  Lrreal = 1 + 16, Lrfree = 2 + 16,
  Lrsize = 3 + 16 };
static cchar * const locnames[Lmask + 1] = { " ","realloc","free","size","malloc","allocal","calloc","stats","test","signal","?","?","?","?","?","?" };

static _Atomic ub4 g_errcnt;
static _Atomic ub4 g_msgcnt;

static _Atomic unsigned long global_pid;

static char global_cmdline[256];

static Cold ub4 diagfln(char *buf,ub4 pos,ub4 len,ub4 fln)
{
  char fbuf[64];
  ub4 fn = fln >> 16;
  ub4 ln = fln & Hi16;
  cchar *fnam = nil,*pfx = "yal/";

  if (fn < Fcount) fnam = filenames[fn];
  else if (fn < Fcount + Trcnames) {
    fnam = trcnames[fn - Fcount];
    pfx = "";
  }
  if (fnam) snprintf_mini(fbuf,0,64,"%s%.8s:%-4u",pfx,fnam,ln);
  else snprintf_mini(fbuf,0,64,"%s(%u):%-4u",pfx,fn,ln);
  pos += snprintf_mini(buf,pos,len,"%18s ",fbuf);
  return pos;
}

// simple diag, see diag.h for elaborate
static Printf(5,6) ub4 minidiag(ub4 fln,enum Loc loc,enum Loglvl lvl,ub4 id,char *fmt,...)
{
  va_list ap;
  char buf[256];
  ub4 cnt,pos = 0,len = 254;
  cchar *lvlnam = lvl < Nolvl ? lvlnames[lvl] : "?";
  cchar *locnam = locnames[loc & Lmask];
  unsigned long pid = Atomget(global_pid,Monone);
  int fd,fd2;

  if (lvl > Yal_log_level) return 0;

  cnt = Atomad(g_msgcnt,1,Moacqrel);
  if (cnt == 0) buf[pos++] = '\n';

  if (*fmt == '\n')  buf[pos++] = *fmt++;
  pos = diagfln(buf,pos,len,fln);

  pos += snprintf_mini(buf,pos,len,"%-4u %-5lu %-4u %-3u %c %-8s ",cnt,pid,id,0,*lvlnam,locnam);

  va_start(ap,fmt);
  pos += mini_vsnprintf(buf,pos,len,fmt,ap);
  va_end(ap);

  if (pos < 255) buf[pos++] = '\n';

  if (lvl > Error) {
    fd = Yal_log_fd;
    if (fd == -1) fd = Yal_log_fd = newlogfile(Yal_log_file,"",0,pid);
    fd2 = fd;
  } else {
    fd = Yal_err_fd;
    if (fd == -1) fd = Yal_err_fd = newlogfile(Yal_err_file,"",0,pid);
    fd2 = Yal_Err_fd;
  }
  oswrite(fd,buf,pos,__LINE__);
  if (fd2 != -1 && fd2 != fd) oswrite(fd2,buf,pos,__LINE__);
  if (loc == Lsig) {
    return pos;
  }
  if (lvl < Warn) _Exit(1);
  return pos;
}

// -- main admin structures --

enum Rtype { Rnone,Rslab,Rbump,Rmini,Rmmap,Rcount };
static cchar * const regnames[Rcount + 1] = { "none","slab","bump","mini","mmap", "?" };

enum Status { St_ok, St_oom,St_tmo,St_intr,St_error,St_free2,St_nolock,St_trim };

typedef unsigned char celset_t;

// per-region statistics
struct regstat {
  size_t allocs,Allocs,callocs,reallocles,reallocgts,binallocs,iniallocs,xallocs;
  size_t frees,rfrees;
  ub4 minlen,maxlen;
  size_t rbin;
  size_t invalidfrees;
  ub4 aligns[32];
};

struct Align(16) st_xregion { // base, only type

  // + common
  size_t user; // user aka client block
  size_t len; // gross client block len as allocated

  struct st_heap *hb;

  _Atomic ub4 lock;
  enum Rtype typ;

  ub4 hid;
  ub4 id;

  // - common
  Ub8  filler;
};
typedef struct st_xregion xregion;

struct Align(16) st_mpregion { // mmap region. allocated as pool from heap

  // + common
  size_t user; // user aka client block
  size_t len; // gross client block len as allocated

  struct st_heap *hb;

  _Atomic ub4 lock;
  enum Rtype typ;

  ub4 hid;
  ub4 id;

  // - common

  _Atomic ub4 set; // 0 never used 1 alloc 2 free

  ub4 clr;

  size_t ulen;  // net len
  size_t align;  // offset if alioc_align > pagesize
  ub4 order;
  ub4 gen;
  size_t prvlen;

  struct st_mpregion *nxt; // for ageing and stats
  struct st_mpregion *frenxt,*freprv; // for reuse aka regbin
  _Atomic ub4 age;
  ub4 aged;
};
typedef struct st_mpregion mpregion;

struct Align(16) st_bregion { // bump region. statically present in heap

  // + common
  size_t user; // user aka client block
  size_t len; // gross client block len as allocated

  struct st_heap *hb;

  _Atomic ub4 lock;
  enum Rtype typ;

  ub4 hid;
  ub4 id;

  // - common

  ub4 *meta;  // fmetadata aka admin
  ub4 metalen;  // metadata aka admin size

  // in ub4
  ub4 freorg;
  ub4 tagorg;

  ub4 filler;

  ub8 uid;

  ub4 pos;
  ub4 cnt;

  // stats
  ub4 allocs;
  _Atomic ub4 frees;
  ub4 albytes,frebytes;
};
typedef struct st_bregion bregion;

struct Align(16) st_region { // slab region. allocated as pool from heap

  // + common
  size_t user; // user aka client block
  size_t len; // client block len

  struct st_heap *hb;

  _Atomic ub4 lock;
  enum Rtype typ;

  ub4 hid;
  ub4 id;

  // - common

  ub4 * meta;  // metadata aka admin
  size_t metalen;  // metadata aka admin size

  ub4 cellen; // gross and aligned cel len

  ub4 inipos; // never-allocated marker
  ub4 clas;

  ub4 celcnt;

  ub8 uid; // unique for each creation or reuse

  struct st_region *nxt; // for ageing and stats
  struct st_region *frenxt,*freprv; // for reuse aka regbin

  ub4 claspos;
  ub4 clr; // set if calloc needs to clear

  // bin
  ub4 binpos;

  ub4 claseq;
  ub4 celord;  //   cel len if pwr2 0 if not
  ub4 cntord;
  ub4 order; // region len = 1 << order

  ub4 age;

  size_t binorg; // offset in meta
  size_t lenorg;
  size_t tagorg;
  size_t flnorg;

  // remote bin
  ub4 * _Atomic rembin; // allocated on demand by sender from sender's heapmem
  _Atomic ub4 remref;

  ub4 rbinpos;
  ub4 rbinlen,rbininc;

  ub4 aged;
  ub4 inuse;

  size_t prvlen,prvmetalen;

  ub4 gen;
  ub4 fln;

  struct regstat stat;

  size_t metautop; // as required
};
typedef struct st_region region;

// local buffering for remote free
struct remote {
  region *reg;
  ub8 uid;
  ub4 *bin;
  ub4 pos,cnt,inc;
  ub4 celcnt;
};

struct rembuf {
  struct remote *rem;
  Ub8 clas[Clascnt / 64 + 1];
  Ub8 seq[Clascnt];
};

// thread heap base including starter kit. page-aligned
struct Align(16) st_heap {
  _Atomic ub4 lock;
  ub4 id; // ident

  char l1fill[L1line - 8];

  // slab allocator
  ub4 clascnts[Xclascnt]; // track popularity of sizes
  ub4 claslens[Xclascnt]; // size covered

  ub2 claspos[Clascnt]; // currently used
  Ub8 clasmsk[Clascnt]; // bit mask for clasregs having space
  Ub8 cfremsk[Xclascnt]; // bit mask for empty clasregs

  ub2 clasregcnt[Clascnt]; // #regions per class

  struct st_region *clasregs[Clascnt * Clasregs];

  struct st_region *smalclas[Clascnt];

  // region bases
  struct st_region *regmem;
  struct st_mpregion *xregmem;
  ub4 regmem_pos;
  ub4 xregmem_pos;

  // mrf list of freed regions, per order
  struct st_region * freeregs[Regorder + 1];
  struct st_mpregion* freempregs[Vmbits - Mmap_threshold + 1];
  struct st_mpregion* freemp0regs;

  struct st_heap *nxt; // list for reassign

  // page dir root
  struct st_xregion *** rootdir[Dir1len];

  // starter mem for dir pages
  struct st_xregion ***dirmem;
  struct st_xregion **leafdirmem;
  ub4 dirmem_pos,ldirmem_pos;
  ub4 dirmem_top,ldirmem_top;

  // region lists
  struct st_region *reglst,*regprv,*regtrim;// todo prv for stats ?
  struct st_mpregion *mpreglst,*mpregprv,*mpregtrim;

  // remote free (slab)
  struct rembuf *rembufs[Remhid];
  Ub8 remask;

  ub4 *rbinmem;  // mempool for rembins
  ub4 rbmempos,rbmemlen;

  ub4 trimcnt;
  _Atomic ub4 locfln;

  struct yal_stats stat;

  ub4 rmeminc;

  char filler[12];

  // bump allocator
  struct st_bregion bumpregs[Bumpregions];
};
typedef struct st_heap heap;

struct hdstats {
  ub4 newheaps,useheaps;
  size_t getheaps,nogetheaps,nogetheap0s;
  ub4 nolink;
  ub4 filler;
  size_t xfreebatch;
  size_t alloc0s,free0s,freenils;
  size_t xminifrees;
  size_t invalid_frees,errors;
  size_t xmapfrees;
  size_t delregions,munmaps;
};

#if Yal_thread_exit // install thread exit handler to recycle heap descriptor
  #include <pthread.h>
  #include "thread.h"

#else
  #define Thread_clean_info int
#endif

#define Miniord 16

struct st_heapdesc {
  struct st_heapdesc *nxt,*frenxt;
  struct st_heap *hb;
  struct st_bregion *mhb;

  char *errbuf;

  ub4 errfln;
  ub4 id;

  enum Status status;
  _Atomic ub4 lock;
  ub4 locked;
  ub4 trace;

  size_t getheaps,nogetheaps;

  struct hdstats stat;

  ub1 minicnts[Miniord - 4];
  ub4 minidir;

#if Yal_enable_stack
  ub4 flnstack[16];
  ub4 flnpos;
//  ub4 tag;
#endif

  Thread_clean_info thread_clean_info;
};
typedef struct st_heapdesc heapdesc;

static ub4 global_stats_opt; // set from Yal_stats_envvar
static ub4 global_trace = Yal_trace_default;
static ub4 global_check = Yal_check_default;

static heapdesc * _Atomic global_freehds;

#if Yal_thread_exit // install thread exit handler to recycle heap descriptor
  // add hd to head of free list
  static void thread_cleaner(void *arg)
  {
     heapdesc *hd = (heapdesc *)arg;
     heapdesc *prv = Atomget(global_freehds,Moacq);

     // minidiag( (Fyalloc << 16) | __LINE__,Lnone,Info,0,"thread %u exit ",hd->id);

     hd->frenxt = prv;
     Cas(global_freehds,prv,hd);
  }

  static void thread_setclean(heapdesc *hd)
  {
     Thread_clean_push(&hd->thread_clean_info,thread_cleaner, hd);
  }
#else
  static void thread_setclean(heapdesc *hd) { hd->thread_clean_info = 0; }
#endif

static size_t Align(16) zeroarea[16];
static size_t *zeroblock = zeroarea + 4; // malloc(0)

#include "boot.h"

// -- global heap structures and access

static _Thread_local struct st_heapdesc *thread_heap;

static struct st_heapdesc * _Atomic global_heapdescs;
static struct st_heap * _Atomic global_heaps;

static _Atomic ub4 global_tid = 1;
static _Atomic ub4 global_hid = 1;

static Hot heapdesc *getheapdesc(enum Loc loc)
{
  heapdesc *org,*hd = thread_heap;
  ub4 id,iter;
  ub4 fln;
  ub4 len = sizeof(struct st_heapdesc);
  bool didcas;

  if (likely(hd != nil)) return hd;

  id = Atomad(global_tid,1,Moacqrel);

  if (id == 1) init_env();

  fln = Fyalloc << 16;
  minidiag(fln|__LINE__,loc,Debug,id,"new base heap size %u.%u",len,(ub4)sizeof(struct hdstats));
  len = doalign4(len,L1line);

  // reuse ?
  hd = Atomget(global_freehds,Moacq);
  if (hd) {
    org = hd->frenxt;
    didcas = Cas(global_freehds,hd,org);
    if (didcas) {
      thread_heap = hd;
      thread_setclean(hd);
      hd->hb = nil;
      return hd;
    }
  }

  // new
  hd = bootalloc(fln|__LINE__,id,Lnone,len);
  if (unlikely(hd == nil)) {
    minidiag(fln|__LINE__,loc,Fatal,id,"cannot allocate heap descriptor %u",id);
    _Exit(1);
  }

  thread_heap = hd;
  thread_setclean(hd);

  iter = 10;

  // create list of all heapdescs for stats
  do {
    org = hd->nxt = Atomget(global_heapdescs,Moacq);
    didcas = Cas(global_heapdescs,org,hd);
  } while (didcas == 0 && --iter);

  if (didcas == 0) hd->stat.nolink++; // not essential

  hd->id = id;
  hd->trace = global_trace;
  return hd;
}

static cchar *regname(xregion *reg)
{
  enum Rtype typ = reg->typ;
  return typ <= Rcount ? regnames[typ] : "??";
}

#include "dbg.h"

#include "diag.h" // main diags

// -- main heap init and access --

static _Atomic unsigned int global_mapadd;
static _Atomic unsigned int global_mapdel;

#if Yal_prep_TLS
 static bool yal_tls_inited; // set by accessing TLS from a 'constructor' aka .ini section function before main()
#endif

static ub1 mapshifts[24] = { // progressively increase region sizes the more we have
  0,0,0,0,
  0,0,1,1, // 8
  1,1,2,2,
  2,2,3,4, // 16
  5,6,7,8,
  10,12,14,14 };

#include "heap.h"

#define Logfile Fyalloc

static void *oom(ub4 fln,enum Loc loc,size_t n1,size_t n2)
{
  char buf[64];
  heapdesc *hd = thread_heap;

  if (n2) snprintf_mini(buf,0,64," * %zu`",n2);
  else *buf = 0;

  do_ylog(Yal_diag_oom,loc,fln,Error,0,"heap %u out of memory allocating %zu`%s",hd ? hd->id : 0,n1,buf);
  Enomem
  return nil;
}

static ub4 free2(ub4 fln,enum Loc loc,xregion *reg,size_t ip,size_t len,ub4 tag,cchar *msg)
{
  do_ylog(Yal_diag_dblfree,loc,fln,Error,1,"double free of ptr %zx len %zu` %s region %u.%u fretag %.01u %s ",ip,len,regnames[reg->typ],reg->hid,reg->id,tag,msg);
  return 0;
}

// Get chunk of memory from the O.S.
static void *osmem(ub4 fln,ub4 hid,size_t len,cchar *desc)
{
  void *p;

  // if (len <= Pagesize) do_ylog(Diagcode,Lnone,fln,len < Pagesize ? Warn : Info,0,"heap %u osmem len %u %s",hid,(ub4)len,desc);

  p = osmmap(len);
  // ydbg1(fln,Lnone,"hid %-2u osmem %-6zu` = %-7zx %s",hid,len,(size_t)p,desc)
  if (p) {
    Atomad(global_mapadd,1,Monone);
    return p;
  }

  errorctx(fln,Lnone,"heap %u %s",hid,desc)
  oom(Fln,Lnone,len,0);
  return p;
}

static bool osunmem(ub4 fln,heapdesc *hd,void *p,size_t len,cchar *desc)
{
  ydbg3(Lnone,"heap %u osunmem %zu` for %s at %u = %p",hd->id,len,desc,fln & Hi16,p)
  hd->stat.munmaps++;
  if (osmunmap(p,len)) {
    error2(Lnone,fln,"invalid munmap of %p for %s in heap %u - %m",p,desc,hd->id)
    return 1;
  }
  Atomad(global_mapdel,1,Monone);
  return 0;
}

#undef Logfile

static ub4 slabstats(region *reg,struct yal_stats *sp,char *buf,ub4 pos,ub4 len,bool print,ub4 opts,ub4 cnt);
#include "region.h"

#define Nolen (size_t)(-1)

#include "bump.h"
#include "mini.h"

#include "slab.h"

#include "size.h"
#include "free.h"

#include "alloc.h"
#include "realloc.h"

#include "stats.h"

#include "std.h"

#define Logfile Fyalloc

// -- nonstandard extensions
void *__je_bootstrap_malloc(size_t len)
{
  if (len >= Hi32) return nil;
  return bootalloc(Fln,0,Lnone,(ub4)len);
}

void *__je_bootstrap_calloc(size_t num, size_t size)
{
  if (size >= Hi32 || num >= Hi32) return nil;
  return __je_bootstrap_malloc(num * size); // bootmem is zeroed
}

void __je_bootstrap_free(void Unused * p) { } // no metadata or length

// --- optional ---

#if Yal_enable_extensions

void * yal_alloc(size_t size,unsigned int tag)
{
  return ymalloc(size,tag + (Fcount << 16));
}

void * yal_calloc(size_t size,unsigned int tag)
{
  return yalloc(size,size,Lcalloc,tag + (Fcount << 16));
}

void yal_free(void *p,unsigned int tag)
{
  yfree(p,0,tag + (Fcount << 16));
}

void * yal_realloc(void *p,size_t oldsize,size_t newsize,unsigned int tag)
{
  return yrealloc(p,oldsize,newsize,tag + (Fcount << 16));
}

void *yal_aligned_alloc(size_t align, size_t len,ub4 tag)
{
  return yalloc_align(align,len,tag + (Fcount << 16));
}

size_t yal_getsize(void *p,unsigned int tag)
{
  return ysize(p,tag + (Fcount << 16));
}

ub4 yal_options(enum Yal_options opt,size_t arg1,size_t arg2)
{
  ub4 rv;
  ub4 a1 = (ub4)arg1;

  switch (opt) {
    case Yal_diag_enable: return diag_enable(arg1,(ub4)arg2);

    case Yal_trace_enable: return trace_enable(a1);
    case Yal_trace_name:
       if (arg2 <= Pagesize || arg2 >= Vmsize) { do_ylog(Yal_diag_ill,Lnone,Fln,Warn,0,"unknown option '%d'",opt); return __LINE__; }
       return trace_name(a1,(char *)arg2);

    case Yal_logmask: rv = ylog_mask; ylog_mask = a1; return rv;
    default: do_ylog(Yal_diag_ill,Lnone,Fln,Warn,0,"unknown option '%d'",opt); return __LINE__;
  }
}

#endif // extensions

size_t malloc_usable_size(void * ptr)
{
  return ysize(ptr,Fln);
}

#if defined __APPLE__ && defined __MACH__

extern size_t malloc_size(const void * ptr); // todo malloc.h
size_t malloc_size(const void * ptr)
{
  return ysize((void *)ptr,Fln);
}
#endif

#if Yal_mallopt

int mallopt(int param, int value)
{
  switch (param) {
  case M_MMAP_THRESHOLD:
  default: break;
  }
  return 0;
}
#endif

#if Yal_mallinfo

struct mallinfo2 *mallinfo2(void)
{
  static struct mallinfo2 mi;

  return &mi;
}

int malloc_info(int options, void *stream)
{
  return 0;
}
#endif

#if Yal_malloc_stats

void malloc_stats(void)
{
  yal_mstats(nil,1,0,"malloc_stats");
}
#endif

#if Yal_glibc_mtrace
void mtrace(void)
{
  ydbg1(Fln,Lnone,"region %zu` heap %zu`",sizeof(struct st_region),sizeof(struct st_heap))
  global_trace = 1;
}

void muntrace(void) {   global_trace = 0; }
#endif
