/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    sigtest.c

Abstract:

    This module implements the tests used to verify that user mode signals are
    functioning properly.

Author:

    Evan Green 31-Mar-2013

Environment:

    User Mode

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <osbase.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

//
// --------------------------------------------------------------------- Macros
//

#define DEBUG_PRINT(...)                                  \
    if (SignalTestVerbosity >= TestVerbosityDebug) {      \
        printf(__VA_ARGS__);                              \
    }

#define PRINT(...)                                        \
    if (SignalTestVerbosity >= TestVerbosityNormal) {     \
        printf(__VA_ARGS__);                              \
    }

#define PRINT_ERROR(...) fprintf(stderr, "sigtest: " __VA_ARGS__)

#define DEFAULT_OPERATION_COUNT 10
#define DEFAULT_CHILD_PROCESS_COUNT 3
#define DEFAULT_THREAD_COUNT 1

//
// ---------------------------------------------------------------- Definitions
//

#define SIGNAL_TEST_VERSION_MAJOR 1
#define SIGNAL_TEST_VERSION_MINOR 0

#define SIGNAL_TEST_USAGE                                                      \
    "Usage: sigtest [options] \n"                                              \
    "This utility hammers on signals. Options are:\n"                          \
    "  -c, --child-count <count> -- Set the number of child processes.\n"      \
    "  -i, --iterations <count> -- Set the number of operations to perform.\n" \
    "  -p, --threads <count> -- Set the number of threads to spin up to \n"    \
    "      simultaneously run the test.\n"                                     \
    "  -t, --test -- Set the test to perform. Valid values are all, \n"        \
    "      waitpid, sigchld, and quickwait.\n"                                 \
    "  --debug -- Print lots of information about what's happening.\n"         \
    "  --quiet -- Print only errors.\n"                                        \
    "  --help -- Print this help text and exit.\n"                             \
    "  --version -- Print the test version and exit.\n"                        \

#define SIGNAL_TEST_OPTIONS_STRING "c:i:t:p:"

//
// ------------------------------------------------------ Data Type Definitions
//

typedef enum _TEST_VERBOSITY {
    TestVerbosityQuiet,
    TestVerbosityNormal,
    TestVerbosityDebug
} TEST_VERBOSITY, *PTEST_VERBOSITY;

typedef enum _SIGNAL_TEST_TYPE {
    SignalTestAll,
    SignalTestWaitpid,
    SignalTestSigchld,
    SignalTestQuickWait,
} SIGNAL_TEST_TYPE, *PSIGNAL_TEST_TYPE;

//
// ----------------------------------------------- Internal Function Prototypes
//

ULONG
RunWaitpidTest (
    ULONG Iterations
    );

ULONG
RunSigchldTest (
    ULONG Iterations,
    ULONG ChildCount
    );

ULONG
RunQuickWaitTest (
    ULONG Iterations,
    ULONG ChildCount
    );

ULONG
TestWaitpid (
    BOOL BurnTimeInChild,
    BOOL BurnTimeInParent
    );

ULONG
TestSigchild (
    ULONG ChildCount,
    ULONG ChildAdditionalThreads,
    BOOL UseSigsuspend,
    BOOL ChildrenExitVoluntarily
    );

void
TestWaitpidChildSignalHandler (
    int Signal,
    siginfo_t *SignalInformation,
    void *Context
    );

void
TestSigchldRealtime1SignalHandler (
    int Signal,
    siginfo_t *SignalInformation,
    void *Context
    );

VOID
TestThreadSpinForever (
    PVOID Parameter
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Higher levels here print out more stuff.
//

TEST_VERBOSITY SignalTestVerbosity = TestVerbosityNormal;

struct option SignalTestLongOptions[] = {
    {"child-count", required_argument, 0, 'c'},
    {"iterations", required_argument, 0, 'i'},
    {"threads", required_argument, 0, 'p'},
    {"test", required_argument, 0, 't'},
    {"debug", no_argument, 0, 'd'},
    {"quiet", no_argument, 0, 'q'},
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'V'},
    {NULL, 0, 0, 0},
};

//
// These variables communicate between the signal handler and main function.
//

volatile ULONG ChildSignalsExpected;
volatile LONG ChildSignalPid;
volatile ULONG ChildSignalFailures;
volatile ULONG ChildProcessesReady;

//
// ------------------------------------------------------------------ Functions
//

int
main (
    int ArgumentCount,
    char **Arguments
    )

/*++

Routine Description:

    This routine implements the signal test program.

Arguments:

    ArgumentCount - Supplies the number of elements in the arguments array.

    Arguments - Supplies an array of strings. The array count is bounded by the
        previous parameter, and the strings are null-terminated.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    PSTR AfterScan;
    pid_t Child;
    INT ChildIndex;
    INT ChildProcessCount;
    pid_t *Children;
    INT Failures;
    BOOL IsParent;
    INT Iterations;
    INT Option;
    INT Status;
    SIGNAL_TEST_TYPE Test;
    INT Threads;

    Children = NULL;
    Failures = 0;
    ChildProcessCount = DEFAULT_CHILD_PROCESS_COUNT;
    Iterations = DEFAULT_OPERATION_COUNT;
    Test = SignalTestAll;
    Threads = DEFAULT_THREAD_COUNT;
    Status = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    srand(time(NULL));

    //
    // Process the control arguments.
    //

    while (TRUE) {
        Option = getopt_long(ArgumentCount,
                             Arguments,
                             SIGNAL_TEST_OPTIONS_STRING,
                             SignalTestLongOptions,
                             NULL);

        if (Option == -1) {
            break;
        }

        if ((Option == '?') || (Option == ':')) {
            Status = 1;
            goto MainEnd;
        }

        switch (Option) {
        case 'c':
            ChildProcessCount = strtol(optarg, &AfterScan, 0);
            if ((ChildProcessCount <= 0) || (AfterScan == optarg)) {
                PRINT_ERROR("Invalid child process count %s.\n", optarg);
                Status = 1;
                goto MainEnd;
            }

            break;

        case 'i':
            Iterations = strtol(optarg, &AfterScan, 0);
            if ((Iterations < 0) || (AfterScan == optarg)) {
                PRINT_ERROR("Invalid iteration count %s.\n", optarg);
                Status = 1;
                goto MainEnd;
            }

            break;

        case 'p':
            Threads = strtol(optarg, &AfterScan, 0);
            if ((Threads <= 0) || (AfterScan == optarg)) {
                PRINT_ERROR("Invalid thread count %s.\n", optarg);
                Status = 1;
                goto MainEnd;
            }

            break;

        case 't':
            if (strcasecmp(optarg, "all") == 0) {
                Test = SignalTestAll;

            } else if (strcasecmp(optarg, "waitpid") == 0) {
                Test = SignalTestWaitpid;

            } else if (strcasecmp(optarg, "sigchld") == 0) {
                Test = SignalTestSigchld;

            } else if (strcasecmp(optarg, "quickwait") == 0) {
                Test = SignalTestQuickWait;

            } else {
                PRINT_ERROR("Invalid test: %s.\n", optarg);
                Status = 1;
                goto MainEnd;
            }

            break;

        case 'd':
            SignalTestVerbosity = TestVerbosityDebug;
            break;

        case 'q':
            SignalTestVerbosity = TestVerbosityQuiet;
            break;

        case 'V':
            printf("Minoca signal test version %d.%d.%d\n",
                   SIGNAL_TEST_VERSION_MAJOR,
                   SIGNAL_TEST_VERSION_MINOR,
                   REVISION);

            return 1;

        case 'h':
            printf(SIGNAL_TEST_USAGE);
            return 1;

        default:

            assert(FALSE);

            Status = 1;
            goto MainEnd;
        }
    }

    IsParent = TRUE;
    if (Threads > 1) {
        Children = malloc(sizeof(pid_t) * (Threads - 1));
        if (Children == NULL) {
            Status = ENOMEM;
            goto MainEnd;
        }

        memset(Children, 0, sizeof(pid_t) * (Threads - 1));
        for (ChildIndex = 0; ChildIndex < Threads - 1; ChildIndex += 1) {
            Child = fork();

            //
            // If this is the child, break out and run the tests.
            //

            if (Child == 0) {
                srand(time(NULL) + ChildIndex);
                IsParent = FALSE;
                break;
            }

            Children[ChildIndex] = Child;
        }
    }

    //
    // Run the tests.
    //

    if ((Test == SignalTestAll) || (Test == SignalTestWaitpid)) {
        Failures += RunWaitpidTest(Iterations);
    }

    if ((Test == SignalTestAll) || (Test == SignalTestSigchld)) {
        Failures += RunSigchldTest(Iterations, ChildProcessCount);
    }

    if ((Test == SignalTestAll) || (Test == SignalTestQuickWait)) {
        Failures += RunQuickWaitTest(Iterations, ChildProcessCount);
    }

    //
    // Wait for any children.
    //

    if (IsParent != FALSE) {
        if ((Threads > 1) && (IsParent != FALSE)) {
            for (ChildIndex = 0; ChildIndex < Threads - 1; ChildIndex += 1) {
                Child = waitpid(Children[ChildIndex], &Status, 0);
                if (Child == -1) {
                    PRINT_ERROR("Failed to wait for child %d: %s.\n",
                                Children[ChildIndex],
                                strerror(errno));

                    Status = errno;

                } else {

                    assert(Child == Children[ChildIndex]);

                    if (!WIFEXITED(Status)) {
                        PRINT_ERROR("Child %d returned with status %x\n",
                                    Child,
                                    Status);

                        Failures += 1;
                    }

                    Failures += WEXITSTATUS(Status);
                    Status = 0;
                }
            }
        }

    //
    // If this is a child, just report back the number of failures to the
    // parent.
    //

    } else {
        if (Failures > 100) {
            exit(100);

        } else {
            exit(Failures);
        }
    }

MainEnd:
    if (Children != NULL) {
        free(Children);
    }

    if (Status != 0) {
        PRINT_ERROR("Error: %d.\n", Status);
    }

    if (Failures != 0) {
        PRINT_ERROR("\n   *** %d failures in signal test ***\n", Failures);
        return Failures;
    }

    return 0;
}

//
// --------------------------------------------------------- Internal Functions
//

ULONG
RunWaitpidTest (
    ULONG Iterations
    )

/*++

Routine Description:

    This routine runs several variations of the waitpid test.

Arguments:

    Iterations - Supplies the number of times to run the test.

Return Value:

    Returns the number of failures in the test.

--*/

{

    ULONG Errors;
    ULONG Iteration;
    ULONG Percent;

    Percent = Iterations / 100;
    if (Percent == 0) {
        Percent = 1;
    }

    PRINT("Running waitpid test with %d iterations.\n", Iterations);
    Errors = 0;
    for (Iteration = 0; Iteration < Iterations; Iteration += 1) {
        Errors += TestWaitpid(FALSE, FALSE);
        Errors += TestWaitpid(TRUE, FALSE);
        Errors += TestWaitpid(FALSE, TRUE);
        Errors += TestWaitpid(TRUE, TRUE);
        if ((Iteration % Percent) == 0) {
            PRINT("w");
        }
    }

    PRINT("\n");
    return Errors;
}

ULONG
RunSigchldTest (
    ULONG Iterations,
    ULONG ChildCount
    )

/*++

Routine Description:

    This routine runs several variations of the waitpid test.

Arguments:

    Iterations - Supplies the number of times to run the test.

    ChildCount - Supplies the number of child processes to spin up and wait
        for.

Return Value:

    Returns the number of failures in the test.

--*/

{

    ULONG Errors;
    ULONG Iteration;
    ULONG Percent;

    PRINT("Running sigchld test with %d iterations and %d children.\n",
          Iterations,
          ChildCount);

    Percent = Iterations / 100;
    if (Percent == 0) {
        Percent = 1;
    }

    Errors = 0;
    for (Iteration = 0; Iteration < Iterations; Iteration += 1) {
        Errors += TestSigchild(ChildCount, 3, FALSE, FALSE);
        Errors += TestSigchild(ChildCount, 3, FALSE, TRUE);
        Errors += TestSigchild(ChildCount, 3, TRUE, FALSE);
        Errors += TestSigchild(ChildCount, 3, TRUE, TRUE);
        if ((Iteration % Percent) == 0) {
            PRINT("c");
        }
    }

    PRINT("\n");
    return Errors;
}

ULONG
RunQuickWaitTest (
    ULONG Iterations,
    ULONG ChildCount
    )

/*++

Routine Description:

    This routine runs the quick wait test, which just forks a process that dies
    and waits for it.

Arguments:

    Iterations - Supplies the number of times to run the test.

    ChildCount - Supplies the number of child processes to spin up and wait
        for.

Return Value:

    Returns the number of failures in the test.

--*/

{

    pid_t Child;
    LONG ChildIndex;
    pid_t *Children;
    ULONG Failures;
    ULONG Iteration;
    ULONG Percent;
    int Status;

    Failures = 0;
    PRINT("Running QuickWait test with %d iterations and %d children.\n",
          Iterations,
          ChildCount);

    assert(ChildCount != 0);

    Percent = Iterations / 100;
    if (Percent == 0) {
        Percent = 1;
    }

    Children = malloc(sizeof(pid_t) * ChildCount);
    if (Children == NULL) {
        Failures += 1;
        goto RunQuickWaitTestEnd;
    }

    for (Iteration = 0; Iteration < Iterations; Iteration += 1) {
        memset(Children, 0, sizeof(pid_t) * ChildCount);

        //
        // Loop creating all the child processes.
        //

        for (ChildIndex = 0; ChildIndex < ChildCount; ChildIndex += 1) {
            Child = fork();
            if (Child == -1) {
                PRINT_ERROR("Failed to fork: %s.\n", strerror(errno));
                Failures += 1;
                continue;
            }

            //
            // If this is the child, die immediately.
            //

            if (Child == 0) {
                exit(ChildIndex);
            }

            Children[ChildIndex] = Child;
        }

        //
        // Loop reaping all the child processes. Backwards, for added flavor.
        //

        for (ChildIndex = ChildCount - 1; ChildIndex >= 0; ChildIndex -= 1) {
            Child = waitpid(Children[ChildIndex], &Status, 0);
            if (Child == -1) {
                PRINT_ERROR("Failed to wait for child %d: %s.\n",
                            Child,
                            strerror(errno));

                Failures += 1;
                continue;
            }

            if ((!WIFEXITED(Status)) ||
                (WEXITSTATUS(Status) != (ChildIndex & 0x7F))) {

                PRINT_ERROR("Child returned with invalid status %x\n", Status);
                Failures += 1;
            }
        }

        if ((Iteration % Percent) == 0) {
            PRINT("q");
        }
    }

    PRINT("\n");

RunQuickWaitTestEnd:
    if (Children != NULL) {
        free(Children);
    }

    return Failures;
}

ULONG
TestWaitpid (
    BOOL BurnTimeInChild,
    BOOL BurnTimeInParent
    )

/*++

Routine Description:

    This routine tests that an application can exit, be waited on, and
    successfully report its status.

Arguments:

    BurnTimeInParent - Supplies a boolean indicating if some time should be
        wasted in the parent process.

    BurnTimeInChild - Supplies a boolean indicating if some time should be
        wasted in the child process.

Return Value:

    Returns the number of failures in the test.

--*/

{

    pid_t Child;
    struct sigaction ChildAction;
    sigset_t ChildSignalMask;
    ULONG Errors;
    struct sigaction OriginalChildAction;
    sigset_t OriginalSignalMask;
    int Status;
    pid_t WaitPid;

    //
    // Block child signals, and set up a handler.
    //

    sigemptyset(&ChildSignalMask);
    sigaddset(&ChildSignalMask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &ChildSignalMask, &OriginalSignalMask);
    ChildAction.sa_sigaction = TestWaitpidChildSignalHandler;
    sigemptyset(&(ChildAction.sa_mask));
    ChildAction.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGCHLD, &ChildAction, &OriginalChildAction);
    Errors = 0;
    Child = fork();
    if (Child == -1) {
        PRINT_ERROR("Failed to fork()!\n");
        return 1;
    }

    //
    // If this is the child process, exit with a specific status code. Only the
    // first 8 bits can be accessed with the macro.
    //

    if (Child == 0) {
        if (BurnTimeInChild != FALSE) {
            sleep(1);
        }

        DEBUG_PRINT("Child %d exiting with status 99.\n", getpid());
        exit(99);

    //
    // In the parent process, wait for the child.
    //

    } else {
        if (BurnTimeInParent != FALSE) {
            sleep(1);
        }

        DEBUG_PRINT("Parent waiting for child %d.\n", Child);
        Status = 0;
        WaitPid = waitpid(Child, &Status, WUNTRACED | WCONTINUED);
        if (WaitPid != Child) {
            PRINT_ERROR("waitpid returned %d instead of child pid %d.\n",
                        WaitPid,
                        Child);

            Errors += 1;
        }

        //
        // Check the flags and return value.
        //

        if ((!WIFEXITED(Status)) ||
            (WIFCONTINUED(Status)) ||
            (WIFSIGNALED(Status)) ||
            (WIFSTOPPED(Status))) {

            PRINT_ERROR("Child status was not exited as expected. Was %x\n",
                        Status);

            Errors += 1;
        }

        if (WEXITSTATUS(Status) != 99) {
            PRINT_ERROR("Child exit status was an unexpected %d.\n",
                        WEXITSTATUS(Status));

            Errors += 1;
        }
    }

    //
    // Restore the original signal mask.
    //

    sigaction(SIGCHLD, &OriginalChildAction, NULL);
    sigprocmask(SIG_SETMASK, &OriginalSignalMask, NULL);
    Errors += ChildSignalFailures;
    ChildSignalFailures = 0;
    return Errors;
}

ULONG
TestSigchild (
    ULONG ChildCount,
    ULONG ChildAdditionalThreads,
    BOOL UseSigsuspend,
    BOOL ChildrenExitVoluntarily
    )

/*++

Routine Description:

    This routine tests child signals.

Arguments:

    ChildCount - Supplies the number of simultaneous children to create.

    ChildAdditionalThreads - Supplies the number of additional threads each
        child should spin up.

    UseSigsuspend - Supplies a boolean indicating whether sigsuspend should be
        called in the main loop (TRUE) or busy waiting (FALSE).

    ChildrenExitVoluntarily - Supplies a boolean indicating whether children
        exit on their own or need to be killed.

Return Value:

    Returns the number of failures in the test.

--*/

{

    ULONG BurnIndex;
    pid_t Child;
    struct sigaction ChildAction;
    ULONG ChildIndex;
    volatile ULONG ChildInitializing;
    pid_t *Children;
    sigset_t ChildSignalMask;
    ULONG Errors;
    struct sigaction OriginalChildAction;
    struct sigaction OriginalRealtimeAction;
    sigset_t OriginalSignalMask;
    union sigval SignalValue;
    int Status;
    ULONG ThreadIndex;
    pid_t WaitPid;

    //
    // Allocate child array.
    //

    DEBUG_PRINT("Testing SIGCHLD: %d children each with %d extra "
                "threads. UseSigsuspend: %d, ChildrenExitVoluntarily: "
                "%d.\n\n",
                ChildCount,
                ChildAdditionalThreads,
                UseSigsuspend,
                ChildrenExitVoluntarily);

    Children = malloc(sizeof(pid_t) * ChildCount);
    if (Children == NULL) {
        PRINT_ERROR("Failed to malloc %d bytes.\n", sizeof(pid_t) * ChildCount);
        return 1;
    }

    RtlZeroMemory(Children, sizeof(pid_t) * ChildCount);

    //
    // Block child signals, and set up a handler.
    //

    sigemptyset(&ChildSignalMask);
    sigaddset(&ChildSignalMask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &ChildSignalMask, &OriginalSignalMask);
    ChildAction.sa_sigaction = TestWaitpidChildSignalHandler;
    sigemptyset(&(ChildAction.sa_mask));
    ChildAction.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGCHLD, &ChildAction, &OriginalChildAction);
    ChildAction.sa_sigaction = TestSigchldRealtime1SignalHandler;
    sigaction(SIGRTMIN + 0, &ChildAction, &OriginalRealtimeAction);
    Errors = 0;

    //
    // Create child processes.
    //

    ChildProcessesReady = 0;
    ChildSignalsExpected = ChildCount;
    Child = -1;
    for (ChildIndex = 0; ChildIndex < ChildCount; ChildIndex += 1) {
        Child = fork();
        if (Child == -1) {
            PRINT_ERROR("Failed to fork()!\n");
            return 1;
        }

        //
        // If this is the child process, spin up any additional threads
        // requested, send the signal once everything's up and running, and
        // exit.
        //

        if (Child == 0) {
            DEBUG_PRINT("Child %d alive.\n", getpid());
            for (ThreadIndex = 0;
                 ThreadIndex < ChildAdditionalThreads;
                 ThreadIndex += 1) {

                ChildInitializing = 1;
                Status = OsCreateThread(NULL,
                                        0,
                                        TestThreadSpinForever,
                                        (PVOID)&ChildInitializing,
                                        NULL,
                                        0,
                                        NULL,
                                        NULL);

                if (!KSUCCESS(Status)) {
                    PRINT_ERROR("Child %d failed to create thread: %x.\n",
                                getpid(),
                                Status);
                }

                //
                // Wait for the thread to come to life and start doing
                // something.
                //

                for (BurnIndex = 0; BurnIndex < 20; BurnIndex += 1) {
                    if (ChildInitializing == 0) {
                        break;
                    }

                    sleep(1);
                }

                if (BurnIndex == 20) {
                    PRINT_ERROR("Thread failed to initialize!\n");
                }
            }

            //
            // Send a signal to the parent letting them know everything's
            // initialized.
            //

            SignalValue.sival_int = getpid();
            Status = sigqueue(getppid(), SIGRTMIN + 0, SignalValue);
            if (Status != 0) {
                PRINT_ERROR("Failed to sigqueue to parent: errno %d.\n",
                            errno);
            }

            //
            // Exit the process or spin forever.
            //

            if (ChildrenExitVoluntarily != FALSE) {
                DEBUG_PRINT("Child %d exiting with status 99.\n", getpid());
                exit(99);

            } else {
                DEBUG_PRINT("Child %d spinning forever.\n", getpid());
                while (TRUE) {
                    sleep(1);
                }
            }

        //
        // This is the parent process, save the child PID.
        //

        } else {
            Children[ChildIndex] = Child;
        }
    }

    //
    // This is the parent process, wait for all processes to be ready.
    //

    for (BurnIndex = 0; BurnIndex < 100; BurnIndex += 1) {
        if (ChildProcessesReady == ChildCount) {
            break;
        }

        sleep(1);
    }

    if (ChildProcessesReady != ChildCount) {
        PRINT_ERROR("Only %d of %d children ready.\n",
                    ChildProcessesReady,
                    ChildCount);

        Errors += 1;
    }

    //
    // If the children aren't going to go quietly, kill them.
    //

    if (ChildrenExitVoluntarily == FALSE) {
        for (ChildIndex = 0; ChildIndex < ChildCount; ChildIndex += 1) {
            DEBUG_PRINT("Killing child index %d PID %d.\n",
                        ChildIndex,
                        Children[ChildIndex]);

            Status = kill(Children[ChildIndex], SIGKILL);
            if (Status != 0) {
                PRINT_ERROR("Failed to kill pid %d, errno %d.\n",
                            Children[ChildIndex],
                            errno);

                Errors += 1;
            }
        }
    }

    //
    // In the parent process, wait for the children.
    //

    DEBUG_PRINT("Parent waiting for children UsingSuspend %d.\n",
                UseSigsuspend);

    Status = 0;
    if (UseSigsuspend != FALSE) {
        for (BurnIndex = 0; BurnIndex < 20; BurnIndex += 1) {
            if (ChildSignalsExpected == 0) {
                break;
            }

            DEBUG_PRINT("Expecting %d more child signals. "
                        "Running sigsuspend.\n",
                        ChildSignalsExpected);

            sigsuspend(&OriginalSignalMask);
            DEBUG_PRINT("Returned from sigsuspend.\n");
        }

    } else {
        sigprocmask(SIG_UNBLOCK, &ChildSignalMask, NULL);
        for (BurnIndex = 0; BurnIndex < 20; BurnIndex += 1) {
            if (ChildSignalsExpected == 0) {
                break;
            }

            sleep(1);
        }

        sigprocmask(SIG_BLOCK, &ChildSignalMask, NULL);
    }

    if (ChildSignalsExpected != 0) {
        PRINT_ERROR("Error: Never saw SIGCHLD.\n");
        Errors += 1;
    }

    ChildSignalsExpected = 0;

    //
    // Waitpid better not find anything.
    //

    WaitPid = waitpid(-1, &Status, WUNTRACED | WCONTINUED | WNOHANG);
    if (WaitPid != -1) {
        PRINT_ERROR("Error: waitpid unexpectedly gave up a %d\n",
                    WaitPid);

        Errors += 1;
    }

    if (ChildSignalFailures != 0) {
        PRINT_ERROR("Error: %d child signal failures.\n", ChildSignalFailures);
    }

    Errors += ChildSignalFailures;
    ChildSignalFailures = 0;
    ChildProcessesReady = 0;

    //
    // Restore the original signal mask.
    //

    sigaction(SIGCHLD, &OriginalChildAction, NULL);
    sigaction(SIGRTMIN + 0, &OriginalRealtimeAction, NULL);
    sigprocmask(SIG_SETMASK, &OriginalSignalMask, NULL);
    free(Children);
    DEBUG_PRINT("Done with SIGCHLD test.\n");
    return Errors;
}

void
TestWaitpidChildSignalHandler (
    int Signal,
    siginfo_t *SignalInformation,
    void *Context
    )

/*++

Routine Description:

    This routine responds to child signals.

Arguments:

    Signal - Supplies the signal number coming in, in this case always SIGCHLD.

    SignalInformation - Supplies a pointer to the signal information.

    Context - Supplies a pointer to some unused context information.

Return Value:

    None.

--*/

{

    int PidStatus;
    BOOL SignaledPidFound;
    int Status;
    LONG WaitPidResult;

    DEBUG_PRINT("SIGCHLD Pid %d Status %d.\n",
                SignalInformation->si_pid,
                SignalInformation->si_status);

    if (Signal != SIGCHLD) {
        PRINT_ERROR("Error: Signal %d came in instead of SIGCHLD.\n", Signal);
        ChildSignalFailures += 1;
    }

    if (ChildSignalsExpected == 0) {
        PRINT_ERROR("Error: Unexpected child signal.\n");
        ChildSignalFailures += 1;
    }

    if (SignalInformation->si_signo != SIGCHLD) {
        PRINT_ERROR("Error: Signal %d came in si_signo instead of SIGCHLD.\n",
                    SignalInformation->si_signo);

        ChildSignalFailures += 1;
    }

    if (SignalInformation->si_code == CLD_EXITED) {
        if (SignalInformation->si_status != 99) {
            PRINT_ERROR("Error: si_status was %d instead of %d.\n",
                        SignalInformation->si_status,
                        99);

            ChildSignalFailures += 1;
        }

    } else if (SignalInformation->si_code != CLD_KILLED) {
        PRINT_ERROR("Error: unexpected si_code %x.\n",
                    SignalInformation->si_code);

        ChildSignalFailures += 1;
    }

    //
    // Make sure a wait also gets the same thing.
    //

    if (ChildSignalsExpected == 1) {
        SignaledPidFound = TRUE;
        WaitPidResult = waitpid(-1, &Status, WNOHANG);
        if (WaitPidResult != SignalInformation->si_pid) {
            SignaledPidFound = FALSE;
            PRINT_ERROR("Error: SignalInformation->si_pid = %x but "
                        "waitpid() = %x\n.",
                        SignalInformation->si_pid,
                        WaitPidResult);

            ChildSignalFailures += 1;
        }

        ChildSignalsExpected -= 1;

    } else {
        SignaledPidFound = FALSE;
        while (ChildSignalsExpected != 0) {
            WaitPidResult = waitpid(-1, &PidStatus, WNOHANG);
            if (WaitPidResult == SignalInformation->si_pid) {
                Status = PidStatus;
                SignaledPidFound = TRUE;
            }

            DEBUG_PRINT("SIGCHLD handler waited and got %d.\n", WaitPidResult);
            if ((WaitPidResult == -1) || (WaitPidResult == 0)) {
                break;
            }

            ChildSignalsExpected -= 1;
        }
    }

    if (SignaledPidFound == FALSE) {
        PRINT_ERROR("Error: Pid %d signaled but waitpid could not find "
                    "it.\n",
                    SignalInformation->si_pid);

        ChildSignalFailures += 1;

    } else {
        if (SignalInformation->si_code == CLD_EXITED) {
            if ((!WIFEXITED(Status)) || (WEXITSTATUS(Status) != 99)) {
                PRINT_ERROR("Error: Status was %x, not returning exited or "
                            "exit status %d.\n",
                            Status,
                            99);

                ChildSignalFailures += 1;
            }

        } else if (SignalInformation->si_code == CLD_KILLED) {
            if ((!WIFSIGNALED(Status)) || (WTERMSIG(Status) != SIGKILL)) {
                PRINT_ERROR("Error: Status was %x, not returning signaled or "
                            "SIGKILL.\n",
                            Status);

                ChildSignalFailures += 1;
            }
        }
    }

    //
    // If all the children have been accounted for, make sure there's not
    // another signal in the queue too.
    //

    if (ChildSignalsExpected == 0) {
        WaitPidResult = waitpid(-1, NULL, WNOHANG);
        if (WaitPidResult != -1) {
            PRINT_ERROR("Error: waitpid got another child %x unexpectedly.\n",
                        WaitPidResult);

            ChildSignalFailures += 1;
        }
    }

    ChildSignalPid = SignalInformation->si_pid;
    return;
}

void
TestSigchldRealtime1SignalHandler (
    int Signal,
    siginfo_t *SignalInformation,
    void *Context
    )

/*++

Routine Description:

    This routine responds to the first real time signal, used to count ready
    processes.

Arguments:

    Signal - Supplies the signal number coming in, in this case always SIGCHLD.

    SignalInformation - Supplies a pointer to the signal information.

    Context - Supplies a pointer to some unused context information.

Return Value:

    None.

--*/

{

    DEBUG_PRINT("SIGRTMIN+0 %d\n", SignalInformation->si_value);
    if (SignalInformation->si_signo != SIGRTMIN + 0) {
        PRINT_ERROR("Got si_signo %d when expected %d.\n",
                    SignalInformation->si_signo,
                    SIGRTMIN + 0);

        ChildSignalFailures += 1;
    }

    ChildProcessesReady += 1;
    return;
}

VOID
TestThreadSpinForever (
    PVOID Parameter
    )

/*++

Routine Description:

    This routine implements a thread routine that simply spins forever.

Arguments:

    Parameter - Supplies a parameter assumed to be of type PULONG whose
        contents will be set to 0.

Return Value:

    None. This thread never returns voluntarily.

--*/

{

    *((PULONG)Parameter) = 0;
    while (TRUE) {
        sleep(1);
    }

    return;
}
