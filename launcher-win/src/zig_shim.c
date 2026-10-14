/* zcc.exe / zar.exe / zranlib.exe / zrc.exe -- what CMake runs as the
 * compiler and archiver on Windows. Each one starts tools\zig\zig.exe with
 * its sub-command and passes its own command line through untouched.
 *
 * A .cmd wrapper would be simpler, but Windows runs one through cmd.exe,
 * which strips the first and last quote from a command line that has more
 * than two -- so a compile with a quoted path (a user name with a space in
 * it, say) breaks. An exe passes its arguments through exactly.
 *
 * Lives in tools\shims\; finds tools\zig\ and tools\implib\ beside it.
 * Built four times by build.sh, with ZIG_SUB naming the sub-command. */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#ifndef ZIG_SUB
#error define ZIG_SUB (cc, ar, ranlib or rc)
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

/* The part of our command line after our own name. */
static const wchar_t *after_argv0(const wchar_t *cmd)
{
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"') cmd++;
        if (*cmd) cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') cmd++;
    }
    while (*cmd == L' ' || *cmd == L'\t') cmd++;
    return cmd;
}

int wmain(void)
{
    static wchar_t tools[MAX_PATH * 4], line[32768];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    int n;

    GetModuleFileNameW(NULL, tools, MAX_PATH * 4);
    if ((slash = wcsrchr(tools, L'\\'))) *slash = 0;   /* ...\tools\shims */
    if ((slash = wcsrchr(tools, L'\\'))) *slash = 0;   /* ...\tools */

    if (wcscmp(WIDEN(ZIG_SUB), L"cc") == 0)
        n = swprintf(line, 32768, L"\"%ls\\zig\\zig.exe\" cc -target x86_64-windows-gnu -L\"%ls\\implib\" %ls",
                     tools, tools, after_argv0(GetCommandLineW()));
    else
        n = swprintf(line, 32768, L"\"%ls\\zig\\zig.exe\" %ls %ls", tools, WIDEN(ZIG_SUB),
                     after_argv0(GetCommandLineW()));
    if (n < 0) {
        fwprintf(stderr, L"zig shim: command line too long\n");
        return 1;
    }

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"zig shim: cannot start %ls\\zig\\zig.exe (error %lu)\n", tools, GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
