// stdatomic.h - dummy interface for yalloc analysis

#if defined __clang__
#pragma clang diagnostic ignored "-Watomic-implicit-seq-cst"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"
#elif defined __gcc__
#endif

enum Memorder { memory_order_relaxed, memory_order_acquire,memory_order_release,memory_order_acq_rel };

static _Bool a_cas_u1(unsigned char *cmp,unsigned char *exp,unsigned char des)
{
  if (*cmp == *exp) { *cmp = des; return 1; } else { *exp = *cmp; return 0; }
}
static _Bool a_cas_u4(_Atomic unsigned int *cmp,unsigned int *exp,unsigned int des)
{
  if (*cmp == *exp) { *cmp = des; return 1; } else { *exp = *cmp; return 0; }
}
static _Bool a_cas_p(void *_Atomic *cmp,void **exp,void *des)
{
  if (*cmp == *exp) { *cmp = des; return 1; } else { *exp = *cmp; return 0; }
}
static _Bool a_cas_x(void *_Atomic *cmp,void **exp,void *des)
{
  if (*cmp == *exp) { *cmp = des; return 1; } else { *exp = *cmp; return 0; }
}

// todo work in progress
#define atomic_compare_exchange_weak_explicit(cmp,exp,des,suc,fail) _Generic( ( (cmp) , _Atomic unsigned char * : a_cas_u1, _Atomic unsigned int * : a_cas_u4, void * : a_cas_p, default: a_cas_x)(cmp,exp,des)
#define atomic_compare_exchange_strong_explicit(cmp,exp,des,suc,fail) _Generic( (cmp) , _Atomic unsigned char * : a_cas_u1, _Atomic unsigned int * : a_cas_u4, void * _Atomic * : a_cas_p, default: a_cas_x)((cmp),(exp),(des))

#define atomic_fetch_add_explicit(a,b,m) *(a) += (b)
#define atomic_fetch_sub_explicit(a,b,m) *(a) -= (b)

#define atomic_fetch_or_explicit(a,b,m) (a) |= (b)
#define atomic_fetch_and_explicit(a,b,m) (a) &= (b)

#define atomic_load_explicit(a,m) *(a)
#define atomic_store_explicit(a,b,m) *(a) = (b)

#define atomic_thread_fence(m)
