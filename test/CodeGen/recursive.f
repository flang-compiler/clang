! RUN: %flang -O0 -frecursive -c %s -S -emit-llvm -o - | FileCheck -check-prefix=RECURSIVE %s
! RUN: %flang -O0 -fno-recursive -c %s -S -emit-llvm -o - | FileCheck -check-prefix=NORECURSIVE %s
! RUN: %flang -O0 -Mrecursive -c %s -S -emit-llvm -o - | FileCheck -check-prefix=RECURSIVE %s
! RUN: %flang -O0 -Mnorecursive -c %s -S -emit-llvm -o - | FileCheck -check-prefix=NORECURSIVE %s
      PROGRAM RECURSIVE
        IMPLICIT NONE
        INTEGER FUN
        EXTERNAL FUN
        PRINT *, FUN(4, FUN)
      END PROGRAM RECURSIVE

      FUNCTION FUN(X, DUMMY)
        IMPLICIT NONE
        INTEGER FUN
        INTEGER X
        INTEGER DUMMY
        EXTERNAL DUMMY
        INTEGER ARRAY(4)
! RECURSIVE: %array_{{[0-9]+}} = alloca [4 x i32]
! RECURSIVE-NOT: BSS
! NORECURSIVE: BSS
! NORECURSIVE-NOT: %array_{{[0-9]+}} = alloca [4 x i32]

        ARRAY = (/ X, 2, 3, 4 /)
        IF (X .GT. 0) THEN
          ARRAY(X) = DUMMY(X - 1, DUMMY)
        ENDIF
        FUN = ARRAY(1)
      END FUNCTION
