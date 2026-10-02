#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

/* Pre-generated from upstream include/ogg/config_types.h.in (libogg 1.3.5).
   Upstream fills the values in from configure/cmake (CheckSizes.cmake); the
   values below are the fixed-width types reported on every platform lime
   targets (MSVC x64, GNU/Clang, macOS/iOS, Android NDK, Emscripten). */

#define INCLUDE_INTTYPES_H 1
#define INCLUDE_STDINT_H 1
#define INCLUDE_SYS_TYPES_H 1

#if INCLUDE_INTTYPES_H
#  include <inttypes.h>
#endif
#if INCLUDE_STDINT_H
#  include <stdint.h>
#endif
#if INCLUDE_SYS_TYPES_H
#  include <sys/types.h>
#endif

typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
