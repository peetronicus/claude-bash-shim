// bash.exe relay for native Windows parents of Git Bash (MSYS2).
//
// A native parent hands a child one command-line string. Node, Bun and Python
// encode an argument vector into it by the Microsoft C runtime rules; the MSYS2
// runtime decodes it by its own rules when the parent is not an MSYS2 process.
// The two disagree inside a quoted argument: MSYS2 reads every backslash pair
// as one backslash, and it cuts a quoted argument near 8K characters. So a
// `bash -c <script>` launch loses backslashes and long scripts.
//
// This program is launched in place of bash.exe. It decodes its own command
// line by the Microsoft rules, hands the -c script to the real bash through an
// environment variable (which no argument decoder touches) and re-encodes the
// remaining arguments by the MSYS2 rules. Standard handles, the working
// directory, the environment and the exit code pass through unchanged.
//
// The real bash is the first of: CLAUDE_BASH_SHIM_TARGET; bin\bash.exe of the
// Git for Windows installation the registry names (per user, then per
// machine); the default installation folders; the installation that owns the
// git.exe on PATH.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SCRIPT_VARIABLE L"CLAUDE_BASH_SHIM_SCRIPT"
#define TARGET_VARIABLE L"CLAUDE_BASH_SHIM_TARGET"

// The script bash runs in place of the original: the original text arrives by
// expansion, and the variable is gone before any of it executes.
#define RELAY_SCRIPT L"eval \"unset CLAUDE_BASH_SHIM_SCRIPT; $CLAUDE_BASH_SHIM_SCRIPT\""

// Windows refuses a longer value for one environment variable.
#define VARIABLE_LIMIT 32766

// Longest path Windows names, in characters with the terminator.
#define PATH_CAPACITY 32768

typedef struct {
    wchar_t *data;
    size_t length;
    size_t capacity;
} Text;

static wchar_t self[PATH_CAPACITY];

static void fail(const char *message, DWORD code) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written;
    WriteFile(err, "bash-shim: ", 11, &written, NULL);
    WriteFile(err, message, (DWORD)strlen(message), &written, NULL);
    WriteFile(err, "\n", 1, &written, NULL);
    ExitProcess(code);
}

static void put(Text *text, const wchar_t *source, size_t count) {
    if (text->length + count + 1 > text->capacity) {
        size_t capacity = (text->length + count + 1) * 2;
        wchar_t *data = realloc(text->data, capacity * sizeof(wchar_t));
        if (!data) fail("out of memory", 126);
        text->data = data;
        text->capacity = capacity;
    }
    memcpy(text->data + text->length, source, count * sizeof(wchar_t));
    text->length += count;
    text->data[text->length] = 0;
}

static void put_char(Text *text, wchar_t c) { put(text, &c, 1); }

// True when the path names a file other than this program. On success the
// path is left in its full form.
static int usable(wchar_t *path) {
    static wchar_t full[PATH_CAPACITY];
    DWORD length = GetFullPathNameW(path, PATH_CAPACITY, full, NULL);
    if (!length || length >= PATH_CAPACITY) return 0;
    DWORD attributes = GetFileAttributesW(full);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    if (!_wcsicmp(full, self)) return 0;
    memcpy(path, full, (length + 1) * sizeof(wchar_t));
    return 1;
}

// folder + tail, when that names a usable bash.
static int in_folder(wchar_t *target, const wchar_t *folder, const wchar_t *tail) {
    if (wcslen(folder) + wcslen(tail) + 1 > PATH_CAPACITY) return 0;
    wcscpy_s(target, PATH_CAPACITY, folder);
    wcscat_s(target, PATH_CAPACITY, tail);
    return usable(target);
}

static int from_registry(wchar_t *target, HKEY root) {
    static wchar_t folder[PATH_CAPACITY];
    DWORD bytes = sizeof(folder);
    if (RegGetValueW(root, L"SOFTWARE\\GitForWindows", L"InstallPath", RRF_RT_REG_SZ, NULL, folder, &bytes) != ERROR_SUCCESS)
        return 0;
    return in_folder(target, folder, L"\\bin\\bash.exe");
}

static int from_variable_folder(wchar_t *target, const wchar_t *variable, const wchar_t *tail) {
    static wchar_t folder[PATH_CAPACITY];
    DWORD length = GetEnvironmentVariableW(variable, folder, PATH_CAPACITY);
    return length && length < PATH_CAPACITY && in_folder(target, folder, tail);
}

// Git's own layouts put git.exe in cmd\, bin\ or mingw64\bin\ of the
// installation whose bin\ holds bash.exe.
static int from_git_on_path(wchar_t *target) {
    static wchar_t git[PATH_CAPACITY];
    wchar_t *name = NULL;
    DWORD length = SearchPathW(NULL, L"git.exe", NULL, PATH_CAPACITY, git, &name);
    if (!length || length >= PATH_CAPACITY || !name) return 0;
    *name = 0;
    return in_folder(target, git, L"..\\bin\\bash.exe") || in_folder(target, git, L"bash.exe") ||
           in_folder(target, git, L"..\\..\\bin\\bash.exe");
}

static void find_target(wchar_t *target) {
    DWORD length = GetModuleFileNameW(NULL, self, PATH_CAPACITY);
    if (!length || length >= PATH_CAPACITY) self[0] = 0;

    length = GetEnvironmentVariableW(TARGET_VARIABLE, target, PATH_CAPACITY);
    if (length && length < PATH_CAPACITY) {
        if (usable(target)) return;
        fail("CLAUDE_BASH_SHIM_TARGET does not name a bash.exe other than this relay", 127);
    }
    if (from_registry(target, HKEY_CURRENT_USER) || from_registry(target, HKEY_LOCAL_MACHINE)) return;
    if (from_variable_folder(target, L"ProgramFiles", L"\\Git\\bin\\bash.exe")) return;
    if (from_variable_folder(target, L"LOCALAPPDATA", L"\\Programs\\Git\\bin\\bash.exe")) return;
    if (from_git_on_path(target)) return;
    fail("cannot find Git for Windows; set CLAUDE_BASH_SHIM_TARGET to its bin\\bash.exe", 127);
}

// True when MSYS2 passes the argument through as written: no separator, no
// quote and nothing its wildcard expansion or @file expansion would act on.
static int is_plain(const wchar_t *arg) {
    if (!*arg || *arg == L'@' || *arg == L'~') return 0;
    for (; *arg; arg++) {
        wchar_t c = *arg;
        int safe = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                   c == L'_' || c == L'-' || c == L'+' || c == L'=' || c == L'.' || c == L'/' || c == L':' ||
                   c == L',' || c == L'%';
        if (!safe) return 0;
    }
    return 1;
}

// One argument in MSYS2's encoding: inside double quotes a backslash pair is
// one backslash and backslash-quote is a quote; every other character is
// literal and is not expanded.
static void put_argument(Text *line, const wchar_t *arg) {
    put_char(line, L' ');
    if (is_plain(arg)) {
        put(line, arg, wcslen(arg));
        return;
    }
    put_char(line, L'"');
    for (; *arg; arg++) {
        if (*arg == L'\\' || *arg == L'"') put_char(line, L'\\');
        put_char(line, *arg);
    }
    put_char(line, L'"');
}

// Index of the command string of `bash [options] -c string [name [args]]`,
// or 0 when the invocation has none. Mirrors bash's own option scan: single
// letter options cluster, -o and -O (and their + forms) take the next
// argument, as do the long options that name a file.
static int script_index(int argc, wchar_t **argv) {
    int command = 0;
    int i = 1;
    for (; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (a[0] == L'-' && a[1] == L'-') {
            if (!a[2]) {
                i++;
                break;
            }
            if (!wcscmp(a, L"--rcfile") || !wcscmp(a, L"--init-file")) i++;
            continue;
        }
        if ((a[0] == L'-' || a[0] == L'+') && a[1]) {
            int takes_value = 0;
            for (const wchar_t *c = a + 1; *c; c++) {
                if (*c == L'c' && a[0] == L'-') command = 1;
                if (*c == L'o' || *c == L'O') takes_value = 1;
            }
            if (takes_value) i++;
            continue;
        }
        if (a[0] == L'-' && !a[1]) i++;
        break;
    }
    return command && i < argc ? i : 0;
}

static BOOL WINAPI leave_signals_to_bash(DWORD event) {
    (void)event;
    return TRUE;
}

int wmain(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 1) fail("cannot read the command line", 126);

    static wchar_t target[PATH_CAPACITY];
    find_target(target);

    Text line = {0};
    put_char(&line, L'"');
    put(&line, target, wcslen(target));
    put_char(&line, L'"');

    int script = script_index(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (i != script) {
            put_argument(&line, argv[i]);
            continue;
        }
        if (wcslen(argv[i]) > VARIABLE_LIMIT) fail("the -c script is longer than Windows allows in one environment variable", 126);
        if (!SetEnvironmentVariableW(SCRIPT_VARIABLE, argv[i])) fail("cannot hand the -c script to bash", 126);
        put_argument(&line, RELAY_SCRIPT);
    }

    STARTUPINFOW startup;
    GetStartupInfoW(&startup);
    PROCESS_INFORMATION process;
    SetConsoleCtrlHandler(leave_signals_to_bash, TRUE);
    if (!CreateProcessW(target, line.data, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
        fail("cannot start the real bash; set CLAUDE_BASH_SHIM_TARGET to its bin\\bash.exe", 127);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 126;
    GetExitCodeProcess(process.hProcess, &code);
    ExitProcess(code);
}
