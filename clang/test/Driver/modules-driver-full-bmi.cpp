// Checks that -fmodules-driver supports full BMIs. Module units are always
// compiled in two phases: a precompile job emits the full BMI, and a second job
// compiles it into an object file. With full BMIs, importers use the full BMI.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang -std=c++20 -fmodules-driver -fno-modules-reduced-bmi \
// RUN:   -fmodules-cache-path=%t/cache %t/main.cpp %t/A.cppm -### 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t

// CHECK:      "-cc1" {{.*}} "-o" "[[A_PCM:[^"]+]]" {{.*}} "-emit-module-interface" {{.*}} "[[PREFIX]]/A.cppm"
// CHECK-NOT:  "-fmodule-output=
// CHECK-SAME: "-fno-implicit-modules"
// CHECK:      "-cc1" {{.*}} "-emit-obj" {{.*}} "[[PREFIX]]/main.cpp" {{.*}} "-fmodule-file=A=[[A_PCM]]"
// CHECK:      "-cc1" {{.*}} "-emit-obj" {{.*}} "-x" "pcm" "[[A_PCM]]"

// The same compilation works with Reduced BMIs, where importers use the
// Reduced BMI emitted as a by-product of the precompile job.
// RUN: %clang -std=c++20 -fmodules-driver \
// RUN:   -fmodules-cache-path=%t/cache %t/main.cpp %t/A.cppm -### 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t --check-prefix=REDUCED

// REDUCED:      "-cc1" {{.*}} "-fmodule-output=[[A_REDUCED_PCM:[^"]+]]" {{.*}} "-o" "[[A_PCM:[^"]+]]" {{.*}} "-emit-module-interface" {{.*}} "[[PREFIX]]/A.cppm"
// REDUCED:      "-cc1" {{.*}} "-emit-obj" {{.*}} "[[PREFIX]]/main.cpp" {{.*}} "-fmodule-file=A=[[A_REDUCED_PCM]]"
// REDUCED:      "-cc1" {{.*}} "-emit-obj" {{.*}} "-x" "pcm" "[[A_PCM]]"

//--- main.cpp
import A;
int main() { return a(); }

//--- A.cppm
export module A;
export int a() { return 0; }
