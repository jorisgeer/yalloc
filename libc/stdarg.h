// stdarg.h - dummy interface for yalloc analysis

typedef void * va_list;

#define va_start(ap,fmt) (ap) = (va_list)(size_t)(fmt)
#define va_arg(ap,typ) (typ)(ap)
#define va_end(ap)
