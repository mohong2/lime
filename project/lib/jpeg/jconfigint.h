/*
 * jconfigint.h --- libjpeg-turbo 3.0.4 internal build configuration.
 *
 * Upstream generates this file with CMake (configure_file(jconfigint.h.in)).
 * Only the Windows/MSVC result can be produced on this host, so the few
 * toolchain-dependent values are selected here with #ifdef and everything else
 * is copied verbatim from the generated file.  CMake cannot select this header
 * per platform because jinclude.h / jdhuff.h include it by name.
 */

/* libjpeg-turbo build number (3.0.4 tag commit date) */
#define BUILD  "20240914"

/* How to hide global symbols. */
#if defined(__GNUC__) || defined(__clang__)
#define HIDDEN  __attribute__((visibility("hidden")))
#else
#define HIDDEN
#endif

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#if defined(_MSC_VER)
#define INLINE  __forceinline
#elif defined(__clang__)
#define INLINE  inline __attribute__((always_inline))
#elif defined(__GNUC__)
#define INLINE  __inline__ __attribute__((always_inline))
#else
#define INLINE  inline
#endif

/* How to obtain thread-local storage */
#if defined(_MSC_VER)
#define THREAD_LOCAL  __declspec(thread)
#elif defined(__EMSCRIPTEN__)
#define THREAD_LOCAL
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL  __thread
#else
#define THREAD_LOCAL
#endif

/* Define to the full name of this package. */
#define PACKAGE_NAME  "libjpeg-turbo"

/* Version number of package */
#define VERSION  "3.0.4"

/* The size of `size_t', as computed by sizeof. */
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64) || \
    defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__) || \
    defined(__powerpc64__) || defined(__s390x__) || defined(__wasm64__) || \
    (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
#define SIZEOF_SIZE_T  8
#else
#define SIZEOF_SIZE_T  4
#endif

/* Define if your compiler has __builtin_ctzl() and sizeof(unsigned long) == sizeof(size_t). */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#define HAVE_BUILTIN_CTZL
#endif

/* Define to 1 if you have the <intrin.h> header file. */
#if defined(_MSC_VER)
#define HAVE_INTRIN_H
#endif

#if defined(_MSC_VER) && defined(HAVE_INTRIN_H)
#if (SIZEOF_SIZE_T == 8)
#define HAVE_BITSCANFORWARD64
#elif (SIZEOF_SIZE_T == 4)
#define HAVE_BITSCANFORWARD
#endif
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8

/* Support arithmetic encoding */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding */
#define D_ARITH_CODING_SUPPORTED 1

/* Use accelerated SIMD routines.
 *
 * Upstream CMake emits "#define WITH_SIMD 1" here when configured with
 * -DWITH_SIMD=1.  It is switched on only for the targets where the NASM
 * kernels are actually vendored and linked (libjpeg-turbo 3.0.4 ships x86
 * assembly only for x86/x64, and lib/jpeg/simd/*.lib is assembled from it for
 * the MSVC targets -- see Build.xml).  Everywhere else the scalar C code is
 * used exactly as before, so no other target loses its build. */
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
#define WITH_SIMD 1
#endif

#endif
