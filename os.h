/* os.h - operating system bindings

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.
*/

struct osstat {
  unsigned long len;
  unsigned long mtime;
};

struct osrusage {
  unsigned long utime,stime;
  unsigned long maxrss;
  unsigned long minflt,maxflt;
  unsigned long volctx,ivolctx;
};

extern int osopen(const char *name,struct osstat *sp);
extern int oscreate(const char *name);
extern void osclose(int fd);
extern long osread(int fd,char *buf,size_t len);
extern unsigned int oswrite(int fd,const char *buf,size_t len,unsigned int fln);

extern void *osmmap(size_t len);
extern int osmunmap(void *p,size_t len);
extern void *osmremap(void *p,size_t orglen,size_t ulen,size_t newlen);
extern unsigned int ospagesize(void);
extern unsigned long ospid(void);

extern int osrusage(struct osrusage *usg);
