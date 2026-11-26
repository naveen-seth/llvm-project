// Checks that named modules whose interface sources are provided via
// -fmodule-source= are built implicitly during the scan, and reported like
// Clang modules with explicit build commands.
// UNSUPPORTED: target={{.*}}-aix{{.*}}

// RUN: rm -rf %t && split-file %s %t

// RUN: clang-scan-deps -format=experimental-full -- \
// RUN:   %clang++ -std=c++20 -c %t/use.cpp -o %t/use.o \
// RUN:   -Xclang -fmodule-source=M=%t/M.cppm \
// RUN:   -Xclang -fmodule-source=M:part=%t/M-part.cppm \
// RUN:   -Xclang -fmodule-source=N=%t/N.cppm \
// RUN:   -Xclang -fmodules-cache-path=%t/cache \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t

// CHECK:      "modules": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "clang-module-deps": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "context-hash": "[[PART_HASH:.*]]",
// CHECK-NEXT:         "module-name": "M:part"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "clang-modulemap-file": "",
// CHECK-NEXT:     "command-line": [
// CHECK:            "-emit-reduced-module-interface"
// CHECK:            "[[PREFIX]]/M.cppm"
// CHECK:            "-fmodule-file=M:part={{.*}}"
// CHECK:            "-fmodule-file=N={{.*}}"
// CHECK:          ],
// CHECK-NEXT:     "context-hash": "[[M_HASH:.*]]",
// CHECK-NEXT:     "file-deps": [
// CHECK-NEXT:       "[[PREFIX]]/M.cppm"
// CHECK-NEXT:     ],
// CHECK:          "name": "M"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "clang-module-deps": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "context-hash": "[[N_HASH:.*]]",
// CHECK-NEXT:         "module-name": "N"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "clang-modulemap-file": "",
// CHECK-NEXT:     "command-line": [
// CHECK:            "-emit-reduced-module-interface"
// CHECK:            "[[PREFIX]]/M-part.cppm"
// CHECK:            "-fmodule-file=N={{.*}}"
// CHECK:          ],
// CHECK-NEXT:     "context-hash": "[[PART_HASH]]",
// CHECK-NEXT:     "file-deps": [
// CHECK-NEXT:       "[[PREFIX]]/M-part.cppm"
// CHECK-NEXT:     ],
// CHECK:          "name": "M:part"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "clang-module-deps": [],
// CHECK-NEXT:     "clang-modulemap-file": "",
// CHECK-NEXT:     "command-line": [
// CHECK:            "-emit-reduced-module-interface"
// CHECK:            "[[PREFIX]]/N.cppm"
// CHECK:          ],
// CHECK-NEXT:     "context-hash": "[[N_HASH]]",
// CHECK-NEXT:     "file-deps": [
// CHECK-NEXT:       "[[PREFIX]]/N.cppm",
// CHECK-NEXT:       "[[PREFIX]]/n.h"
// CHECK-NEXT:     ],
// CHECK:          "name": "N"
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK-NEXT: "translation-units": [
// CHECK:          "named-module-deps": [
// CHECK-NEXT:       "M"
// CHECK-NEXT:     ],
// CHECK:          "clang-module-deps": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "context-hash": "[[M_HASH]]",
// CHECK-NEXT:         "module-name": "M"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK:          "command-line": [
// The TU needs mappings for all transitively imported named modules.
// CHECK:            "-fmodule-file=M={{.*}}"
// CHECK-NEXT:       "-fmodule-file=M:part={{.*}}"
// CHECK-NEXT:       "-fmodule-file=N={{.*}}"
// CHECK-NOT:        "-fmodule-source=
// CHECK:          ],

// A module implementation unit implicitly imports its primary module
// interface, which is built implicitly as well.
// RUN: clang-scan-deps -format=experimental-full -- \
// RUN:   %clang++ -std=c++20 -c %t/M-impl.cpp -o %t/M-impl.o \
// RUN:   -Xclang -fmodule-source=M=%t/M.cppm \
// RUN:   -Xclang -fmodule-source=M:part=%t/M-part.cppm \
// RUN:   -Xclang -fmodule-source=N=%t/N.cppm \
// RUN:   -Xclang -fmodules-cache-path=%t/cache \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t --check-prefix=IMPL

// IMPL:      "translation-units": [
// IMPL:          "clang-module-deps": [
// IMPL-NEXT:       {
// IMPL-NEXT:         "context-hash": "{{.*}}",
// IMPL-NEXT:         "module-name": "M"
// IMPL-NEXT:       }
// IMPL-NEXT:     ],
// IMPL:            "-fmodule-file=M={{.*}}"

// Without provided interface sources, named modules are only reported by name.
// RUN: clang-scan-deps -format=experimental-full -- \
// RUN:   %clang++ -std=c++20 -c %t/use.cpp -o %t/use.o \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s --check-prefix=NO-SOURCES

// NO-SOURCES:      "modules": [],
// NO-SOURCES:      "named-module-deps": [
// NO-SOURCES-NEXT:   "M"
// NO-SOURCES-NEXT: ],
// NO-SOURCES:      "clang-module-deps": [],

//--- n.h
#define N_VALUE 42

//--- N.cppm
module;
#include "n.h"
export module N;
export int n() { return N_VALUE; }

//--- M-part.cppm
export module M:part;
import N;
export int part() { return n(); }

//--- M.cppm
export module M;
export import :part;
export int m() { return part() + 1; }

//--- M-impl.cpp
module M;
int helper() { return m(); }

//--- use.cpp
import M;
int main() { return m(); }
