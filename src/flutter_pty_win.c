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

// Bounded, thread-safe byte ring buffer. The ConPTY output reader thread
// produces into it (never blocks: drops on overflow); a separate Dart-comms
// thread consumes from it (blocks on data / stop event). This separates
// the kernel I/O from the Dart message-port path so the read thread
// cannot be stuck behind `Dart_PostCObject_DL` while the ConPTY needs it
// to drain conout to make progress.
typedef struct RingBuffer
{
    uint8_t *data;
    size_t capacity;
    size_t head;     // next write position
    size_t tail;     // next read position
    size_t count;    // bytes currently buffered
    CRITICAL_SECTION lock;
    HANDLE hDataEvent;   // manual-reset, signaled when count > 0
    HANDLE hStopEvent;   // shared with both threads; signaled to drain and exit
} RingBuffer;

static void ring_init(RingBuffer *rb, size_t capacity, HANDLE hStopEvent)
{
    rb->data = (uint8_t *)malloc(capacity);
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    InitializeCriticalSection(&rb->lock);
    rb->hDataEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    rb->hStopEvent = hStopEvent;
}

static void ring_destroy(RingBuffer *rb)
{
    if (rb == NULL) return;
    if (rb->hDataEvent != NULL) CloseHandle(rb->hDataEvent);
    if (rb->data != NULL) free(rb->data);
    DeleteCriticalSection(&rb->lock);
}

// Producer. Never blocks. Returns the number of bytes actually written
// (may be < len if the buffer is full — excess bytes are dropped, which is
// acceptable for terminal output and is the whole point of having a
// bounded buffer between the I/O and Dart paths).
static size_t ring_write(RingBuffer *rb, const uint8_t *src, size_t len)
{
    size_t written = 0;

    EnterCriticalSection(&rb->lock);
    while (written < len && rb->count < rb->capacity)
    {
        size_t to_write = rb->capacity - rb->count;
        size_t remaining = len - written;
        if (to_write > remaining) to_write = remaining;

        size_t first_chunk = rb->capacity - rb->head;
        if (first_chunk > to_write) first_chunk = to_write;
        memcpy(rb->data + rb->head, src + written, first_chunk);

        size_t second_chunk = to_write - first_chunk;
        if (second_chunk > 0)
        {
            memcpy(rb->data, src + written + first_chunk, second_chunk);
        }

        rb->head = (rb->head + to_write) % rb->capacity;
        rb->count += to_write;
        written += to_write;
    }
    bool became_nonempty = rb->count > 0;
    LeaveCriticalSection(&rb->lock);

    if (became_nonempty) SetEvent(rb->hDataEvent);

    return written;
}

// Consumer. Blocks until at least 1 byte is available OR the stop event
// is signaled. Returns the number of bytes read; 0 means stop was signaled
// or an error occurred (caller should check GetLastError / stop state).
//
// IMPORTANT: the event-set/reset transitions must happen INSIDE the
// lock, otherwise a producer write that lands between the consumer's
// "drain to empty" and "reset event" steps will have its SetEvent
// clobbered by the consumer's reset, deadlocking the next ring_read.
static size_t ring_read(RingBuffer *rb, uint8_t *dst, size_t max)
{
    HANDLE handles[] = { rb->hDataEvent, rb->hStopEvent };
    DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1) return 0;  // stop
    if (result != WAIT_OBJECT_0) return 0;        // error

    EnterCriticalSection(&rb->lock);
    size_t to_read = rb->count;
    if (to_read > max) to_read = max;

    size_t first_chunk = rb->capacity - rb->tail;
    if (first_chunk > to_read) first_chunk = to_read;
    memcpy(dst, rb->data + rb->tail, first_chunk);

    size_t second_chunk = to_read - first_chunk;
    if (second_chunk > 0)
    {
        memcpy(dst + first_chunk, rb->data, second_chunk);
    }

    rb->tail = (rb->tail + to_read) % rb->capacity;
    rb->count -= to_read;
    // Decide on the event state UNDER the lock so the producer can't
    // write+SetEvent between our decision and the actual reset.
    if (rb->count == 0) ResetEvent(rb->hDataEvent);
    LeaveCriticalSection(&rb->lock);

    return to_read;
}

typedef struct ReadLoopOptions
{
    HANDLE fd;

    BOOL ackRead;

    HANDLE hMutex;

    RingBuffer *ring;

    HANDLE hStopEvent;

} ReadLoopOptions;

// Per-read state. OVERLAPPED is required to be the FIRST member so
// CONTAINING_RECORD in the completion routine recovers the per-call
// buffer pointer and the shared ring buffer. Keeping it on the read
// thread's stack is safe because we wait for the I/O to complete (or
// cancel) before the stack frame goes out of scope.
typedef struct ReadRequest
{
    OVERLAPPED ovl;
    char *buffer;
    RingBuffer *ring;
} ReadRequest;

// ReadFileEx completion routine. Runs in the context of the read thread
// when it enters an alertable wait state. Pushes the bytes into the ring
// buffer; the dart-comms thread is the one that eventually posts to the
// Dart message port, so this routine never blocks on Dart.
static VOID CALLBACK read_completion_routine(
    DWORD errCode,
    DWORD bytesRead,
    LPOVERLAPPED ovl)
{
    ReadRequest *req = (ReadRequest *)CONTAINING_RECORD(ovl, ReadRequest, ovl);
    if (errCode == 0 && bytesRead > 0)
    {
        ring_write(req->ring, (const uint8_t *)req->buffer, bytesRead);
    }
    // On cancellation (errCode == ERROR_OPERATION_ABORTED) or EOF
    // (bytesRead == 0 with errCode == 0) we just return; the read
    // thread's WaitForSingleObjectEx has already returned and will
    // break the loop on the next iteration's stop check.
}

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[4096];
    ReadRequest req;
    req.buffer = buffer;
    req.ring = options->ring;

    // NOTE: CreatePipe() does not set FILE_FLAG_OVERLAPPED, so
    // ReadFileEx() with a NULL completion routine would run
    // synchronously and block the thread — exactly the behavior we
    // need to avoid. Passing a real completion routine makes the I/O
    // properly asynchronous even on a non-overlapped pipe handle: the
    // system calls the APC when the I/O finishes and the thread is
    // in an alertable wait.
    while (1)
    {
        if (WaitForSingleObject(options->hStopEvent, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        ZeroMemory(&req.ovl, sizeof(req.ovl));

        if (!ReadFileEx(options->fd, buffer, sizeof(buffer), &req.ovl, read_completion_routine))
        {
            // Synchronous failure: pipe closed, invalid handle, etc.
            break;
        }

        // Alertable wait: returns when EITHER the stop event is signaled
        // OR the I/O completes (via the read_completion_routine APC).
        // WAIT_IO_COMPLETION (= 0xC0) is the success path; WAIT_OBJECT_0
        // means we were asked to stop and need to cancel the in-flight
        // read.
        DWORD waitResult = WaitForSingleObjectEx(options->hStopEvent, INFINITE, TRUE);
        if (waitResult == WAIT_OBJECT_0)
        {
            // Stop requested. Cancel the in-flight read so the kernel
            // releases the pipe; let the cancellation APC drain so we
            // don't leak the OVERLAPPED slot.
            CancelIoEx(options->fd, &req.ovl);
            SleepEx(0, TRUE);
            break;
        }
        // Otherwise WAIT_IO_COMPLETION: read is done, completion routine
        // already wrote the bytes to the ring buffer.
    }

    return 0;
}

typedef struct DartCommsOptions
{
    RingBuffer *ring;
    Dart_Port port;
    HANDLE hMutex;
    BOOL ackRead;
    HANDLE hStopEvent;
} DartCommsOptions;

static DWORD WINAPI dart_comms_loop(LPVOID arg)
{
    DartCommsOptions *options = (DartCommsOptions *)arg;

    uint8_t buffer[4096];

    while (1)
    {
        size_t bytesRead = ring_read(options->ring, buffer, sizeof(buffer));
        if (bytesRead == 0)
        {
            // 0 from ring_read means stop signaled or error. Either way,
            // we're done. Also exit if the stop event is signaled
            // (ring_read could in theory return 0 on a spurious wakeup
            // if the data event was reset; the stop check is cheap and
            // makes the loop robust).
            if (WaitForSingleObject(options->hStopEvent, 0) == WAIT_OBJECT_0)
            {
                break;
            }
            continue;
        }

        // ackRead: gate posting until Dart signals it's consumed the
        // previous batch. This replaces the per-read mutex wait in the
        // old read_loop; the gating now sits between the ring buffer
        // and Dart, where blocking is acceptable (the read thread is
        // independent and keeps draining the ConPTY).
        if (options->ackRead)
        {
            WaitForSingleObject(options->hMutex, INFINITE);
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = (int)bytesRead;
        result.value.as_typed_data.values = buffer;

        Dart_PostCObject_DL(options->port, &result);
    }

    return 0;
}

static HANDLE start_read_thread(HANDLE fd, RingBuffer *ring, HANDLE hStopEvent)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    options->fd = fd;
    options->hMutex = NULL;        // gating now lives in dart_comms_loop
    options->ackRead = FALSE;      // ditto
    options->ring = ring;
    options->hStopEvent = hStopEvent;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, read_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
        return NULL;
    }
    return thread;
}

static HANDLE start_dart_comms_thread(RingBuffer *ring, Dart_Port port, HANDLE hMutex, BOOL ackRead, HANDLE hStopEvent)
{
    DartCommsOptions *options = malloc(sizeof(DartCommsOptions));

    options->ring = ring;
    options->port = port;
    options->hMutex = hMutex;
    options->ackRead = ackRead;
    options->hStopEvent = hStopEvent;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, dart_comms_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
        return NULL;
    }
    return thread;
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

    // Async I/O wiring. The read thread reads from conout into the ring
    // buffer; a separate Dart-comms thread drains the ring and posts to
    // Dart. pty_close uses hReadThread / hDartCommsThread to wait for
    // clean shutdown before calling ClosePseudoConsole.
    HANDLE hReadThread;
    HANDLE hDartCommsThread;
    HANDLE hStopEvent;
    RingBuffer *ring;

} PtyHandle;

static char *error_message = NULL;

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

    HANDLE hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (hStopEvent == NULL)
    {
        error_message = "Failed to create stop event";
        CloseHandle(mutex);
        return NULL;
    }

    RingBuffer *ring = (RingBuffer *)malloc(sizeof(RingBuffer));
    if (ring == NULL)
    {
        error_message = "Failed to allocate ring buffer";
        CloseHandle(hStopEvent);
        CloseHandle(mutex);
        return NULL;
    }
    // 64 KiB ring = ~16x the typical 4 KiB pipe buffer. Enough headroom
    // for a bursty shell (e.g. `cat` of a large file) without forcing
    // the read thread to block on Dart_PostCObject_DL, but small enough
    // to bound memory and drop promptly when the consumer stalls.
    ring_init(ring, 64 * 1024, hStopEvent);

    HANDLE hReadThread = start_read_thread(outputReadSide, ring, hStopEvent);
    HANDLE hDartCommsThread = start_dart_comms_thread(ring, options->stdout_port, mutex, options->ackRead, hStopEvent);

    start_wait_exit_thread(processInfo.hProcess, options->exit_port, mutex);

    PtyHandle *pty = malloc(sizeof(PtyHandle));

    if (pty == NULL)
    {
        error_message = "Failed to allocate pty handle";
        CloseHandle(hStopEvent);
        CloseHandle(mutex);
        ring_destroy(ring);
        free(ring);
        return NULL;
    }

    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->hPty = hPty;
    pty->hProcess = processInfo.hProcess;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->ackRead = options->ackRead;
    pty->hMutex = mutex;
    pty->hReadThread = hReadThread;
    pty->hDartCommsThread = hDartCommsThread;
    pty->hStopEvent = hStopEvent;
    pty->ring = ring;

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

    return;
}

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }

    // Shutdown order matters (see microsoft/terminal Discussion #17716 —
    // ConPTY's ClosePseudoConsole blocks while conout is full and the
    // OpenConsole RenderThread holds the ticket lock; the kernel-level
    // fix is to keep conout drained while ClosePseudoConsole runs, and
    // to cancel any in-flight read so the read thread is unblocked
    // before the ConPTY tries to finalize):
    //
    //   1. Signal both worker threads to stop (read + dart-comms).
    //   2. Cancel any in-flight overlapped ReadFileEx on conout.
    //   3. Wait for the read thread to exit. With the ring buffer
    //      decoupling the Dart port, the read thread is no longer
    //      gated on Dart_PostCObject_DL, so once the cancellation
    //      arrives it exits within microseconds. The ConPTY
    //      RenderThread's WriteFile then sees the pipe go away and
    //      releases the ticket lock.
    //   4. ClosePseudoConsole — now unblocked because the read side
    //      has drained and the writer is no longer stuck.
    //   5. Close all handles. Wait briefly for the dart-comms thread
    //      (it's usually finishing a Dart_PostCObject_DL); if it
    //      doesn't, leave it to die on isolate shutdown.
    //   6. Free the ring buffer and the handle itself.

    if (handle->hStopEvent != NULL)
    {
        SetEvent(handle->hStopEvent);
    }
    if (handle->outputReadSide != NULL)
    {
        // CancelIoEx on the read side unblocks the alertable
        // WaitForSingleObjectEx in read_loop, which then breaks out.
        // Passing NULL cancels ALL pending I/O on the handle.
        CancelIoEx(handle->outputReadSide, NULL);
    }

    if (handle->hReadThread != NULL)
    {
        // Give the read thread a moment to react. In practice it's
        // immediate (single CancelIoEx + SleepEx(0,TRUE) inside the
        // loop), but allow a small ceiling for safety.
        WaitForSingleObject(handle->hReadThread, 1000);
        CloseHandle(handle->hReadThread);
        handle->hReadThread = NULL;
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

    if (handle->hDartCommsThread != NULL)
    {
        // The dart-comms thread is either waiting in ring_read (fast
        // exit on stop) or blocked in Dart_PostCObject_DL (waiting
        // for the Dart isolate to drain its message queue). If the
        // latter, we don't want to block ClosePseudoConsole's caller
        // (e.g. the UI thread). Give it a short window, then leak the
        // thread — it'll exit when the isolate is torn down and the
        // OS reclaims its resources. This is the only known case
        // where a worker thread outlives the PtyHandle.
        if (WaitForSingleObject(handle->hDartCommsThread, 500) != WAIT_OBJECT_0)
        {
            // Detach: don't CloseHandle, the thread is responsible
            // for freeing its own DartCommsOptions.
        }
        else
        {
            CloseHandle(handle->hDartCommsThread);
        }
        handle->hDartCommsThread = NULL;
    }

    if (handle->hStopEvent != NULL)
    {
        CloseHandle(handle->hStopEvent);
        handle->hStopEvent = NULL;
    }

    if (handle->ring != NULL)
    {
        ring_destroy(handle->ring);
        free(handle->ring);
        handle->ring = NULL;
    }

    free(handle);
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
