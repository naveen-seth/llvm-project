// Checks for diagnostics that report invalid options used with
// -fmodules-driver.

// RUN: split-file %s %t
// RUN: not %clang -std=c++20 -fmodules-driver -fmodules-driver-jobs=0 %t/main.cpp -### 2>&1 | FileCheck %s --check-prefix=JOBS-ZERO
// RUN: not %clang -std=c++20 -fmodules-driver -fmodules-driver-jobs=x %t/main.cpp -### 2>&1 | FileCheck %s --check-prefix=JOBS-INVALID

// JOBS-ZERO: clang: error: invalid value '0' in '-fmodules-driver-jobs='; expected a positive integer
// JOBS-INVALID: clang: error: invalid value 'x' in '-fmodules-driver-jobs='; expected a positive integer

// -fno-modules-reduced-bmi is supported.
// RUN: %clang -std=c++20 -fmodules-driver -fno-modules-reduced-bmi %t/main.cpp -### 2>&1 | FileCheck %s --check-prefix=FULL-BMI
// FULL-BMI-NOT: error:

//--- main.cpp
int main() {}
