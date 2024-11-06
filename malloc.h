/* malloc.h - yalloc defines for semistandard extensions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Export a set of functions and defines that are typically prtovided in a header with this name, e.g  glibc.
   In addition, our own extensions are here.
*/

#ifdef __cplusplus
extern "C" {
#endif

// statistics
struct yal_stats {

  unsigned int tag;
  unsigned int id;
  const char *errormsg; // latest error msg
  const char *version;

  // conditional stats - summable
  size_t allocs,Allocs,callocs,alloc0s,slaballocs,slabAllocs,mapallocs;
  size_t reallocles,reallocgts,mreallocles,mreallocgts;
  size_t miniallocs,bumpallocs;
  size_t frees,free0s,freenils,slabfrees,mapfrees,slabxfrees,xslabfrees,mapxfrees,xmapfrees,minifrees,bumpfrees;
  size_t bumpalbytes;
  size_t binallocs;

  size_t mmaps,munmaps;

  size_t findregions;
  size_t locks,clocks;
  size_t xfreebuf; // unconditional
  size_t xfreesum,xfreebatch,xfreebatch1,xfreedropped,rbinallocs,xbufbytes;

  size_t invalid_frees;
  size_t invalid_reallocs;
  size_t errors;

  size_t newregions,useregions,noregions,curnoregions; // unconditional
  size_t delregions,region_cnt,freeregion_cnt,delregion_cnt,noregion_cnt;
  size_t newmpregions,usempregions,delmpregions,nompregions,curnompregions;
  size_t xregion_cnt,slab_cnt,mmap_cnt;
  size_t trimregions[8];

  unsigned int newheaps,useheaps;
  size_t getheaps,nogetheaps,nogetheap0s;

  // stats - unsummable
  unsigned int minlen,maxlen;
  size_t minrelen,maxrelen;
  size_t mapminlen,mapmaxlen;
  size_t loadr,hiadr,lomapadr,himapadr;

  size_t  frecnt,fresiz,fremapsiz,inuse,inusecnt,inmapuse,inmapusecnt;
  size_t slabmem,mapmem;
  size_t xmaxbin;

  unsigned int minclass,maxclass;
};

// print and/or return statistics
enum Yal_stats_opts { Yal_stats_sum = 1, Yal_stats_detail = 2, Yal_stats_totals = 4, Yal_stats_state = 8, Yal_stats_print = 16, Yal_stats_cfg = 32  };

extern size_t yal_mstats(struct yal_stats *sp,unsigned int opts,unsigned int tag,const char *desc);

// diags and control
enum Yal_diags { Yal_diag_none, Yal_diag_dblfree, Yal_diag_oom,Yal_diag_ill,Yal_diag_count };
enum Yal_options { Yal_logmask, Yal_diag_enable, Yal_trace_enable };
extern unsigned int yal_options(enum Yal_options opt,size_t arg1,size_t arg2);

// provide callsite info
extern void * yal_alloc(size_t size,unsigned int tag);
extern void * yal_calloc(size_t size,unsigned int tag);
extern void yal_free(void *p,unsigned int tag);
extern void * yal_realloc(void *p,size_t oldsize,size_t newsize,unsigned int tag);
extern void *yal_aligned_alloc(size_t align, size_t len,unsigned int tag);
extern size_t yal_getsize(void *p,unsigned int tag);

#define Yal_sftag(file) (((file) << 16) | (__LINE__ & 0xffff)) // basic callsite identification

// bump allocation from small static pool. Compatible with jemalloc.
extern void *__je_bootstrap_malloc(size_t len);
extern void *__je_bootstrap_calloc(size_t num, size_t size);
extern void __je_bootstrap_free(void *p);


// Following for compatibility with glibc nonstandard extensions. Most are dummies.
extern void malloc_stats(void);
extern int mallopt (int param, int value);

enum Ymallopt { M_MMAP_MAX = 1, M_MMAP_THRESHOLD, M_PERTURB, M_TOP_PAD, M_TRIM_THRESHOLD, M_ARENA_TEST, M_ARENA_MAX };

struct mallinfo2 {
size_t arena;     /* Non-mmapped space allocated (bytes) */
  size_t ordblks;   /* Number of free chunks */
  size_t smblks;    /* Number of free fastbin blocks */
  size_t hblks;     /* Number of mmapped regions */
  size_t hblkhd;    /* Space allocated in mmapped regions (bytes) */
  size_t usmblks;   /* See below */
  size_t fsmblks;   /* Space in freed fastbin blocks (bytes) */
  size_t uordblks;  /* Total allocated space (bytes) */
  size_t fordblks;  /* Total free space (bytes) */
  size_t keepcost;  /* Top-most, releasable space (bytes) */
};

extern struct mallinfo2 *mallinfo2(void);
extern int malloc_info(int options, void *stream);

extern int malloc_trim(size_t pad);

extern void mtrace(void);
extern void muntrace(void);

// macos malloc_size
extern size_t malloc_usable_size(void *ptr);

// freebsd reallocf

#ifdef __cplusplus
}
#endif
