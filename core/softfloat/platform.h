/* platform.h — Berkeley SoftFloat 3e's build configuration for dos-monster
 * (both hosts are little-endian 64-bit, with clang or GCC). Not upstream:
 * upstream keeps one of these per build directory. */
#define LITTLEENDIAN 1
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"
