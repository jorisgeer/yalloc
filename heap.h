/* heap.h - generic heap admin

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#undef Logfile
#define Logfile Fheap

static heap * _Atomic global_heaps;
static mheap * _Atomic global_mheaps;
static heap * _Atomic free_heaps;

static void at_exit(void)
{
  yal_mstats(nil,1,0,1,"atexit");
}

static void init_env(void)
{
  char c;
  cchar *envs;
  ub4 val = 0;

  envs = getenv(Yal_stats_envvar);
  if (envs == nil) return;

  while ( (c = *envs++) >= '0' && c <= '9') val = val * 10 + (ub4)c - '0';

  if (val & 1) atexit(at_exit);
  if (val & 2) diag_init();
}

static void heap_init(heap *hb)
{
  static_assert(Basealign2 >1,"Basealign2 > 1");
  static_assert(Basealign2 < 14,"Basealign2 < 14");
  static_assert(Bin > 2,"Bin > 2");
  static_assert(Clascnt < 65536,"Clascnt < 64K");
  static_assert(Mmap_initial_threshold < (1ul << (Maxorder - Addorder)),"mmap threashold < max buddy order");
  static_assert(Maxorder - Addorder < 32,"Maxorder - Addorder < 32");
  static_assert(Regions >= (1ul << sizeof(reg_t)),"region id  map exceeds type");

  // static_assert( (Vmbits - Page) > Dir,"VM directory too coarse");
  // static_assert( (Vmbits - Page) <= 4 * Dir,"VM directory too fine-grained");

  // static_assert( (Maxvm - Page) % Dir == 0,"partial directory");

  // static_assert(Dir > 1,"Dir > 1");
  // static_assert(Dir < 16,"Dir < 16");

  hb->prvallreg = &dummyreg;
  hb->mrufrereg = &dummyxreg;
  dummyreg.id = 123456789;
  hb->stats.minlen = hi64;
  hb->stats.loadr = hi64;
  hb->mmap_threshold = 1u << Mmap_initial_threshold;

  hb->regchks[0] = 1; // first region
  hb->regchkpos = 1;
  hb->xregchks[0] = 1;
  hb->xregchkpos = 1;

#if Yal_enable_bist
  bist_init(hb);
#endif
}

#if Yal_thread_model > 0 && ! defined Std_c11
  // #warning "Threading support requires C11"
  #undef Yal_thread_model
  #define Yal_thread_model 0
#endif

#if Yal_thread_model != 1
 #undef Yal_prep_TLS
 #define Yal_prep_TLS 0
#endif

#if Yal_thread_model > 0

static mheap *newmheap(void) // mini-heap
{
  static _Atomic unsigned int mheap_gid;
  mheap *mhb,*orghb;
  ub4 id;
  id = Atomad(mheap_gid,1);

  static_assert( (Bumplen & 15) == 0,"Bump mem aligns 16");
  static_assert(Bumplen / Stdalign < 65536,"Bump heap < 64K cells");
  static_assert(Bumpmax < Bumplen / 2,"Bump limit < len");

  mhb = osmmap(sizeof(mheap));
  if (mhb == nil) return nil;
  mhb->id = id;

  orghb = atomic_exchange(&global_mheaps,mhb);
  mhb->nxt = orghb;

  return mhb;
}

// create heap base for new thread
static heap *newheap(ub4 delcnt)
{
  void *vbase;
  size_t base;
  heap *hb,*orghb;
  ub4 len;
  ub4 id;
  ub4 hlen = sizeof(heap);
  ub4 rsiz = sizeof(region);
  ub4 xrsiz = sizeof(xregion);
  ub4 mlen = Iniregs * sizeof(size_t);
  ub4 rlen = Regmem_inc * rsiz;
  ub4 rxlen = Xregmem_inc * xrsiz;
  ub4 dlen = Dirmem_init * Dir3len * sizeof(reg_t);
  static _Atomic unsigned int heap_gid;

  static_assert(Basealign2 >1,"Basealign2 > 1");
  static_assert(Basealign2 < 14,"Basealign2 < 14");
  static_assert( (sizeof(xregion) & 7) == 0,"xregion size % 8");
  static_assert( (sizeof(region) & 7) == 0,"region size % 8");

  len = doalign(hlen + rlen + rxlen + dlen + mlen,128u);

  id = Atomad(heap_gid,1);

  ylog(Lnone,"new heap id %u hb %u regs %u dir %u = %u",id,hlen,rlen + rxlen,dlen,len);
  ylog(Lnone,"clasregs %u rootdir %u regbins %u",(ub4)sizeof(hb->clasregs),(ub4)sizeof(hb->rootdir),(ub4)sizeof(hb->regbins));

  vbase = osmmap(len);
  if (vbase == nil) return nil;
  base = (size_t)vbase;
  ylog(Lnone,"mmap for heap hb = %zx",base);
  hb = (heap *)base;

  base += hlen;
  ycheck(nil,Lnone,(base & 15),"xregmem align %zx hlen %x",base,hlen)
  hb->regs = (xregion **)base;
  base += mlen;
  hb->regmaplen = Iniregs;
  ycheck(nil,Lnone,(base & 15),"regmem align %zx hlen %x",base,hlen)
  hb->regmem = (region *)base;
  base += rlen;
  hb->xregmem = (xregion *)base;
  base += rxlen;

  hb->dirmem = (char *)base;
  hb->dirmem_top = dlen;
  hb->dirmem_pos = 0;
  ydbg(Lnone,"root dir %p",(void *)hb->rootdir)

  hb->delcnt = delcnt;
  hb->baselen = len;
  hb->id = id;

  heap_init(hb);

#if Yal_locking == 3 // c11 threads
  hb->boot = 1;
  mtx_init(&hb->oslock,mtx_timed);
  hb->boot = 0;
#elif Yal_locking == 2 // pthreads
  hb->oslock = PTHREAD_MUTEX_INITIALIZER;
#endif

  orghb = atomic_exchange(&global_heaps,hb);

  if (orghb == nil) { // first heap
    init_env();
  }
  hb->nxt = orghb;
  return hb;
}
#endif // Yal_thread_model > 0

#if Yal_thread_model == 1 // TLS

static _Thread_local heap *thread_heap;
static _Thread_local mheap *thread_mheap;

static inline heap *getheap(void)
{
  heap *hb;

  hb = thread_heap;
  return hb;
}

static heap *new_heap(void)
{
  heap *hb;

  hb = Atomget(free_heaps);
  if (hb) {
    atomic_exchange(&free_heaps,hb->free);
    heap_init(hb);
  } else hb = newheap(0);
  thread_heap = hb;
  return hb;
}

static mheap *getminiheap(bool new)
{
  mheap *mhb = thread_mheap;

  if (mhb || new == 0) return mhb;
   mhb = newmheap();
   thread_mheap = mhb;
   return mhb;
}

static void delheap(heap *hb)
{
  heap *orghb;

  thread_heap = nil;
  orghb = atomic_exchange(&free_heaps,hb);
  hb->free = orghb;
}

#if Yal_prep_TLS
static void __attribute__((constructor)) yal_before_TLS(void)
{
  ylog(Lnone,"prep TLS 0");
  yal_tls_inited = 0;
  thread_delcnt = 1; // on some platforms, e.g. apple, TLS is inited with malloc(). Trigger it before main
  thread_heap = nil;
  ylog(Lnone,"prep TLS 1");
  yal_tls_inited = 1;
}
#endif

#elif Yal_thread_model == 2 // pthreads + hash

 #include <pthread.h>

struct tidentry {
  struct st_heap *heap;
  _Atomic size_t tid;
  ub4 delcnt;
};

#include "map.h"

// per-thread heap base
static struct tidentry initab[1024];
static struct tidentry * _Atomic hashtabs[Hash_order] = { nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,initab };

static _Atomic uint32_t hash_ord = 10;

static heap *do_getheap(size_t tid,bool new)
{
  struct tidentry *tp,*newtp,*hp;
  heap *hb;
  ub4 ord,neword;
  ub4 cnt,lim,len;
  ub4 zero;
  ub4 iter;
  bool locked;
  static _Atomic ub4 resize_lock;

  ord = atomic_load(&hash_ord);

  tp = atomic_load(hashtabs + ord);

  cnt = 0;
  hp = map_getadd(tp,tid,ord,&cnt);
  if (likely(hp != nil)) {
    hb = hp->heap;
    if (likely(hb != nil)) {
      return hb;
    }
    if (new == 0) return nil;
    hb = hp->heap = newheap(hp->delcnt);
    if (hb == nil) return nil;
    hb->hash = hp;
  } else return nil;

  ylog(Lnone,"get heap %u tid %zx",hb->id,tid);
  len = 1U << ord;
  lim = min(len >> 2,64);
  if (likely(cnt < lim)) return hb;

  // resize ->  build new map at order + 1, leave orig untouched
  len *= 4 * sizeof(struct tidentry);
  neword = ord + 2;
  newtp = osmmap(len);
  if (newtp == nil) return nil;

  if (trylock(&hb->lock) && trylock2(&hb->lock)) {
    atomic_store(&hb->lock,0);
    return hb; // do not  rehash
  }

  ylog(Lnone,"tid %zx hash order %u resize",(size_t)tid,neword);
  atomic_store(hashtabs + neword,newtp);
  atomic_store(&hash_ord,neword);
  map_grow(tp,newtp,ord,neword);

  atomic_store(&hb->lock,0);

  return hb;
}

static struct st_dummyheap dummyheap = { (size_t)-1 };
static _Atomic size_t prvhb = (size_t)&dummyheap; // Remember last heap;

static Hot heap *getheap(void)
{
  size_t tid;
  heap *hb;

  tid = (size_t)pthread_self();

  hb = (heap *)atomic_load_explicit(&prvhb,memory_order_relaxed);
  if (likely(hb->tid == tid)) return hb;

  hb = do_getheap(tid,0);
  if (hb == nil) {
    return nil;
  }
  hb->tid = tid;

  Atomset(prvhb,(size_t)hb);
  return hb;
}

static heap *new_heap()
{
  hb = do_getheap(tid,1);
  return hb;
}

static void delheap(heap *hb)
{
}

#elif Yal_thread_model == 0 // no threading, static

#define delheap(hb,trim) // empty

static heap *newheap(ub4 delcnt)
{
  heap *base = global_heaps = &baseheap;

  base->iniheap = 1;

  base->regmem = iniregs;
  base->regmem_top = 4;

  ylog(Lnone,"static heap id %u",base->id);

  base->dirmem = inidirs;
  base->dirmem_top = Dirmem_init * Dir3len * sizeof(reg_t);
  base->dirmem_pos = 0;
  ylog(Lnone,"root dir %p",(void *)base->rootdir);

  heapinit(base);
  init_env();
  return base;
}

static heap *static_heap;

static heap *getheap(void)
{
  return static_heap;
}

static heap *new_heap(void)
{
  heap *hb = static_heap = newheap(0);
  return hb;
}

#else // Yal_thread_model

 #error "unknown threading model for config.h:Yal_thread_model"

#endif

static ub4 newregorder(void)
{
  uint32_t mapcnt;
  ub4 mapord=0,ord;
  ub4 shift = 0;

#if Yal_thread_model == 0
  mapcnt = global_mapcnt;
#else
  mapcnt = atomic_load_explicit(&global_mapcnt,memory_order_relaxed);
#endif

  if (mapcnt == 0 || mapcnt > (unsigned int)INT_MAX) ord = Minregion;
  else {
    mapord = 32u - clz(mapcnt);
    if (mapcnt & (mapcnt - 1)) mapord++;
    shift = mapshifts[min(mapord,31)];
    ord = Minregion + shift;
  }
  return ord;
}

static ub4 regstats(heap *hb,char *buf,ub4 pos,ub4 len,bool print,bool clear);

static size_t yal_mstats_heap(heap *hb,struct yal_stats *ret,bool print,bool global,unsigned char clear,cchar *desc)
{
  struct yal_stats *sp = &hb->stats;
  size_t errs,maxlen;
  size_t allocs,callocs,reallocles,reallocgts,reallocs;
  size_t frees;
  ub4 pos = 0,len = 0;
  char *buf = nil,*mmbuf = nil;
  char bckbuf[4096];
  char realbuf[64];

  if (print) {
    len = Yal_stats_buf;
    mmbuf = osmmap(len);
    if (mmbuf) buf = mmbuf;
    else {
      len = sizeof(bckbuf);
      buf = bckbuf;
    }
  }

  if (global == 0) pos = regstats(hb,buf,pos,len,print,clear);

  allocs = sp->allocs;
  callocs = sp->callocs;
  reallocles = sp->reallocles;
  reallocgts = sp->reallocgts;
  reallocs = reallocles + reallocgts;
  frees = sp->frees;

  maxlen = sp->maxlen;
  errs = sp->invalid_frees + sp->errors;

  if (ret) {
    memcpy(ret,sp,sizeof(struct yal_stats));
    ret->version = Version;
  }
  if (print) {
#if Yal_enable_stats
    if (reallocs) snprintf_mini(realbuf,0,64," realloc < %-6zu` > %-6zu`",reallocles,reallocgts);
    pos += snprintf_mini(buf,pos,len,"  alloc %zx` calloc %zu free %zx`%s size %zu - %zu",
      allocs,callocs,frees,reallocs ? realbuf : "",maxlen ? sp->minlen : 0,maxlen);
    pos += snprintf_mini(buf,pos,len,"  mmap %zx unmap %zu",sp->mmaps,sp->munmaps);
    pos += snprintf_mini(buf,pos,len,"  lock %zu oslock %zu timeout %zu\n",sp->locks,sp->oslocks,sp->oslocktimeouts);
#endif
    pos += snprintf_mini(buf,pos,len,"regions %u\n",sp->region_cnt);
    if (errs) pos += snprintf_mini(buf,pos,len,"  invalid-free %-4zu error %-3zu maxlen %-5zu` regions %s\n",sp->invalid_frees,sp->errors,maxlen,global ? "no" : "yes");
    ylog(Lnone,"--- yalloc %s stats for heap %u --- %s\n",Version,hb->id,desc);
    oswrite(Yal_log_fd,buf,pos);
  }

    if (clear) {
      memset(sp,0,sizeof(struct yal_stats));
      sp->minlen = Vmsize;
  }

  if (mmbuf) osmunmap(mmbuf,len);

  return errs;
}

size_t yal_mstats(struct yal_stats *sp,unsigned char print,unsigned char global,unsigned char clear,const char *desc)
{
  heap *hb = thread_heap;
  if (hb == nil) return 1;

  return yal_mstats_heap(hb,sp,print,global,clear,desc);
}
