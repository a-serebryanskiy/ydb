IF (HOST_OS_DARWIN AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:7948642784)
ELSEIF (HOST_OS_DARWIN AND HOST_ARCH_ARM64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:7948641644)
ELSEIF (HOST_OS_LINUX AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:7948644946)
ELSEIF (HOST_OS_LINUX AND HOST_ARCH_AARCH64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:7948640691)
ELSEIF (HOST_OS_WINDOWS AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:7948644187)

ENDIF()
