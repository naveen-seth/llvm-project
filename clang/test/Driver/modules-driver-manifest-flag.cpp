// Checks that -fmodules-driver builds modules from manifests given via
// -fmodules-manifest= on demand, including modules which are not part of the
// Standard library. Such modules only get a BMI-only job, since their object
// code is expected to be in a prebuilt library.

// RUN: rm -rf %t && split-file %s %t
// RUN: sed "s|DIR|%/t|g" %t/mylib.modules.json.in > %t/mylib.modules.json
// RUN: %clang -std=c++20 -fmodules-driver -Rmodules-driver \
// RUN:   -fmodules-manifest=%t/mylib.modules.json \
// RUN:   -fmodules-cache-path=%t/cache -c %t/main.cpp -### 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t

// CHECK:     "[[LIB:mylib-[A-Z0-9]+]]" [fillcolor=2, label="{ Filename: [[PREFIX]]/src/mylib.cppm | Module type: Named module | Module name: mylib | Hash: {{[A-Z0-9]+}} }"];
// CHECK-NOT: unused
// CHECK:     "[[LIB]]" -> "[[PREFIX]]/main.cpp-{{.*}}";

// CHECK:      "-cc1" {{.*}} "-o" "[[LIB_PCM:[^"]+]]" {{.*}} "-emit-reduced-module-interface" {{.*}} "[[PREFIX]]/src/mylib.cppm"
// CHECK-NOT:  "-Wno-reserved-module-identifier"
// CHECK:      "-cc1" {{.*}} "-emit-obj" {{.*}} "[[PREFIX]]/main.cpp" {{.*}} "-fmodule-file=mylib=[[LIB_PCM]]"
// CHECK-NOT:  "-cc1"

// A module whose source listed in the manifest doesn't exist is diagnosed when
// imported.
// RUN: sed "s|src/mylib.cppm|src/missing.cppm|" %t/mylib.modules.json > %t/missing.modules.json
// RUN: not %clang -std=c++20 -fmodules-driver \
// RUN:   -fmodules-manifest=%t/missing.modules.json \
// RUN:   -fmodules-cache-path=%t/cache -c %t/main.cpp -### 2>&1 \
// RUN:   | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t --check-prefix=MISSING

// MISSING: clang: error: module 'mylib' imported by '[[PREFIX]]/main.cpp' is listed in a module manifest, but its source file '[[PREFIX]]/src/missing.cppm' does not exist

//--- main.cpp
import mylib;
int main() { return lib(); }

//--- src/mylib.cppm
export module mylib;
export int lib() { return 0; }

//--- src/unused.cppm
export module unused;

//--- mylib.modules.json.in
{
  "version": 1,
  "revision": 1,
  "modules": [
    {
      "logical-name": "mylib",
      "source-path": "src/mylib.cppm",
      "is-std-library": false
    },
    {
      "logical-name": "unused",
      "source-path": "src/unused.cppm",
      "is-std-library": false
    }
  ]
}
