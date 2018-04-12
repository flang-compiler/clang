! RUN: %flang -target aarch64 -v -x f77 -### -c - 2>&1 >/dev/null | FileCheck --check-prefix=AARCH64 %s
! RUN: %flang -target x86_64 -v -x f77 -### -c - 2>&1 >/dev/null | FileCheck --check-prefix=X86_64 %s

! AARCH64: __aarch64__
! AARCH64: __ARM_ARCH=8
! AARCH64: __ARM_ARCH__=8
! AARCH64-NOT: __x86_64__
! AARCH64-NOT: __amd_64__amd64__
! AARCH64-NOT: __k8__

! X86_64: __x86_64__
! X86_64: __amd_64__amd64__
! X86_64: __k8__
! X86_64-NOT: __aarch64__
! X86_64-NOT: __ARM_ARCH
