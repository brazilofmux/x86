/* platform.h — Berkeley SoftFloat 3e's build configuration for dos-monster
 * (every host is little-endian 64-bit: clang or GCC, or MSVC on Windows,
 * which has neither __int128 nor the builtins and takes SoftFloat's
 * portable paths). Not upstream: upstream keeps one of these per build
 * directory. */
#define LITTLEENDIAN 1
#if defined(_MSC_VER) && !defined(__clang__)
#define INLINE static __inline
#elif defined(__GNUC_STDC_INLINE__)
#define INLINE inline
#else
#define INLINE extern inline
#endif
#if !defined(_MSC_VER) || defined(__clang__)
#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"
#endif
