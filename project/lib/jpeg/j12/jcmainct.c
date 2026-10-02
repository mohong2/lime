/* j12/jcmainct.c - libjpeg-turbo 3.0.4 src/jcmainct.c compiled at 12-bit
 * sample precision.  Upstream links these as the jpeg12-static object library
 * (CMakeLists.txt JPEG12_SOURCES with -DBITS_IN_JSAMPLE=12).  jconfig.h and
 * jconfigint.h only default BITS_IN_JSAMPLE when it is undefined, so the define
 * below is exactly equivalent to that compiler flag, and jsamplecomp.h renames the
 * precision-dependent symbols (jinit_* -> j12init_*) accordingly.
 */
#define BITS_IN_JSAMPLE 12
#include "../src/jcmainct.c"
