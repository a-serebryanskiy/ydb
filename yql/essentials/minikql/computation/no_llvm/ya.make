LIBRARY()

CXXFLAGS(-DMKQL_DISABLE_CODEGEN)

ADDINCL(GLOBAL yql/essentials/minikql/codegen/llvm_stub)

INCLUDE(../ya.make.inc)

END()
