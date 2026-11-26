// Checks that -fmodules-driver does not run jobs depending on a module whose
// job failed.

// RUN: rm -rf %t && split-file %s %t && cd %t
// RUN: not %clang -std=c++20 -fmodules-driver -fmodules-cache-path=%t/cache \
// RUN:   -c %t/Bad.cppm %t/Good.cppm %t/use-bad.cpp %t/use-good.cpp 2>&1 \
// RUN:   | FileCheck %s
// RUN: test -f %t/use-good.o
// RUN: not test -f %t/use-bad.o

// CHECK: Bad.cppm:2:27: error: use of undeclared identifier 'undeclared'
// CHECK-NOT: use-bad.cpp
// CHECK-NOT: error:

//--- Bad.cppm
export module Bad;
export int bad() { return undeclared; }

//--- Good.cppm
export module Good;
export int good() { return 0; }

//--- use-bad.cpp
import Bad;
int f() { return bad(); }

//--- use-good.cpp
import Good;
int g() { return good(); }
