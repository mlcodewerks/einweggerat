# GCC/MSYS2 loader builds

The standalone loader supports Windows x86 and x64. A 32-bit host loads PE32
DLLs; a 64-bit host loads PE32+ DLLs. Opposite-architecture images are rejected
before their code executes. The desktop frontend's build remains x64.

The complete loader implementation, including TLS and exception support, is in
`third_party/MemoryModulePP.c`. Its import helpers are in the
`MEMORYMODULEPP_IMPLEMENTATION` section of `MemoryModulePP.h`; ordinary users
include that header without defining the implementation macro and compile the
C file once. Loading uses memory throughout, without TLS proxy DLLs or files.

From PowerShell with MSYS2 installed at `C:\msys64`:

```powershell
$env:PATH = "C:\msys64\mingw32\bin;$env:PATH"
meson setup build-loader-x86 loader --cross-file loader/x86-msys2.ini
meson compile -C build-loader-x86
meson test -C build-loader-x86 --no-rebuild --print-errorlogs
```

The cross file makes the target architecture explicit even when using 64-bit
Meson. Adjust its compiler paths for a different MSYS2 installation. Keep the
MINGW32 runtime directory on PATH when running these executables, or supply the
matching runtime DLLs beside them. GCC's x86 DW2 exception implementation
requires the host and DLLs to share `libgcc_s_dw2-1.dll` for exceptions that cross
DLL boundaries. This build uses `-shared-libgcc` for x86. Independently linked
static DW2 runtimes cannot provide that interoperability.

From a UCRT64 or MINGW64 shell for x64:

```sh
CC=gcc CXX=g++ meson setup build-loader-x64 loader
meson compile -C build-loader-x64
meson test -C build-loader-x64 --no-rebuild --print-errorlogs
```

Both builds produce `libmemorymodulepp.a` and a matching test executable. The
tests exercise stored/deflated ZIPs, bundled dependencies, imports by name and
ordinal, forwarded exports, bound imports, eager delay imports, native static
TLS, zero fill, existing/new threads, C++ exception propagation, destructor
unwinding, SEH, failed initialization, repeated unload, and architecture checks.
PE32 also tests legacy VA-based delay descriptors with relocations.

x86 exception handlers residing in mapped DLLs must have valid SafeSEH metadata.
The loader validates that metadata and uses Windows' native exception cache,
including removal on unload; it does not disable SafeSEH or debugger checks.
The x86 fixture generates the load configuration and handler table using GCC
and GNU assembler, then catches a real access violation outside a debugger.
x64 uses registered runtime function tables and a real `RaiseException` test.

Native TLS and x86 exception-cache registration rely on validated private
Windows interfaces. Both architectures were tested on Windows build 26300.
Unrecognized implementations fail with `ERROR_NOT_SUPPORTED`.
