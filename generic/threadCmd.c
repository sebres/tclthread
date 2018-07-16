/*
 * threadCmd.c --
 *
 * This file implements the Tcl thread commands that allow script
 * level access to threading. It will not load into a core that was
 * not compiled for thread support.
 *
 * See http://www.tcl.tk/doc/howto/thread_model.html
 *
 * Some of this code is based on work done by Richard Hipp on behalf of
 * Conservation Through Innovation, Limited, with their permission.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 * ----------------------------------------------------------------------------
 */

#include "tclThreadInt.h"
#include "threadSvCmd.h"

/*
 * Provide package version in build contexts which do not provide
 * -DPACKAGE_VERSION, like building a shell with the Thread object
 * files built as part of that shell. Example: basekits.
 */
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "3.0.1"
#endif

/*
 * Check if this is Tcl 8.5 or higher.  In that case, we will have the TIP
 * #143 APIs (i.e. interpreter resource limiting) available.
 */

#ifndef TCL_TIP143
# if TCL_MINIMUM_VERSION(8,5)
#  define TCL_TIP143
# endif
#endif

/*
 * If TIP #143 support is enabled and we are compiling against a pre-Tcl 8.5
 * core, hard-wire the necessary APIs using the "well-known" offsets into the
 * stubs table.
 */

#define haveInterpLimit (threadTclVersion>=85)
#if defined(TCL_TIP143) && !TCL_MINIMUM_VERSION(8,5)
# if defined(USE_TCL_STUBS)
#  define Tcl_LimitExceeded ((int (*)(Tcl_Interp *)) \
     ((&(tclStubsPtr->tcl_PkgProvideEx))[524]))
# else
#  error "Supporting TIP #143 requires USE_TCL_STUBS before Tcl 8.5"
# endif
#endif

/*
 * Check if this is Tcl 8.6 or higher.  In that case, we will have the TIP
 * #285 APIs (i.e. asynchronous script cancellation) available.
 */

#define haveInterpCancel (threadTclVersion>=86)
#ifndef TCL_TIP285
# if TCL_MINIMUM_VERSION(8,6)
#  define TCL_TIP285
# endif
#endif

/*
 * If TIP #285 support is enabled and we are compiling against a pre-Tcl 8.6
 * core, hard-wire the necessary APIs using the "well-known" offsets into the
 * stubs table.
 */

#if defined(TCL_TIP285) && !TCL_MINIMUM_VERSION(8,6)
# if defined(USE_TCL_STUBS)
#  define TCL_CANCEL_UNWIND 0x100000
#  define Tcl_CancelEval ((int (*)(Tcl_Interp *, Tcl_Obj *, ClientData, int)) \
     ((&(tclStubsPtr->tcl_PkgProvideEx))[580]))
#  define Tcl_Canceled ((int (*)(Tcl_Interp *, int)) \
     ((&(tclStubsPtr->tcl_PkgProvideEx))[581]))
# else
#  error "Supporting TIP #285 requires USE_TCL_STUBS before Tcl 8.6"
# endif
#endif

/*
 * Access to the list of threads and to the thread send results
 * (defined below) is guarded by this mutex.
 */

TCL_DECLARE_MUTEX(threadMutex)

/*
 * Each thread has an single instance of the following structure. There
 * is one instance of this structure per thread even if that thread contains
 * multiple interpreters. The interpreter identified by this structure is
 * the main interpreter for the thread. The main interpreter is the one that
 * will process any messages received by a thread. Any interpreter can send
 * messages but only the main interpreter can receive them, unless you're
 * not doing asynchronous script backfiring. In such cases the caller might
 * signal the thread to which interpreter the result should be delivered.
 */

typedef struct ThreadSpecificData {
    Tcl_ThreadId threadId;                /* The real ID of this thread */
    Tcl_Obj     *threadObjPtr;            /* The object ptr used inside the thread self ONLY. 
                                           * Caution: don't share it between threads! */
    Tcl_Obj     *threadIdHexObj;          /* Thread id in hex (mostly used in logging) */
    Tcl_Interp *interp;                   /* Main/Current interp for this thread */
    Tcl_Condition doOneEvent;             /* Signalled just before running
                                             an event from the event loop */
    Tcl_Mutex     mutex;                  /* per thread mutex to facilitate common concurrency
                                             (used instead of common threadMutex - often 1 x 1, instead of N x M) */
    int flags;                            /* One of the ThreadFlags below */
    int refCount;                         /* Used for thread reservation */
    int objRefCount;                      /* Used for reservation of thread data (tcl object sharing) */
    int eventsPending;                    /* # of unprocessed events */
    int maxEventsCount;                   /* Maximum # of pending events */
    Tcl_Obj *evalScript;                  /* Main eval script object or NULL */
    Tcl_Obj *exitProcObj;                 /* Command will be executed on exit */
    struct ThreadSpecificData *nextPtr;
    struct ThreadSpecificData *prevPtr;
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;

#ifndef TCL_TSD_INIT
__forceinline ThreadSpecificData *
TCL_TSD_INIT(int autoCreate)
{
    ThreadSpecificData **tsdPtrPtr;

    if (*(tsdPtrPtr = (ThreadSpecificData**)Tcl_GetThreadData(
            (&dataKey),sizeof(ThreadSpecificData*))) != NULL
        || !autoCreate
    ) {
        return *tsdPtrPtr;
    }
    *tsdPtrPtr = (ThreadSpecificData*)ckalloc(sizeof(ThreadSpecificData));
    memset(*tsdPtrPtr, 0, sizeof(ThreadSpecificData));
    (*tsdPtrPtr)->threadId = Tcl_GetCurrentThread();
    // _log(" !!! tsd ++ %p", *tsdPtrPtr);
    return *tsdPtrPtr;
}
__forceinline void
TCL_TSD_SET(ThreadSpecificData *tsdPtr)
{
    ThreadSpecificData **tsdPtrPtr;

    tsdPtrPtr = (ThreadSpecificData**)Tcl_GetThreadData(
        (&dataKey),sizeof(ThreadSpecificData*));
    *tsdPtrPtr = tsdPtr;
}
__forceinline void
TCL_TSD_REMOVE(ThreadSpecificData *tsdPtr)
{
    // _log(" !!! tsd -- %p", tsdPtr);
    /* free condition */
    if (tsdPtr->doOneEvent) {
        Tcl_ConditionFinalize(&tsdPtr->doOneEvent);
    }
    /* free mutex */
    Tcl_MutexFinalize(&tsdPtr->mutex);
    /* free TSD */
    ckfree((char*)tsdPtr);
}
#endif

#define THREAD_FLAGS_NONE          0      /* None */
#define THREAD_FLAGS_STOPPED       1      /* Thread is being stopped */
#define THREAD_FLAGS_INERROR       2      /* Thread is in error */
#define THREAD_FLAGS_UNWINDONERROR 4      /* Thread unwinds on script error */
#define THREAD_FLAGS_OWN_THREAD    8      /* Own thread, delete interpreter on exit, etc. */

/*
 * This list is used to list all threads that have interpreters.
 */

static struct ThreadSpecificData *threadList = NULL;

/*
 * Used to represent the empty result.
 */

char *threadEmptyResult = (char *)"";

int threadTclVersion = 0;

/*
 * An instance of the following structure contains all information that is
 * passed into a new thread when the thread is created using either the
 * "thread create" Tcl command or the ThreadCreate() C function.
 */

typedef struct ThreadCtrl {
    Tcl_Obj *script;                      /* Script to execute */
    int flags;                            /* Initial value of the "flags"
                                           * field in ThreadSpecificData */
    Tcl_Condition condWait;               /* Condition variable used to
                                           * sync parent and child threads */
#ifdef NS_AOLSERVER
    ClientData cd;                        /* Opaque ptr to pass to thread */
#endif
    ThreadSpecificData *tsdPtr;           /* TSD pointer of created thread */
} ThreadCtrl;

/*
 * Structure holding result of the command executed in target thread.
 */

typedef struct ThreadEventResult {
    Tcl_Condition done;                   /* Set when the script completes */
    Tcl_Obj *resultObj;                   /* Result or Return state object (content of the errorCode, errorInfo, etc.) */
    Tcl_ThreadId srcThreadId;             /* Id of sender */
    struct ThreadEvent *eventPtr;         /* Back pointer */
} ThreadEventResult;

/*
 * This is the event used to send commands to other threads.
 */

typedef struct ThreadEvent {
    Tcl_Event event;                      /* Must be first */
    struct ThreadSendData *sendData;      /* See below */
    struct ThreadClbkData *clbkData;      /* See below */
    struct ThreadEventResult *resultPtr;  /* To communicate the result back.
                                           * NULL if we don't care about it */
} ThreadEvent;

typedef int  (ThreadSendProc) (Tcl_Interp*, ClientData);
typedef void (ThreadSendFree) (ClientData);

static ThreadSendProc ThreadSendEval;     /* Does a regular Tcl_Eval */
static ThreadSendProc ThreadClbkSetVar;   /* Sets the named variable */
static ThreadSendFree ThreadClbkFree;

/*
 * These structures are used to communicate commands between source and target
 * threads. The ThreadSendData is used for source->target command passing,
 * while the ThreadClbkData is used for doing asynchronous callbacks.
 *
 * Important: structures below must have first four elements identical!
 */

typedef struct ThreadSendData {
    ThreadSendProc *execProc;             /* Func to exec in remote thread */
    ClientData clientData;                /* Ptr to pass to send function */
    ThreadSendFree *freeProc;             /* Function to free client data */
    Tcl_Interp *interp;                   /* Interp to run the command */
     /* ---- */
} ThreadSendData;

typedef struct ThreadClbkData {
    ThreadSendProc *execProc;             /* The callback function */
    ClientData clientData;                /* Ptr to pass to clbk function */
    ThreadSendFree *freeProc;             /* Function to free client data */
    Tcl_Interp *interp;                   /* Interp to run the command */
    /* ---- */
    Tcl_ThreadId threadId;                /* Thread where to post callback */
    Tcl_Obj *resultObj;                   /* Result or Return state object for the asynchronously callback */
} ThreadClbkData;

/*
 * Event used to transfer a channel between threads.
 */
typedef struct TransferEvent {
    Tcl_Event event;                      /* Must be first */
    Tcl_Channel chan;                     /* The channel to transfer */
    struct TransferResult *resultPtr;     /* To communicate the result */
} TransferEvent;

typedef struct TransferResult {
    Tcl_Condition done;                   /* Set when transfer is done */
    int resultCode;                       /* Set to TCL_OK or TCL_ERROR when
                                             the transfer is done. Def = -1 */
    Tcl_Obj *resultMsg;                   /* Initialized to NULL. Set to a
                                             message object by the target
                                             thread in case of an error  */
    Tcl_ThreadId srcThreadId;             /* Id of src thread */
    struct TransferEvent *eventPtr;       /* Back pointer */
} TransferResult;

typedef struct DetachedChannel {
    Tcl_Channel chan;                     /* Detached channel */
    struct DetachedChannel *nextPtr;      /* List pointers */
    struct DetachedChannel *prevPtr;
} DetachedChannel;

static DetachedChannel *detachedList = NULL;

/*
 * This is for simple error handling when a thread script exits badly.
 */

#define THREAD_BGERROR_CMD (char *)-1               /* By error execute background command registered with 
                                                     * "interp bgerror" (typically ::tcl::Bgerror) */

static Tcl_ThreadId errorThreadId = NULL;           /* Id of thread to post error message */
static char *errorProcString = THREAD_BGERROR_CMD;  /* Tcl script to run when reporting error */

/*
 * Definition of flags for ThreadSend.
 */

#define THREAD_SEND_WAIT (1<<1)
#define THREAD_SEND_HEAD (1<<2)
#define THREAD_SEND_CLBK (1<<3)

#ifdef BUILD_thread
# undef  TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLEXPORT
#endif


/*
 * Miscellaneous functions used within this file
 */

static Tcl_EventDeleteProc ThreadDeleteEvent;

static Tcl_ThreadCreateType
NewThread(ClientData clientData);

static ThreadSpecificData*
ThreadExistsInner(Tcl_ThreadId id);

static int
ThreadInit(Tcl_Interp *interp);

static int
ThreadCreate(Tcl_Interp *interp,
                               Tcl_Obj *script,
                               int stacksize,
                               int flags,
                               int preserve);
static int
ThreadSend(Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               ThreadSpecificData *tsdPtr,
                               ThreadSendData *sendPtr,
                               ThreadClbkData *clbkPtr,
                               int flags);
static Tcl_Obj *
ThreadGetResult(Tcl_Interp *interp,
                               int code);
static int
ThreadGetOption(Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               ThreadSpecificData *tsdPtr,
                               char *option,
                               Tcl_DString *ds);
static int
ThreadSetOption(Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               ThreadSpecificData *tsdPtr,
                               char *option,
                               char *value);
static int
ThreadEventProc(Tcl_Event *evPtr,
                               int mask);
static int
ThreadWait(Tcl_Interp *interp);

static int
ThreadExists(Tcl_ThreadId id, ThreadSpecificData *tsdPtr, int shutDownLevel);

#define THREADLIST_ALL     1
#define THREADLIST_NOSELF  2
#define THREADLIST_FULL    4
static Tcl_Obj*
ThreadList(int mode);

void
ThreadErrorProc(Tcl_Interp *interp);

static void
ThreadFreeProc(ClientData clientData);

static void
ThreadFreeError(ClientData clientData);

static int
ThreadJoin(Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               ThreadSpecificData *tsdPtr);
static int
ThreadTransfer(Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               ThreadSpecificData *tsdPtr,
                               Tcl_Channel chan);
static int
ThreadDetach(Tcl_Interp *interp,
                               Tcl_Channel chan);
static int
ThreadAttach(Tcl_Interp *interp,
                               char *chanName);
static int
TransferEventProc(Tcl_Event *evPtr,
                               int mask);

static void
ErrorNoSuchThread(Tcl_Interp *interp,
                               Tcl_ThreadId thrId);
static void
ThreadCutChannel(Tcl_Interp *interp,
                               Tcl_Channel channel);

#ifdef TCL_TIP285
static int
ThreadCancel(Tcl_Interp *interp,
                               Tcl_ThreadId thrId,
                               ThreadSpecificData *tsdPtr,
                               const char *result,
                               int flags);
#endif

static int
GetThreadFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, ThreadSpecificData **tsdPtrPtr, int shutDownLevel);
static ThreadSpecificData*
GetThreadFromId_Lock(Tcl_Interp *interp, Tcl_ThreadId thrId, int shutDownLevel);

static void
ThreadObj_DupInternalRep(Tcl_Obj *srcPtr, Tcl_Obj *copyPtr);
static void
ThreadObj_FreeInternalRep(Tcl_Obj *objPtr);
static int
ThreadObj_SetFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void
ThreadObj_UpdateString(Tcl_Obj *objPtr);

#define ThreadObj_SetObjIntRep(objPtr, tsdPtr, thrId) \
    objPtr->internalRep.twoPtrValue.ptr1 = tsdPtr, \
    objPtr->internalRep.twoPtrValue.ptr2 = thrId, \
    objPtr->typePtr = &ThreadObjType;

__forceinline void
ThreadObj_InitLocalObj(ThreadSpecificData *tsdPtr)
{
    if (!tsdPtr->threadObjPtr) {
        Tcl_Obj *objPtr = Tcl_NewObj();
        objPtr->bytes = NULL;
        ThreadObj_SetObjIntRep(objPtr, tsdPtr, tsdPtr->threadId);
        tsdPtr->objRefCount++;
        Tcl_InitObjRef(tsdPtr->threadObjPtr, objPtr);
    }
}

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc ThreadCreateObjCmd;
static Tcl_ObjCmdProc ThreadReserveObjCmd;
static Tcl_ObjCmdProc ThreadReleaseObjCmd;
static Tcl_ObjCmdProc ThreadSendObjCmd;
static Tcl_ObjCmdProc ThreadBroadcastObjCmd;
static Tcl_ObjCmdProc ThreadUnwindObjCmd;
static Tcl_ObjCmdProc ThreadExitObjCmd;
static Tcl_ObjCmdProc ThreadIdObjCmd;
static Tcl_ObjCmdProc ThreadNamesObjCmd;
static Tcl_ObjCmdProc ThreadWaitObjCmd;
static Tcl_ObjCmdProc ThreadExistsObjCmd;
static Tcl_ObjCmdProc ThreadConfigureObjCmd;
static Tcl_ObjCmdProc ThreadErrorProcObjCmd;
static Tcl_ObjCmdProc ThreadJoinObjCmd;
static Tcl_ObjCmdProc ThreadTransferObjCmd;
static Tcl_ObjCmdProc ThreadDetachObjCmd;
static Tcl_ObjCmdProc ThreadAttachObjCmd;
static Tcl_ObjCmdProc ThreadInscopeProcObjCmd;
static Tcl_ObjCmdProc ThreadExitProcObjCmd;
static Tcl_ObjCmdProc ThreadWaitStatObjCmd;

#ifdef TCL_TIP285
static Tcl_ObjCmdProc ThreadCancelObjCmd;
#endif

static int
ThreadInit(interp)
    Tcl_Interp *interp; /* The current Tcl interpreter */
{
    _log_debug(" !!! threadinit");

    if (!threadTclVersion) {

        /*
         * Check whether we are running threaded Tcl.
         * Get the current core version to decide whether to use
         * some lately introduced core features or to back-off.
         */

        int major, minor;

        Tcl_MutexLock(&threadMutex);
        if (threadMutex == NULL){
            /* If threadMutex==NULL here, it means that Tcl_MutexLock() is
             * a dummy function, which is the case in unthreaded Tcl */
            const char *msg = "Tcl core wasn't compiled for threading";
            Tcl_SetObjResult(interp, Tcl_NewStringObj(msg, -1));
            return TCL_ERROR;
        }
        Tcl_GetVersion(&major, &minor, NULL, NULL);
        threadTclVersion = 10 * major + minor;
        Tcl_MutexUnlock(&threadMutex);
    }

    TCL_CMD(interp, THREAD_CMD_PREFIX"create",    ThreadCreateObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"send",      ThreadSendObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"broadcast", ThreadBroadcastObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"exit",      ThreadExitObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"unwind",    ThreadUnwindObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"id",        ThreadIdObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"names",     ThreadNamesObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"exists",    ThreadExistsObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"wait",      ThreadWaitObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"configure", ThreadConfigureObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"errorproc", ThreadErrorProcObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"preserve",  ThreadReserveObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"release",   ThreadReleaseObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"join",      ThreadJoinObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"transfer",  ThreadTransferObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"detach",    ThreadDetachObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"attach",    ThreadAttachObjCmd);
#ifdef TCL_TIP285
    TCL_CMD(interp, THREAD_CMD_PREFIX"cancel",    ThreadCancelObjCmd);
#endif
    TCL_CMD(interp, THREAD_CMD_PREFIX"inscope",   ThreadInscopeProcObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"exitproc",  ThreadExitProcObjCmd);
    TCL_CMD(interp, THREAD_CMD_PREFIX"waitstat",  ThreadWaitStatObjCmd);
   
    /*
     * Add shared variable commands
     */

    Sv_Init(interp);

    /*
     * Add commands to access thread
     * synchronization primitives.
     */

    Sp_Init(interp);

    /*
     * Add threadpool commands.
     */

    Tpool_Init(interp);

    _log_debug(" !!! threadinit - done");
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Thread_Init --
 *
 *  Initialize the thread commands.
 *
 * Results:
 *  TCL_OK if the package was properly initialized.
 *
 * Side effects:
 *  Adds package commands to the current interp.
 *
 *----------------------------------------------------------------------
 */

DLLEXPORT int
Thread_Init(interp)
    Tcl_Interp *interp; /* The current Tcl interpreter */
{
    int status;

    if (Tcl_InitStubs(interp, "8.4", 0) == NULL) {
        if ((sizeof(size_t) != sizeof(int)) ||
            !Tcl_InitStubs(interp, "8.4-", 0)) {
            return TCL_ERROR;
        }
        Tcl_ResetResult(interp);
    }

    /* if already present in this interp */
    if (Tcl_PkgPresent(interp, "Thread", PACKAGE_VERSION, 1) != NULL) {
        return TCL_OK;
    }

    status = ThreadInit(interp);

    if (status != TCL_OK) {
        return status;
    }

    return Tcl_PkgProvideEx(interp, "Thread", PACKAGE_VERSION, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * Init --
 *
 *  Make sure internal list of threads references the current thread.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  The list of threads is initialized to include the current thread.
 *
 *----------------------------------------------------------------------
 */

static ThreadSpecificData *
Init(interp)
    Tcl_Interp *interp;         /* Current interpreter. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(1);

    /*
        _log_debug(" !!! init ...");
        Tcl_EvalEx(interp, "puts [info level [info level]]", -1, 0);
    */

    if (tsdPtr->interp == (Tcl_Interp*)NULL) {
        tsdPtr->interp = interp;

        Tcl_CreateThreadExitHandler(ThreadExitHandler, NULL);

        /* 
         * Create a tcl object with thread-id, used in this thread only 
         * This reference will be decremented in ThreadExitHandler
         */
        if (!tsdPtr->threadObjPtr && (tsdPtr->flags & THREAD_FLAGS_STOPPED) == 0) {
            ThreadObj_InitLocalObj(tsdPtr);
        }

        if (!tsdPtr->nextPtr) {
            Tcl_MutexLock(&threadMutex);
            SpliceIn(tsdPtr, threadList);
            Tcl_MutexUnlock(&threadMutex);
        }
    }
    return tsdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreateObjCmd --
 *
 *  This procedure is invoked to process the "thread::create" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCreateObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int argc, rsrv = 0;
    const char *arg;
    Tcl_Obj *script = NULL;
    int flags = TCL_THREAD_NOFLAGS;

    Init(interp);

    /*
     * Syntax: thread::create ?-joinable? ?-preserved? ?script?
     */

    for (argc = 1; argc < objc; argc++) {
        arg = Tcl_GetString(objv[argc]);
        if (OPT_CMP(arg, "--")) {
            argc++;
            if ((argc + 1) == objc) {
                script = objv[argc];
            } else {
                goto usage;
            }
            break;
        } else if (OPT_CMP(arg, "-joinable")) {
            flags |= TCL_THREAD_JOINABLE;
        } else if (OPT_CMP(arg, "-preserved")) {
            rsrv = 1;
        } else if ((argc + 1) == objc) {
            script = objv[argc];
        } else {
            goto usage;
        }
    }

    return ThreadCreate(interp, script, TCL_THREAD_STACK_DEFAULT, flags, rsrv);

 usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-joinable? ?script?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadReserveObjCmd --
 *
 *  This procedure is invoked to process the "thread::preserve" and
 *  "thread::release" Tcl commands, depending on the flag passed by
 *  the ClientData argument. See the user documentation for details
 *  on what those command do.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadReserveObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    Tcl_ThreadId thrId = (Tcl_ThreadId)0;
    Tcl_Obj *handleObj = NULL;
    ThreadSpecificData *tsdPtr = Init(interp);

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?threadId?");
        return TCL_ERROR;
    }
    if (objc == 2) {
        handleObj = objv[1];
    }

    if (handleObj) {
        if (GetThreadFromObj(interp, handleObj, &tsdPtr, 0) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    return ThreadReserve(interp, thrId, tsdPtr, THREAD_RESERVE, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadReleaseObjCmd --
 *
 *  This procedure is invoked to process the "thread::release" Tcl
 *  command. See the user documentation for details on what this
 *  command does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadReleaseObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;           /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int wait = 0;
    Tcl_ThreadId thrId = (Tcl_ThreadId)0;
    Tcl_Obj *handleObj = NULL;
    ThreadSpecificData *tsdPtr = Init(interp);

    if (objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?-wait? ?threadId?");
        return TCL_ERROR;
    }
    if (objc > 1) {
        if (OPT_CMP(Tcl_GetString(objv[1]), "-wait")) {
            wait = 1;
            if (objc > 2) {
                handleObj = objv[2];
            }
        } else {
            handleObj = objv[1];
        }
    }
    if (handleObj) {
        if (GetThreadFromObj(interp, handleObj, &tsdPtr, 1) != TCL_OK) {
            return TCL_ERROR;
        }
    }

    wait = ThreadReserve(interp, thrId, tsdPtr, THREAD_RELEASE, wait);
    return wait;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadUnwindObjCmd --
 *
 *  This procedure is invoked to process the "thread::unwind" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadUnwindObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadSpecificData *tsdPtr = Init(interp);

    if (objc > 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }

    return ThreadReserve(interp, (Tcl_ThreadId)0, tsdPtr, THREAD_RELEASE, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitObjCmd --
 *
 *  This procedure is invoked to process the "thread::exit" Tcl
 *  command.  This causes an unconditional close of the thread
 *  and is GUARANTEED to cause memory leaks.  Use this with caution.
 *
 * Results:
 *  Doesn't actually return.
 *
 * Side effects:
 *  Lots.  improper clean up of resources.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int status = 666;
    ThreadSpecificData *tsdPtr = Init(interp);

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?status?");
        return TCL_ERROR;
    }

    if (objc == 2) {
        if (Tcl_GetIntFromObj(interp, objv[1], &status) != TCL_OK) {
            return TCL_ERROR;
        }
    }


    ThreadReserve(interp, (Tcl_ThreadId)0, tsdPtr, THREAD_RELEASE, 0);

    Tcl_ExitThread(status);

    return TCL_OK; /* NOT REACHED */
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadIdObjCmd --
 *
 *  This procedure is invoked to process the "thread::id" Tcl command.
 *  This returns the ID of the current thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadIdObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadSpecificData *tsdPtr = Init(interp);

    if (objc > 2) {
        goto usage;
    }

    if (objc == 2) {
        Tcl_Obj *threadIdHexObj = tsdPtr->threadIdHexObj;
        if (strcmp(Tcl_GetString(objv[1]), "-hex") != 0) {
            goto usage;
        }
        if (!threadIdHexObj) {
            Tcl_ThreadId thrId = Tcl_GetCurrentThread();
            if (thrId >= 0 && thrId <= (Tcl_ThreadId)0xffff) {
                threadIdHexObj = Tcl_ObjPrintf("%04X", (int)thrId);
            } else {
                threadIdHexObj = Tcl_ObjPrintf("%p", thrId);
            }
            Tcl_InitObjRef(tsdPtr->threadIdHexObj, threadIdHexObj);
        }
        Tcl_SetObjResult(interp, threadIdHexObj);
        return TCL_OK;
    }

    if (tsdPtr->threadObjPtr) {
        Tcl_SetObjResult(interp, tsdPtr->threadObjPtr);
    } else {
        Tcl_Obj *objPtr = Tcl_NewObj();
        objPtr->bytes = NULL;
        /* id of thread only - no reference to TSD, no mutex lock */
        ThreadObj_SetObjIntRep(objPtr, NULL, tsdPtr->threadId);
        Tcl_SetObjResult(interp, objPtr);
    }

    return TCL_OK;

usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-hex?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadNamesObjCmd --
 *
 *  This procedure is invoked to process the "thread::names" Tcl
 *  command. This returns a list of all known thread IDs.
 *  These are only threads created via this module (e.g., not
 *  driver threads or the notifier).
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadNamesObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int mode = 0;
    Tcl_Obj * listPtr;
    Init(interp);

    if (objc > 3) {
        goto usage; 
    }
    while (objc > 1) {
        const char * opt = Tcl_GetString(objv[1]);
        if (strcmp(opt, "-all") == 0) {
            mode |= THREADLIST_ALL;
        } else if (strcmp(opt, "-noself") == 0) {
            mode |= THREADLIST_NOSELF;
        } else {
            goto usage;
        }
        objc--;
        objv++;
    }
    
    listPtr = ThreadList(mode);

    Tcl_SetObjResult(interp, listPtr);

    return TCL_OK;

usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-all? ?-noself?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 */
static void
ThreadSendFreeClientDataObj(ClientData ptr)
{
    ThreadSendData *sendPtr = (ThreadSendData*)ptr;
    Tcl_UnsetObjRef((Tcl_Obj*)sendPtr->clientData);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSendObjCmd --
 *
 *  This procedure is invoked to process the "thread::send" Tcl
 *  command. This sends a script to another thread for execution.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
 static int
ThreadSendObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ret, ii = 0, flags = 0;
    Tcl_Obj *handleObj;
    ThreadSpecificData *tsdPtr;
    Tcl_ThreadId currentId;
    const char *arg;
    Tcl_Obj *script, *var = NULL;

    ThreadClbkData *clbkPtr = NULL;
    ThreadSendData *sendPtr = NULL;

    tsdPtr = Init(interp);
    currentId = tsdPtr->threadId;

    /*
     * Syntax: thread::send ?-async? ?-head? threadId script ?varName?
     */

    if (objc < 3 || objc > 6) {
        goto usage;
    }

    flags = THREAD_SEND_WAIT;

    for (ii = 1; ii < objc; ii++) {
        arg = Tcl_GetString(objv[ii]);
        if (OPT_CMP(arg, "-async")) {
            flags &= ~THREAD_SEND_WAIT;
        } else if (OPT_CMP(arg, "-head")) {
            flags |= THREAD_SEND_HEAD;
        } else {
            break;
        }
    }
    if (ii >= objc) {
        goto usage;
    }

    handleObj = objv[ii];

    if (++ii >= objc) {
        goto usage;
    }

    if (GetThreadFromObj(interp, handleObj, &tsdPtr, 0) != TCL_OK) {
        return TCL_ERROR;
    }

    script = objv[ii];
    if (++ii < objc) {
        var = objv[ii];
    }
    if (var && (flags & THREAD_SEND_WAIT) == 0) {
        /*
         * Prepare record for the callback. This is asynchronously
         * posted back to us when the target thread finishes processing.
         * We should do a vwait on the "var" to get notified.
         */

        clbkPtr = (ThreadClbkData*)ckalloc(sizeof(ThreadClbkData));
        clbkPtr->execProc   = ThreadClbkSetVar;
        clbkPtr->freeProc   = ThreadClbkFree;
        clbkPtr->interp     = interp;
        clbkPtr->threadId   = currentId;
        clbkPtr->resultObj  = NULL;
        if (tsdPtr->threadId != currentId) {
          var = Sv_DuplicateObj(var);
        }
        Tcl_IncrRefCount(var);
        clbkPtr->clientData = (ClientData)var;
    }

    /*
     * Prepare job record for the target thread
     */

    sendPtr = (ThreadSendData*)ckalloc(sizeof(ThreadSendData));
    sendPtr->interp     = NULL; /* Signal to use thread main interp */
    sendPtr->execProc   = ThreadSendEval;
    sendPtr->freeProc   = ThreadSendFreeClientDataObj;
    if (tsdPtr->threadId != currentId) {
        script = Sv_DuplicateObj(script);
    }
    Tcl_IncrRefCount(script);
    sendPtr->clientData = (ClientData)script;

    ret = ThreadSend(interp, (Tcl_ThreadId)-1, tsdPtr, sendPtr, clbkPtr, flags);

    if (var && (flags & THREAD_SEND_WAIT)) {

        /*
         * Leave job's result in passed variable
         * and return the code, like "catch" does.
         */

        Tcl_Obj *resultObj = Tcl_GetObjResult(interp);
        /* avoid usage of interim released object (the result released in error case) */
        Tcl_IncrRefCount(resultObj);
        if (Tcl_ObjSetVar2(interp, var, NULL, resultObj, TCL_LEAVE_ERR_MSG)) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
            ret = TCL_OK;
        } else {
            ret = TCL_ERROR;
        }
        Tcl_DecrRefCount(resultObj);
    }

    return ret;

usage:
    Tcl_WrongNumArgs(interp,1,objv,"?-async? ?-head? id script ?varName?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadBroadcastObjCmd --
 *
 *  This procedure is invoked to process the "thread::broadcast" Tcl
 *  command. This asynchronously sends a script to all known threads.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  Script is sent to all known threads except the caller thread.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadBroadcastObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ii, nthreads, nbroad = 0;
    Tcl_Obj *listPtr;
    Tcl_Obj *script;
    ThreadSendData *sendPtr, job;

    Init(interp);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script");
        return TCL_ERROR;
    }

    script = objv[1];

    /*
     * Get the list of known threads. Note that this one may
     * actually change (thread may exit or otherwise cease to
     * exist) while we circle in the loop below. We really do
     * not care about that here since we don't return any
     * script results to the caller and TSD is reserved in list.
     * And do not broadcast self (THREADLIST_NOSELF).
     */

    listPtr = ThreadList(THREADLIST_NOSELF | THREADLIST_FULL);
    
    if (Tcl_ListObjLength(interp, listPtr, &nthreads) != TCL_OK) {
        /* Normally NEVER OCURRED */
        Tcl_DecrRefCount(listPtr);
        return TCL_ERROR;
    }

    if (nthreads == 0) {
        goto done;
    }

    /*
     * Prepare the structure with the job description
     * to be sent asynchronously to each known thread.
     */

    job.interp     = NULL; /* Signal to use thread's main interp */
    job.execProc   = ThreadSendEval;
    job.freeProc   = ThreadSendFreeClientDataObj;
    job.clientData = NULL;

    /*
     * Now, circle this list and send each thread the script.
     * This is sent asynchronously, since we do not care what
     * are they going to do with it. Also, the event is queued
     * to the head of the event queue (as out-of-band message).
     */

    for (ii = 0; ii < nthreads; ii++) {
        ThreadSpecificData *tsdPtr;
        Tcl_Obj *objPtr;

        if (Tcl_ListObjIndex(interp, listPtr, ii, &objPtr) != TCL_OK) {
            /* Normally NEVER OCURRED */
            continue;
        }

        if (
             GetThreadFromObj(NULL, objPtr, &tsdPtr, 2) != TCL_OK
          || tsdPtr == NULL || (tsdPtr->flags & THREAD_FLAGS_STOPPED)
        ) {
            continue;
        }

        sendPtr  = (ThreadSendData*)ckalloc(sizeof(ThreadSendData));
        *sendPtr = job;
        sendPtr->clientData = (ClientData)Sv_DupIncrObj(script);
        if (ThreadSend(interp, (Tcl_ThreadId)-1, tsdPtr, sendPtr, NULL, THREAD_SEND_HEAD) != TCL_OK) {
            continue;
        };

        nbroad++;
    }

done:
    Tcl_DecrRefCount(listPtr);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(nbroad));

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWaitObjCmd --
 *
 *  This procedure is invoked to process the "thread::wait" Tcl
 *  command. This enters the event loop.
 *
 * Results:
 *  Standard Tcl result.
 *
 * Side effects:
 *  Enters the event loop.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadWaitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    Init(interp);

    if (objc > 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }

    return ThreadWait(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadErrorProcObjCmd --
 *
 *  This procedure is invoked to process the "thread::errorproc"
 *  command. This registers a procedure to handle thread errors.
 *  Empty string as the name of the procedure will reset the
 *  default behaviour, which is writing to standard error channel.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  Registers an errorproc.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadErrorProcObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    size_t len;
    char *proc;

    Init(interp);

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?proc?");
        return TCL_ERROR;
    }
    Tcl_MutexLock(&threadMutex);
    if (objc == 1) {
        if (errorProcString) {
            Tcl_SetObjResult(interp, 
                Tcl_NewStringObj(
                    errorProcString != THREAD_BGERROR_CMD ? errorProcString : "-bgerror", -1
                )
            );
        }
    } else {
        if (errorProcString && errorProcString != THREAD_BGERROR_CMD) {
            ckfree(errorProcString);
            Tcl_DeleteThreadExitHandler(ThreadFreeError, NULL);
        }
        proc = Tcl_GetString(objv[1]);
        len = objv[1]->length;
        errorThreadId = NULL;
        if (len == 0) {
            errorProcString = NULL;
        } else {
            if (strcmp(proc, "-bgerror") != 0) {
            errorThreadId = Tcl_GetCurrentThread();
                errorProcString = ckalloc(1+strlen(proc));
                strcpy(errorProcString, proc);
            Tcl_CreateThreadExitHandler(ThreadFreeError, NULL);
            } else {
                errorProcString = THREAD_BGERROR_CMD;
            }
        }
    }
    Tcl_MutexUnlock(&threadMutex);

    return TCL_OK;
}

static void
ThreadFreeError(clientData)
    ClientData clientData;
{
    // _log_debug(" !!! thread free error handler ...");
    Tcl_MutexLock(&threadMutex);
    if (errorThreadId != Tcl_GetCurrentThread()) {
        goto done;
    }
    if (errorProcString && errorProcString != THREAD_BGERROR_CMD)
        ckfree(errorProcString);
    errorThreadId = NULL;
    errorProcString = NULL;
done:
    Tcl_MutexUnlock(&threadMutex);
    // _log_debug(" !!! thread free error handler - done.");
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadJoinObjCmd --
 *
 *  This procedure is invoked to process the "thread::join" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadJoinObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadSpecificData *tsdPtr;

    Init(interp);

    /*
     * Syntax of 'join': id
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "id");
        return TCL_ERROR;
    }


    /* accept threads in any state (alive, in shutdown, or exited) */
    if (GetThreadFromObj(interp, objv[1], &tsdPtr, 2) != TCL_OK) {
        return TCL_ERROR;
    }

    if (!tsdPtr) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        return TCL_OK;
    }

    return ThreadJoin(interp, (Tcl_ThreadId)-1, tsdPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadTransferObjCmd --
 *
 *  This procedure is invoked to process the "thread::transfer" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadTransferObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{

    ThreadSpecificData *tsdPtr;
    Tcl_Channel chan;

    Init(interp);

    /*
     * Syntax of 'transfer': id channel
     */

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "id channel");
        return TCL_ERROR;
    }

    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[2]), NULL);
    if (chan == (Tcl_Channel)NULL) {
        return TCL_ERROR;
    }

    if (GetThreadFromObj(interp, objv[1], &tsdPtr, 0) != TCL_OK) {
        return TCL_ERROR;
    }

    return ThreadTransfer(interp, (Tcl_ThreadId)-1, tsdPtr, Tcl_GetTopChannel(chan));
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadDetachObjCmd --
 *
 *  This procedure is invoked to process the "thread::detach" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadDetachObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    Tcl_Channel chan;

    Init(interp);

    /*
     * Syntax: thread::detach channel
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel");
        return TCL_ERROR;
    }

    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);
    if (chan == (Tcl_Channel)NULL) {
        return TCL_ERROR;
    }

    return ThreadDetach(interp, Tcl_GetTopChannel(chan));
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadAttachObjCmd --
 *
 *  This procedure is invoked to process the "thread::attach" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadAttachObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    char *chanName;

    Init(interp);

    /*
     * Syntax: thread::attach channel
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel");
        return TCL_ERROR;
    }

    chanName = Tcl_GetString(objv[1]);
    if (Tcl_IsChannelExisting(chanName)) {
        return TCL_OK;
    }

    return ThreadAttach(interp, chanName);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExistsObjCmd --
 *
 *  This procedure is invoked to process the "thread::exists" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExistsObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int all = 0;
    ThreadSpecificData *tsdPtr;
    Tcl_Obj *objPtr;
    Init(interp);

    if (objc < 2 || objc > 3) {
        goto usage; 
    }
    if (objc == 3) {
        /* unusable also */
        if (strcmp(Tcl_GetString(objv[1]), "-all") == 0) {
            all = 1;
            objc--;
            objv++;
        } else {
            goto usage;
        }
    }
    
    if (GetThreadFromObj(interp, objv[1], &tsdPtr, 2) != TCL_OK) {
        return TCL_ERROR;
    }

    if (all) {
        objPtr = Tcl_NewIntObj(tsdPtr != NULL && tsdPtr->threadObjPtr);
    } else {
        objPtr = Tcl_NewIntObj(tsdPtr != NULL && !(tsdPtr->flags & THREAD_FLAGS_STOPPED));
    }
    Tcl_SetObjResult(interp, objPtr);
    return TCL_OK;

usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-all? id");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadConfigureObjCmd --
 *
 *  This procedure is invoked to process the Tcl "thread::configure"
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *----------------------------------------------------------------------
 */
static int
ThreadConfigureObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    char *option, *value;
    ThreadSpecificData *tsdPtr;
    int i;                      /* Iterate over arg-value pairs. */
    Tcl_DString ds;             /* DString to hold result of
                                 * calling GetThreadOption. */

    if (objc < 2 || (objc % 2 == 1 && objc != 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "threadlId ?optionName? "
                         "?value? ?optionName value?...");
        return TCL_ERROR;
    }

    Init(interp);

    if (GetThreadFromObj(interp, objv[1], &tsdPtr, 0) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        Tcl_DStringInit(&ds);
        if (ThreadGetOption(interp, (Tcl_ThreadId)-1, tsdPtr, NULL, &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;
    }
    if (objc == 3) {
        Tcl_DStringInit(&ds);
        option = Tcl_GetString(objv[2]);
        if (ThreadGetOption(interp, (Tcl_ThreadId)-1, tsdPtr, option, &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;
    }
    for (i = 3; i < objc; i += 2) {
        option = Tcl_GetString(objv[i-1]);
        value  = Tcl_GetString(objv[i]);
        if (ThreadSetOption(interp, (Tcl_ThreadId)-1, tsdPtr, option, value) != TCL_OK) {
            return TCL_ERROR;
        }
    }

    return TCL_OK;
}

#ifdef TCL_TIP285
/*
 *----------------------------------------------------------------------
 *
 * ThreadCancelObjCmd --
 *
 *  This procedure is invoked to process the "thread::cancel" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCancelObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadSpecificData *tsdPtr;
    int ii, flags;
    const char *result;

    if ((objc < 2) || (objc > 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "?-unwind? id ?result?");
        return TCL_ERROR;
    }

    flags = 0;
    ii = 1;
    if ((objc == 3) || (objc == 4)) {
        if (OPT_CMP(Tcl_GetString(objv[ii]), "-unwind")) {
            flags |= TCL_CANCEL_UNWIND;
            ii++;
        }
    }

    if (GetThreadFromObj(interp, objv[ii], &tsdPtr, 0) != TCL_OK) {
        return TCL_ERROR;
    }

    ii++;
    if (ii < objc) {
        result = Tcl_GetString(objv[ii]);
    } else {
        result = NULL;
    }

    return ThreadCancel(interp, (Tcl_ThreadId)-1, tsdPtr, result, flags);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadInscopeProcObjCmd -- , thread::inscope --
 *
 *  This simply evaluate a script, but guaranteed that all async callbacks 
 *  will be executed in the same interpreter. 
 *  Namely this will execute a script and so long not ends all other thread
 *  specified events in scope of the current interpreter.
 *
 *  Tcl core events like "after", will be executed still in the same interpreter,
 *  that has created an event.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  If thread has more as one interp, all events concern another interp will be
 *  deferred until thread not leaves this command.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadInscopeProcObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    Tcl_Interp *prevInterp;
    int ret;
    ThreadSpecificData *tsdPtr;

    if ((objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "arg ?arg ...?");
        return TCL_ERROR;
    }

    tsdPtr = Init(interp);

    /* save current tsd interp and set this one */
    prevInterp = tsdPtr->interp;
    tsdPtr->interp = interp;

    if (objc == 2) {
        ret = Tcl_EvalObjEx(interp, objv[1], 0);
    } else {
        ret = Tcl_EvalObjv(interp, objc-1, objv+1, 0);
    }

    /* because of possible removing a TSD (async callback) */
    if ( (tsdPtr = TCL_TSD_INIT(0)) ) {
        /* restore previous tsd interp (main interp) */
        tsdPtr->interp = prevInterp;
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitProcObjCmd -- , thread::exitproc --
 *
 *  This registers a procedure to handle thread exit.
 *  This command will be executed before thread ends and its 
 *  interp will be deleted.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  Registers an exitproc of thread.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadExitProcObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadSpecificData *tsdPtr;

    tsdPtr = Init(interp);

    if (tsdPtr->interp != interp) {
        return TCL_OK;
    }

    if (objc == 1) {
        if (tsdPtr->exitProcObj) {
            Tcl_SetObjResult(interp, tsdPtr->exitProcObj);
        }
    } else {
        Tcl_Obj *exitProcObj = tsdPtr->exitProcObj;
        if (objc == 2) {
            exitProcObj = objv[1];
        } else {
            exitProcObj = Tcl_NewListObj(objc-1, objv+1);
        }
        Tcl_SetObjRef(tsdPtr->exitProcObj, exitProcObj);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWaitStatObjCmd -- , thread::waitstat --
 *
 *  This registers a procedure to return the remote execution status.
 *  This command can be executed after "vwait job" to recognize a job send
 *  to the thread succeeded.
 *
 * Results:
 *  Returns status of the remote execution: 0 if success,
 *  1 - error if remote execution failed, 2 - return, etc.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadWaitStatObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "arg");
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(ThreadGetStatusOfInterpStateObj(objv[1])));

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSendEval --
 *
 *  Evaluates Tcl script passed from source to target thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSendEval(interp, clientData)
    Tcl_Interp *interp;
    ClientData clientData;
{
    ThreadSendData *sendPtr = (ThreadSendData*)clientData;
    Tcl_Obj *script = (Tcl_Obj*)sendPtr->clientData;
    int code;

/*
    int len;
    char * scrStr = Tcl_GetStringFromObj(script, &len);
    /* todo: replace with Tcl_EvalObjEx if tcl-bug fixed (return code should not be wrapped return = 2, break = 3, etc...) * /
    return Tcl_EvalEx(interp, scrStr, len, TCL_EVAL_GLOBAL);
*/

    /* prevent wrap return code, should be possible to return to the caller thread: return = 2, break = 3, etc... */
    ThreadIncrNumLevels(interp, 1);
    /* todo: make possible to eval in current context/scope/namesapace (not global) */
    code = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
    ThreadIncrNumLevels(interp, -1);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadClbkSetVar --
 *
 *  Sets the Tcl variable in the source thread, as the result
 *  of the asynchronous callback.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  New Tcl variable may be created
 *
 *----------------------------------------------------------------------
 */

static int
ThreadClbkSetVar(interp, clientData)
    Tcl_Interp *interp;
    ClientData clientData;
{
    ThreadClbkData *clbkPtr = (ThreadClbkData*)clientData;
    Tcl_Obj *var = (Tcl_Obj*)clbkPtr->clientData;
    Tcl_Obj *valObj = clbkPtr->resultObj;
    int code = TCL_OK;
    int rc = TCL_OK;


    /*
     * Get the result of the posted command.
     * We will use it to fill-in the result variable.
     */

    if (valObj == NULL) {
        valObj = Tcl_NewStringObj("unexpected state", -1);
        if (!valObj)
            return TCL_ERROR;
        Tcl_IncrRefCount(valObj);
    }
    clbkPtr->resultObj = NULL;

    /*
     * Set the result variable
     */

    if (Tcl_ObjSetVar2(interp, var, NULL, valObj,
                      TCL_GLOBAL_ONLY | TCL_LEAVE_ERR_MSG) == NULL) {
        code = TCL_ERROR;
        goto done;
    }

    /*
     * In case of error, restore error from interp state, for the real ::errorInfo/::errorCode 
     * after checking status via waitstat resp. for the bgerror mechansim
     */
    if ( ThreadGetStatusOfInterpStateObj(valObj) == TCL_ERROR ) {
        code = ThreadRestoreInterpStateFromObj(interp, valObj);
        /* don't log error via ThreadErrorProc(interp) here, because it will be
         * done in ThreadEventProc for the current thread */
    }

  done:
    Tcl_DecrRefCount(valObj);
    return code;
}

/*
 *----------------------------------------------------------------------
 */
void
ThreadClbkFree(clientData)
    ClientData clientData;
{
    ThreadClbkData *clbkPtr = (ThreadClbkData*)clientData;
    /* tcl object with variable reference: */
    Tcl_UnsetObjRef((Tcl_Obj*)clbkPtr->clientData);
    /* tcl object with back reference (result of error state): */
    Tcl_UnsetObjRef(clbkPtr->resultObj);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreate --
 *
 *  This procedure is invoked to create a thread containing an
 *  interp to run a script. This returns after the thread has
 *  started executing.
 *
 * Results:
 *  A standard Tcl result, which is the thread ID.
 *
 * Side effects:
 *  Create a thread.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCreate(interp, script, stacksize, flags, preserve)
    Tcl_Interp *interp;         /* Current interpreter. */
    Tcl_Obj    *script;         /* Script to evaluate or NULL for "thread::wait" */
    int         stacksize;      /* Zero for default size */
    int         flags;          /* Zero for no flags */
    int         preserve;       /* If true, reserve the thread */
{
    ThreadCtrl ctrl;
    Tcl_ThreadId thrId;
    Tcl_Obj * objPtr;

#ifdef NS_AOLSERVER
    ctrl.cd = Tcl_GetAssocData(interp, "thread:nsd", NULL);
#endif
    ctrl.script   = script;
    ctrl.condWait = NULL;
    ctrl.flags    = 0;
    ctrl.tsdPtr = NULL;

    Tcl_MutexLock(&threadMutex);
    if (Tcl_CreateThread(&thrId, NewThread, (ClientData)&ctrl,
            stacksize, flags) != TCL_OK) {
        Tcl_MutexUnlock(&threadMutex);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("can't create a new thread", -1));
        return TCL_ERROR;
    }

    /*
     * Wait for the thread to start because it is using
     * the ThreadCtrl argument which is on our stack.
     */

    while (ctrl.flags == 0) {
        Tcl_ConditionWait(&ctrl.condWait, &threadMutex, NULL);
    }
    if (preserve) {
        ctrl.tsdPtr->refCount++;
    }

    Tcl_MutexUnlock(&threadMutex);
    Tcl_ConditionFinalize(&ctrl.condWait);

    _log_debug("thread created ... [%04X]", thrId);

    objPtr = Tcl_NewObj();
    objPtr->bytes = NULL;
    ThreadObj_SetObjIntRep(objPtr, ctrl.tsdPtr, ctrl.tsdPtr->threadId);
    /* tsdPtr->objRefCount++; - already incremented in NewThread. */

    Tcl_SetObjResult(interp, objPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NewThread --
 *
 *    This routine is the "main()" for a new thread whose task is to
 *    execute a single TCL script. The argument to this function is
 *    a pointer to a structure that contains the text of the Tcl script
 *    to be executed, plus some synchronization primitives. Those are
 *    used so the caller gets signalized when the new thread has
 *    done its initialization.
 *
 *    Space to hold the ThreadControl structure itself is reserved on
 *    the stack of the calling function. The two condition variables
 *    in the ThreadControl structure are destroyed by the calling
 *    function as well. The calling function will destroy the
 *    ThreadControl structure and the condition variable as soon as
 *    ctrlPtr->condWait is signaled, so this routine must make copies
 *    of any data it might need after that point.
 *
 * Results:
 *    none
 *
 * Side effects:
 *    A Tcl script is executed in a new thread.
 *    Value of objRefCount for new thread is incremented.
 *
 *----------------------------------------------------------------------
 */

Tcl_ThreadCreateType
NewThread(clientData)
    ClientData clientData;
{
    ThreadCtrl *ctrlPtr = (ThreadCtrl *)clientData;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(1);
    Tcl_Interp *interp;
    int result = TCL_OK;

    /* own thread */
    tsdPtr->flags |= THREAD_FLAGS_OWN_THREAD;

    /* create exit handler for cleanups */
    Tcl_CreateThreadExitHandler(ThreadExitHandler, NULL);

    /* 
     * Create a tcl object with thread-id, used in this thread only 
     * This reference will be decremented in ThreadExitHandler
     * We don't lock mutex here, because TSD is still unknown (not in the list).
     */
    ThreadObj_InitLocalObj(tsdPtr);

    /*
     * Initialize the interpreter. The bad thing here is that we
     * assume that initialization of the Tcl interp will be
     * error free, which it may not. In the future we must recover
     * from this and exit gracefully (this is not that easy as
     * it seems on the first glance...)
     *
     * [TODO] check result of interp/thread init.
     */

#ifdef NS_AOLSERVER
    NsThreadInterpData *md = (NsThreadInterpData *)ctrlPtr->cd;
    Ns_ThreadSetName("-tclthread-");
    interp = (Tcl_Interp*)Ns_TclAllocateInterp(md ? md->server : NULL);
#else
    interp = Tcl_CreateInterp();
    result = Tcl_Init(interp);
#endif
    
    tsdPtr->interp = interp;

#if !defined(NS_AOLSERVER) || (defined(NS_MAJOR_VERSION) && NS_MAJOR_VERSION >= 4)
    result = Thread_Init(interp);
#endif

    /*
     * We need to keep a pointer to the alloc'ed mem of the script
     * we are eval'ing, for the case that we exit during evaluation
     */

    tsdPtr->evalScript = Sv_DupIncrObj(ctrlPtr->script);

    /*
     * Increment objRefCount for caller right now.
     * Notify the parent we are alive.
     * Update the list of threads.
     */

    ctrlPtr->tsdPtr = tsdPtr;
    tsdPtr->objRefCount++;
    ctrlPtr->flags = 1;

    if (!tsdPtr->nextPtr) {
        Tcl_MutexLock(&threadMutex);
        SpliceIn(tsdPtr, threadList);
        Tcl_MutexUnlock(&threadMutex);
    }

    Tcl_ConditionNotify(&ctrlPtr->condWait);

    /*
     * Run the script or just wait.
     */

    Tcl_Preserve((ClientData)tsdPtr->interp);

    if (tsdPtr->evalScript) {
        result = Tcl_EvalObjEx(tsdPtr->interp, tsdPtr->evalScript, TCL_EVAL_GLOBAL);
        if (result != TCL_OK) {
            ThreadErrorProc(tsdPtr->interp);
        }
    } else {
        result = ThreadWait(tsdPtr->interp);
    }

    _log_debug("ending thread proc ...");

    /*
     * if thread leaves processing without "thread::release"
     * (some work without thread::wait, waiting inside vwait, etc.) 
     */
    if (tsdPtr->refCount > 0 || (tsdPtr->flags & THREAD_FLAGS_STOPPED) == 0) {
        Tcl_MutexLock(&tsdPtr->mutex);
        tsdPtr->refCount = 0;
        tsdPtr->flags |= THREAD_FLAGS_STOPPED;
        Tcl_MutexUnlock(&tsdPtr->mutex);
    }

    /*
     * Execute on-exit handler if it was set
     */
    if (tsdPtr->exitProcObj) {
        if (tsdPtr->interp && !Tcl_InterpDeleted(tsdPtr->interp)) {
            if (Tcl_EvalObjEx(tsdPtr->interp, tsdPtr->exitProcObj, TCL_EVAL_GLOBAL) != TCL_OK) {
                ThreadErrorProc(tsdPtr->interp);
            }
        }
    }
    /* remove if it was not reset in handler */
    Tcl_UnsetObjRef(tsdPtr->exitProcObj);

    /*
     * Now that the event processor for this thread is closing,
     * delete all pending thread::send and thread::transfer events.
     * These events are owned by us.  We don't delete anyone else's
     * events, but ours.
     */

    Tcl_DeleteEvents((Tcl_EventDeleteProc*)ThreadDeleteEvent, NULL);

    /*
     * It is up to all other extensions, including Tk, to be responsible
     * for their own events when they receive their Tcl_CallWhenDeleted
     * notice when we delete this interp.
     */

    if (tsdPtr->interp) {
#ifdef NS_AOLSERVER
        Ns_TclMarkForDelete(tsdPtr->interp);
        Ns_TclDeAllocateInterp(tsdPtr->interp);
#else
        Tcl_DeleteInterp(tsdPtr->interp);
#endif
        Tcl_Release((ClientData)tsdPtr->interp);

        /*tsdPtr->interp = NULL;*/
    }
    /*
     * Tcl_ExitThread calls Tcl_FinalizeThread() indirectly which calls
     * ThreadExitHandlers and cleans the notifier as well as other sub-
     * systems that save thread state data.
     */

    Tcl_ExitThread(result);

    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadErrorProc --
 *
 *  Send a message to the thread willing to hear about errors.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  Send an event.
 *
 *----------------------------------------------------------------------
 */

void
ThreadErrorProc(interp)
    Tcl_Interp *interp;         /* Interp that failed */
{
    if (errorProcString == THREAD_BGERROR_CMD) {
        /* 
         * execute background command registered with "interp bgerror" (typically ::tcl::Bgerror)
         * if it is not registered, standard error will be used to read
         */
        _log_debug(" !!! bgerror ...")
        Tcl_BackgroundError(interp);
        return;
    }
    if (errorProcString == NULL) {
        const char *errorInfo;
        char buf[THREAD_HNDLMAXLEN];
        Tcl_Channel errChannel;

        errorInfo = Tcl_GetVar2(interp, "errorInfo", NULL, TCL_GLOBAL_ONLY);
        if (errorInfo == NULL) {
            errorInfo = "";
        }

#ifdef NS_AOLSERVER
        Ns_Log(Error, "%s\n%s", Tcl_GetString(Tcl_GetObjResult(interp)), errorInfo);
#else
        errChannel = Tcl_GetStdChannel(TCL_STDERR);
        if (errChannel == NULL) {
            /* Fixes the [#634845] bug; credits to
             * Wojciech Kocjan <wojciech@kocjan.org> */
            return;
        }
        ThreadGetHandle(Tcl_GetCurrentThread(), buf);
        Tcl_WriteChars(errChannel, "Error from thread ", -1);
        Tcl_WriteChars(errChannel, buf, -1);
        Tcl_WriteChars(errChannel, "\n", 1);
        Tcl_WriteChars(errChannel, errorInfo, -1);
        Tcl_WriteChars(errChannel, "\n", 1);
#endif
    } else {
        ThreadSendData *sendPtr;
        Tcl_Obj *script, *objv[3];
        /* 
         * send error to registered command errorProcString of errorThreadId
         */

        objv[0] = Tcl_NewStringObj(errorProcString, -1);
        objv[1] = Tcl_NewObj();
        objv[1]->bytes = NULL;
        ThreadObj_SetObjIntRep(objv[1], NULL, Tcl_GetCurrentThread());
        objv[2] = Tcl_GetVar2Ex(interp, "errorInfo", NULL, TCL_GLOBAL_ONLY);
        objv[2] = Sv_DuplicateObj(objv[2]);
        script = Tcl_NewListObj(3, objv);
        Tcl_IncrRefCount(script);

        sendPtr = (ThreadSendData*)ckalloc(sizeof(ThreadSendData));
        sendPtr->execProc   = ThreadSendEval;
        sendPtr->freeProc   = ThreadSendFreeClientDataObj;
        sendPtr->clientData = (ClientData)script;
        sendPtr->interp     = NULL;

        ThreadSend(interp, errorThreadId, NULL, sendPtr, NULL, 0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadList --
 *
 *  Return a list of threads running Tcl interpreters.
 *
 * Parameters:
 *  mode - or-ed combination of flags:
 *    THREADLIST_ALL    - also unusable threads, otherwise alive only;
 *    THREADLIST_NOSELF - ignore caller thread;
 *    THREADLIST_FULL   - full internal representation (also referenced TSD), otherwise thread id only.
 *
 * Results:
 *  List of thread objects.
 *
 * Side effects:
 *  refCount of list object should be decremented if unused.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
ThreadList(int mode)
{
    Tcl_Obj *listPtr, *objPtr;
    ThreadSpecificData *tsdPtr;
    Tcl_ThreadId selfId = Tcl_GetCurrentThread();

    listPtr = Tcl_NewListObj(0, NULL);
    Tcl_MutexLock(&threadMutex);

    for (tsdPtr = threadList; tsdPtr; tsdPtr = tsdPtr->nextPtr) {
        if (!(mode & THREADLIST_ALL) && (tsdPtr->flags & THREAD_FLAGS_STOPPED)) {
            continue;
        }
        objPtr = NULL;
        if (tsdPtr->threadId == selfId) {
            if (mode & THREADLIST_NOSELF) {
                continue;
            }
            /* we can use threadObjPtr (can't be released - we are in this thread) */
            objPtr = tsdPtr->threadObjPtr;
        } 
        if (!objPtr) {
            objPtr = Tcl_NewObj();
            objPtr->bytes = NULL;
            if (!(mode & THREADLIST_FULL)) {
                /* id of thread only - no reference to TSD, no TSD mutex lock each time */
                ThreadObj_SetObjIntRep(objPtr, NULL, tsdPtr->threadId);
            } else {
                Tcl_MutexLock(&tsdPtr->mutex);
                tsdPtr->objRefCount++;
                Tcl_MutexUnlock(&tsdPtr->mutex);
                ThreadObj_SetObjIntRep(objPtr, tsdPtr, tsdPtr->threadId);
            }
        }
        /* add to list */
        if (Tcl_ListObjAppendElement(NULL, listPtr, objPtr) != TCL_OK) {
            /* Normally NEVER OCURRED */
            if (objPtr != tsdPtr->threadObjPtr) Tcl_DecrRefCount(objPtr); 
            Tcl_DecrRefCount(listPtr); listPtr = NULL;
            goto done;
        }
    }

done:
    Tcl_MutexUnlock(&threadMutex);

    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExists --
 *
 *  Test whether a thread given by it's id is known to us.
 *
 * Results:
 *  Pointer to thread specific data structure or
 *  NULL if no thread with given ID found
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExists(thrId, tsdPtr, shutDownLevel)
     Tcl_ThreadId thrId;
     ThreadSpecificData *tsdPtr;
     int shutDownLevel;
{
    if (!tsdPtr) {
        tsdPtr = GetThreadFromId_Lock(NULL, thrId, shutDownLevel);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
        if (!tsdPtr)
            return 0;
        Tcl_MutexUnlock(&tsdPtr->mutex);

        return 1;
    }
    

    return tsdPtr != NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExistsInner --
 *
 *  Test whether a thread given by it's id is known to us. Assumes
 *  caller holds the thread mutex.
 *
 * Results:
 *  Pointer to thread specific data structure or
 *  NULL if no thread with given ID found
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static ThreadSpecificData *
ThreadExistsInner(thrId)
    Tcl_ThreadId thrId;              /* Thread id to look for. */
{
    ThreadSpecificData *tsdPtr;

    for (tsdPtr = threadList; tsdPtr; tsdPtr = tsdPtr->nextPtr) {
        if (tsdPtr->threadId == thrId) {
            return tsdPtr;
        }
    }

    return NULL;
}

#ifdef TCL_TIP285
/*
 *----------------------------------------------------------------------
 *
 * ThreadCancel --
 *
 *    Cancels a script in another thread.
 *
 * Results:
 *    A standard Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCancel(interp, thrId, tsdPtr, result, flags)
    Tcl_Interp  *interp;        /* The current interpreter. */
    Tcl_ThreadId thrId;         /* Thread ID of other interpreter. */
    ThreadSpecificData *tsdPtr; /* TSD of the thread or NULL */
    const char *result;         /* The error message or NULL for default. */
    int flags;                  /* Flags for Tcl_CancelEval. */
{
    int code;
    Tcl_Obj *resultObj = NULL;

    if (!haveInterpCancel) {
        Tcl_AppendResult(interp, "not supported with this Tcl version", NULL);
        return TCL_ERROR;
    }

    if (tsdPtr) {
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        tsdPtr = GetThreadFromId_Lock(interp, thrId, 0);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
        if (!tsdPtr) {
            return TCL_ERROR;
        }
    }

    if (tsdPtr->flags & THREAD_FLAGS_STOPPED) {
        Tcl_MutexUnlock(&tsdPtr->mutex);
        ErrorNoSuchThread(interp, thrId);
        return TCL_ERROR;
    }

    if (result != NULL) {
        resultObj = Tcl_NewStringObj(result, -1);
    }

    code = Tcl_CancelEval(tsdPtr->interp, resultObj, NULL, flags);

    Tcl_MutexUnlock(&tsdPtr->mutex);
    return code;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadJoin --
 *
 *  Wait for the exit of a different thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  The status of the exiting thread is left in the interp result
 *  area, but only in the case of success.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadJoin(interp, thrId, tsdPtr)
    Tcl_Interp  *interp;        /* The current interpreter. */
    Tcl_ThreadId thrId;         /* Thread ID of other interpreter. */
    ThreadSpecificData *tsdPtr; /* TSD of the thread or NULL */
{
    int ret, state;

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
    }

    ret = Tcl_JoinThread(thrId, &state);

    if (ret == TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(state));
    } else {
        char thrHandle[THREAD_HNDLMAXLEN];
        ThreadGetHandle(thrId, thrHandle);
        Tcl_AppendResult(interp, "cannot join thread ", thrHandle, NULL);
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadTransfer --
 *
 *  Transfers the specified channel which must not be shared and has
 *  to be registered in the given interp from that location to the
 *  main interp of the specified thread.
 *
 *  Thanks to Anreas Kupries for the initial implementation.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  The thread-global lists of all known channels of both threads
 *  involved (specified and current) are modified. The channel is
 *  moved, all event handling for the channel is killed.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadTransfer(interp, thrId, tsdPtr, chan)
    Tcl_Interp *interp;         /* The current interpreter. */
    Tcl_ThreadId thrId;         /* Thread Id of other interpreter. */
    ThreadSpecificData *tsdPtr; /* TSD of the target thread or NULL */
    Tcl_Channel  chan;          /* The channel to transfer */
{
    /* Steps to perform for the transfer:
     *
     * i.   Sanity checks: chan has to registered in interp, must not be
     *      shared. This automatically excludes the special channels for
     *      stdin, stdout and stderr!
     * ii.  Clear event handling.
     * iii. Bump reference counter up to prevent destruction during the
     *      following unregister, then unregister the channel from the
     *      interp. Remove it from the thread-global list of all channels
     *      too.
     * iv.  Wrap the channel into an event and send that to the other
     *      thread, then wait for the other thread to process our message.
     * v.   The event procedure called by the other thread is
     *      'TransferEventProc'. It links the channel into the
     *      thread-global list of channels for that thread, registers it
     *      in the main interp of the other thread, removes the artificial
     *      reference, at last notifies this thread of the sucessful
     *      transfer. This allows this thread then to proceed.
     */

    TransferEvent *evPtr;
    TransferResult *resultPtr;

    if (!Tcl_IsChannelRegistered(interp, chan)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("channel is not registered here", -1));
    }
    if (Tcl_IsChannelShared(chan)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("channel is shared", -1));
        return TCL_ERROR;
    }

    /*
     * Short circuit transfers to ourself.  Nothing to do.
     */

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
        if (thrId == Tcl_GetCurrentThread()) {
            return TCL_OK;
        }
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        if (thrId == Tcl_GetCurrentThread()) {
            return TCL_OK;
        }
        tsdPtr = GetThreadFromId_Lock(interp, thrId, 0);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
        if (!tsdPtr) {
            return TCL_ERROR;
        }
    }

    /*
     * Verify the thread exists.
     */

    if (tsdPtr->flags & THREAD_FLAGS_STOPPED) {
        Tcl_MutexUnlock(&tsdPtr->mutex);
        ErrorNoSuchThread(interp, thrId);
        return TCL_ERROR;
    }

    /*
     * Cut the channel out of the interp/thread
     */

    ThreadCutChannel(interp, chan);

    /*
     * Wrap it into an event.
     */

    resultPtr = (TransferResult*)ckalloc(sizeof(TransferResult));
    evPtr     = (TransferEvent *)ckalloc(sizeof(TransferEvent));

    evPtr->chan       = chan;
    evPtr->event.proc = TransferEventProc;
    evPtr->resultPtr  = resultPtr;

    /*
     * Initialize the result fields.
     */

    resultPtr->done       = (Tcl_Condition) NULL;
    resultPtr->resultCode = -1;
    resultPtr->resultMsg  = NULL;

    /*
     * Maintain the cleanup list.
     */

    resultPtr->srcThreadId = Tcl_GetCurrentThread();
    resultPtr->eventPtr    = evPtr;

    /*
     * Queue the event and poke the other thread's notifier.
     */

    Tcl_ThreadQueueEvent(thrId, (Tcl_Event*)evPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(thrId);

    /*
     * (*) Block until the other thread has either processed the transfer
     * or rejected it.
     */

    while (resultPtr->resultCode < 0) {
        Tcl_ConditionWait(&resultPtr->done, &tsdPtr->mutex, NULL);
    }

    resultPtr->eventPtr = NULL;

    Tcl_MutexUnlock(&tsdPtr->mutex);

    Tcl_ConditionFinalize(&resultPtr->done);

    /*
     * Process the result now.
     */

    if (resultPtr->resultCode != TCL_OK) {

        /*
         * Transfer failed, restore old state of channel with respect
         * to current thread and specified interp.
         */

        Tcl_SpliceChannel(chan);
        Tcl_RegisterChannel(interp, chan);
        Tcl_UnregisterChannel((Tcl_Interp *) NULL, chan);
        Tcl_AppendResult(interp, "transfer failed: ", NULL);

        if (resultPtr->resultMsg) {
            Tcl_SetObjResult(interp, resultPtr->resultMsg);
            Tcl_DecrRefCount(resultPtr->resultMsg);
        } else {
            Tcl_AppendResult(interp, "for reasons unknown", NULL);
        }
        ckfree((char *)resultPtr);

        return TCL_ERROR;
    }

    if (resultPtr->resultMsg) {
        Tcl_DecrRefCount(resultPtr->resultMsg);
    }
    ckfree((char *)resultPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadDetach --
 *
 *  Detaches the specified channel which must not be shared and has
 *  to be registered in the given interp. The detached channel is
 *  left in the transfer list until some other thread attaches it
 +  by calling the "thread::attach" command.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  The thread-global lists of all detached channels (detachedList)
 *  is modified. All event handling for the channel is killed.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadDetach(interp, chan)
    Tcl_Interp *interp;         /* The current interpreter. */
    Tcl_Channel chan;           /* The channel to detach */
{
    DetachedChannel *detChan;

    if (!Tcl_IsChannelRegistered(interp, chan)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("channel is not registered here", -1));
    }
    if (Tcl_IsChannelShared(chan)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("channel is shared", -1));
        return TCL_ERROR;
    }

    /*
     * Cut the channel out of the interp/thread
     */

    ThreadCutChannel(interp, chan);

    /*
     * Add it to list of detached channels, 
     * to be found again with ThreadAttach resp. "thread::attach"
     */

    detChan = (DetachedChannel*)ckalloc(sizeof(DetachedChannel));
    detChan->chan = chan;

    Tcl_MutexLock(&threadMutex);
    SpliceIn(detChan, detachedList);
    Tcl_MutexUnlock(&threadMutex);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadAttach --
 *
 *  Attaches the previously detached channel into the current
 *  interpreter.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  The thread-global lists of all detached channels (detachedList)
 *  is modified.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadAttach(interp, chanName)
    Tcl_Interp *interp;         /* The current interpreter. */
    char *chanName;             /* The name of the channel to detach */
{
    Tcl_Channel chan = NULL;
    DetachedChannel *detChan;

    /*
     * Locate the channel to attach by looking up its name in
     * the list of transfered channels. Watch that we don't
     * hit the regular channel transfer event.
     */

    Tcl_MutexLock(&threadMutex);
    for (detChan = detachedList; detChan; detChan = detChan->nextPtr) {
        chan = detChan->chan;
        if (strcmp(Tcl_GetChannelName(chan), chanName) == 0) {
            if (Tcl_IsChannelExisting(chanName)) {
                Tcl_MutexUnlock(&threadMutex);
                Tcl_AppendResult(interp, "channel already exists", NULL);
                Tcl_SetErrorCode(interp, "TCL", "EEXIST", NULL);
                return TCL_ERROR;
            }
            SpliceOut(detChan, detachedList);
            break;
        }
    }
    Tcl_MutexUnlock(&threadMutex);

    if (!detChan) {
        Tcl_AppendResult(interp, "channel not detached", NULL);
        Tcl_SetErrorCode(interp, "TCL", "ENOENT", NULL);
        return TCL_ERROR;
    }

    ckfree((char*)detChan);
    /*
     * Splice channel into the current interpreter
     */

    Tcl_SpliceChannel(chan);
    Tcl_RegisterChannel(interp, chan);
    Tcl_UnregisterChannel((Tcl_Interp *)NULL, chan);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSend --
 *
 *  Run the procedure in other thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSend(interp, thrId, tsdPtr, send, clbk, flags)
    Tcl_Interp     *interp;      /* The current interpreter. */
    Tcl_ThreadId    thrId;       /* Thread Id of other thread. */
    ThreadSpecificData *tsdPtr;  /* TSD of the target thread or NULL */
    ThreadSendData *send;        /* Pointer to structure with work to do */
    ThreadClbkData *clbk;        /* Opt. callback structure (may be NULL) */
    int             flags;       /* Wait or queue to tail */
{

    int code;
    ThreadEvent *eventPtr;
    ThreadEventResult *resultPtr;

    /*
     * Caller wants to be notified, so we must take care
     * it's interpreter stays alive until we've finished.
     */

    if (send && send->interp && !(flags & THREAD_SEND_CLBK)) {
        Tcl_Preserve((ClientData)send->interp);
    }

    if (clbk && clbk->interp) {
        Tcl_Preserve((ClientData)clbk->interp);
    }

    /*
     * Verify the thread exists and is not in the error state.
     * The thread is in the error state only if we've configured
     * it to unwind on script evaluation error and last script
     * evaluation resulted in error actually.
     */

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        tsdPtr = GetThreadFromId_Lock(interp, thrId, 0);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
    }

    if ( !tsdPtr || tsdPtr->flags & (THREAD_FLAGS_INERROR|THREAD_FLAGS_STOPPED) ) {
        int inerror = 0;
        if (tsdPtr) {
            inerror = (tsdPtr->flags & THREAD_FLAGS_INERROR);
            Tcl_MutexUnlock(&tsdPtr->mutex);
        }
        ThreadFreeProc((ClientData)send);
        if (clbk) {
            ThreadFreeProc((ClientData)clbk);
        }
        if (inerror) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("thread is in error", -1));
        } else {
            ErrorNoSuchThread(interp, thrId);
        }
        return TCL_ERROR;
    }

    /*
     * Execute direct in self by sync exec (async exec processed below).
     */

    if (thrId == Tcl_GetCurrentThread()) {
        if ((flags & THREAD_SEND_WAIT)) {
            int code;
            Tcl_MutexUnlock(&tsdPtr->mutex);
            code = (*send->execProc)(interp, (ClientData)send);
            ThreadFreeProc((ClientData)send);
            return code;
        }
    }

    /*
     * Create the event for target thread event queue.
     */

    eventPtr = (ThreadEvent*)ckalloc(sizeof(ThreadEvent));
    eventPtr->sendData = send;
    eventPtr->clbkData = clbk;

    /*
     * Target thread about to service
     * another event
     */

    if (tsdPtr->maxEventsCount) {
        tsdPtr->eventsPending++;
    }

    if ((flags & THREAD_SEND_WAIT) == 0) {
        resultPtr              = NULL;
        eventPtr->resultPtr    = NULL;
    } else {
        resultPtr = (ThreadEventResult*)ckalloc(sizeof(ThreadEventResult));
        resultPtr->done        = (Tcl_Condition)NULL;
        resultPtr->resultObj   = NULL;
        resultPtr->srcThreadId = Tcl_GetCurrentThread();
        resultPtr->eventPtr    = eventPtr;

        eventPtr->resultPtr    = resultPtr;
    }

    /*
     * Queue the event and poke the other thread's notifier.
     */

    eventPtr->event.proc = ThreadEventProc;
    if (thrId != Tcl_GetCurrentThread()) {
        Tcl_ThreadQueueEvent(thrId, (Tcl_Event*)eventPtr, 
            (flags & THREAD_SEND_HEAD) ? TCL_QUEUE_HEAD : TCL_QUEUE_TAIL);
        Tcl_ThreadAlert(thrId);
    } else {
        /* event to ourself */
        Tcl_QueueEvent((Tcl_Event*)eventPtr, 
            (flags & THREAD_SEND_HEAD) ? TCL_QUEUE_HEAD : TCL_QUEUE_TAIL);
    }

    if ((flags & THREAD_SEND_WAIT) == 0) {
        /*
         * Might potentially spend some time here, until the
         * worker thread cleans up its queue a little bit.
         */
        if ((flags & THREAD_SEND_CLBK) == 0) {
            while (tsdPtr->maxEventsCount &&
                   tsdPtr->eventsPending > tsdPtr->maxEventsCount) {
                Tcl_ConditionWait(&tsdPtr->doOneEvent, &tsdPtr->mutex, NULL);
            }
        }
        Tcl_MutexUnlock(&tsdPtr->mutex);
        return TCL_OK;
    }

    /*
     * Block on the result indefinitely.
     */

    Tcl_ResetResult(interp);

    while ( resultPtr->resultObj == NULL ) {
        Tcl_ConditionWait(&resultPtr->done, &tsdPtr->mutex, NULL);
    }

    Tcl_MutexUnlock(&tsdPtr->mutex);

    /*
     * Return result to caller
     */

    code = ThreadRestoreInterpStateFromObj(interp, resultPtr->resultObj);
    Tcl_DecrRefCount(resultPtr->resultObj);
    resultPtr->resultObj = NULL;


    /*
     * Cleanup
     */

    Tcl_ConditionFinalize(&resultPtr->done);
    ckfree((char*)resultPtr);

    return code;
}

/*
 *----------------------------------------------------------------------
 */
struct ThreadSpecificData *
Thread_TSD_Init() {
    ThreadSpecificData *tsdPtr;
    tsdPtr = TCL_TSD_INIT(1);

    /* create exit handler for cleanups */
    Tcl_CreateThreadExitHandler(ThreadExitHandler, NULL);

    /* 
     * Create a tcl object with thread-id, used in this thread only 
     * This reference will be decremented in ThreadExitHandler
     */
    ThreadObj_InitLocalObj(tsdPtr);

    return tsdPtr;
}
/*
 *----------------------------------------------------------------------
 */
int 
Thread_Stopped(struct ThreadSpecificData * tsdPtr)
{
    return (tsdPtr->flags & THREAD_FLAGS_STOPPED) != 0;
};


/*
 *----------------------------------------------------------------------
 *
 * ThreadWait --
 *
 *  Waits for events and process them as they come, until signaled
 *  to stop.
 *
 * Results:
 *  Standard Tcl result.
 *
 * Side effects:
 *  Deletes any thread::send or thread::transfer events that are
 *  pending.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadWait(Tcl_Interp *interp)
{
    int code = TCL_OK;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(1);
    int canrun = (tsdPtr->flags & THREAD_FLAGS_STOPPED) == 0;

    /*
     * Process events until signaled to stop.
     */

    _log_debug("thread wait ... %d", canrun);
    while (canrun) {

        /*
         * About to service another event.
         * Wake-up eventual sleepers.
         */

        if (tsdPtr->maxEventsCount) {
            Tcl_MutexLock(&tsdPtr->mutex);
            tsdPtr->eventsPending--;
            if (tsdPtr->doOneEvent) {
                Tcl_ConditionNotify(&tsdPtr->doOneEvent);
            }
            Tcl_MutexUnlock(&tsdPtr->mutex);
        }

        /*
         * Attempt to process one event, blocking forever until an
         * event is actually received.  The event processed may cause
         * a script in progress to be canceled or exceed its limit;
         * therefore, check for these conditions if we are able to
         * (i.e. we are running in a high enough version of Tcl).
         */

        _log_debug("thread wait ... %d", canrun);
        Tcl_DoOneEvent(TCL_ALL_EVENTS);

#ifdef TCL_TIP285
        if (haveInterpCancel) {

            /*
             * If the script has been unwound, bail out immediately. This does
             * not follow the recommended guidelines for how extensions should
             * handle the script cancellation functionality because this is
             * not a "normal" extension. Most extensions do not have a command
             * that simply enters an infinite Tcl event loop. Normal extensions
             * should not specify the TCL_CANCEL_UNWIND when calling the
             * Tcl_Canceled function to check if the command has been canceled.
             */

            if (Tcl_Canceled(tsdPtr->interp,
                    TCL_LEAVE_ERR_MSG | TCL_CANCEL_UNWIND) == TCL_ERROR) {
                code = TCL_ERROR;
                break;
            }
        }
#endif
#ifdef TCL_TIP143
        if (haveInterpLimit) {
            if (Tcl_LimitExceeded(tsdPtr->interp)) {
                code = TCL_ERROR;
                break;
            }
        }
#endif

        /*
         * Test stop condition under mutex since
         * some other thread may flip our flags.
         */

        canrun = (tsdPtr->flags & THREAD_FLAGS_STOPPED) == 0;
    }

#if defined(TCL_TIP143) || defined(TCL_TIP285)
    /*
     * If the event processing loop above was terminated due to a
     * script in progress being canceled or exceeding its limits,
     * transfer the error to the current interpreter.
     */

    if (code != TCL_OK) {
        char buf[THREAD_HNDLMAXLEN];
        Tcl_Obj *errorInfo;

        errorInfo = Tcl_GetVar2Ex(tsdPtr->interp, "errorInfo", NULL, TCL_GLOBAL_ONLY);
        if (errorInfo == NULL) {
            errorInfo = Tcl_GetObjResult(tsdPtr->interp);
        }

        ThreadGetHandle(Tcl_GetCurrentThread(), buf);
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("Error from thread %s\n%s",
                buf, Tcl_GetString(errorInfo)));
    }
#endif

    _log_debug("thread end of wait ... %d", canrun);

    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadReserve --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadNopProc(Tcl_Event *evPtr, int mask) {
    return 1;
}

int
ThreadReserve(interp, thrId, tsdPtr, operation, wait)
    Tcl_Interp  *interp;                /* Current interpreter */
    Tcl_ThreadId thrId;                 /* Target thread ID */
    ThreadSpecificData *tsdPtr;         /* TSD of the thread or NULL */
    int operation;                      /* THREAD_RESERVE | THREAD_RELEASE */
    int wait;                           /* Wait for thread to exit */
{
    int users;

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        if (thrId == (Tcl_ThreadId)0) {
            tsdPtr = TCL_TSD_INIT(1);
            Tcl_MutexLock(&tsdPtr->mutex);
        } else {
            tsdPtr = GetThreadFromId_Lock(interp, thrId, (operation == THREAD_RESERVE ? 0 : 1) );
            /* Tcl_MutexLock(&tsdPtr->mutex); */
        }
    }
    if (!tsdPtr) {
        return TCL_ERROR;
    }

    /*
     * Check the given thread
     */

    switch (operation) {
      case THREAD_RESERVE:
        if ( (tsdPtr->flags & THREAD_FLAGS_STOPPED) ) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            if (interp) {
                ErrorNoSuchThread(interp, thrId);
            }
            return TCL_ERROR;
        }
        ++tsdPtr->refCount;
        wait = 0;
        _log_debug(" !!! thread reserve [%04X] ...", thrId);
      break;
      case THREAD_RELEASE:
        --tsdPtr->refCount; 
        _log_debug(" !!! thread release [%04X] ... %d", thrId, wait);
      break;
    }

    users = tsdPtr->refCount;

    if (users <= 0 && !(tsdPtr->flags & THREAD_FLAGS_STOPPED)) {

        /*
         * We're last attached user, so tear down the *target* thread
         */

        _log_debug("thread stop ... [%04X]", thrId);
        tsdPtr->flags |= THREAD_FLAGS_STOPPED;

        if (thrId && thrId != Tcl_GetCurrentThread() /* Not current! */) {
            Tcl_Event *evPtr;

            /*
             * Don't remove thread from list right now - cause it can't be found 
             * with "thread::names" (for example no wait possibility),
             * instead of we will check THREAD_FLAGS_STOPPED in "thread::send".
             */

            /*
             * Send an NOP event, just to wake-up target thread.
             * It should immediately exit thereafter. We might get
             * stuck here for long time if user really wants to
             * be absolutely sure that the thread has exited.
             */

            evPtr = (Tcl_Event*)ckalloc(sizeof(Tcl_Event));
            evPtr->proc = ThreadNopProc;

            Tcl_ThreadQueueEvent(thrId, (Tcl_Event*)evPtr, TCL_QUEUE_TAIL);
            Tcl_ThreadAlert(thrId);

            if (wait) {
                /* wait for real shutdown of thread */
                while (tsdPtr->threadObjPtr) {
                    Tcl_ConditionWait(&tsdPtr->doOneEvent, &tsdPtr->mutex, NULL);
                }
            }
        }
        _log_debug(" !!! thread released [%04X].", thrId);
    }

    Tcl_MutexUnlock(&tsdPtr->mutex);
    if (interp) {
        Tcl_SetIntObj(Tcl_GetObjResult(interp), (users > 0) ? users : 0);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadEventProc --
 *
 *  Handle the event in the target thread.
 *
 * Results:
 *  Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *  Fills out the ThreadEventResult struct.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadEventProc(evPtr, mask)
    Tcl_Event *evPtr;           /* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData* tsdPtr = TCL_TSD_INIT(1);

    Tcl_Interp           *interp = NULL;
    Tcl_ThreadId           thrId = Tcl_GetCurrentThread();
    ThreadEvent        *eventPtr = (ThreadEvent*)evPtr;
    ThreadSendData      *sendPtr = eventPtr->sendData;
    ThreadClbkData      *clbkPtr = eventPtr->clbkData;
    ThreadEventResult* resultPtr = eventPtr->resultPtr;

    int code = TCL_ERROR; /* Pessimistic assumption */

    /*
     * See whether user has any preferences about which interpreter
     * to use for running this job. The job structure might identify
     * one. If not, just use the thread's main interpreter which is
     * stored in the thread specific data structure.
     * Note that later on we might discover that we're running the
     * async callback script. In this case, interpreter will be
     * changed to one given in the callback.
     */

    interp = (sendPtr && sendPtr->interp) ? sendPtr->interp : tsdPtr->interp;

    if (interp != NULL) {
        Tcl_ResetResult(interp);
    }

    if (sendPtr) {
        code = (*sendPtr->execProc)(interp, (ClientData)sendPtr);
    } else {
        code = TCL_OK;
    }

    if (sendPtr) {
        ThreadFreeProc((ClientData)sendPtr);
    }

    if (resultPtr) {

        /*
         * Report job result synchronously to waiting caller
         */

        Tcl_MutexLock(&tsdPtr->mutex);
        Tcl_SetObjRef(resultPtr->resultObj, ThreadGetResult(interp, code));
        Tcl_ConditionNotify(&resultPtr->done);
        Tcl_MutexUnlock(&tsdPtr->mutex);


    } else {

        if (clbkPtr) {

            Tcl_SetObjRef(clbkPtr->resultObj, ThreadGetResult(interp, code));
            if (clbkPtr->threadId == thrId) {

                /* the same thread - callback direct */
                code = (*clbkPtr->execProc)(clbkPtr->interp, (ClientData)clbkPtr);

            } else {

                ThreadSendData *tmpPtr = (ThreadSendData*)clbkPtr;

                /*
                 * Route the callback back to it's originator.
                 * Do not wait for the result.
                 */

                ThreadSend(interp, clbkPtr->threadId, NULL, tmpPtr, NULL, THREAD_SEND_CLBK);
                /* 
                 * Don't free reference - it was (re)sent.
                 */
                clbkPtr = NULL;
            }
        } else {

            /*
             * Pass errors onto the registered error handler
             * when we don't have a result target for this event.
             */

            if (interp && code != TCL_OK) {
                ThreadErrorProc(interp);
            }

        }

    }

    /*
     * We still need to release the reference to the callback
     */

    if (clbkPtr) {
        ThreadFreeProc((ClientData)clbkPtr);
    }

    /*
     * Mark unwind scenario for this thread if the script resulted
     * in error condition and thread has been marked to unwind.
     * This will cause thread to disappear from the list of active
     * threads, clean-up its event queue and exit.
     */

    if (code != TCL_OK) {
        Tcl_MutexLock(&tsdPtr->mutex);
        if (tsdPtr->flags & THREAD_FLAGS_UNWINDONERROR) {
            tsdPtr->flags |= THREAD_FLAGS_INERROR;
            if (tsdPtr->refCount == 0) {
                tsdPtr->flags |= THREAD_FLAGS_STOPPED;
            }
        }
        Tcl_MutexUnlock(&tsdPtr->mutex);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadGetResult --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
ThreadGetResult(interp, code)
    Tcl_Interp *interp;
{
    register Tcl_Obj * resultObj;
    if (interp != NULL) {
        resultObj = ThreadGetReturnInterpStateObj(interp, code);
    } else {
        resultObj = ThreadErrorInterpStateObj(TCL_ERROR, "no target interp!", "TCL EFAULT");
    }
    return resultObj;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadGetOption --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadGetOption(interp, thrId, tsdPtr, option, dsPtr)
    Tcl_Interp  *interp;
    Tcl_ThreadId thrId;
    ThreadSpecificData *tsdPtr; /* TSD of the thread or NULL */
    char *option;
    Tcl_DString *dsPtr;
{
    int len;

    /*
     * If the optionName is NULL it means that we want
     * a list of all options and values.
     */

    len = (option == NULL) ? 0 : strlen(option);

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        tsdPtr = GetThreadFromId_Lock(interp, thrId, 0);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
        if (!tsdPtr) {
            return TCL_ERROR;
        }
    }

    if ( tsdPtr->flags & THREAD_FLAGS_STOPPED ) {
        Tcl_MutexUnlock(&tsdPtr->mutex);
        ErrorNoSuchThread(interp, thrId);
        return TCL_ERROR;
    }

    if (len == 0 || (len > 3 && option[1] == 'e' && option[2] == 'v'
                     && !strncmp(option,"-eventmark", len))) {
        char buf[16];
        if (len == 0) {
            Tcl_DStringAppendElement(dsPtr, "-eventmark");
        }
        sprintf(buf, "%d", tsdPtr->maxEventsCount);
        Tcl_DStringAppendElement(dsPtr, buf);
        if (len != 0) {
            goto done;
        }
    }

    if (len == 0 || (len > 2 && option[1] == 'u'
                     && !strncmp(option,"-unwindonerror", len))) {
        int flag = tsdPtr->flags & THREAD_FLAGS_UNWINDONERROR;
        if (len == 0) {
            Tcl_DStringAppendElement(dsPtr, "-unwindonerror");
        }
        Tcl_DStringAppendElement(dsPtr, flag ? "1" : "0");
        if (len != 0) {
            goto done;
        }
    }

    if (len == 0 || (len > 3 && option[1] == 'e' && option[2] == 'r'
                     && !strncmp(option,"-errorstate", len))) {
        int flag = tsdPtr->flags & THREAD_FLAGS_INERROR;
        if (len == 0) {
            Tcl_DStringAppendElement(dsPtr, "-errorstate");
        }
        Tcl_DStringAppendElement(dsPtr, flag ? "1" : "0");
        if (len != 0) {
            goto done;
        }
    }

    if (len != 0) {
        Tcl_MutexUnlock(&tsdPtr->mutex);
        Tcl_AppendResult(interp, "bad option \"", option,
                         "\", should be one of -eventmark, "
                         "-unwindonerror or -errorstate", NULL);
        return TCL_ERROR;
    }

done:
    Tcl_MutexUnlock(&tsdPtr->mutex);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSetOption --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSetOption(interp, thrId, tsdPtr, option, value)
    Tcl_Interp  *interp;
    Tcl_ThreadId thrId;
    ThreadSpecificData *tsdPtr; /* TSD of the thread or NULL */
    char *option;
    char *value;
{
    int len = strlen(option);

    if (tsdPtr) {
        thrId = tsdPtr->threadId;
        Tcl_MutexLock(&tsdPtr->mutex);
    } else {
        tsdPtr = GetThreadFromId_Lock(interp, thrId, 0);
        /* Tcl_MutexLock(&tsdPtr->mutex); */
        if (!tsdPtr) {
            return TCL_ERROR;
        }
    }

    if ( tsdPtr->flags & THREAD_FLAGS_STOPPED ) {
        Tcl_MutexUnlock(&tsdPtr->mutex);
        ErrorNoSuchThread(interp, thrId);
        return TCL_ERROR;
    }
    if (len > 3 && option[1] == 'e' && option[2] == 'v'
        && !strncmp(option,"-eventmark", len)) {
        if (sscanf(value, "%d", &tsdPtr->maxEventsCount) != 1) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            Tcl_AppendResult(interp, "expected integer but got \"",
                             value, "\"", NULL);
            return TCL_ERROR;
        }
    } else if (len > 2 && option[1] == 'u'
               && !strncmp(option,"-unwindonerror", len)) {
        int flag = 0;
        if (Tcl_GetBoolean(interp, value, &flag) != TCL_OK) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            return TCL_ERROR;
        }
        if (flag) {
            tsdPtr->flags |=  THREAD_FLAGS_UNWINDONERROR;
        } else {
            tsdPtr->flags &= ~THREAD_FLAGS_UNWINDONERROR;
        }
    } else if (len > 3 && option[1] == 'e' && option[2] == 'r'
               && !strncmp(option,"-errorstate", len)) {
        int flag = 0;
        if (Tcl_GetBoolean(interp, value, &flag) != TCL_OK) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            return TCL_ERROR;
        }
        if (flag) {
            tsdPtr->flags |=  THREAD_FLAGS_INERROR;
        } else {
            tsdPtr->flags &= ~THREAD_FLAGS_INERROR;
        }
    }

    Tcl_MutexUnlock(&tsdPtr->mutex);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TransferEventProc --
 *
 *  Handle a transfer event in the target thread.
 *
 * Results:
 *  Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *  Fills out the TransferResult struct.
 *
 *----------------------------------------------------------------------
 */

static int
TransferEventProc(evPtr, mask)
    Tcl_Event *evPtr;           /* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData    *tsdPtr = TCL_TSD_INIT(1);
    TransferEvent       *eventPtr = (TransferEvent *)evPtr;
    TransferResult     *resultPtr = eventPtr->resultPtr;
    Tcl_Interp            *interp = tsdPtr->interp;
    int code;
    const char* msg = NULL;

    if (interp == NULL) {
        /*
         * Reject transfer in case of a missing target.
         */
        code = TCL_ERROR;
        msg  = "target interp missing";
    } else {
        /*
         * Add channel to current thread and interp.
         * See ThreadTransfer for more explanations.
         */
        if (Tcl_IsChannelExisting(Tcl_GetChannelName(eventPtr->chan))) {
            /*
             * Reject transfer. Channel of same name already exists in target.
             */
            code = TCL_ERROR;
            msg  = "channel already exists in target";
        } else {
            Tcl_SpliceChannel(eventPtr->chan);
            Tcl_RegisterChannel(interp, eventPtr->chan);
            Tcl_UnregisterChannel((Tcl_Interp *) NULL, eventPtr->chan);
            code = TCL_OK; /* Return success. */
        }
    }
    if (resultPtr) {
        Tcl_MutexLock(&tsdPtr->mutex);
        resultPtr->resultCode = code;
        if (msg != NULL) {
            Tcl_SetObjRef(resultPtr->resultMsg, Tcl_NewStringObj(msg, -1));
        }
        Tcl_ConditionNotify(&resultPtr->done);
        Tcl_MutexUnlock(&tsdPtr->mutex);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadFreeProc --
 *
 *  Called when we are exiting and memory needs to be freed.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Clears up mem specified in ClientData
 *
 *----------------------------------------------------------------------
 */
static void
ThreadFreeProc(clientData)
    ClientData clientData;
{
    /*
     * This will free send and/or callback structures
     * since both are the same in the beginning.
     */

    ThreadSendData *anyPtr = (ThreadSendData*)clientData;
    // _log_debug(" !!! thread free proc handler ...");

    if (anyPtr) {
        (*anyPtr->freeProc)(anyPtr);
        /* We should release target/callback interpreter */
        if (anyPtr->interp) {
            Tcl_Release((ClientData)anyPtr->interp);
        }
        ckfree((char*)anyPtr);
    }
    // _log_debug(" !!! thread free proc handler - done.");
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadDeleteEvent --
 *
 *  This is called from the ThreadExitHandler to delete memory related
 *  to events that we put on the queue.
 *
 * Results:
 *  1 it was our event and we want it removed, 0 otherwise.
 *
 * Side effects:
 *  It cleans up our events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadDeleteEvent(eventPtr, clientData)
    Tcl_Event *eventPtr;        /* Really ThreadEvent */
    ClientData clientData;      /* dummy */
{
    const char *die_ec = "TCL ESHUTDOWN";

    if (eventPtr->proc == ThreadEventProc) {

        /*
         * Regular script event. Just dispose memory. Pass result
         * back to the originator (and wake it), if possible.
         */
        ThreadEvent       *evPtr = (ThreadEvent*)eventPtr;
        ThreadEventResult *resultPtr = evPtr->resultPtr;

        if (resultPtr) {

            /*
             * Dang. The target is going away. Unblock the caller.
             * The result string must be dynamically allocated
             * because the main thread is going to call free on it.
             */

            Tcl_SetObjRef(resultPtr->resultObj,
                ThreadErrorInterpStateObj(TCL_ERROR, "target thread died", "TCL ESHUTDOWN"));
            Tcl_ConditionNotify(&resultPtr->done);
        }

        if (evPtr->sendData) {
            ThreadFreeProc((ClientData)evPtr->sendData);
            evPtr->sendData = NULL;
        }
        if (evPtr->clbkData) {
            ThreadFreeProc((ClientData)evPtr->clbkData);
            evPtr->clbkData = NULL;
        }
        return 1;
    }
    if (eventPtr->proc == TransferEventProc) {
        /*
         * A channel is in flight toward the thread just exiting.
         * Pass it back to the originator, if possible.
         * Else kill it.
         */
        TransferEvent  *evPtr = (TransferEvent*)eventPtr;
        TransferResult *resultPtr = evPtr->resultPtr;

        if (resultPtr) {
            /*
             * Dang. The target is going away. Unblock the caller.
             * The result string must be dynamically allocated
             * because the main thread is going to call free on it.
             */

            Tcl_SetObjRef(resultPtr->resultMsg,
                ThreadErrorInterpStateObj(TCL_ERROR, "transfer failed: target thread died", "TCL ESHUTDOWN"));
            resultPtr->resultCode = TCL_ERROR;
            Tcl_ConditionNotify(&resultPtr->done);
        
        } else {
            /* No thread to pass the channel back to. Kill it.
             * This requires to splice it temporarily into our channel
             * list and then forcing the ref.counter down to the real
             * value of zero. This destroys the channel.
             */

            Tcl_SpliceChannel(evPtr->chan);
            Tcl_UnregisterChannel((Tcl_Interp *) NULL, evPtr->chan);
            return 1;
        }

        /* Our caller (ThreadExitHandler) will pass the channel back.
         */

        return 1;
    }

    /*
     * If it was NULL, we were in the middle of servicing the event
     * and it should be removed
     */

    return (eventPtr->proc == NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitHandler --
 *
 *  This is called when the thread exits.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  It unblocks anyone that is waiting on a send to this thread.
 *  It cleans up any events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
void
ThreadExitHandler(clientData)
    ClientData clientData;
{
    Tcl_Obj *threadObjPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(0);

    _log_debug(" !!! thread exit handler ...");

    /*
     * Clean up, callbacks etc.
     */

    if (tsdPtr == NULL) {
        return;
    }

    Tcl_UnsetObjRef(tsdPtr->evalScript);

    /*
     * Execute on exit handler if it was set
     */
    if (!(tsdPtr->flags & THREAD_FLAGS_OWN_THREAD)) {
        if (tsdPtr->exitProcObj) {
            if (tsdPtr->interp && !Tcl_InterpDeleted(tsdPtr->interp)) {
                if (Tcl_EvalObjEx(tsdPtr->interp, tsdPtr->exitProcObj, TCL_EVAL_GLOBAL) != TCL_OK) {
                    ThreadErrorProc(tsdPtr->interp);
                }
            }        
        }
        /* remove if it was not reset in handler */
        Tcl_UnsetObjRef(tsdPtr->exitProcObj);
    }

    /*
     * Delete events posted to our queue while we were running.
     * For threads exiting from the NewThread, this has already
     * been done.
     * For one-shot threads, having something here is a very
     * strange condition. It *may* happen if somebody posts us
     * an event while we were in the middle of processing some
     * lengthly user script. It is unlikely to happen, though.
     *
     * Thereby inform all the threads waiting for result/transfer.
     */
   
    Tcl_DeleteEvents((Tcl_EventDeleteProc*)ThreadDeleteEvent, NULL);
    
    Tcl_MutexLock(&threadMutex);

    /*
     * Remove TSD pointer from list, reset TSD in the data of thread self.
     * Shutdown process fulfilled.
     */
    
    if (tsdPtr->nextPtr) {
        SpliceOut(tsdPtr, threadList);
        tsdPtr->nextPtr = tsdPtr->prevPtr = NULL;
    }
    TCL_TSD_SET(NULL);

    /*
     * thread exited without "thread::release", be sure we mark it as stopped
     */
    if (tsdPtr->refCount > 0 || (tsdPtr->flags & THREAD_FLAGS_STOPPED) == 0) {
        Tcl_MutexLock(&tsdPtr->mutex);
        tsdPtr->refCount = 0;
        tsdPtr->flags |= THREAD_FLAGS_STOPPED;
        Tcl_MutexUnlock(&tsdPtr->mutex);
    }

    Tcl_MutexUnlock(&threadMutex);

    /*
     * Clean up. Firstly reset thread object (tear down / tread self reference).
     */

    threadObjPtr = tsdPtr->threadObjPtr;
    tsdPtr->threadObjPtr = NULL;

    Tcl_UnsetObjRef(tsdPtr->threadIdHexObj);

    /* notify thread(s) waiting for shutdown (by release -wait) */
    if (tsdPtr->doOneEvent) {
        Tcl_ConditionNotify(&tsdPtr->doOneEvent);
    }

    /*
     * Remove reference of thread tcl object used in the thread self.
     * If it was the last reference of threadObjPtr and the last object, 
     * it will also decrement a TSD objRefCount, so it removes TSD also 
     * within ThreadObj_FreeInternalRep.
     */
    if (threadObjPtr) {
        Tcl_MutexLock(&tsdPtr->mutex);
        /* free TSD if not referenced (resp. only once referenced in threadObjPtr)  */
        if (tsdPtr->objRefCount <= 1 && tsdPtr->refCount <= 0) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            TCL_TSD_REMOVE(tsdPtr);
            tsdPtr = NULL;
        }
        /* free internal representation of object self - thread does not exists anymore */
        if (threadObjPtr->typePtr == &ThreadObjType) {
            if (!tsdPtr) {
                threadObjPtr->internalRep.twoPtrValue.ptr1 = NULL;
                /* ThreadObj_FreeInternalRep(threadObjPtr); */
            };
        }
        if (tsdPtr) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
        }
        /* decrement obj refCount, clean object if no more references */
        Tcl_DecrRefCount(threadObjPtr);
    }

    _log_debug(" !!! thread exit handler - done.");
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadGetHandle --
 *
 *  Construct the handle of the thread which is suitable
 *  to pass to Tcl.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

int
ThreadGetHandle(thrId, handlePtr)
    Tcl_ThreadId thrId;
    char *handlePtr;
{
    if (thrId >= 0 && thrId <= (Tcl_ThreadId)0xffff) {
        return sprintf(handlePtr, THREAD_HNDLPREFIX"%04X", (int)thrId);
    } else {
        return sprintf(handlePtr, THREAD_HNDLPREFIX"%p", thrId);
    }
}

/*
 *----------------------------------------------------------------------
 *
 *  ErrorNoSuchThread --
 *
 *  Convenience function to set interpreter result when the thread
 *  given by it's ID cannot be found.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void
ErrorNoSuchThread(interp, thrId)
    Tcl_Interp *interp;
    Tcl_ThreadId thrId;
{
    char thrHandle[THREAD_HNDLMAXLEN];

    ThreadGetHandle(thrId, thrHandle);
    Tcl_AppendResult(interp, "thread \"", thrHandle,
                     "\" does not exist", NULL);
    Tcl_SetErrorCode(interp, "TCL", "ENOENT", NULL);
}

/*
 * Type definition.
 */

Tcl_ObjType ThreadObjType = {
    "thread",                  /* name */
    ThreadObj_FreeInternalRep, /* freeIntRepProc */
    ThreadObj_DupInternalRep,  /* dupIntRepProc */
    ThreadObj_UpdateString,    /* updateStringProc */
    ThreadObj_SetFromAny       /* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 *
 * GetThreadFromObj --
 *
 *  Retrieve TSD from tcl object.
 *
 *   shutDownLevel - 0 - thread should be alive (THREAD_FLAGS_STOPPED not set);
 *   shutDownLevel - 1 - thread can be stopped, but not yet exited (threadObjPtr still exists);
 *   shutDownLevel - 2 - thread handle (TSD) in any state;
 *
 * Results:
 *  TSD or NULL.
 *
 * Side effects:
 *  Opposites to "GetThreadFromId_Lock" does not leave thread mutex locked,
 *  because tcl object hold a reference to TSD.
 *
 *----------------------------------------------------------------------
 */
static int
GetThreadFromObj(interp, objPtr, tsdPtrPtr, shutDownLevel)
    Tcl_Interp *interp;
    Tcl_Obj    *objPtr;
    ThreadSpecificData **tsdPtrPtr;
    int shutDownLevel;
{
    ThreadSpecificData *tsdPtr;
    if (objPtr->typePtr != &ThreadObjType &&
        ThreadObj_SetFromAny(interp, objPtr) != TCL_OK
    ) {
        *tsdPtrPtr = NULL;
        return TCL_ERROR;
    }

    tsdPtr = (ThreadSpecificData*)objPtr->internalRep.twoPtrValue.ptr1;
    if (!tsdPtr) {
        Tcl_ThreadId thrId = (Tcl_ThreadId)objPtr->internalRep.twoPtrValue.ptr2;

        Tcl_MutexLock(&threadMutex);
        if ((tsdPtr = ThreadExistsInner(thrId)) == NULL) {
            Tcl_MutexUnlock(&threadMutex);
            *tsdPtrPtr = NULL;
            if (shutDownLevel == 2) {
                return TCL_OK;
            }
            if (interp) {
                ErrorNoSuchThread(interp, thrId);
            }
            return TCL_ERROR;
        }
        Tcl_MutexLock(&tsdPtr->mutex);
        tsdPtr->objRefCount++;
        Tcl_MutexUnlock(&tsdPtr->mutex);
        Tcl_MutexUnlock(&threadMutex);
        objPtr->internalRep.twoPtrValue.ptr1 = tsdPtr;
    }

    if (
         (tsdPtr->flags & THREAD_FLAGS_STOPPED)
      && (!shutDownLevel || (shutDownLevel == 1 && !tsdPtr->threadObjPtr))
    ) {
        if (interp) {
            char thrHandle[THREAD_HNDLMAXLEN];

            ThreadGetHandle(tsdPtr->threadId, thrHandle);
            Tcl_AppendResult(interp, "thread \"", thrHandle, "\" is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        *tsdPtrPtr = NULL;
        return TCL_ERROR;
    }
    *tsdPtrPtr = tsdPtr;
    return TCL_OK;
}
/*
 *----------------------------------------------------------------------
 *
 * GetThreadFromId_Lock --
 *
 *  Retrieve TSD by thread id.
 *
 * Results:
 *  TSD or NULL.
 *
 * Side effects:
 *  if thread available - its mutex locked.
 *
 *----------------------------------------------------------------------
 */
static ThreadSpecificData*
GetThreadFromId_Lock(interp, thrId, shutDownLevel)
    Tcl_Interp  *interp;
    Tcl_ThreadId thrId;
    int shutDownLevel;
{
    ThreadSpecificData *tsdPtr;

    Tcl_MutexLock(&threadMutex);
    if ((tsdPtr = ThreadExistsInner(thrId)) == NULL) {
        Tcl_MutexUnlock(&threadMutex);
        if (interp && shutDownLevel != 2) {
            ErrorNoSuchThread(interp, thrId);
        }
        return NULL;
    }
    Tcl_MutexLock(&tsdPtr->mutex);
    Tcl_MutexUnlock(&threadMutex);

    if (
         (tsdPtr->flags & THREAD_FLAGS_STOPPED)
      && (!shutDownLevel || (shutDownLevel == 1 && !tsdPtr->threadObjPtr))
    ) {
        if (interp) {
            char thrHandle[THREAD_HNDLMAXLEN];

            ThreadGetHandle(thrId, thrHandle);
            Tcl_AppendResult(interp, "thread \"", thrHandle, "\" is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        Tcl_MutexUnlock(&tsdPtr->mutex);
        return NULL;
    }
    return tsdPtr;
}
/*
 *----------------------------------------------------------------------
 */
static void
ThreadObj_DupInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;
    Tcl_Obj *copyPtr;
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData*)srcPtr->internalRep.twoPtrValue.ptr1;
    Tcl_ThreadId thrId = (Tcl_ThreadId)srcPtr->internalRep.twoPtrValue.ptr2;

    if (tsdPtr) {
        Tcl_MutexLock(&tsdPtr->mutex);
        tsdPtr->objRefCount++;
        Tcl_MutexUnlock(&tsdPtr->mutex);
    }
    ThreadObj_SetObjIntRep(copyPtr, tsdPtr, thrId);

}
/*
 *----------------------------------------------------------------------
 */
static void
ThreadObj_FreeInternalRep(objPtr)
    Tcl_Obj *objPtr;
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData*)objPtr->internalRep.twoPtrValue.ptr1;
    if (tsdPtr) {
        Tcl_MutexLock(&tsdPtr->mutex);
        /* decrement object reference count of thread */
        tsdPtr->objRefCount--;
        /* if last reference and not reserved also - free TSD */
        if (!tsdPtr->threadObjPtr && tsdPtr->objRefCount <= 0 && tsdPtr->refCount <= 0) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
            TCL_TSD_REMOVE(tsdPtr);
            tsdPtr = NULL;
        }
        if (tsdPtr) {
            Tcl_MutexUnlock(&tsdPtr->mutex);
        }
    }
    objPtr->internalRep.twoPtrValue.ptr1 = NULL;
    objPtr->internalRep.twoPtrValue.ptr2 = NULL;
    objPtr->typePtr = NULL;
};
/*
 *----------------------------------------------------------------------
 */
static int
ThreadObj_SetFromAny(interp, objPtr)
    Tcl_Interp *interp;
    Tcl_Obj    *objPtr;
{
    Tcl_ThreadId thrId = 0;
    char *thrHandle = objPtr->bytes;
    if (!thrHandle)
        thrHandle = Tcl_GetString(objPtr);
    
    if (!thrHandle || (sscanf(thrHandle, THREAD_HNDLPREFIX"%p", &thrId) != 1)) {
        if (interp) {
            Tcl_AppendResult(interp, "invalid thread handle \"",
                thrHandle ? thrHandle : "", "\"", NULL);
            Tcl_SetErrorCode(interp, "TCL", "EINVAL", NULL);
        }
        return TCL_ERROR;
    }
    
    if (objPtr->typePtr && objPtr->typePtr->freeIntRepProc)
        objPtr->typePtr->freeIntRepProc(objPtr);
    ThreadObj_SetObjIntRep(objPtr, NULL, thrId);
    return TCL_OK;
};
/*
 *----------------------------------------------------------------------
 */
static void
ThreadObj_UpdateString(objPtr)
    Tcl_Obj  *objPtr;
{
    Tcl_ThreadId thrId = (Tcl_ThreadId)objPtr->internalRep.twoPtrValue.ptr2;
    char buf[64];
    int  len = ThreadGetHandle(thrId, buf);
    objPtr->length = len,
    objPtr->bytes = ckalloc((size_t)++len);
    if (objPtr->bytes)
        memcpy(objPtr->bytes, buf, len);
}
/*
 *----------------------------------------------------------------------
 */
Tcl_Obj*
ThreadNewThreadObj(
    Tcl_ThreadId threadId)
{
    Tcl_Obj * objPtr = Tcl_NewObj();
    objPtr->bytes = NULL;
    ThreadObj_SetObjIntRep(objPtr, NULL, threadId);
    return objPtr;
};

/*
 *----------------------------------------------------------------------
 *
 *  ThreadCutChannel --
 *
 *  Dissociate a Tcl channel from the current thread/interp.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Events still pending in the thread event queue and ready to fire
 *  are not processed.
 *
 *----------------------------------------------------------------------
 */

static void
ThreadCutChannel(interp, chan)
    Tcl_Interp *interp;
    Tcl_Channel chan;
{
    Tcl_DriverWatchProc *watchProc;

    Tcl_ClearChannelHandlers(chan);

    watchProc   = Tcl_ChannelWatchProc(Tcl_GetChannelType(chan));

    /*
     * This effectively disables processing of pending
     * events which are ready to fire for the given
     * channel. If we do not do this, events will hit
     * the detached channel which is potentially being
     * owned by some other thread. This will wreck havoc
     * on our memory and eventually badly hurt us...
     */

    if (watchProc) {
        (*watchProc)(Tcl_GetChannelInstanceData(chan), 0);
    }

    /*
     * Artificially bump the channel reference count
     * which protects us from channel being closed
     * during the Tcl_UnregisterChannel().
     */

    Tcl_RegisterChannel((Tcl_Interp *) NULL, chan);
    Tcl_UnregisterChannel(interp, chan);

    Tcl_CutChannel(chan);
}




#include "tclInt.h"
/*
 *----------------------------------------------------------------------
 */
static void
TRISObj_DupInternalRep(
    Tcl_Obj *srcPtr,
    Tcl_Obj *copyPtr)
{
    Tcl_Obj *objResult = (Tcl_Obj*)srcPtr->internalRep.twoPtrValue.ptr1;
    ThreadReturnInterpState *statePtr = NULL, 
        *srcStatePtr = (ThreadReturnInterpState*)srcPtr->internalRep.twoPtrValue.ptr2;
    if (srcStatePtr) {
        statePtr = (ThreadReturnInterpState*)ckalloc(sizeof(ThreadReturnInterpState));
        if (statePtr) {
            memcpy(statePtr, srcStatePtr, sizeof(*statePtr));
            if (statePtr->errorInfo) {
                Tcl_IncrRefCount(statePtr->errorInfo);
            }
            if (statePtr->errorCode) {
                Tcl_IncrRefCount(statePtr->errorCode);
            }
            if (statePtr->returnOpts) {
                Tcl_IncrRefCount(statePtr->returnOpts);
            }
        }
    }
    Tcl_InitObjRef(copyPtr->internalRep.twoPtrValue.ptr1, objResult);
    copyPtr->internalRep.twoPtrValue.ptr2 = statePtr,
		copyPtr->typePtr = &ThreadReturnInterpStateObjType;
}
/*
 *----------------------------------------------------------------------
 */
static void
TRISObj_FreeInternalRep(
    Tcl_Obj *objPtr)
{
    Tcl_Obj *objResult = (Tcl_Obj*)objPtr->internalRep.twoPtrValue.ptr1;
    ThreadReturnInterpState *statePtr = (ThreadReturnInterpState*)objPtr->internalRep.twoPtrValue.ptr2;

    objPtr->internalRep.twoPtrValue.ptr1 = NULL;
    objPtr->internalRep.twoPtrValue.ptr2 = NULL;
    objPtr->typePtr = NULL;
    if (statePtr) {
        if (statePtr->errorInfo) {
            Tcl_DecrRefCount(statePtr->errorInfo);
        }
        if (statePtr->errorCode) {
            Tcl_DecrRefCount(statePtr->errorCode);
        }
        if (statePtr->returnOpts) {
            Tcl_DecrRefCount(statePtr->returnOpts);
        }
        ckfree((char *) statePtr);
    }
    if (objResult)
        Tcl_DecrRefCount(objResult);
};
/*
 *----------------------------------------------------------------------
 */
static void
TRISObj_UpdateString(
    Tcl_Obj  *objPtr)
{
    Tcl_Obj *objResult = (Tcl_Obj*)objPtr->internalRep.twoPtrValue.ptr1;
    int  len;
    char * buf = Tcl_GetStringFromObj(objResult, &len);
    objPtr->length = len,
    objPtr->bytes = ckalloc((size_t)len+1);
    if (objPtr->bytes) {
        memcpy(objPtr->bytes, buf, len);
        objPtr->bytes[len] = '\0';
    }
}
/*
 *----------------------------------------------------------------------
 */
Tcl_ObjType ThreadReturnInterpStateObjType = {
    "thread-return-interp-state", /* name */
    TRISObj_FreeInternalRep,      /* freeIntRepProc */
    TRISObj_DupInternalRep,       /* dupIntRepProc */
    TRISObj_UpdateString,         /* updateStringProc */
    NULL                          /* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 */
Tcl_Obj *
ThreadGetReturnInterpStateObj(
    Tcl_Interp *interp,     /* Interpreter's state to be saved */
    int status)             /* status code for current operation */
{
    Interp *iPtr = (Interp *)interp;
    ThreadReturnInterpState *statePtr;
    Tcl_Obj *objPtr;

    /*
     * Special handling of the common case of normal command return code and 
     * no explicit return options. Just returns duplicate of interp result object
     */
    if (status == TCL_OK && iPtr->returnOpts == NULL) {
        /* caller holds the reference to object, so no increment here */
        return Sv_DuplicateObj(Tcl_GetObjResult(interp));
    }

    objPtr = Tcl_NewObj();
    if (!objPtr) {
        return NULL;
    }

    statePtr = (ThreadReturnInterpState*)ckalloc(sizeof(ThreadReturnInterpState));

    objPtr->internalRep.twoPtrValue.ptr1 = Sv_DupIncrObj(Tcl_GetObjResult(interp)), 
    objPtr->internalRep.twoPtrValue.ptr2 = statePtr, 
    objPtr->typePtr = &ThreadReturnInterpStateObjType,
    objPtr->bytes = NULL;

    if (statePtr) {
        statePtr->status = status;
        statePtr->returnLevel = iPtr->returnLevel > 0 ? iPtr->returnLevel : 1;
        statePtr->returnCode = iPtr->returnCode;
        statePtr->errorInfo = Sv_DupIncrObj(iPtr->errorInfo);
        statePtr->errorCode = Sv_DupIncrObj(iPtr->errorCode);
        statePtr->returnOpts = Sv_DupIncrObj(iPtr->returnOpts);
    }
    return objPtr;
}

Tcl_Obj *
ThreadErrorInterpStateObj(
    int status,             /* status code for current operation */
    const char *errMsg,
    const char *errCode)
{
    ThreadReturnInterpState *statePtr;
    Tcl_Obj *objPtr;

    objPtr = Tcl_NewObj();
    if (!objPtr) {
        return NULL;
    }

    statePtr = (ThreadReturnInterpState*)ckalloc(sizeof(ThreadReturnInterpState));

    Tcl_InitObjRef(objPtr->internalRep.twoPtrValue.ptr1, Tcl_NewStringObj(errMsg, -1));
    objPtr->internalRep.twoPtrValue.ptr2 = statePtr, 
    objPtr->typePtr = &ThreadReturnInterpStateObjType,
    objPtr->bytes = NULL;

    if (statePtr) {
        statePtr->status = status;
        statePtr->returnLevel = 1;
        statePtr->returnCode = status;
        statePtr->errorInfo = NULL;
        Tcl_InitObjRef(statePtr->errorCode, Tcl_NewStringObj(errCode, -1));
        statePtr->returnOpts = NULL;
    }
    return objPtr;
}

int
ThreadRestoreInterpStateFromObj(
    Tcl_Interp *interp,     /* Interpreter's state to be restored. */
    Tcl_Obj *objPtr)        /* Saved interpreter state as object. */
{
    if (objPtr) {
        if (objPtr->typePtr != &ThreadReturnInterpStateObjType) {
            /* object is just a result - direct return object and ok */
            Tcl_SetObjResult(interp, objPtr);
            return TCL_OK;
        } else {
            /* set saved state to interp */
            Interp *iPtr = (Interp *)interp;
            Tcl_Obj *objResult = (Tcl_Obj*)objPtr->internalRep.twoPtrValue.ptr1;
            ThreadReturnInterpState *statePtr = (ThreadReturnInterpState*)objPtr->internalRep.twoPtrValue.ptr2;
            int status = TCL_OK;

            iPtr->flags &= ERR_ALREADY_LOGGED;
            iPtr->flags |= ERR_LEGACY_COPY;
            if (objResult) {
                Tcl_SetObjResult(interp, objResult);
            }

            if (statePtr) {
                status = statePtr->status;
                iPtr->returnLevel = statePtr->returnLevel;
                iPtr->returnCode = statePtr->returnCode;
                Tcl_SetObjRef(iPtr->errorInfo, statePtr->errorInfo);
                Tcl_SetObjRef(iPtr->errorCode, statePtr->errorCode);
                Tcl_SetObjRef(iPtr->returnOpts, statePtr->returnOpts);
            }

            return status;
        }
    } else {
        /* possible no-mem situation */
        Tcl_AppendResult(interp, "unexpected result", NULL);
        Tcl_SetErrorCode(interp, "TCL", "EFAULT", NULL);
        return TCL_ERROR;
    }
}

int
ThreadGetStatusOfInterpStateObj(
    Tcl_Obj *objPtr)        /* Saved interpreter state as object or any other */
{
    if (objPtr->typePtr == &ThreadReturnInterpStateObjType) {
        ThreadReturnInterpState *statePtr = (ThreadReturnInterpState*)objPtr->internalRep.twoPtrValue.ptr2;
        if (statePtr) {
            return statePtr->status;
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 */

int
ThreadIncrNumLevels(
    Tcl_Interp *interp, 
    int incr)
{
    Interp *iPtr = (Interp *)interp;
    return (iPtr->numLevels += incr);
}

/* EOF $RCSfile: threadCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
