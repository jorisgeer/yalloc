/* os.h - operating system bindings
*/

extern int oswrite(int fd,const char *buf,size_t len);

extern void *osmmap(size_t len);
extern void *osmunmap(void *p,size_t len);
extern void *osmremap(void *p,size_t orglen,size_t newlen);
