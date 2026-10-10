/* TinyCC's configuration on Banana OS (ports/tcc/build.sh copies it in).
 * The compiler lives in Banana Code; its files are in /apps/code/tcc:
 *   include/          the SDK's headers
 *   x86_64/, i386/    libbanana.a (the SDK's C library) and libtcc1.a */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCC_STATIC 1          /* no dlopen */
#define CONFIG_TCC_SEMLOCK 0         /* one compile at a time */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_NO_NATIVE 1       /* no -run: programs are packaged and installed */
#define CONFIG_TCC_PREDEFS 1         /* tccdefs.h built in */
#ifdef __x86_64__
#define CONFIG_TCCDIR "/apps/code/tcc/x86_64"
#else
#define CONFIG_TCCDIR "/apps/code/tcc/i386"
#endif
#define CONFIG_TCC_SYSINCLUDEPATHS "/apps/code/tcc/include"
#define CONFIG_TCC_LIBPATHS "{B}"
#define CONFIG_TCC_CRTPREFIX "{B}"
#define CONFIG_TCC_ELFINTERP "-"
