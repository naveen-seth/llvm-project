// Checks that -fmodules-driver diagnoses imports of named modules which are
// neither defined by any input nor provided by any module manifest, before
// any job is run.

// RUN: rm -rf %t && split-file %s %t
// RUN: not %clang -std=c++20 -fmodules-driver -c %t/main.cpp %t/A.cppm \
// RUN:   -fmodules-cache-path=%t/cache -### 2>&1 | FileCheck %s

// CHECK: clang: error: module 'Missing' imported by '{{.*}}main.cpp' was not found; it is not provided by any input or module manifest
// CHECK-NOT: "-cc1"

//--- main.cpp
import A;
import Missing;

//--- A.cppm
export module A;
