/* printf.h - printf-style string formatting

   Miniature version of the printf(3) family of functions
   All C11 features are recognised and all except multibye / wide chars implemented
 */

extern unsigned int mini_snprintf(char *dst,unsigned int ofs,unsigned int len,const char *fmt,...) _Printf(4,5);
// unlike snprintf, starting at offset 'ofs', always null-terminating and returning the actual length

extern unsigned int mini_vsnprintf(char *dst,unsigned int pos,unsigned int dlen,const char *fmt,va_list ap);
