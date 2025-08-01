# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(
    Apache-2.0 AND
    Apache-2.0 WITH LLVM-exception AND
    MIT AND
    NCSA AND
    Public-Domain
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(20.1.7)

ORIGINAL_SOURCE(https://github.com/llvm/llvm-project/archive/llvmorg-20.1.7.tar.gz)

SET(SANITIZER_CFLAGS)

PEERDIR(
    library/cpp/sanitizer/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

IF (SANITIZE_COVERAGE MATCHES "trace-pc")
    MESSAGE(FATAL_ERROR "I will crash you with trace-pc or trace-pc-guard. Use inline-8bit-counters.")
ENDIF()

NO_SANITIZE_COVERAGE()

IF (SANITIZER_TYPE == "undefined")
    NO_SANITIZE()
ENDIF()

IF (OS_WINDOWS)
    SRCS(
        lib/fuzzer/standalone/StandaloneFuzzTargetMain.c
    )
ELSE()
    SRCS(
        lib/fuzzer/FuzzerCrossOver.cpp
        lib/fuzzer/FuzzerDataFlowTrace.cpp
        lib/fuzzer/FuzzerDriver.cpp
        lib/fuzzer/FuzzerExtFunctionsDlsym.cpp
        lib/fuzzer/FuzzerExtFunctionsWeak.cpp
        lib/fuzzer/FuzzerExtFunctionsWindows.cpp
        lib/fuzzer/FuzzerExtraCounters.cpp
        lib/fuzzer/FuzzerExtraCountersDarwin.cpp
        lib/fuzzer/FuzzerExtraCountersWindows.cpp
        lib/fuzzer/FuzzerFork.cpp
        lib/fuzzer/FuzzerIO.cpp
        lib/fuzzer/FuzzerIOPosix.cpp
        lib/fuzzer/FuzzerIOWindows.cpp
        lib/fuzzer/FuzzerLoop.cpp
        lib/fuzzer/FuzzerMain.cpp
        lib/fuzzer/FuzzerMerge.cpp
        lib/fuzzer/FuzzerMutate.cpp
        lib/fuzzer/FuzzerSHA1.cpp
        lib/fuzzer/FuzzerTracePC.cpp
        lib/fuzzer/FuzzerUtil.cpp
        lib/fuzzer/FuzzerUtilDarwin.cpp
        lib/fuzzer/FuzzerUtilLinux.cpp
        lib/fuzzer/FuzzerUtilPosix.cpp
        lib/fuzzer/FuzzerUtilWindows.cpp
    )
ENDIF()

END()

RECURSE(
    lib/fuzzer/afl
)
