/* Apple Silicon (arm64) macOS configuration.
 *
 * It shares the x86_64 feature set except for the x86-only CPU feature probes
 * (SSE/SSE2/SSE3 and CPUID); the SSE mixer sources are not built for arm64. */
#include "config-macos-x86_64.h"

#undef HAVE_SSE
#undef HAVE_SSE2
#undef HAVE_SSE3
#undef HAVE_CPUID_H
#undef HAVE_GCC_GET_CPUID
