// Checks that -fmodules-driver can run independent jobs in parallel with
// -fmodules-driver-jobs=, while respecting module dependencies.

// RUN: rm -rf %t && split-file %s %t && cd %t
// RUN: %clang -std=c++20 -fmodules-driver -fmodules-driver-jobs=4 \
// RUN:   -fmodules-cache-path=%t/cache \
// RUN:   -c %t/A.cppm %t/B.cppm %t/C.cppm %t/use.cpp
// RUN: test -f %t/A.o && test -f %t/B.o && test -f %t/C.o && test -f %t/use.o

//--- A.cppm
export module A;
export int a() { return 1; }

//--- B.cppm
export module B;
import A;
export int b() { return a() + 1; }

//--- C.cppm
export module C;
export import B;
export int c() { return b() + 1; }

//--- use.cpp
import C;
import A;
int main() { return c() + a(); }
