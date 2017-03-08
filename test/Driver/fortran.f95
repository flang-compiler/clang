// Check that the clang driver can invoke gcc to compile Fortran.

// RUN: %clang -target x86_64-unknown-linux-gnu -integrated-as -c %s -### 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-OBJECT %s
// CHECK-OBJECT-NOT: cc1as

// RUN: %clang -target x86_64-unknown-linux-gnu -integrated-as -S %s -### 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-ASM %s
// CHECK-ASM: "-S"

// RUN: %clang -Wall -target x86_64-unknown-linux-gnu -integrated-as %s -o %t -### 2>&1 | FileCheck --check-prefix=CHECK-WARN %s
// CHECK-WARN: ld
