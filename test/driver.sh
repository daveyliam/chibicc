#!/bin/bash
chibicc=$1

tmp=`mktemp -d /tmp/chibicc-test-XXXXXX`
trap 'rm -rf $tmp' INT TERM HUP EXIT

check() {
    if [ $? -eq 0 ]; then
        echo "testing $1 ... passed"
    else
        echo "testing $1 ... failed"
        exit 1
    fi
}

# -o
rm -f $tmp/out
echo 'void _start() {}' > $tmp/out.c
$chibicc -o $tmp/out $tmp/out.c
[ -f $tmp/out ]
check -o

# --help
$chibicc --help 2>&1 | grep -q chibicc
check --help

# Default output file
rm -f $tmp/out.o $tmp/out.s
echo 'void _start() {}' > $tmp/out.c
(cd $tmp; $OLDPWD/$chibicc out.c)
[ -f $tmp/a.out ]
check 'default output file'

# Multiple input files
rm -f $tmp/foo.o $tmp/bar.o
echo 'int x;' > $tmp/foo.c
echo 'int y;' > $tmp/bar.c
(cd $tmp; $OLDPWD/$chibicc $tmp/foo.c $tmp/bar.c)
[ -f $tmp/a.out ]
check 'multiple input files'

# a.out
rm -f $tmp/a.out
echo 'void _start() {}' > $tmp/foo.c
(cd $tmp; $OLDPWD/$chibicc foo.c)
[ -f $tmp/a.out ]
check a.out

# -I
mkdir $tmp/dir
echo 'void _start() {}' > $tmp/dir/i-option-test
echo "#include \"i-option-test\"" > $tmp/foo.c
(cd $tmp; $OLDPWD/$chibicc -o /dev/null -I$tmp/dir foo.c)
check -I

# -D
echo 'void _start() {int a = foo;}' | $chibicc -o /dev/null -Dfoo -
check -D

# -D
echo 'int foo = 123; void _start() {}' > $tmp/foo.c
$chibicc -o $tmp/foo -Dfoo=bar $tmp/foo.c
strings $tmp/foo | grep -q bar
! strings $tmp/foo | grep -q foo
check -D

# -U
echo 'int foo = 123; void _start() {}' > $tmp/foo.c
$chibicc -o $tmp/foo -Dfoo=bar -Ufoo $tmp/foo.c
strings $tmp/foo | grep -q foo
! strings $tmp/foo | grep -q bar
check -U

# ignored options
echo 'void _start() {}' > $tmp/foo.c
$chibicc -O -Wall -g -std=c11 -ffreestanding -fno-builtin \
         -fno-omit-frame-pointer -fno-stack-protector -fno-strict-aliasing \
         -m64 -mno-red-zone -w -o /dev/null $tmp/foo.c
check 'ignored options'

# BOM marker
printf '\xef\xbb\xbfvoid _start() {}\n' | $chibicc -o /dev/null -
check 'BOM marker'

# Inline functions
echo 'inline void foo() {}' > $tmp/inline1.c
echo 'inline void foo() {}' > $tmp/inline2.c
echo 'void _start() {}' > $tmp/inline3.c
$chibicc -o /dev/null $tmp/inline1.c $tmp/inline2.c $tmp/inline3.c
check inline

echo 'extern inline void foo() {}' > $tmp/inline1.c
echo 'int foo(); void _start() { foo(); }' > $tmp/inline2.c
$chibicc -o /dev/null $tmp/inline1.c $tmp/inline2.c
check inline

# echo 'static inline void f1() {}' | $chibicc -o- - | grep -v -q f1:
# check inline

# echo 'static inline void f1() {} void foo() { f1(); }' | $chibicc -o- - | grep -q f1:
# check inline

# echo 'static inline void f1() {} static inline void f2() { f1(); } void foo() { f1(); }' | $chibicc -o- -S -xc - | grep -q f1:
# check inline

# echo 'static inline void f1() {} static inline void f2() { f1(); } void foo() { f1(); }' | $chibicc -o- -S -xc - | grep -v -q f2:
# check inline

# echo 'static inline void f1() {} static inline void f2() { f1(); } void foo() { f2(); }' | $chibicc -o- -S -xc - | grep -q f1:
# check inline

# echo 'static inline void f1() {} static inline void f2() { f1(); } void foo() { f2(); }' | $chibicc -o- -S -xc - | grep -q f2:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() {}' | $chibicc -o- -S -xc - | grep -v -q f1:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() {}' | $chibicc -o- -S -xc - | grep -v -q f2:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() { f1(); }' | $chibicc -o- -S -xc - | grep -q f1:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() { f1(); }' | $chibicc -o- -S -xc - | grep -q f2:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() { f2(); }' | $chibicc -o- -S -xc - | grep -q f1:
# check inline

# echo 'static inline void f2(); static inline void f1() { f2(); } static inline void f2() { f1(); } void foo() { f2(); }' | $chibicc -o- -S -xc - | grep -q f2:
# check inline

# -idirafter
mkdir -p $tmp/dir1 $tmp/dir2
echo "#define x \"foofoo\"" > $tmp/dir1/idirafter
echo "#define x \"barbar\"" > $tmp/dir2/idirafter
printf "#include \"idirafter\"\nvoid _start() {const char *y = x;}\n" | \
    $chibicc -I$tmp/dir1 -I$tmp/dir2 -o- - | strings | grep -q foofoo
check -idirafter
printf "#include \"idirafter\"\nvoid _start() {const char *y = x;}\n" | \
    $chibicc -idirafter $tmp/dir1 -I$tmp/dir2 -o- - | strings | grep -q barbar
check -idirafter

# -include
echo "char *x = \"foofoo\";" > $tmp/out.h
echo "char *y = \"barbar\"; void _start() {}" | \
    $chibicc -include $tmp/out.h -o$tmp/out -
strings $tmp/out |  grep -q 'foofoo'
strings $tmp/out |  grep -q 'barbar'
check -include
# echo NULL | $chibicc -Iinclude -include stdio.h -o- - | grep -q 0
# check -include

# #include_next
mkdir -p $tmp/next1 $tmp/next2 $tmp/next3
echo '#include "file1.h"' > $tmp/file.c
echo '#include_next "file1.h"' > $tmp/next1/file1.h
echo '#include_next "file2.h"' > $tmp/next2/file1.h
echo "char *x = \"foofoo\"; void _start() {}" > $tmp/next3/file2.h
$chibicc -I$tmp/next1 -I$tmp/next2 -I$tmp/next3 $tmp/file.c -o- | strings | grep -q foofoo
check '#include_next'

echo OK
