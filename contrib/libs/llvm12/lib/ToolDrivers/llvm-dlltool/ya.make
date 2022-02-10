# Generated by devtools/yamaker.

LIBRARY()

OWNER(
    orivej
    g:cpp-contrib
)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm12
    contrib/libs/llvm12/include
    contrib/libs/llvm12/lib/Object
    contrib/libs/llvm12/lib/Option
    contrib/libs/llvm12/lib/Support
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm12/lib/ToolDrivers/llvm-dlltool
    contrib/libs/llvm12/lib/ToolDrivers/llvm-dlltool
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    DlltoolDriver.cpp
)

END()
