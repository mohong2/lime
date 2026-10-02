/* j16/jdmainct.c - libjpeg-turbo 3.0.4 src/jdmainct.c compiled at 16-bit
 * sample precision.  Upstream links these as the jpeg16-static object library
 * (CMakeLists.txt JPEG16_SOURCES with -DBITS_IN_JSAMPLE=16).  jconfig.h and
 * jconfigint.h only default BITS_IN_JSAMPLE when it is undefined, so the define
 * below is exactly equivalent to that compiler flag, and jsamplecomp.h renames the
 * precision-dependent symbols (jinit_* -> j16init_*) accordingly.
 */
#define BITS_IN_JSAMPLE 16
#include "../src/jdmainct.c"
