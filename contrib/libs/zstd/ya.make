# Generated by devtools/yamaker from nixpkgs 21.11.

LIBRARY()

OWNER(
    orivej
    velavokr
    g:cpp-contrib
)

VERSION(1.5.2)

ORIGINAL_SOURCE(https://github.com/facebook/zstd/archive/v1.5.2.tar.gz)

LICENSE(
    "(BSD-2-Clause OR GPL-2.0-only)" AND
    "(BSD-3-Clause OR GPL-2.0-only)" AND
    BSD-2-Clause AND
    BSD-3-Clause AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/xxhash
)

ADDINCL(
    contrib/libs/zstd/lib
    contrib/libs/zstd/lib/common
    contrib/libs/zstd/lib/legacy
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DZSTD_LEGACY_SUPPORT=1
    -DZSTD_MULTITHREAD
)

IF (ARCH_X86_64 AND NOT MSVC)
    CFLAGS(
        -DDYNAMIC_BMI2
    )
    SRCS(
        lib/decompress/huf_decompress_amd64.S
    )
ENDIF()

SRCS(
    lib/common/debug.c
    lib/common/entropy_common.c
    lib/common/error_private.c
    lib/common/fse_decompress.c
    lib/common/pool.c
    lib/common/threading.c
    lib/common/zstd_common.c
    lib/compress/fse_compress.c
    lib/compress/hist.c
    lib/compress/huf_compress.c
    lib/compress/zstd_compress.c
    lib/compress/zstd_compress_literals.c
    lib/compress/zstd_compress_sequences.c
    lib/compress/zstd_compress_superblock.c
    lib/compress/zstd_double_fast.c
    lib/compress/zstd_fast.c
    lib/compress/zstd_lazy.c
    lib/compress/zstd_ldm.c
    lib/compress/zstd_opt.c
    lib/compress/zstdmt_compress.c
    lib/decompress/huf_decompress.c
    lib/decompress/zstd_ddict.c
    lib/decompress/zstd_decompress.c
    lib/decompress/zstd_decompress_block.c
    lib/dictBuilder/cover.c
    lib/dictBuilder/divsufsort.c
    lib/dictBuilder/fastcover.c
    lib/dictBuilder/zdict.c
    lib/legacy/zstd_v01.c
    lib/legacy/zstd_v02.c
    lib/legacy/zstd_v03.c
    lib/legacy/zstd_v04.c
    lib/legacy/zstd_v05.c
    lib/legacy/zstd_v06.c
    lib/legacy/zstd_v07.c
)

END()

RECURSE(
    programs/zstd
)
