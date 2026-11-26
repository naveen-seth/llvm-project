// Checks that -fmodules-driver processes each module unit once: the modules
// defined by command-line inputs are found by reading their module
// declarations, and are then scanned by name. Their implicit builds are shared
// with the scans of their importers.

// UNSUPPORTED: system-windows
// RUN: rm -rf %t && split-file %s %t

// RUN: %clang -std=c++20 -fmodules-driver -fmodules-cache-path=%t/cache \
// RUN:   -fdepscan-log-path=%t/scan.log \
// RUN:   %t/N.cppm %t/M-part.cppm %t/M.cppm %t/M-impl.cpp %t/use.cpp -### \
// RUN:   2>&1 | FileCheck %s --check-prefix=JOBS

// JOBS: "-emit-module-interface" {{.*}}N.cppm
// JOBS: "-emit-module-interface" {{.*}}M-part.cppm
// JOBS: "-emit-module-interface" {{.*}}M.cppm

// Each module is built once by the scanner, and scanned by name once.
// RUN: grep -c 'pcm_write: .*/N-' %t/scan.log | FileCheck %s --check-prefix=ONE
// RUN: grep -c 'pcm_write: .*/M:part-' %t/scan.log | FileCheck %s --check-prefix=ONE
// RUN: grep -c 'pcm_write: .*/M-' %t/scan.log | FileCheck %s --check-prefix=ONE
// RUN: grep -c 'start scan_by_name: N$' %t/scan.log | FileCheck %s --check-prefix=ONE
// RUN: grep -c 'start scan_by_name: M:part$' %t/scan.log | FileCheck %s --check-prefix=ONE
// RUN: grep -c 'start scan_by_name: M$' %t/scan.log | FileCheck %s --check-prefix=ONE
// ONE: {{^}}1{{$}}

// Only the inputs which don't define a module are scanned as translation units.
// RUN: grep 'starting scanning command:' %t/scan.log \
// RUN:   | FileCheck %s --check-prefix=TU
// RUN: not grep 'starting scanning command:.*\.cppm' %t/scan.log
// TU-DAG: M-impl.cpp
// TU-DAG: use.cpp

// A module declaration in a conditional is ill-formed. It isn't found by the
// driver, and the scan reports the error.
// RUN: not %clang -std=c++20 -fmodules-driver -fmodules-cache-path=%t/cache \
// RUN:   %t/cond.cppm -### 2>&1 | FileCheck %s --check-prefix=COND
// COND: error: module directive lines are not allowed on lines controlled by preprocessor conditionals

//--- n.h
#define N_VALUE 42

//--- N.cppm
// The global module fragment may contain conditional blocks, including text in
// skipped groups.
module;
#include "n.h"
#ifdef UNDEFINED
int skipped_text;
#else
#define OTHER \
  1
#endif
export module N;
export int n() { return N_VALUE; }

//--- M-part.cppm
export module M:part;
import N;
export int part() { return n(); }

//--- M.cppm
export module M;
export import :part;
export int m();

//--- M-impl.cpp
module M;
int m() { return part() + 1; }

//--- use.cpp
import M;
int main() { return m(); }

//--- cond.cppm
module;
#if 1
export module C;
#endif
