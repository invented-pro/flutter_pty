#include <stdio.h>
#include <Windows.h>

#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

static LPWSTR build_command(char *executable, char **arguments)
{
    int command_length = 0;

    if (executable != NULL)
    {
        command_length += (int)strlen(executable);
    }

    // CRITICAL: arguments[0] is the executable itself (set by the Dart
    // side as `argv.elementAt(0).value = executable.toNativeUtf8()`).
    // When CreateProcessW is called with `lpApplicationName = NULL` and a
    // multi-token `lpCommandLine`, Windows sets the child's argv[0] from
    // the FIRST token of lpCommandLine. So we must NOT include
    // arguments[0] in the concatenation — that's what this function
    // used to do, and it caused pwsh.exe to receive a duplicate of its
    // own path as argv[1], which it then interpreted as a `-File
    // <pwsh.exe>` script argument and died with
    //   "Processing -File 'C:\Program Files\…\pwsh.exe' failed because
    //    the file does not have a '.ps1' extension."
    // Skip arguments[0]; start the space-separated concatenation at
    // arguments[1] so lpCommandLine = "<executable> <args[1]> <args[2]> …"
    // and Windows parses argv = [executable, args[1], args[2], …].
    if (arguments != NULL)
    {
        int i = 1; // <-- skip arguments[0]

        while (arguments[i] != NULL)
        {
            command_length += (int)strlen(arguments[i]) + 1;
            i++;
        }
    }

    LPWSTR command = malloc((command_length + 1) * sizeof(WCHAR));

    if (command != NULL)
    {
        int i = 0;

        if (executable != NULL)
        {
            int j = 0;

            while (executable[j] != 0)
            {
                command[i] = (WCHAR)executable[j];
                i++;
                j++;
            }
        }

        if (arguments != NULL)
        {
            int j = 1; // <-- skip arguments[0]

            while (arguments[j] != NULL)
            {
                command[i++] = ' ';

                int k = 0;

                while (arguments[j][k] != 0)
                {
                    command[i] = (WCHAR)arguments[j][k];
                    i++;
                    k++;
                }

                j++;
            }
        }

        command[i] = 0;
    }

    return command;
}

static LPWSTR build_environment(char **environment)
{
    LPWSTR environment_block = NULL;
    int environment_block_length = 0;

    if (environment != NULL)
    {
        int i = 0;

        while (environment[i] != NULL)
        {
            environment_block_length += (int)strlen(environment[i]) + 1;
            i++;
        }
    }

    environment_block = malloc((environment_block_length + 1) * sizeof(WCHAR));

    if (environment_block != NULL)
    {
        int i = 0;

        if (environment != NULL)
        {
            int j = 0;

            while (environment[j] != NULL)
            {
                int k = 0;

                while (environment[j][k] != 0)
                {
                    environment_block[i] = (WCHAR)environment[j][k];
                    i++;
                    k++;
                }

                environment_block[i++] = 0;

                j++;
            }
        }

        environment_block[i] = 0;
    }

    return environment_block;
}

static LPWSTR build_working_directory(char *working_directory)
{
    if (working_directory == NULL)
    {
        return NULL;
    }

    int working_directory_length = (int)strlen(working_directory);

    LPWSTR working_directory_block = malloc((working_directory_length + 1) * sizeof(WCHAR));

    if (working_directory_block == NULL)
    {
        return NULL;
    }

    int i = 0;

    while (working_directory[i] != 0)
    {
        working_directory_block[i] = (WCHAR)working_directory[i++];
    }

    working_directory_block[i] = 0;

    return working_directory_block;
}

static char *error_message;

typedef struct ReadLoopOptions
{
    HANDLE fd;

    Dart_Port port;

    HANDLE hMutex;

    BOOL ackRead;

} ReadLoopOptions;

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[1024];

    while (1)
    {
        DWORD readlen = 0;

        if (options->ackRead)
        {
            WaitForSingleObject(options->hMutex, INFINITE);
        }

        BOOL ok = ReadFile(options->fd, buffer, sizeof(buffer), &readlen, NULL);

        if (!ok)
        {
            break;
        }

        if (readlen <= 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = readlen;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        Dart_PostCObject_DL(options->port, &result);
    }

    return 0;
}

static void start_read_thread(HANDLE fd, Dart_Port port, HANDLE mutex, BOOL ackRead)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    options->fd = fd;
    options->port = port;
    options->hMutex = mutex;
    options->ackRead = ackRead;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, read_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }
}

typedef struct WaitExitOptions
{
    HANDLE pid;

    Dart_Port port;

    HANDLE hMutex;
} WaitExitOptions;

static DWORD WINAPI wait_exit_thread(LPVOID arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    DWORD exit_code = 0;

    WaitForSingleObject(options->pid, INFINITE);

    GetExitCodeProcess(options->pid, &exit_code);

    CloseHandle(options->pid);
    CloseHandle(options->hMutex);

    Dart_PostInteger_DL(options->port, exit_code);

    return 0;
}

static void start_wait_exit_thread(HANDLE pid, Dart_Port port, HANDLE mutex)
{
    // Duplicate the process handle so the wait thread can close its copy
    // without invalidating the one stored in PtyHandle->hProcess. Without
    // this, the close below would leave hProcess pointing at a freed
    // handle, and our pty_write liveness check (WaitForSingleObject with
    // a 0 timeout) would never observe WAIT_OBJECT_0.
    HANDLE thread_pid;
    if (!DuplicateHandle(GetCurrentProcess(), pid, GetCurrentProcess(), &thread_pid, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        error_message = "Failed to duplicate process handle for wait thread";
        return;
    }

    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));

    options->pid = thread_pid;
    options->port = port;
    options->hMutex = mutex;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, wait_exit_thread, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }
}

typedef struct PtyHandle
{
    PHANDLE inputWriteSide;

    PHANDLE outputReadSide;

    HPCON hPty;

    HANDLE hProcess;

    DWORD dwProcessId;

    BOOL ackRead;

    HANDLE hMutex;

} PtyHandle;

static char *error_message = NULL;
{
    HANDLE inputReadSide = NULL;
    HANDLE inputWriteSide = NULL;

    HANDLE outputReadSide = NULL;
    HANDLE outputWriteSide = NULL;

    if (!CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0))
    {
        error_message = "Failed to create input pipe";
        return NULL;
    }

    if (!CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0))
    {
        error_message = "Failed to create output pipe";
        return NULL;
    }

    COORD size;

    size.X = options->cols;
    size.Y = options->rows;

    HPCON hPty;

    HRESULT result = CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPty);

    if (FAILED(result))
    {
        error_message = "Failed to create pseudo console";
        return NULL;
    }

    STARTUPINFOEX startupInfo;

    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.StartupInfo.cb = sizeof(startupInfo);

    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = NULL;
    startupInfo.StartupInfo.hStdOutput = NULL;
    startupInfo.StartupInfo.hStdError = NULL;

    SIZE_T bytesRequired;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);
    startupInfo.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(bytesRequired);

    BOOL ok = InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &bytesRequired);

    if (!ok)
    {
        error_message = "Failed to initialize proc thread attribute list";
        return NULL;
    }

    ok = UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hPty,
                                   sizeof(hPty),
                                   NULL,
                                   NULL);

    if (!ok)
    {
        error_message = "Failed to update proc thread attribute list";
        return NULL;
    }

    LPWSTR command = build_command(options->executable, options->arguments);

    LPWSTR environment_block = build_environment(options->environment);

    LPWSTR working_directory = build_working_directory(options->working_directory);

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    ok = CreateProcessW(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment_block,
                        working_directory,
                        &startupInfo.StartupInfo,
                        &processInfo);

    if (command != NULL)
    {
        free(command);
    }

    if (environment_block != NULL)
    {
        free(environment_block);
    }

    if (working_directory != NULL)
    {
        free(working_directory);
    }

    if (!ok)
    {
        error_message = "Failed to create process";
        DWORD error = GetLastError();
        printf("error no: %d\n", error);
        return NULL;
    }

    // free(startupInfo.lpAttributeList);

    // CloseHandle(processInfo.hThread);

    HANDLE mutex = CreateSemaphore(
        NULL, // default security attributes
        1,    // initial count
        1,    // maximum count
        NULL);

    start_read_thread(outputReadSide, options->stdout_port, mutex, options->ackRead);

    start_wait_exit_thread(processInfo.hProcess, options->exit_port, mutex);

    PtyHandle *pty = malloc(sizeof(PtyHandle));

    if (pty == NULL)
    {
        error_message = "Failed to allocate pty handle";
        return NULL;
    }

    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->hPty = hPty;
    pty->hProcess = processInfo.hProcess;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->ackRead = options->ackRead;
    pty->hMutex = mutex;

    return pty;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    DWORD bytesWritten;

    // Bail before WriteFile if the PTY's child process has already exited.
    // Without this, writing to an orphaned ConPTY input pipe blocks the
    // caller (UI thread) until something calls ClosePseudoConsole — and
    // nothing does, because we never invoke pty_close before the host
    // app tears down. The 0-timeout wait returns immediately: WAIT_OBJECT_0
    // means the process handle is signaled (= process dead).
    if (handle == NULL || handle->hProcess == NULL)
    {
        return;
    }
    if (WaitForSingleObject(handle->hProcess, 0) == WAIT_OBJECT_0)
    {
        return;
    }

    WriteFile(handle->inputWriteSide, buffer, length, &bytesWritten, NULL);

    FlushFileBuffers(handle->inputWriteSide);

    return;
}

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }
    if (handle->hPty != NULL)
    {
        ClosePseudoConsole(handle->hPty);
        handle->hPty = NULL;
    }
    if (handle->inputWriteSide != NULL)
    {
        CloseHandle(handle->inputWriteSide);
        handle->inputWriteSide = NULL;
    }
    if (handle->outputReadSide != NULL)
    {
        CloseHandle(handle->outputReadSide);
        handle->outputReadSide = NULL;
    }
    handle->hProcess = NULL;
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle->ackRead)
    {
        ReleaseSemaphore(handle->hMutex, 1, NULL);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    COORD size;

    size.X = cols;
    size.Y = rows;

    return ResizePseudoConsole(handle->hPty, size);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    return (int)handle->dwProcessId;
}

FFI_PLUGIN_EXPORT char *pty_error()
{
    return error_message;
}
