// Checks that -fmodules-driver correctly handles compilations using
// Standard C++20 modules.

// RUN: split-file %s %t

// RUN: %clang -c -std=c++23 \
// RUN:   -fmodules-driver -Rmodules-driver -Rmodule-import \
// RUN:   %t/main.cpp %t/A.cppm %t/A-part1.cppm %t/A-part1-impl.cppm %t/B.cppm 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' \
// RUN:   | FileCheck -DPREFIX=%/t --check-prefix=CHECK-REMARKS %s

// Skip past remarks emitted during the dependency scan.
// CHECK-REMARKS: remark: printing module dependency graph [-Rmodules-driver]
// CHECK-REMARKS: [[PREFIX]]/A.cppm:2:8: remark: importing module 'A:part1' from
// CHECK-REMARKS: [[PREFIX]]/A.cppm:3:1: remark: importing module 'B' from
// CHECK-REMARKS: [[PREFIX]]/main.cpp:1:1: remark: importing module 'A' from
// CHECK-REMARKS: [[PREFIX]]/main.cpp:1:1: remark: importing module 'A:part1' into 'A' from
// CHECK-REMARKS: [[PREFIX]]/main.cpp:1:1: remark: importing module 'B' into 'A' from
// CHECK-REMARKS: [[PREFIX]]/main.cpp:2:1: remark: importing module 'B' from

// RUN: %clang -std=c++23 \
// RUN:   -fmodules-driver -Rmodules-driver -Rmodule-import \
// RUN:   %t/main.cpp %t/A.cppm %t/A-part1.cppm %t/A-part1-impl.cppm %t/B.cppm \
// RUN:   -### 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' \
// RUN:   | FileCheck --check-prefix=CHECK-CC1 %s

// Module units are compiled in two phases. The precompile job emits the full
// BMI, and the Reduced BMI for importers as a by-product.
// CHECK-CC1: "-cc1" {{.*}} "-fmodule-output=[[B_PCM:[^"]+]]" {{.*}} "-emit-module-interface" {{.*}} "{{.*}}/B.cppm" {{.*}} "-fno-implicit-modules"

// A-part1-impl is not imported, but is built by the scanner's command too.
// CHECK-CC1: "-cc1" {{.*}} "-fmodule-output={{[^"]+}}" {{.*}} "-emit-module-interface" {{.*}} "{{.*}}/A-part1-impl.cppm" {{.*}} "-fno-implicit-modules"

// CHECK-CC1: "-cc1" {{.*}} "-fmodule-output=[[A_PART1_PCM:[^"]+]]" {{.*}} "-emit-module-interface" {{.*}} "{{.*}}/A-part1.cppm" {{.*}} "-fno-implicit-modules"

// CHECK-CC1: "-cc1" {{.*}} "-fmodule-output=[[A_PCM:[^"]+]]" {{.*}} "-emit-module-interface" {{.*}} "{{.*}}/A.cppm"
// CHECK-CC1-SAME: "-fmodule-file=A:part1=[[A_PART1_PCM]]"
// CHECK-CC1-SAME: "-fmodule-file=B=[[B_PCM]]"
// CHECK-CC1-SAME: "-fno-implicit-modules"

// CHECK-CC1: "-cc1" {{.*}} "-emit-obj" {{.*}} "{{.*}}/main.cpp"
// CHECK-CC1-SAME: "-fmodule-file=A=[[A_PCM]]"
// CHECK-CC1-SAME: "-fmodule-file=A:part1=[[A_PART1_PCM]]"
// CHECK-CC1-SAME: "-fmodule-file=B=[[B_PCM]]"
// CHECK-CC1-SAME: "-fno-implicit-modules"

// The object file of a module unit is compiled from its full BMI.
// CHECK-CC1: "-cc1" {{.*}} "-emit-obj" {{.*}} "-x" "pcm" "{{.*}}/A-{{[0-9a-f]+}}.pcm"
// CHECK-CC1-SAME: "-fmodule-file=A:part1=[[A_PART1_PCM]]"
// CHECK-CC1-SAME: "-fmodule-file=B=[[B_PCM]]"

//--- main.cpp
import A;
import B;

int main() {
  return a() + b();
}

//--- A.cppm
export module A;
export import :part1;
import B;

export int a() {
  return part1() + b();
}

//--- A-part1.cppm
export module A:part1;
export int part1();

//--- A-part1-impl.cppm
module A:part1_impl;

int part1() {
  return 30;
}

//--- B.cppm
export module B;

export int b() {
  return 12;
}
