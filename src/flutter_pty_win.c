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

static char *error_message = NULL;

// =============================================================================
// Option A: dedicated read thread + dedicated write thread.
//
// Modeled on alacritty's tty/windows/{conpty,blocking}.rs:
//   * A reader OS thread does synchronous ReadFile() on the conout pipe
//     and posts each chunk to the Dart ReceivePort. It is the ONLY
//     consumer of conout, so ClosePseudoConsole can drain conout
//     without deadlocking on the OpenConsole RenderThread ticket lock
//     (microsoft/terminal Discussion #17716).
//   * A writer OS thread drains a small CV-protected queue of
//     WriteRequests and calls WriteFile() on the conin pipe. The Dart
//     side pty_write() only enqueues (non-blocking) so the UI thread
//     is never blocked on a stale or orphaned ConPTY input pipe.
//   * pty_close() executes a strict teardown order — see the comment
//     in that function for why each step matters.
// =============================================================================

// One pending write from Dart. Heap-allocated so the UI thread (which
// owns the lifetime of pty_write) can enqueue and return immediately
// without blocking on the writer thread.
typedef struct WriteRequest
{
    uint8_t *data;       // heap-owned; freed by the writer thread
    DWORD length;
    struct WriteRequest *next;
} WriteRequest;

// Thread-safe FIFO of WriteRequests. Unbounded by design — terminal
// input is bursty but tiny (keystrokes, pastes < 1 MB). If the queue
// grows too large in practice we'll add a cap, but a CV + linked list
// is the simplest correct design.
typedef struct WriteQueue
{
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    WriteRequest *head;
    WriteRequest *tail;
    BOOL stopped;     // set by pty_close; writer drains and exits
} WriteQueue;

typedef struct PtyHandle
{
    // ConPTY plumbing
    HPCON hPty;
    HANDLE hProcess;
    DWORD dwProcessId;

    // Pipe halves we keep long-term. The other halves are closed
    // immediately after CreatePseudoConsole / CreateProcess (per
    // Microsoft's "Creating a Pseudoconsole session" guidance).
    HANDLE inputWriteSide;   // given to the writer thread
    HANDLE outputReadSide;   // given to the reader thread

    // Worker threads
    HANDLE hReadThread;
    HANDLE hWriteThread;
    WriteQueue writeQueue;

    // ackRead backpressure (optional; mirrors flutter_pty's old flag).
    BOOL ackRead;
    HANDLE hAckMutex;        // semaphore, 0/1, used only if ackRead

    // Dart ports. Cached at create time so the threads don't need to
    // reach into PtyOptions (which may be on the caller's stack).
    Dart_Port stdout_port;
    Dart_Port exit_port;
} PtyHandle;

typedef struct WaitExitOptions
{
    HANDLE pid;
    Dart_Port port;
} WaitExitOptions;

static DWORD WINAPI wait_exit_thread(LPVOID arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    DWORD exit_code = 0;
    WaitForSingleObject(options->pid, INFINITE);
    GetExitCodeProcess(options->pid, &exit_code);
    CloseHandle(options->pid);

    Dart_PostInteger_DL(options->port, exit_code);
    free(options);
    return 0;
}

static void start_wait_exit_thread(HANDLE pid, Dart_Port port)
{
    // Duplicate the process handle so the wait thread can close its
    // copy without invalidating PtyHandle->hProcess (which the writer
    // thread peeks at via WaitForSingleObject to short-circuit writes
    // to a dead process).
    HANDLE thread_pid;
    if (!DuplicateHandle(GetCurrentProcess(), pid, GetCurrentProcess(),
                         &thread_pid, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        error_message = "Failed to duplicate process handle for wait thread";
        return;
    }

    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));
    if (options == NULL)
    {
        CloseHandle(thread_pid);
        error_message = "Failed to allocate wait_exit options";
        return;
    }
    options->pid = thread_pid;
    options->port = port;

    DWORD thread_id;
    HANDLE thread = CreateThread(NULL, 0, wait_exit_thread, options, 0, &thread_id);
    if (thread == NULL)
    {
        free(options);
        CloseHandle(thread_pid);
    }
    else
    {
        CloseHandle(thread);  // we don't need to wait for the wait thread
    }
}

typedef struct ReadLoopOptions
{
    HANDLE fd;
    Dart_Port port;
    BOOL ackRead;
    HANDLE hAckMutex;
} ReadLoopOptions;

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;
    HANDLE fd = options->fd;
    Dart_Port port = options->port;

    // Stack buffer; small enough not to bloat the thread, large enough
    // to coalesce typical terminal bursts.
    char buffer[4096];

    while (1)
    {
        // ReadFile is the only blocking call here. The read is unblocked
        // by pty_close, which closes outputReadSide — that makes
        // ReadFile return with bytesRead==0 and we break out.
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(fd, buffer, sizeof(buffer), &bytesRead, NULL);
        if (!ok || bytesRead == 0)
        {
            // EOF (bytesRead==0, ok==TRUE) or pipe broken / handle
            // closed. Either way we're done — the parent will (or has)
            // already called ClosePseudoConsole and the pipe is gone.
            break;
        }

        // ackRead: gate posting on the previous batch being ack'd by
        // Dart. Without this, the reader could race ahead of the
        // engine and the Dart-side message port could grow without
        // bound under sustained bursty output.
        if (options->ackRead)
        {
            WaitForSingleObject(options->hAckMutex, INFINITE);
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = (intptr_t)bytesRead;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        Dart_PostCObject_DL(port, &result);
    }

    free(options);
    return 0;
}

typedef struct WriteLoopOptions
{
    HANDLE fd;
    WriteQueue *queue;
} WriteLoopOptions;

static DWORD WINAPI write_loop(LPVOID arg)
{
    WriteLoopOptions *options = (WriteLoopOptions *)arg;
    HANDLE fd = options->fd;
    WriteQueue *q = options->queue;

    while (1)
    {
        EnterCriticalSection(&q->lock);

        // Wait for either data to enqueue or pty_close to flip stopped.
        while (q->head == NULL && !q->stopped)
        {
            SleepConditionVariableCS(&q->cv, &q->lock, INFINITE);
        }

        if (q->head == NULL && q->stopped)
        {
            LeaveCriticalSection(&q->lock);
            break;
        }

        WriteRequest *req = q->head;
        q->head = req->next;
        if (q->head == NULL)
        {
            q->tail = NULL;
        }

        LeaveCriticalSection(&q->lock);

        // WriteFile is allowed to block here — that's the whole point
        // of the writer thread. If the shell is dead, pty_write's
        // pre-check (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0)
        // would normally have bailed before enqueuing, so this is
        // mostly relevant for a paste mid-flight when the shell exits
        // or pty_close runs concurrently. WriteFile returns
        // ERROR_BROKEN_PIPE in those cases, which we just ignore —
        // the dropped bytes are not actionable.
        DWORD bytesWritten = 0;
        WriteFile(fd, req->data, req->length, &bytesWritten, NULL);

        free(req->data);
        free(req);
    }

    // Drain any remaining requests that arrived after stopped was
    // observed (race window) so we don't leak them.
    EnterCriticalSection(&q->lock);
    WriteRequest *r = q->head;
    q->head = NULL;
    q->tail = NULL;
    LeaveCriticalSection(&q->lock);
    while (r != NULL)
    {
        WriteRequest *next = r->next;
        free(r->data);
        free(r);
        r = next;
    }

    free(options);
    return 0;
}

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
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
        CloseHandle(inputReadSide);
        CloseHandle(inputWriteSide);
        return NULL;
    }

    COORD size;
    size.X = (SHORT)options->cols;
    size.Y = (SHORT)options->rows;

    HPCON hPty;
    HRESULT result = CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPty);
    if (FAILED(result))
    {
        error_message = "Failed to create pseudo console";
        CloseHandle(inputReadSide);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        CloseHandle(outputWriteSide);
        return NULL;
    }

    // Microsoft docs: "Upon completion of the CreateProcess call ... the
    // handles given during creation should be freed from this process.
    // This will decrease the reference count on the underlying device
    // object and allow I/O operations to properly detect a broken
    // channel when the pseudoconsole session closes its copy of the
    // handles."
    CloseHandle(inputReadSide);
    CloseHandle(outputWriteSide);

    STARTUPINFOEX startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = NULL;
    startupInfo.StartupInfo.hStdOutput = NULL;
    startupInfo.StartupInfo.hStdError = NULL;

    SIZE_T bytesRequired = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);
    startupInfo.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(bytesRequired);
    if (startupInfo.lpAttributeList == NULL)
    {
        error_message = "Failed to allocate proc thread attribute list";
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        return NULL;
    }

    BOOL ok = InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &bytesRequired);
    if (!ok)
    {
        error_message = "Failed to initialize proc thread attribute list";
        free(startupInfo.lpAttributeList);
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
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
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        free(startupInfo.lpAttributeList);
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
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

    if (command != NULL) free(command);
    if (environment_block != NULL) free(environment_block);
    if (working_directory != NULL) free(working_directory);
    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    free(startupInfo.lpAttributeList);
    CloseHandle(processInfo.hThread);

    if (!ok)
    {
        error_message = "Failed to create process";
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        return NULL;
    }

    PtyHandle *pty = (PtyHandle *)calloc(1, sizeof(PtyHandle));
    if (pty == NULL)
    {
        error_message = "Failed to allocate pty handle";
        CloseHandle(processInfo.hProcess);
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        return NULL;
    }

    pty->hPty = hPty;
    pty->hProcess = processInfo.hProcess;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->ackRead = options->ackRead;
    pty->stdout_port = options->stdout_port;
    pty->exit_port = options->exit_port;

    InitializeCriticalSection(&pty->writeQueue.lock);
    InitializeConditionVariable(&pty->writeQueue.cv);
    pty->writeQueue.head = NULL;
    pty->writeQueue.tail = NULL;
    pty->writeQueue.stopped = FALSE;

    if (options->ackRead)
    {
        // Initial=1, max=1: the first read passes without requiring a
        // prior ack; each subsequent read consumes a permit, which
        // pty_ack_read reposts. Matches flutter_pty's pre-Option-B
        // semantics.
        pty->hAckMutex = CreateSemaphore(NULL, 1, 1, NULL);
        if (pty->hAckMutex == NULL)
        {
            error_message = "Failed to create ack semaphore";
            CloseHandle(processInfo.hProcess);
            ClosePseudoConsole(hPty);
            CloseHandle(inputWriteSide);
            CloseHandle(outputReadSide);
            DeleteCriticalSection(&pty->writeQueue.lock);
            free(pty);
            return NULL;
        }
    }
    else
    {
        pty->hAckMutex = NULL;
    }

    // Start the writer thread first. It can only consume requests we
    // enqueue from pty_write, so its early life is just a cv wait.
    WriteLoopOptions *wopts = (WriteLoopOptions *)malloc(sizeof(WriteLoopOptions));
    wopts->fd = inputWriteSide;
    wopts->queue = &pty->writeQueue;
    DWORD write_tid;
    pty->hWriteThread = CreateThread(NULL, 0, write_loop, wopts, 0, &write_tid);
    if (pty->hWriteThread == NULL)
    {
        error_message = "Failed to create writer thread";
        if (pty->hAckMutex != NULL) CloseHandle(pty->hAckMutex);
        CloseHandle(processInfo.hProcess);
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        DeleteCriticalSection(&pty->writeQueue.lock);
        free(pty);
        return NULL;
    }

    // Start the reader thread.
    ReadLoopOptions *ropts = (ReadLoopOptions *)malloc(sizeof(ReadLoopOptions));
    ropts->fd = outputReadSide;
    ropts->port = options->stdout_port;
    ropts->ackRead = options->ackRead;
    ropts->hAckMutex = pty->hAckMutex;
    DWORD read_tid;
    pty->hReadThread = CreateThread(NULL, 0, read_loop, ropts, 0, &read_tid);
    if (pty->hReadThread == NULL)
    {
        error_message = "Failed to create reader thread";
        // Stop the writer thread we already started.
        EnterCriticalSection(&pty->writeQueue.lock);
        pty->writeQueue.stopped = TRUE;
        LeaveCriticalSection(&pty->writeQueue.lock);
        WakeConditionVariable(&pty->writeQueue.cv);
        WaitForSingleObject(pty->hWriteThread, INFINITE);
        CloseHandle(pty->hWriteThread);
        if (pty->hAckMutex != NULL) CloseHandle(pty->hAckMutex);
        CloseHandle(processInfo.hProcess);
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        DeleteCriticalSection(&pty->writeQueue.lock);
        free(pty);
        return NULL;
    }

    // Wait-for-exit thread posts the exit code to Dart.
    start_wait_exit_thread(processInfo.hProcess, options->exit_port);

    return pty;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    if (handle == NULL || length <= 0)
    {
        return;
    }

    // Pre-flight liveness check. The shell process is the one we
    // launched via CreateProcess; once it exits, the kernel keeps the
    // ConPTY input pipe open until ClosePseudoConsole is called. If
    // we don't bail here, we'd enqueue bytes that the writer thread
    // will then attempt to WriteFile — those will eventually
    // ERROR_BROKEN_PIPE once ClosePseudoConsole runs, but in the
    // meantime they consume queue space and delay pty_close (the
    // writer needs to drain the queue before exiting). A 0-timeout
    // WaitForSingleObject on hProcess is cheap and prevents the
    // unbounded queue growth that otherwise occurs when the user
    // hammers keys into a dead shell.
    if (handle->hProcess == NULL ||
        WaitForSingleObject(handle->hProcess, 0) == WAIT_OBJECT_0)
    {
        return;
    }

    WriteRequest *req = (WriteRequest *)malloc(sizeof(WriteRequest));
    if (req == NULL)
    {
        return;
    }
    req->data = (uint8_t *)malloc((size_t)length);
    if (req->data == NULL)
    {
        free(req);
        return;
    }
    memcpy(req->data, buffer, (size_t)length);
    req->length = (DWORD)length;
    req->next = NULL;

    EnterCriticalSection(&handle->writeQueue.lock);
    if (handle->writeQueue.stopped)
    {
        // pty_close has been called; drop the request on the floor
        // rather than handing it to a writer that is about to exit.
        LeaveCriticalSection(&handle->writeQueue.lock);
        free(req->data);
        free(req);
        return;
    }
    if (handle->writeQueue.tail == NULL)
    {
        handle->writeQueue.head = req;
    }
    else
    {
        handle->writeQueue.tail->next = req;
    }
    handle->writeQueue.tail = req;
    LeaveCriticalSection(&handle->writeQueue.lock);

    WakeConditionVariable(&handle->writeQueue.cv);
}

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }

    // Teardown order matters. The two failure modes we're avoiding are:
    //
    //   (A) ClosePseudoConsole deadlocks because conout is full and the
    //       OpenConsole RenderThread holds the ticket lock. Fix: keep
    //       the reader thread draining conout while ClosePseudoConsole
    //       runs, and only close conout AFTER it returns.
    //
    //   (B) A post-exit keystroke blocks the UI thread because
    //       WriteFile on an orphaned ConPTY input pipe never returns
    //       (the kernel holds the pipe until ClosePseudoConsole).
    //       Fix: the writer thread absorbs the block; the UI thread's
    //       pty_write just enqueues and returns.
    //
    // Sequence:
    //   1. Flip the write queue to "stopped" and wake the writer.
    //      The writer will drain any pending requests and exit. The
    //      UI thread is never blocked because pty_write only enqueues.
    //   2. Wait for the writer thread to finish. Bounded — once the
    //      queue is empty and stopped is set, the writer's cv wait
    //      returns and the thread exits.
    //   3. CloseHandle(inputWriteSide). The shell may still hold its
    //      end; that's fine — the kernel still tears down the pipe
    //      when both sides close OR when ClosePseudoConsole runs.
    //   4. ClosePseudoConsole(hPty). This terminates the attached
    //      shell process, waits for conhost to flush conout, and
    //      returns. The reader thread is still alive and draining
    //      conout, so (A) cannot happen.
    //   5. CloseHandle(outputReadSide). The reader thread's ReadFile
    //      returns immediately (handle closed). It exits its loop.
    //   6. Wait for the reader thread. Bounded — it's already past
    //      ReadFile and about to free its options struct.
    //   7. Close remaining handles (hReadThread, hWriteThread,
    //      hProcess, hAckMutex if any) and free the handle.

    // (1) + (2)
    EnterCriticalSection(&handle->writeQueue.lock);
    handle->writeQueue.stopped = TRUE;
    LeaveCriticalSection(&handle->writeQueue.lock);
    WakeConditionVariable(&handle->writeQueue.cv);
    if (handle->hWriteThread != NULL)
    {
        WaitForSingleObject(handle->hWriteThread, INFINITE);
        CloseHandle(handle->hWriteThread);
        handle->hWriteThread = NULL;
    }

    // (3)
    if (handle->inputWriteSide != NULL)
    {
        CloseHandle(handle->inputWriteSide);
        handle->inputWriteSide = NULL;
    }

    // (4)
    if (handle->hPty != NULL)
    {
        ClosePseudoConsole(handle->hPty);
        handle->hPty = NULL;
    }

    // (5)
    if (handle->outputReadSide != NULL)
    {
        CloseHandle(handle->outputReadSide);
        handle->outputReadSide = NULL;
    }

    // (6)
    if (handle->hReadThread != NULL)
    {
        WaitForSingleObject(handle->hReadThread, INFINITE);
        CloseHandle(handle->hReadThread);
        handle->hReadThread = NULL;
    }

    // (7)
    if (handle->hProcess != NULL)
    {
        CloseHandle(handle->hProcess);
        handle->hProcess = NULL;
    }
    if (handle->hAckMutex != NULL)
    {
        CloseHandle(handle->hAckMutex);
        handle->hAckMutex = NULL;
    }
    DeleteCriticalSection(&handle->writeQueue.lock);

    free(handle);
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle != NULL && handle->ackRead && handle->hAckMutex != NULL)
    {
        ReleaseSemaphore(handle->hAckMutex, 1, NULL);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    if (handle == NULL || handle->hPty == NULL)
    {
        return -1;
    }
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    return ResizePseudoConsole(handle->hPty, size);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return -1;
    }
    return (int)handle->dwProcessId;
}

FFI_PLUGIN_EXPORT char *pty_error()
{
    return error_message;
}
