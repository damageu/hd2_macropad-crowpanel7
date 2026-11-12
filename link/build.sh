#!/bin/sh
# HD2 Macropad Link v1.2.3 build (box: llvm-mingw-msvcrt at /home/box/llvm-mingw-msvcrt).
set -e
MZ="-DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES"
CC_LINUX=${CC_LINUX:-gcc}
# Linux tests (no Windows APIs, no serial hardware)
$CC_LINUX -Wall -Wextra -Werror -O2 -o test_core test_core.c && ./test_core
$CC_LINUX -Wall -Wextra -Werror -O2 -o test_update test_update.c update_core.c sha256.c && ./test_update
$CC_LINUX -Wall -Wextra -Werror -O2 $MZ -o test_romflash test_romflash.c romflash.c update_core.c sha256.c md5.c third_party/miniz/miniz.c && ./test_romflash
# Windows exe. miniz 3.0.2 warns on a 32-bit range check under this mingw; that
# file is compiled without -Werror. Our sources stay -Werror.
x86_64-w64-mingw32-windres res.rc -O coff -o res.o
x86_64-w64-mingw32-gcc -Wall -Wextra -O2 $MZ -c third_party/miniz/miniz.c -o miniz.o
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -O2 $MZ -municode -mwindows \
  hd2link.c updater_win.c install_win.c romflash.c update_core.c sha256.c md5.c miniz.o res.o \
  -o HD2MacropadLink-v1.2.4.exe -lsetupapi -lwinmm -lwinhttp -lcomdlg32 -lshell32 -static
