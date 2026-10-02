/*
 * jconfig.h --- selects the platform-specific libjpeg-turbo configuration.
 *
 * libjpeg-turbo normally generates one jconfig.h per platform with CMake.  As
 * with the IJG libjpeg that this directory used to hold, the real content
 * lives in the jconfig.* siblings below and a single dispatcher selects it, so
 * that lime's build (which compiles every platform with -I.../jpeg/ and no
 * per-platform include directory) keeps working unchanged.
 */

#ifdef IPHONE
#include "jconfig.iphoneos"
#elif defined(ANDROID)
#include "jconfig.mac"
#elif defined(__APPLE__)
#include "jconfig.mac"
#elif defined(_WIN32)
#include "jconfig.vc"
#else
#include "jconfig.linux"
#endif
