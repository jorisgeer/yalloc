/* os.h - operating system bindings
*/

extern int osopen(const char *name);
extern void osclose(int fd);
extern ssize_t osread(int fd,char *buf,size_t len);
extern void oswrite(int fd,const char *buf,size_t len);

extern void *osmmap(size_t len);
extern int osmunmap(void *p,size_t len);
extern void *osmremap(void *p,size_t orglen,size_t newlen);
extern unsigned int ospagesize(void);
