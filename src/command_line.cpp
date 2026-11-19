#include "native.h"
#include <fcntl.h>
#include <io.h>

namespace {
HANDLE duplicate_output(DWORD id) {
    HANDLE handle = GetStdHandle(id), copy = nullptr;
    if (handle && handle != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    return copy;
}
void connect_output(FILE *stream, HANDLE handle) {
    if (!handle)
        return;
    // GUI CRTs can start with descriptor -2. Reopen before binding the owned handle.
    if (!std::freopen("NUL", "w", stream)) {
        CloseHandle(handle);
        return;
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_TEXT);
    if (descriptor < 0) {
        CloseHandle(handle);
        return;
    }
    if (descriptor != _fileno(stream)) {
        _dup2(descriptor, _fileno(stream));
        _close(descriptor);
    }
}
} // namespace
void init_command_line_output() {
    // Preserve redirected pipes/files before AttachConsole can replace standard handles.
    HANDLE output = duplicate_output(STD_OUTPUT_HANDLE), error = duplicate_output(STD_ERROR_HANDLE);
    if (!GetConsoleWindow())
        AttachConsole(ATTACH_PARENT_PROCESS); // Never allocate a new console window.
    if (!output)
        output = duplicate_output(STD_OUTPUT_HANDLE);
    if (!error)
        error = duplicate_output(STD_ERROR_HANDLE);
    connect_output(stdout, output);
    connect_output(stderr, error);
}
