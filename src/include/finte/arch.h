/* Compile-time architecture identity. */
#ifndef FINTE_ARCH_H
#define FINTE_ARCH_H

#if defined(__x86_64__) || defined(_M_X64)
#define FINTE_ARCH_NAME "x86_64"
#define FINTE_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define FINTE_ARCH_NAME "arm64"
#define FINTE_ARCH_ARM64 1
#else
#define FINTE_ARCH_NAME "unknown"
#endif

#endif /* FINTE_ARCH_H */
