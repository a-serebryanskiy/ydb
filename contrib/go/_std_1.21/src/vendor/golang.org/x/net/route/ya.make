GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 AND RACE AND CGO_ENABLED OR OS_DARWIN AND ARCH_ARM64 AND RACE AND NOT CGO_ENABLED OR OS_DARWIN AND ARCH_ARM64 AND NOT RACE AND CGO_ENABLED OR OS_DARWIN AND ARCH_ARM64 AND NOT RACE AND NOT CGO_ENABLED OR OS_DARWIN AND ARCH_X86_64 AND RACE AND CGO_ENABLED OR OS_DARWIN AND ARCH_X86_64 AND RACE AND NOT CGO_ENABLED OR OS_DARWIN AND ARCH_X86_64 AND NOT RACE AND CGO_ENABLED OR OS_DARWIN AND ARCH_X86_64 AND NOT RACE AND NOT CGO_ENABLED)
    SRCS(
		address.go
		binary.go
		empty.s
		interface.go
		interface_classic.go
		interface_multicast.go
		message.go
		route.go
		route_classic.go
		sys.go
		sys_darwin.go
		syscall.go
		zsys_darwin.go
    )
ENDIF()
END()