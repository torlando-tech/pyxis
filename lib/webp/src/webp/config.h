/* Pyxis vendored stub: minimal config for the decode-only libwebp build.
 * Upstream generates this file via autotools. With HAVE_CONFIG_H defined the
 * decode translation units include this file, which pins the pure-C decoder
 * (no runtime SIMD auto-detect) and supplies the macros those units consume:
 * the decoder build markers, the little-endian assumption, and the gcc
 * byte-swap builtins. The SIMD USE/HAVE macros are deliberately left
 * undefined so the decoder compiles to the C baseline on every target,
 * matching the xtensa build exactly.
 */
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_
#ifndef WEBP_DECODER_BUILD
#define WEBP_DECODER_BUILD 1
#endif
#ifndef WEBP_BUILD
#define WEBP_BUILD 1
#endif
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif
#ifndef HAVE_BUILTIN_BSWAP16
#define HAVE_BUILTIN_BSWAP16 1
#endif
#ifndef HAVE_BUILTIN_BSWAP32
#define HAVE_BUILTIN_BSWAP32 1
#endif
#ifndef HAVE_BUILTIN_BSWAP64
#define HAVE_BUILTIN_BSWAP64 1
#endif
#endif  /* WEBP_WEBP_CONFIG_H_ */
