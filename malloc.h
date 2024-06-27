/* malloc.h - yalloc defines for semistandard extensions
*/

#ifdef __cplusplus
extern "C" {
#endif

extern int mallopt (int param, int value);

enum Yal_mallopt { M_MMAP_MAX = 1, M_MMAP_THRESHOLD, M_PERTURB, M_TOP_PAD, M_TRIM_THRESHOLD, M_ARENA_TEST, M_ARENA_MAX };

struct yal_stats {
  size_t allocs,callocs,mapallocs,reallocles,reallocgts;
  size_t frees,remotefrees,freeremotes,binned;
  size_t binallocs;

  size_t minlen,maxlen;
  size_t loadr,hiadr;

  size_t mmaps,munmaps;

  size_t invalid_frees;
  size_t invalid_reallocs;
  size_t errors;

  size_t findregions;
  size_t locks,oslocks,oslocktimeouts;

  unsigned int region_cnt,xregion_cnt,slab_cnt,mmap_cnt;
  const char *errormsg;
  const char *version;
};

// print and/or return statistics
extern size_t yal_mstats(struct yal_stats *sp,unsigned char print,unsigned char global,unsigned char clear,const char *desc);

enum Yal_options { Yal_log };
extern void yal_options(enum Yal_options opt,size_t arg);

extern void mtrace(void);
extern void muntrace(void);

// returns the size allocated to 'ptr' or 0 if not allocated
extern size_t malloc_usable_size(void * ptr);

// bump allocations from small preallocated pool
extern void *__je_bootstrap_malloc(size_t len);
extern void *__je_bootstrap_calloc(size_t num, size_t size);
extern void __je_bootstrap_free(void *p);

extern unsigned int bist_check(unsigned int id);

extern int test_region(unsigned int regno,size_t arg1,size_t arg2);

#ifdef __cplusplus
}
#endif
