/*
 * threadPoolCmd.c --
 *
 * This file implements the Tcl thread pools.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 * ----------------------------------------------------------------------------
 */

#include "tclThreadInt.h"
#include "threadSvCmd.h"

/*
 * Structure to maintain workers of thread pool
 */

typedef struct ThreadPool ThreadPool;

typedef struct TpoolWorker {
    Tcl_ThreadId threadId;         /* Thread id of the current thread */
    int          flags;            /* Flags and signals to recognize idle/wait state */
    ThreadPool  *tpoolPtr;         /* Thread pool */
    Tcl_Interp  *interp;           /* Tcl interp of this thread */
    Tcl_Obj     *idleScript;       /* Idle script command object */
    Tcl_Obj     *idleIntScript;    /* Idle interval script command object */
    Tcl_Time     idleStartTm;      /* Last time the worker goes idle (used for timeout) */
    Tcl_Time     idleIntTm;        /* Last time the worker executed idle interval command */
    struct TpoolWorker *nextIdl;   /* Next idle in the ring list */
    struct TpoolWorker *prevIdl;   /* Previous idle in the ring list */
    struct TpoolWorker *nextWrk;   /* Next structure in the ring list */
    struct TpoolWorker *prevWrk;   /* Previous structure in the ring list */
} TpoolWorker;

#define TPWRK_FLAG_IDLE    (1<<0)  /* Still idle */
#define TPWRK_FLAG_INWORK  (1<<1)  /* Worker process pool work */
#define TPWRK_FLAG_IDLLAT  (1<<2)  /* Idle latency elapsed */
#define TPWRK_FLAG_IDLINT  (1<<3)  /* Idle interval "timer" created (but not yet executed) */
#define TPWRK_FLAG_IDLTMR  (1<<4)  /* Idle timeout event created (but not yet executed) */
#define TPWRK_FLAG_IDLTOUT (1<<5)  /* Signaled if idle timeout occurred */
#define TPWRK_FLAG_SVC     (1<<6)  /* Signaled it was a service event (no work), idle will be continued */

#define TPWRK_FLAG_ERROR   (1<<8)  /* Signaled thread has initialization error (error in initScript) */

/*
 * Structure describing an instance of a thread pool.
 */

typedef struct ThreadPool {
    Tcl_WideInt jobId;              /* Job counter */
    Tcl_Time    idleTime;           /* Time a worker thread tear down after it is idle */
    Tcl_Time    idleLatency;        /* Latency after idle at the earliest a worker will execute an idle script */
    Tcl_Time    idleIntTime;        /* Interval a worker will execute an idle interval script */
    int tearDown;                   /* Set to 1 to tear down the pool */
    int suspend;                    /* Set to 1 to suspend pool processing */
    int flags;                      /* Pool flags (discrete mode, etc) */
    Tcl_Obj *initScript;            /* Script to initialize worker thread */
    Tcl_Obj *exitScript;            /* Script to cleanup the worker */
    Tcl_Obj *idleScript;            /* Script executed if thread goes idle (once it has no work) */
    Tcl_Obj *idleIntScript;         /* Script executed if thread goes idle (once it has no work) */
    int minWorkers;                 /* Minimum number or worker threads */
    int maxWorkers;                 /* Maximum number of worker threads */
    int numWorkers;                 /* Current number of worker threads */
    int stopWorkers;                /* Current number of worker will shut down */
    int idleWorkers;                /* Number of idle workers */
    int refCount;                   /* Reference counter for reserve/release */
    int objRefCount;                /* Reference counter (all tcl objects), protected within listMutex */
    Tcl_Mutex mutex;                /* Pool mutex (work queue, properties, etc.) */
    Tcl_Mutex startMutex;           /* Start mutex (start worker, suspend/resume, etc.) */
    Tcl_Condition cond;             /* Pool condition variable */
    Tcl_HashTable jobsDone;         /* Stores processed job results */
    Tcl_ThreadId  waiterId;         /* Thread id of single waiter, currently used for tear down only (thread free pool) */
    struct TpoolResult *workTail;   /* Tail of the list with jobs pending*/
    struct TpoolResult *workHead;   /* Head of the list with jobs pending*/
    struct TpoolWorker *workerRing; /* Head/Tail of the workers ring buffer */
    struct TpoolWorker *idleRing;   /* Head/Tail of the idle workers list (ring buffer) */
    struct ThreadPool *nextPtr;     /* Next structure in the threadpool list */
    struct ThreadPool *prevPtr;     /* Previous structure in threadpool list */
} ThreadPool;

#define TPOOL_SUSPEND_JOBS (1<<0)   /* Suspend jobs (work queue or distributed), but process tcl events */
#define TPOOL_SUSPEND_FULL (1<<1)   /* Totally suspend work (also tcl events) */

#define TPOOL_FLAG_DISCRETE       (1<<2)  /* Discrete mode - disable process queued or distributed work (but process tcl events) */
#define TPOOL_FLAG_SHIFT_IDLERING (1<<3)  /* Shift idle ring mode - if on, add to tail, otherwise (default) add again to head
                                           * of idle ring (prefer the last working thread) */

#define TPOOL_HNDLPREFIX  "tpool"   /* Prefix to generate Tcl pool handles */
#define TPOOL_MINWORKERS  0         /* Default minimum # of worker threads */
#define TPOOL_MAXWORKERS  4         /* Default maximum # of worker threads */
#define TPOOL_IDLETIMER   0         /* Default worker thread idle timer */
#define TPOOL_IDLELATENCY 0.5       /* Default time of idle command of worker thread */
#define TPOOL_IDLEINT     5         /* Default idle interval of worker thread */
                                    /* Default pool flags (discrete mode) */
#define TPOOL_FLAGS       TPOOL_FLAG_DISCRETE
            

/*
 * Structure for passing evaluation results
 */

typedef struct TpoolResult {
    int detached;                   /* Result is to be ignored */
    Tcl_WideInt jobId;              /* The job id of the current job */
    Tcl_Obj *script;                /* Script to evaluate in worker thread */
    Tcl_Obj *resultObj;             /* Result or Return state object (content of the errorCode, errorInfo, etc.) */
    Tcl_ThreadId threadId;          /* Originating thread id */
    ThreadPool *tpoolPtr;           /* Current thread pool */
    struct TpoolResult *nextPtr;
    struct TpoolResult *prevPtr;
} TpoolResult;

/* Event structure used to signal / distribute work to the worker */
typedef struct TpoolEvent {
    TpoolWorker *poolWrk;             /* Structure of worker thread */
    TpoolResult *rPtr;                /* Work used to distribute to the worker directly */
} TpoolEvent;

/*
 * Private structure for each worker/poster thread.
 */

typedef struct TpoolSpecificData {
    int stop;                       /* Set stop event; exit from event loop */
} TpoolSpecificData;

static Tcl_ThreadDataKey dataKey;

/*
 * This global list maintains thread pools.
 */

static ThreadPool *tpoolList;
static Tcl_Mutex listMutex;

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc TpoolCreateObjCmd;
static Tcl_ObjCmdProc TpoolReloadObjCmd;
static Tcl_ObjCmdProc TpoolPostObjCmd;
static Tcl_ObjCmdProc TpoolWaitObjCmd;
static Tcl_ObjCmdProc TpoolCancelObjCmd;
static Tcl_ObjCmdProc TpoolGetObjCmd;
static Tcl_ObjCmdProc TpoolReserveObjCmd;
static Tcl_ObjCmdProc TpoolReleaseObjCmd;
static Tcl_ObjCmdProc TpoolSuspendObjCmd;
static Tcl_ObjCmdProc TpoolResumeObjCmd;
static Tcl_ObjCmdProc TpoolNamesObjCmd;

/*
 * Miscelaneous functions used within this file
 */

static int
CreateWorker(Tcl_Interp *interp, ThreadPool *tpoolPtr, int lock);

static Tcl_ThreadCreateType
TpoolWorkerProc(ClientData clientData);

static int
TpoolReloadEvent(Tcl_Event *evPtr, int mask);

static int
RunStopEvent(Tcl_Event *evPtr, int mask);

static void
PushWork(TpoolResult *rPtr, ThreadPool *tpoolPtr);

#define HasWork(tpoolPtr) (tpoolPtr->workTail != NULL)

static TpoolResult*
PopWork(ThreadPool *tpoolPtr);

static int
ServiceEvent(Tcl_Event *eventPtr, int mask);

static int
WorkEvent(Tcl_Event *eventPtr, int mask);

static void
SignalWaiter(ThreadPool *tpoolPtr, Tcl_ThreadId threadId);

static int
TpoolEval(Tcl_Interp *interp, Tcl_Obj *script, TpoolResult *rPtr);
static int
SetResult(Tcl_Interp *interp, TpoolResult *rPtr);

static ThreadPool*
GetTpoolFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, int shutDownLevel);

static ThreadPool*
GetTpool(const char *tpoolName);

static ThreadPool*
GetTpoolUnl(const char *tpoolName);

static void
AppExitHandler(ClientData clientData);

static int
TpoolReserve(ThreadPool *tpoolPtr);

static int
TpoolRelease(ThreadPool *tpoolPtr);

static void
TpoolSuspend(ThreadPool *tpoolPtr, int mode);

static void
TpoolResume(Tcl_Interp * interp, ThreadPool *tpoolPtr);

static void
TpoolObj_DupInternalRep(Tcl_Obj *srcPtr, Tcl_Obj *copyPtr);
static void
TpoolObj_FreeInternalRep(Tcl_Obj *objPtr);
static int
TpoolObj_SetFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void
TpoolObj_UpdateString(Tcl_Obj *objPtr);

#define TpoolObj_SetObjIntRep(objPtr, tpoolPtr) \
    objPtr->internalRep.twoPtrValue.ptr1 = tpoolPtr, \
    objPtr->internalRep.twoPtrValue.ptr2 = NULL, \
    objPtr->typePtr = &TpoolObjType;

#define TPOOL_SVC_EVENT ((TpoolResult*)ServiceEvent)

#define TPOOL_TSD_INIT() \
  (TpoolSpecificData*)Tcl_GetThreadData((&dataKey),sizeof(TpoolSpecificData))


#define DoubleToTime(tm, tmdbl) { \
    tm.sec = (long)tmdbl; \
    tm.usec = (long)((tmdbl - (long)tmdbl) * 1000000); \
}
#define TimeAddTime(tm, offs) { \
    tm.sec += offs.sec; \
    tm.usec += offs.usec; \
    if (tm.usec >= 1000000) { tm.sec++; tm.usec -= 1000000;} \
}
#define TimeIsZero(tm)                 ((tm.sec | tm.usec) == 0)
#define TimeIsNonZero(tm)              ((tm.sec | tm.usec) != 0)
#define TimeLessOrEqual(tm, comptm)    (tm.sec < comptm.sec || (tm.sec == comptm.sec && tm.usec <= comptm.usec))
#define TimeGreaterOrEqual(tm, comptm) (tm.sec > comptm.sec || (tm.sec == comptm.sec && tm.usec >= comptm.usec))

/*
 *----------------------------------------------------------------------
 */
static inline void
SignalWorkerInner(poolWrk, eventProc, rPtr, queuepos)
    TpoolWorker *poolWrk;
    Tcl_EventProc *eventProc;
    TpoolResult *rPtr;
    Tcl_QueuePosition queuepos;
{
    Tcl_Event   *evPtr;
    TpoolEvent  *poolEvent;

    evPtr = (Tcl_Event*)ckalloc(sizeof(Tcl_Event) + sizeof(TpoolEvent));
    evPtr->proc = eventProc;
    poolEvent = (TpoolEvent*)(evPtr+1);
    poolEvent->poolWrk = poolWrk;
    poolEvent->rPtr = rPtr;
    Tcl_ThreadQueueEvent(poolWrk->threadId, evPtr, queuepos);
    Tcl_ThreadAlert(poolWrk->threadId);
}


/*
 *----------------------------------------------------------------------
 *
 * TpoolGetPoolArgs --
 *
 *----------------------------------------------------------------------
 */

static int
TpoolGetPoolArgs(tpoolPtr, interp, objc, objv, iiPtr)
    ThreadPool *tpoolPtr;       /* Parsed arguments to create or reload pool */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
    int        *iiPtr;          /* Index of current argument */
{
    int ii;
    /*
     * Syntax:  tpool::create|tpool::reload \
     *                        ?-minworkers count?
     *                        ?-maxworkers count?
     *                        ?-initcmd script?
     *                        ?-exitcmd script?
     *                        ?-idletime sec.ms? ?-idlelatency sec.ms?
     *                        ?-idlecmd script?
     *                        ?-idleint script? ?-in sec.ms?
     */

    /*
     * Parse the optional arguments
     */

    for (ii = *iiPtr; ii < objc; ii += 2, *iiPtr = ii) {
        char *opt;
        if (ii+1 >= objc) {
            return TCL_BREAK;
        }
        opt = Tcl_GetString(objv[ii]);
        if (OPT_CMP(opt, "-minworkers")) {
            if (Tcl_GetIntFromObj(interp, objv[ii+1], &tpoolPtr->minWorkers) != TCL_OK) {
                return TCL_ERROR;
            }
        } else if (OPT_CMP(opt, "-maxworkers")) {
            if (Tcl_GetIntFromObj(interp, objv[ii+1], &tpoolPtr->maxWorkers) != TCL_OK) {
                return TCL_ERROR;
            }
            if (tpoolPtr->minWorkers > tpoolPtr->maxWorkers)
                tpoolPtr->minWorkers = tpoolPtr->maxWorkers;
        } else if (OPT_CMP(opt, "-idletime")) {
            double val;
            if (Tcl_GetDoubleFromObj(interp, objv[ii+1], &val) != TCL_OK) {
                return TCL_ERROR;
            }
            DoubleToTime(tpoolPtr->idleTime, val);
        } else if (OPT_CMP(opt, "-initcmd")) {
            tpoolPtr->initScript = objv[ii+1];
        } else if (OPT_CMP(opt, "-exitcmd")) {
            tpoolPtr->exitScript = objv[ii+1];
        } else if (OPT_CMP(opt, "-discrete")) {
            int flag;
            if (Tcl_GetBooleanFromObj(interp, objv[ii+1], &flag) != TCL_OK) {
                return TCL_ERROR;
            }
            if (flag)
                tpoolPtr->flags |= TPOOL_FLAG_DISCRETE;
            else
                tpoolPtr->flags &= ~TPOOL_FLAG_DISCRETE;
        } else if (OPT_CMP(opt, "-idlelatency")) {
            double val;
            if (Tcl_GetDoubleFromObj(interp, objv[ii+1], &val) != TCL_OK) {
                return TCL_ERROR;
            }
            DoubleToTime(tpoolPtr->idleLatency, val);
        } else if (OPT_CMP(opt, "-idlecmd")) {
            tpoolPtr->idleScript = objv[ii+1];
        } else if (OPT_CMP(opt, "-idleint")) {
            tpoolPtr->idleIntScript = objv[ii+1];
            if (ii + 2 < objc && OPT_CMP(Tcl_GetString(objv[ii+2]), "-in")) {
                double val;
                ii+=2;
                if (Tcl_GetDoubleFromObj(interp, objv[ii+1], &val) != TCL_OK) {
                    return TCL_ERROR;
                }
                DoubleToTime(tpoolPtr->idleIntTime, val);
            }
        } else {
            return TCL_BREAK;
        }
    }

    /*
     * Do some consistency checking
     */

    if (tpoolPtr->minWorkers < 0) {
        tpoolPtr->minWorkers = 0;
    }
    if (tpoolPtr->maxWorkers < 0) {
        tpoolPtr->maxWorkers = TPOOL_MAXWORKERS;
    }
    if (tpoolPtr->minWorkers > tpoolPtr->maxWorkers) {
        tpoolPtr->maxWorkers = tpoolPtr->minWorkers;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolCreateObjCmd --
 *
 *  This procedure is invoked to process the "tpool::create" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolCreateObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ii, ret;
    ThreadPool *tpoolPtr;
    Tcl_Obj    *objPtr;

    /*
     * Syntax:  tpool::create ?-minworkers count?
     *                        ?-maxworkers count?
     *                        ?-initcmd script?
     *                        ?-exitcmd script?
     *                        ?-idletime sec.ms? ?-idlelatency sec.ms?
     *                        ?-idlecmd script?
     *                        ?-idleint script? ?-in sec.ms?
     */

    /*
     * Allocate and initialize thread pool structure
     */

    tpoolPtr = (ThreadPool*)ckalloc(sizeof(ThreadPool));
    memset(tpoolPtr, 0, sizeof(ThreadPool));

    /* default arguments */
    tpoolPtr->minWorkers = TPOOL_MINWORKERS;
    tpoolPtr->maxWorkers = TPOOL_MAXWORKERS;
    DoubleToTime(tpoolPtr->idleTime, TPOOL_IDLETIMER);
    DoubleToTime(tpoolPtr->idleLatency, TPOOL_IDLELATENCY);
    DoubleToTime(tpoolPtr->idleIntTime, TPOOL_IDLEINT);
    tpoolPtr->flags = TPOOL_FLAGS;

    /*
     * Parse the optional arguments
     */

    ii = 1;
    ret = TpoolGetPoolArgs(tpoolPtr, interp, objc, objv, &ii);
    if (ret != TCL_OK) {
        ckfree((char*)tpoolPtr);
        if (ret == TCL_BREAK)
            goto usage;
        return TCL_ERROR;
    }

    /* duplicate objects to use its content thread safe in other threads */
    tpoolPtr->initScript    = Sv_DupIncrObj(tpoolPtr->initScript);
    tpoolPtr->exitScript    = Sv_DupIncrObj(tpoolPtr->exitScript);
    tpoolPtr->idleScript    = Sv_DupIncrObj(tpoolPtr->idleScript);
    tpoolPtr->idleIntScript = Sv_DupIncrObj(tpoolPtr->idleIntScript);

    Tcl_InitHashTable(&tpoolPtr->jobsDone, TCL_ONE_WORD_KEYS);

    Tcl_MutexLock(&listMutex);
    SpliceIn(tpoolPtr, tpoolList);
    tpoolPtr->objRefCount++;
    tpoolPtr->refCount++;
    Tcl_MutexUnlock(&listMutex);

    /*
     * Start the required number of worker threads.
     * On failure to start any of them, tear-down
     * partially initialized pool.
     */

    Tcl_MutexLock(&tpoolPtr->mutex);
    for (ii = 0; ii < tpoolPtr->minWorkers; ii++) {
        if (CreateWorker(interp, tpoolPtr, 1) != TCL_OK) {
            _log_debug(" !!! pool master: failed to create worker ");
            Tcl_MutexUnlock(&tpoolPtr->mutex);
            Tcl_MutexLock(&listMutex);
            tpoolPtr->objRefCount--;
            _log_debug(" !!! pool master: release ... %d/%d ", tpoolPtr->objRefCount, tpoolPtr->refCount);
            TpoolRelease(tpoolPtr);
            Tcl_MutexUnlock(&listMutex);
            return TCL_ERROR;
        }
    }
    Tcl_MutexUnlock(&tpoolPtr->mutex);


    objPtr = Tcl_NewObj();
    objPtr->bytes = NULL;
    TpoolObj_SetObjIntRep(objPtr, tpoolPtr);
    Tcl_SetObjResult(interp, objPtr);

    return TCL_OK;

 usage:
    Tcl_WrongNumArgs(interp, 1, objv,
                     "?-minworkers count? ?-maxworkers count? "
                     "?-initcmd script? ?-exitcmd script? "
                     "?-discrete 0|1? "
                     "?-idletime sec.ms? ?-idlelatency sec.ms? "
                     "?-idlecmd script? "
                     "?-idleint script? ?-in sec.ms?"
    );
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReloadObjCmd -- , tpool::reload --
 *
 *  This command will be used to reload pool.
 *  See the user documentation for details on what it does.
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
TpoolReloadObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ii, ret, suspended;
    Tcl_Obj *reloadScript = NULL;
    ThreadPool *tpoolPtr, tpoolArgs;

    /*
     * Syntax:  tpool::reload tpoolId ?-minworkers count?
     *                        ?-maxworkers count?
     *                        ?-initcmd script?
     *                        ?-exitcmd script?
     *                        ?-idletime sec.ms? ?-idlelatency sec.ms?
     *                        ?-idlecmd script?
     *                        ?-idleint script? ?-in sec.ms?
     *                        ?reload-script?
     */

    if (objc <= 1) {
        goto usage;
    }

    /*
     * Find given pool
     */

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 0)) == NULL) {
        return TCL_ERROR;
    }

    /* default arguments */
    memcpy(&tpoolArgs, tpoolPtr, sizeof(ThreadPool));

    /*
     * Parse the optional arguments
     */

    ii = 2;
    ret = TpoolGetPoolArgs(&tpoolArgs, interp, objc, objv, &ii);
    if (ret != TCL_OK) {
        if (ret == TCL_BREAK) {
            /* if not the last parameter (not reload-script) */
            if (ii != objc-1) {
              goto usage;
            }
            reloadScript = objv[ii];
        } else {
            return TCL_ERROR;
        }
    }

    /* temporarely suspend all workers (if not already) */
    if (!(suspended = tpoolPtr->suspend)) {
      TpoolSuspend(tpoolPtr, TPOOL_SUSPEND_JOBS);
    }

    Tcl_MutexLock(&tpoolPtr->mutex);

    /* easy update static parameters */
    tpoolPtr->minWorkers  = tpoolArgs.minWorkers;
    tpoolPtr->maxWorkers  = tpoolArgs.maxWorkers;
    tpoolPtr->idleTime    = tpoolArgs.idleTime;
    tpoolPtr->idleLatency = tpoolArgs.idleLatency;
    tpoolPtr->idleIntTime = tpoolArgs.idleIntTime;
    tpoolPtr->flags       = tpoolArgs.flags;

    /* switch (to duplicate of) objects to use its content thread safe in other threads */
    Sv_SetDupObj(&tpoolPtr->initScript,    tpoolArgs.initScript);
    Sv_SetDupObj(&tpoolPtr->exitScript,    tpoolArgs.exitScript);
    Sv_SetDupObj(&tpoolPtr->idleScript,    tpoolArgs.idleScript);
    Sv_SetDupObj(&tpoolPtr->idleIntScript, tpoolArgs.idleIntScript);

    /*
     * notify all workers using distributed job - we have reloaded pool, 
     * release some if exceed new maxWorkers.
     */
    if (tpoolPtr->workerRing) {
        int count = tpoolPtr->maxWorkers;
        TpoolWorker *poolWrk, *lastHead;
        poolWrk = lastHead = tpoolPtr->workerRing;
        do {
            if (--count >= 0) {
                SignalWorkerInner(poolWrk, TpoolReloadEvent, 
                    (TpoolResult*)(reloadScript ? Sv_DuplicateObj(reloadScript) : NULL), 
                    TCL_QUEUE_HEAD);
            } else {
                ThreadReserve(NULL, poolWrk->threadId, NULL, THREAD_RELEASE, 0);
            }
            poolWrk = poolWrk->nextWrk;

        } while (poolWrk != lastHead);
    };

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (!suspended) {
        /* resume pool, notify workers and start new workers if some needed (numWorkers < minWorkers) */
        TpoolResume(interp, tpoolPtr);
    }

    return TCL_OK;

 usage:
    Tcl_WrongNumArgs(interp, 1, objv, "tpoolId "
                     "?-minworkers count? ?-maxworkers count? "
                     "?-initcmd script? ?-exitcmd script? "
                     "?-discrete 0|1? "
                     "?-idletime sec.ms? ?-idlelatency sec.ms? "
                     "?-idlecmd script? "
                     "?-idleint script? ?-in sec.ms?"
                     "?reload-script?"
    );

    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * SignalWorker --
 *
 *  Notify (wake up) the pool threads waiting in Tcl_DoOneEvent.
 *
 *  Pool mutex should be locked.
 *
 *  count - if positive count of worker to be signaled, 
 *          if negative count of idle to be signaled
 *  rPtr  - if TPOOL_SVC_EVENT - simple service event (wakeup)
 *          if not NULL - work event with distributed job, 
 *          otherwise - work event - get job from queue.
 *
 * [ToDo]:
 *  Replace this function if tcl will support something like 
 *  WaitForMultipleObjects in Tcl_DoOneEvent.
 *
 * Results:
 *  None.
 *
 *----------------------------------------------------------------------
 */
static void
SignalWorker(tpoolPtr, count, rPtr, queuepos)
    ThreadPool  *tpoolPtr;
    int          count;
    TpoolResult *rPtr;
    Tcl_QueuePosition queuepos;
{
    TpoolWorker *poolWrk, *lastHead;
    int          wrk = 0;

    /* go over ring to signal, prefer idle workers if expected */
    poolWrk = NULL;
    if (count < 0) {
        poolWrk = lastHead = tpoolPtr->idleRing;
        count = -count;
        if (poolWrk == NULL && (rPtr != NULL && rPtr != TPOOL_SVC_EVENT)) {
            wrk = 1;
            poolWrk = lastHead = tpoolPtr->workerRing;
        }
    } else {
        wrk = 1;
        poolWrk = lastHead = tpoolPtr->workerRing;
    }
    if (poolWrk == NULL) {
        return;
    }
    while (count) {
        _log_debug(" !!! pool master: signal %s [%04X] ... ", (wrk?"worker":"idle"), poolWrk->threadId);
        if (rPtr != TPOOL_SVC_EVENT) {
            SignalWorkerInner(poolWrk, WorkEvent, rPtr, queuepos);
            rPtr = NULL;
        } else {
            SignalWorkerInner(poolWrk, ServiceEvent, rPtr, queuepos);
        }

        if (wrk) {
            poolWrk = poolWrk->nextWrk;
        } else {
            poolWrk = poolWrk->nextIdl;
        }
        count--;
        if (poolWrk == lastHead) {
            break;
        }
    }
    /* move ring head pointer to last known (to notify later another worker) */
    if (!wrk) {
        tpoolPtr->idleRing = poolWrk;
    }
    /* always move ring head after last processed */
    tpoolPtr->workerRing = poolWrk;
    _log_debug(" !!! pool master: rings ... [%04X] ", poolWrk->threadId);
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolPostObjCmd --
 *
 *  This procedure is invoked to process the "tpool::post" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolPostObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    Tcl_WideInt jobId = 0;
    int ii, detached = 0, nostart = 0, distribute = 0, broadcast = 0;
    Tcl_QueuePosition queuepos = TCL_QUEUE_TAIL;
    Tcl_Obj  *script;
    TpoolResult *rPtr;
    ThreadPool  *tpoolPtr;
    Tcl_Obj *retJobs = NULL; int retCode = TCL_OK;

    TpoolSpecificData *tsdPtr = TPOOL_TSD_INIT();

    /*
     * Syntax: tpool::post ?-async|-detached? ?-nostart|-nowait? ?-distribute? ?-head? ?-broadcast ?count|-1?? tpoolId script
     *         -nowait is an alias for -nostart for backwards compatibility.
     */

    if (objc < 3) {
        goto usage;
    }
    for (ii = 1; ii < objc; ii++) {
        char *opt = Tcl_GetString(objv[ii]);
        if (*opt != '-') {
            break;
        } else if (OPT_CMP(opt, "-async") || OPT_CMP(opt, "-detached")) {
            detached  = 1;
        } else if (OPT_CMP(opt, "-nostart") || OPT_CMP(opt, "-nowait")) {
            nostart = 1;
        } else if (OPT_CMP(opt, "-distribute")) {
            distribute = 1;
        } else if (OPT_CMP(opt, "-broadcast")) {
            broadcast = -1;
            if (ii+1 + 2 < objc) {
                if (Tcl_GetIntFromObj(NULL, objv[ii+1], &broadcast) != TCL_OK) {
                    return TCL_ERROR;
                }
                ii++;
            }
        } else if (OPT_CMP(opt, "-head")) {
            queuepos = TCL_QUEUE_HEAD;
        } else {
            goto usage;
        }
    }

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[ii], 0)) == NULL) {
        return TCL_ERROR;
    }
    script = objv[ii+1];

    /*
     * See if any worker available to run the job.
     */
    _log_debug(" !!! pool master: new job ...");

    Tcl_MutexLock(&tpoolPtr->mutex);

    if (tpoolPtr->tearDown || tpoolPtr->refCount <= 0) {
        if (interp) {
            Tcl_AppendResult(interp, "threadpool is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        goto done;
    }

    if (nostart) {
        if (tpoolPtr->numWorkers == 0) {

            /*
             * Assure there is at least one worker running.
             */

            if (CreateWorker(interp, tpoolPtr, 1) != TCL_OK) {
                retCode = TCL_ERROR;
                goto done;
            }

            /*
             * Wait for worker to start while servicing the event loop
             */

            Tcl_MutexUnlock(&tpoolPtr->mutex);
            tsdPtr->stop = -1;
            while(tsdPtr->stop == -1) {
                Tcl_DoOneEvent((TCL_ALL_EVENTS & ~TCL_IDLE_EVENTS));
            }
            Tcl_MutexLock(&tpoolPtr->mutex);
        }
    }
    else 
    while (tpoolPtr->numWorkers < tpoolPtr->maxWorkers) {

        /*
         * If there are no idle worker threads, start some new
         * unless we are already running max number of workers.
         * Don't wait for the next one to become idle, because
         * we are event-driven now, and can notify workers also 
         * using ring for distribution.
         */

        if (tpoolPtr->idleWorkers == 0 || broadcast) {

            /*
             * No more free workers; start new one
             */

            if (CreateWorker(interp, tpoolPtr, 1) != TCL_OK) {
                retCode = TCL_ERROR;
                goto done;
            }

            /*
             * Wait for worker to start while servicing the event loop
             */

            Tcl_MutexUnlock(&tpoolPtr->mutex);
            tsdPtr->stop = -1;
            while(tsdPtr->stop == -1) {
                Tcl_DoOneEvent((TCL_ALL_EVENTS & ~TCL_IDLE_EVENTS));
            }
            Tcl_MutexLock(&tpoolPtr->mutex);
        }

        if (!broadcast) 
            break;
    }

    if (tpoolPtr->tearDown || tpoolPtr->refCount <= 0) {
        if (interp) {
            Tcl_AppendResult(interp, "threadpool is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        goto done;
    }

    /*
     * Create new job ticket and put it on the list.
     * If broadcast repeat it (given count or numWorkers times),
     * do all this during mutex locked.
     */
    if (broadcast <= -1)
        broadcast = tpoolPtr->numWorkers;

    do {

        rPtr = (TpoolResult*)ckalloc(sizeof(TpoolResult));

        if (detached == 0) {
            jobId = ++tpoolPtr->jobId;
        }
        rPtr->jobId = jobId;
        rPtr->script    = Sv_DupIncrObj(script);
        rPtr->detached  = detached;
        rPtr->threadId  = Tcl_GetCurrentThread();
        rPtr->tpoolPtr  = tpoolPtr;
        rPtr->resultObj = NULL;
        rPtr->nextPtr   = 
        rPtr->prevPtr   = NULL;

        _log_debug(" !!! pool master: job ... %p, %d/%d/%d, [%04X]/[%04X] ", rPtr, 
            tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers,
            tpoolPtr->idleRing?tpoolPtr->idleRing->threadId:0,
            tpoolPtr->workerRing?tpoolPtr->workerRing->threadId:0);

        if (!distribute) {
            /* add job to queue */
            PushWork(rPtr, tpoolPtr);
            /* 
             * wake resp. notify idle worker (because not idle workers will wake itself 
             * later if jobs still available), in broadcast do this only last numWorkers times
             */
            if (!tpoolPtr->suspend && tpoolPtr->idleWorkers 
             && (broadcast <= tpoolPtr->numWorkers)
            ) {
                SignalWorker(tpoolPtr, -1, NULL, queuepos);
            }
        } else {
            /* distribute - send job to first worker directly, that will distribute
             * over all workers, because it moves a ring head to next one */
            SignalWorker(tpoolPtr, 1, rPtr, queuepos);
        }

        if (detached == 0) {
            /* prepare jobid(s) for return to interp */
            if (!broadcast) {
                retJobs = Tcl_NewWideIntObj(jobId);
            } else {
                if (retJobs == NULL) {
                    retJobs = Tcl_NewWideIntObj(jobId);
                    retJobs = Tcl_NewListObj(1, &retJobs);
                } else {
                    if (Tcl_ListObjAppendElement(interp, retJobs, 
                            Tcl_NewWideIntObj(jobId)) != TCL_OK) {
                        Tcl_DecrRefCount(retJobs); 
                        retJobs = NULL, retCode = TCL_ERROR; 
                        goto done;
                    }
                }
            }
        }

    } while (--broadcast > 0);

done:
    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (retJobs != NULL) {
        Tcl_SetObjResult(interp, retJobs);
    }
    return retCode;

  usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-async|-detached? ?-nostart|-nowait?"
        " ?-distribute? ?-head? ?-broadcast ?count|-1??"
        " tpoolId script"
    );
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolWaitObjCmd --
 *
 *  This procedure is invoked to process the "tpool::wait" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolWaitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ii, done, wObjc, retCode = TCL_OK;
    Tcl_WideInt jobId;
    Tcl_Obj *listVar = NULL;
    Tcl_Obj *waitList = NULL, *doneList, **wObjv;
    ThreadPool *tpoolPtr;
    TpoolResult *rPtr;
    Tcl_HashEntry *hPtr;

    TpoolSpecificData *tsdPtr = TPOOL_TSD_INIT();

    /*
     * Syntax: tpool::wait tpoolId jobIdList ?listVar?
     */

    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId jobIdList ?listVar");
        return TCL_ERROR;
    }
    if (objc == 4) {
        listVar = objv[3];
    }
    if (Tcl_ListObjGetElements(interp, objv[2], &wObjc, &wObjv) != TCL_OK) {
        return TCL_ERROR;
    }
    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
        return TCL_ERROR;
    }

    done = 0; /* Number of elements in the done list */
    doneList = Tcl_NewListObj(0, NULL);

    Tcl_MutexLock(&tpoolPtr->mutex);
    while (1) {
        if (tpoolPtr->tearDown > 1) {
            Tcl_AppendResult(interp, "threadpool is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
            retCode = TCL_ERROR;
            goto end;
        }
        if (listVar)
            waitList = Tcl_NewListObj(0, NULL);
        for (ii = 0; ii < wObjc; ii++) {
            if (Tcl_GetWideIntFromObj(interp, wObjv[ii], &jobId) != TCL_OK) {
                retCode = TCL_ERROR;
                goto end;
            }
            hPtr = Tcl_FindHashEntry(&tpoolPtr->jobsDone, (void *)(size_t)jobId);
            if (hPtr) {
                rPtr = (TpoolResult*)Tcl_GetHashValue(hPtr);
            } else {
                rPtr = NULL;
            }
            if (rPtr == NULL) {
                if (listVar) {
                    Tcl_ListObjAppendElement(interp, waitList, wObjv[ii]);
                }
            } else if (!rPtr->detached && rPtr->resultObj) {
                done++;
                Tcl_ListObjAppendElement(interp, doneList, wObjv[ii]);
            } else if (listVar) {
                Tcl_ListObjAppendElement(interp, waitList, wObjv[ii]);
            }
        }
        if (done) {
            break;
        }

        /*
         * None of the jobs done, wait for completion
         * of the next job and try again.
         */

        if (waitList) {
            Tcl_DecrRefCount(waitList); waitList = NULL;
        }

        Tcl_MutexUnlock(&tpoolPtr->mutex);
        tsdPtr->stop = -1;
        while (tsdPtr->stop == -1) {
            Tcl_DoOneEvent(TCL_ALL_EVENTS);
        }
        Tcl_MutexLock(&tpoolPtr->mutex);
    }

end:

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (listVar && waitList) {
        Tcl_ObjSetVar2(interp, listVar, NULL, waitList, 0);
    }

    if (retCode != TCL_OK) {
        Tcl_DecrRefCount(doneList);
        return retCode;
    }

    Tcl_SetObjResult(interp, doneList);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolCancelObjCmd --
 *
 *  This procedure is invoked to process the "tpool::cancel" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolCancelObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ii, wObjc, retCode = TCL_OK;
    Tcl_WideInt jobId;
    Tcl_Obj *listVar = NULL;
    Tcl_Obj *doneList, *waitList = NULL, **wObjv;
    ThreadPool *tpoolPtr;
    TpoolResult *rPtr;

    /*
     * Syntax: tpool::cancel tpoolId jobIdList ?listVar?
     */

    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId jobIdList ?listVar");
        return TCL_ERROR;
    }
    if (objc == 4) {
        listVar = objv[3];
    }
    if (Tcl_ListObjGetElements(interp, objv[2], &wObjc, &wObjv) != TCL_OK) {
        return TCL_ERROR;
    }
    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
        return TCL_ERROR;
    }

    doneList = Tcl_NewListObj(0, NULL);
    if (listVar)
        waitList = Tcl_NewListObj(0, NULL);

    Tcl_MutexLock(&tpoolPtr->mutex);
    if (tpoolPtr->tearDown > 1) {
        goto end;
    }
    for (ii = 0; ii < wObjc; ii++) {
        if (Tcl_GetWideIntFromObj(interp, wObjv[ii], &jobId) != TCL_OK) {
            retCode = TCL_ERROR;
            goto end;
        }
        for (rPtr = tpoolPtr->workHead; rPtr; rPtr = rPtr->nextPtr) {
            if (rPtr->jobId == jobId) {
                if (rPtr->prevPtr != NULL) {
                    rPtr->prevPtr->nextPtr = rPtr->nextPtr;
                } else {
                    tpoolPtr->workHead = rPtr->nextPtr;
                }
                if (rPtr->nextPtr != NULL) {
                    rPtr->nextPtr->prevPtr = rPtr->prevPtr;
                } else {
                    tpoolPtr->workTail = rPtr->prevPtr;
                }
                SetResult(NULL, rPtr); /* Just to free the result */
                Tcl_DecrRefCount(rPtr->script);
                ckfree((char*)rPtr);
                Tcl_ListObjAppendElement(interp, doneList, wObjv[ii]);
                break;
            }
        }
        if (rPtr == NULL && listVar) {
            Tcl_ListObjAppendElement(interp, waitList, wObjv[ii]);
        }
    }

end:

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (listVar && waitList)
        Tcl_ObjSetVar2(interp, listVar, NULL, waitList, 0);

    if (retCode != TCL_OK) {
        Tcl_DecrRefCount(doneList);
        return retCode;
    }

    Tcl_SetObjResult(interp, doneList);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolGetObjCmd --
 *
 *  This procedure is invoked to process the "tpool::get" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolGetObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ret = TCL_OK;
    Tcl_WideInt jobId;
    Tcl_Obj *resVar = NULL;
    ThreadPool *tpoolPtr;
    TpoolResult *rPtr;
    Tcl_HashEntry *hPtr = NULL;

    /*
     * Syntax: tpool::get tpoolId jobId ?result?
     */

    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId jobId ?result?");
        return TCL_ERROR;
    }
    if (Tcl_GetWideIntFromObj(interp, objv[2], &jobId) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 4) {
        resVar = objv[3];
    }

    /*
     * Locate the threadpool
     */

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
        return TCL_ERROR;
    }

    /*
     * Locate the job in question. It is an error to
     * do a "get" on bogus job handle or on the job
     * which did not complete yet.
     */

    Tcl_MutexLock(&tpoolPtr->mutex);
    if (tpoolPtr->tearDown <= 1) {
        hPtr = Tcl_FindHashEntry(&tpoolPtr->jobsDone, (void *)(size_t)jobId);
    }
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "no such job", NULL);
        ret = TCL_ERROR;
        goto done;
    }
    rPtr = (TpoolResult*)Tcl_GetHashValue(hPtr);
    if (rPtr->resultObj == NULL) {
        Tcl_AppendResult(interp, "job not completed", NULL);
        ret = TCL_ERROR;
        goto done;
    }

    /* completed / retrieved - delete entry */
    Tcl_DeleteHashEntry(hPtr);

done:

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (ret != TCL_OK)
        return ret;

    ret = SetResult(interp, rPtr);
    ckfree((char*)rPtr);

    if (resVar) {
        Tcl_ObjSetVar2(interp, resVar, NULL, Tcl_GetObjResult(interp), 0);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
        ret = TCL_OK;
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReserveObjCmd --
 *
 *  This procedure is invoked to process the "tpool::preserve" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolReserveObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ret;
    ThreadPool *tpoolPtr;

    /*
     * Syntax: tpool::preserve tpoolId
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId");
        return TCL_ERROR;
    }

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 0)) == NULL) {
        return TCL_ERROR;
    }

    Tcl_MutexLock(&listMutex);
    ret = TpoolReserve(tpoolPtr);
    Tcl_MutexUnlock(&listMutex);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReleaseObjCmd --
 *
 *  This procedure is invoked to process the "tpool::release" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolReleaseObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    int ret;
    ThreadPool *tpoolPtr;

    /*
     * Syntax: tpool::release tpoolId
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId");
        return TCL_ERROR;
    }

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 2)) == NULL) {
        return TCL_ERROR;
    }

    Tcl_MutexLock(&listMutex);
    ret = TpoolRelease(tpoolPtr);
    Tcl_MutexUnlock(&listMutex);

    /* wrap object to pure string object - indirect decrements a pool refCount and frees a pool if the last one */
    Tcl_GetString(objv[1]);
    TpoolObj_FreeInternalRep(objv[1]);

    Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolSuspendObjCmd --
 *
 *  This procedure is invoked to process the "tpool::suspend" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolSuspendObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadPool *tpoolPtr;
    int mode = TPOOL_SUSPEND_FULL;

    /*
     * Syntax: tpool::suspend ?-full|-jobs? tpoolId
     */

    if (objc < 2 || objc > 3) {
        goto usage;
    }

    if (objc == 3) {
        if (strcmp(Tcl_GetString(objv[1]), "-jobs") == 0) {
            objv++; objc--;
            mode = TPOOL_SUSPEND_JOBS;
        } else if (strcmp(Tcl_GetString(objv[1]), "-full") == 0) {
            objv++; objc--;
            /* mode = TPOOL_SUSPEND_FULL; */
        } else {
            goto usage;
        }
    }

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
        return TCL_ERROR;
    }

    TpoolSuspend(tpoolPtr, mode);

    return TCL_OK;

usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-full|-jobs? tpoolId");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolResumeObjCmd --
 *
 *  This procedure is invoked to process the "tpool::resume" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolResumeObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadPool *tpoolPtr;

    /*
     * Syntax: tpool::resume tpoolId
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId");
        return TCL_ERROR;
    }

    if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
        return TCL_ERROR;
    }

    TpoolResume(interp, tpoolPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolNamesObjCmd --
 *
 *  This procedure is invoked to process the "tpool::names" Tcl
 *  command. See the user documentation for details on what it does.
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
TpoolNamesObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *const objv[];   /* Argument objects. */
{
    ThreadPool *tpoolPtr;
    Tcl_Obj    *listObj;

    if (objc == 1) {
        listObj = Tcl_NewListObj(0, NULL);
        Tcl_MutexLock(&listMutex);
        for (tpoolPtr = tpoolList; tpoolPtr; tpoolPtr = tpoolPtr->nextPtr) {
            Tcl_Obj *objPtr = Tcl_NewObj();
            objPtr->bytes = NULL;
            tpoolPtr->objRefCount++;
            TpoolObj_SetObjIntRep(objPtr, tpoolPtr);
            Tcl_ListObjAppendElement(interp, listObj, objPtr);
        }
        Tcl_MutexUnlock(&listMutex);
        Tcl_SetObjResult(interp, listObj);

        return TCL_OK;
    }

    if (objc == 2) {
        TpoolWorker *poolWrk, *lastHead;

        if ((tpoolPtr = GetTpoolFromObj(interp, objv[1], 1)) == NULL) {
            return TCL_ERROR;
        }

        listObj = Tcl_NewListObj(0, NULL);

        /* go over ring of workers */
        Tcl_MutexLock(&tpoolPtr->mutex);
        poolWrk = lastHead = tpoolPtr->workerRing;
        while (poolWrk) {

            Tcl_Obj *objPtr = ThreadNewThreadObj(poolWrk->threadId);

            if (Tcl_ListObjAppendElement(interp, listObj, objPtr) != TCL_OK) {
                Tcl_MutexUnlock(&tpoolPtr->mutex);
                Tcl_DecrRefCount(listObj);
                return TCL_ERROR;
            }

            poolWrk = poolWrk->nextWrk;
            if (poolWrk == lastHead)
                break;
        }
        Tcl_MutexUnlock(&tpoolPtr->mutex);

        Tcl_SetObjResult(interp, listObj);
        return TCL_OK;
    }
#if 0
 usage:
#endif
    Tcl_WrongNumArgs(interp, 1, objv,
        "?tpoolId?"
    );
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateWorker --
 *
 *  Creates new worker thread for the given pool. Assumes the caller
 *  holds the pool mutex, and if no "lock" holds the pool start mutex also.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Informs waiter thread (if any) about the new worker thread.
 *
 *----------------------------------------------------------------------
 */
static int
CreateWorker(interp, tpoolPtr, lock)
    Tcl_Interp *interp;
    ThreadPool *tpoolPtr;
    int lock;
{
    Tcl_ThreadId id;
    TpoolResult result;

    if (tpoolPtr->tearDown || tpoolPtr->refCount <= 0) {
        if (interp) {
            Tcl_AppendResult(interp, "threadpool is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        return TCL_ERROR;
    }

    /*
     * Initialize the result structure to be
     * passed to the new thread. This is used
     * as communication to and from the thread.
     */

    memset(&result, 0, sizeof(TpoolResult));
    result.tpoolPtr = tpoolPtr;
    result.threadId = Tcl_GetCurrentThread();

    /*
     * Create new worker thread here. Wait for the thread to start
     * because it's using the ThreadResult arg which is on our stack.
     */

    if (lock) {
        Tcl_MutexLock(&tpoolPtr->startMutex);
    }
    if (Tcl_CreateThread(&id, TpoolWorkerProc, (ClientData)&result,
                         TCL_THREAD_STACK_DEFAULT, 0) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("can't create a new thread", -1));
        Tcl_MutexUnlock(&tpoolPtr->startMutex);
        return TCL_ERROR;
    }
    while ( result.resultObj == NULL ) {
        Tcl_ConditionWait(&tpoolPtr->cond, &tpoolPtr->startMutex, NULL);
    }
    if (lock) {
        Tcl_MutexUnlock(&tpoolPtr->startMutex);
    }

    /*
     * Set error-related information if the thread
     * failed to initialize correctly.
     */

    return SetResult(interp, &result);
}

/*
 *----------------------------------------------------------------------
 *
 * RingAttachWorker --, RingDetachWorker ---
 *
 *  Add or remove worker to/from worker ring. Assumes the caller
 *  holds the pool mutex (or possible the pool master holds the 
 *  pool mutex and start mutex - only one started per pool).
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Informs waiter thread (if any) about the new worker thread.
 *
 *----------------------------------------------------------------------
 */
static void
RingAttachWorker(poolWrk)
    TpoolWorker *poolWrk;
{
    ThreadPool *tpoolPtr = poolWrk->tpoolPtr;

    tpoolPtr->numWorkers++;
    if (tpoolPtr->workerRing) {
        /* add to tail (end of ring), ring points to first in buffer */
        TpoolWorker * tailPtr;
        poolWrk->prevWrk = tailPtr = tpoolPtr->workerRing->prevWrk;
        poolWrk->nextWrk = tpoolPtr->workerRing;
        tailPtr->nextWrk = tpoolPtr->workerRing->prevWrk = poolWrk;

    } else {
        /*  first one - is a self-contained ring */
        tpoolPtr->workerRing = poolWrk->prevWrk = poolWrk->nextWrk = poolWrk;
    }
    _log_debug(" !!! ++ worker = %d/%d/%d, [%04X]/[%04X] ", 
        tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers,
        tpoolPtr->idleRing?tpoolPtr->idleRing->threadId:0,
        tpoolPtr->workerRing?tpoolPtr->workerRing->threadId:0);
}
/*
 *----------------------------------------------------------------------
 */
static void
RingDetachWorker(poolWrk)
    TpoolWorker *poolWrk;
{
    ThreadPool *tpoolPtr = poolWrk->tpoolPtr;

    /* if head - move ring to the next */
    if (tpoolPtr->workerRing == poolWrk) {
        tpoolPtr->workerRing = poolWrk->nextWrk;
    }
    /* if ring contains other elements then detach, otherwise make ring empty */
    if (poolWrk != tpoolPtr->workerRing) {
        poolWrk->prevWrk->nextWrk = poolWrk->nextWrk;
        poolWrk->nextWrk->prevWrk = poolWrk->prevWrk;
    } else {
        tpoolPtr->workerRing = NULL;
    }
    poolWrk->nextWrk = poolWrk->prevWrk = NULL;
    tpoolPtr->numWorkers--;
    _log_debug(" !!! -- worker = %d/%d/%d, [%04X]/[%04X] ", 
        tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers,
        tpoolPtr->idleRing?tpoolPtr->idleRing->threadId:0,
        tpoolPtr->workerRing?tpoolPtr->workerRing->threadId:0);
}


/*
 *----------------------------------------------------------------------
 */
static void
RingAttachIdle(poolWrk)
    TpoolWorker *poolWrk;
{
    ThreadPool *tpoolPtr = poolWrk->tpoolPtr;

    /* add to tail of idle workers ring */
    tpoolPtr->idleWorkers++;
    if (tpoolPtr->idleRing) {
        /* different head and tail inserts, corresponding shift-idle-ring mode, 
         * so always prefer last worker if not "shift" */
        if (!(tpoolPtr->flags & TPOOL_FLAG_SHIFT_IDLERING)) {
            /* add to head (begin of ring), to be the turn again, so possible
             * few context switches resp. cpu cache washout's - prevents L1-2 cache misses */
            TpoolWorker * headPtr;
            poolWrk->nextIdl = headPtr = tpoolPtr->idleRing;
            poolWrk->prevIdl = headPtr->prevIdl;
            tpoolPtr->idleRing = headPtr->prevIdl = headPtr->prevIdl->nextIdl = poolWrk;
        } else {
            /* add to tail (end of ring), ring points to first in buffer */
            TpoolWorker * tailPtr;
            poolWrk->prevIdl = tailPtr = tpoolPtr->idleRing->prevIdl;
            poolWrk->nextIdl = tpoolPtr->idleRing;
            tailPtr->nextIdl = tpoolPtr->idleRing->prevIdl = poolWrk;
        }
    } else {
        /*  first one - is a self-contained ring */
        tpoolPtr->idleRing = poolWrk->prevIdl = poolWrk->nextIdl = poolWrk;
        /* always move worker ring head to the first idle */
        tpoolPtr->workerRing = poolWrk;
    }
    _log_debug(" !!! ++ idle = %d/%d/%d, [%04X]/[%04X] ", 
        tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers,
        tpoolPtr->idleRing?tpoolPtr->idleRing->threadId:0,
        tpoolPtr->workerRing?tpoolPtr->workerRing->threadId:0);
}

/*
 *----------------------------------------------------------------------
 */
static void
RingDetachIdle(poolWrk)
    TpoolWorker *poolWrk;
{
    ThreadPool *tpoolPtr = poolWrk->tpoolPtr;

    /* if head - move ring to the next */
    if (tpoolPtr->idleRing == poolWrk) {
        tpoolPtr->idleRing = poolWrk->nextIdl;
    }
    /* if ring contains other elements then detach, otherwise make ring empty */
    if (poolWrk != tpoolPtr->idleRing) {
        /* ring contains other elements then detach */
        poolWrk->prevIdl->nextIdl = poolWrk->nextIdl;
        poolWrk->nextIdl->prevIdl = poolWrk->prevIdl;
    } else {
        /* no more elements - make ring empty */
        tpoolPtr->idleRing = NULL;
    }
    poolWrk->nextIdl = poolWrk->prevIdl = NULL;
    tpoolPtr->idleWorkers--;
    _log_debug(" !!! -- idle = %d/%d/%d, [%04X]/[%04X] ", 
        tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers,
        tpoolPtr->idleRing?tpoolPtr->idleRing->threadId:0,
        tpoolPtr->workerRing?tpoolPtr->workerRing->threadId:0);
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolDeleteEvent --
 *
 *  This will be called by exit of worker to delete pool tasks related
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
TpoolDeleteEvent(eventPtr, clientData)
    Tcl_Event *eventPtr;        /* Really ThreadEvent */
    ClientData clientData;      /* dummy */
{
    if (eventPtr->proc == WorkEvent) {

        TpoolEvent  *poolEvent = (TpoolEvent*)(eventPtr+1);
        TpoolResult *rPtr     = poolEvent->rPtr;
        
        if (rPtr != NULL) {

            TpoolWorker *poolWrk  = poolEvent->poolWrk;
            ThreadPool  *tpoolPtr = poolWrk->tpoolPtr;
            /* if pool still alive */
            if (!tpoolPtr->tearDown) {
                /* we can't process work - add it to the queue */
                Tcl_MutexLock(&tpoolPtr->mutex);

                /* pool still alive in lock */
                if (!tpoolPtr->tearDown) {
                    PushWork(rPtr, tpoolPtr);
                    rPtr = NULL;
                    /* wake resp. notify idle worker (because not idle will wake itself later if jobs available) */
                    if (!tpoolPtr->suspend && tpoolPtr->idleWorkers) {
                        SignalWorker(tpoolPtr, -1, NULL, TCL_QUEUE_TAIL);
                    }
                }

                Tcl_MutexUnlock(&tpoolPtr->mutex);
            } 
            if (rPtr != NULL) {
                /* just free not processed jobs */
                Tcl_DecrRefCount(rPtr->script);
                ckfree((char*)rPtr);
            }
        }
        return 1;
    } 
    else 
    if (eventPtr->proc == TpoolReloadEvent) {
        TpoolEvent  *poolEvent = (TpoolEvent*)(eventPtr+1);
        Tcl_Obj  *reloadScript = (Tcl_Obj*)poolEvent->rPtr;
        if (reloadScript != NULL) {
            Tcl_DecrRefCount(reloadScript);
        }
        return 1;
    }
    /* remain foreign events in the queue ... */
    return (eventPtr->proc == NULL || eventPtr->proc == ServiceEvent);
}

/*
 *----------------------------------------------------------------------
 */
static int
TpoolWorkerServiceCmd(
    ClientData  clientData,     /* Pool worker handle */
    Tcl_Interp *interp,         /* Current interpreter. */
    int         objc,           /* Number of arguments. */
    Tcl_Obj    *const objv[]    /* Argument objects. */
) 
{
    TpoolWorker  *poolWrk = (TpoolWorker*)clientData;
    int inflag;
    if (objc <= 1) {
        goto usage;
    }
    /* svc command (get/set servicing flag, to provide an opportunity to remain the worker in idle state) */
    /* inwork command (get/set in work flag) - provide an opportunity to process other pool works (disable discrete mode once) */
    if (strcmp(Tcl_GetString(objv[1]), "-svc") == 0) {
        inflag = TPWRK_FLAG_SVC;
    } else if (strcmp(Tcl_GetString(objv[1]), "-inwork") == 0) {
        inflag = TPWRK_FLAG_INWORK;
    } else {
        goto usage;
    }
    if (objc == 2) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(poolWrk && poolWrk->flags & inflag ? 1 : 0));
        return TCL_OK;
    }
    if (objc == 3) {
        int flag;
        if (Tcl_GetBooleanFromObj(interp, objv[2], &flag) != TCL_OK) {
            return TCL_ERROR;
        }
        if (poolWrk) {
            if (flag)
                poolWrk->flags |= inflag;
            else
                poolWrk->flags &= ~inflag;
        } else {
            flag = 0;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(flag));
        return TCL_OK;
    }

 usage:
    Tcl_WrongNumArgs(interp, 1, objv,
        "-svc ?boolean?|-inwork ?boolean?"
    );
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 */
static int
ServiceEvent(Tcl_Event *eventPtr, int mask)
{
    TpoolEvent  *poolEvent = (TpoolEvent*)(eventPtr+1);
    TpoolWorker *poolWrk  = poolEvent->poolWrk;
    ThreadPool  *tpoolPtr = poolWrk->tpoolPtr;

    _log_debug("  !!! service event ... %d", HasWork(tpoolPtr));
   
    /* simple service event (wakeup) - set service flag,
     * make possible to leave thread in idle state if no work in queue ... */
    if (!HasWork(tpoolPtr)) {
        poolWrk->flags |= TPWRK_FLAG_SVC;
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 */
static int
WorkEvent(Tcl_Event *eventPtr, int mask)
{
    TpoolEvent  *poolEvent = (TpoolEvent*)(eventPtr+1);
    TpoolWorker *poolWrk  = poolEvent->poolWrk;
    TpoolResult *rPtr     = poolEvent->rPtr;
    ThreadPool  *tpoolPtr = poolWrk->tpoolPtr;
    int          lock = 0;
    int          processed = 1;

    _log_debug("  !!! work event ... %p / %d", rPtr, HasWork(tpoolPtr))

again:

    if (tpoolPtr->tearDown) {

        if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);}

        if (tpoolPtr->tearDown) {
            /* distributed work */
            if (rPtr != NULL) {
                if (tpoolPtr->tearDown <= 1) {
                    /* we can't process work - add it to the queue */
                    PushWork(rPtr, tpoolPtr);
                    /* don't signal worker - pool suspended or shutdown */
                } else {
                    Tcl_DecrRefCount(rPtr->script);
                    ckfree((char*)rPtr);
                }
            }
            goto done;
        }

    }

    if (tpoolPtr->suspend) {

        if (lock) {lock=0; Tcl_MutexUnlock(&tpoolPtr->mutex);}
        /* active or passive wait in suspend ? */
        Tcl_MutexLock(&tpoolPtr->startMutex);
        while (tpoolPtr->suspend && tpoolPtr->suspend != TPOOL_SUSPEND_JOBS) {
            _log_debug("  !!! suspended all: cond wait ...")
            Tcl_ConditionWait(&tpoolPtr->cond, &tpoolPtr->startMutex, NULL);
        }
        if (tpoolPtr->suspend == TPOOL_SUSPEND_JOBS) {
            Tcl_MutexUnlock(&tpoolPtr->startMutex);
            if (rPtr != NULL) {
                /* refuse event - process it later */
                _log_debug("  !!! suspended: refuse event, do work later")
                processed = 0;
            } else {
                /* go to suspend state in worker proc */
                _log_debug("  !!! suspended: service event ...")
                poolWrk->flags |= TPWRK_FLAG_SVC;
            }
            goto done;
        }
        Tcl_MutexUnlock(&tpoolPtr->startMutex);
        
    }

    /* 
     * in discrete mode, prevent execute another work if still in work, so check already in work,
     * can occur when update/vwait/etc was called during the work
     */
    if ( (tpoolPtr->flags & TPOOL_FLAG_DISCRETE) && (poolWrk->flags & TPWRK_FLAG_INWORK) ) {
        /* if distributed, refuse this event - process it later; 
           otherwise - simply ignore this event (PopWork/wake-empty) */
        if (rPtr != NULL) {
            _log_debug("  !!! still in work: refuse event, do this work later")
            processed = 0;
        } else {
            _log_debug("  !!! still in work: ignore event")
        }
        goto done;
    }
    /* idle, can process work */

    /* if not distributed - get job from pool queue */
    if (rPtr == NULL) {
        if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);}
        /* we locked the mutex, check again */
        if (tpoolPtr->suspend || tpoolPtr->tearDown)
            goto again;

        rPtr = PopWork(tpoolPtr);
    }

    if (rPtr != NULL) {
        /* work, reset idle - signal busy */
        poolWrk->flags = (poolWrk->flags & ~TPWRK_FLAG_IDLE) | TPWRK_FLAG_INWORK;
        /* busy - if not yet already */
        if (poolWrk->nextIdl) {
            if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);}
            if (poolWrk->nextIdl) {
                RingDetachIdle(poolWrk);
            }
        }

        if (lock) {lock=0; Tcl_MutexUnlock(&tpoolPtr->mutex);}

        /* execute an job */
        _log_debug(" !!! exec job ... %p ", rPtr);
        TpoolEval(poolWrk->interp, rPtr->script, rPtr);
        
        Tcl_DecrRefCount(rPtr->script);

        /* reset in work flag, don't set idle flag - will be set later in the main cycle */
        poolWrk->flags &= ~TPWRK_FLAG_INWORK;

        /* if result should be delivered to the pool - add the result to jobs done */
        if (!rPtr->detached) {
            int new;
            if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);}
            if (tpoolPtr->tearDown <= 1) {
                Tcl_SetHashValue(
                    Tcl_CreateHashEntry(
                        &tpoolPtr->jobsDone, (void *)(size_t)rPtr->jobId, &new),
                    (ClientData)rPtr
                );
                SignalWaiter(tpoolPtr, rPtr->threadId);
            } else {
                SetResult(NULL, rPtr);
                ckfree((char*)rPtr);
            }
        } else {
            ckfree((char*)rPtr);
        }
        rPtr = NULL;
    } else {
        // no work, if idle - signal it was a service event, idle will be continued:
        if (poolWrk->flags & TPWRK_FLAG_IDLE)
            poolWrk->flags |= TPWRK_FLAG_SVC;
    }

done:

    if (lock) {lock=0; Tcl_MutexUnlock(&tpoolPtr->mutex);}
    return processed;
}

/*
 *----------------------------------------------------------------------
 */
static void
WorkerIdleTOut(ClientData clientData)
{
    TpoolWorker  *poolWrk = (TpoolWorker*)clientData;
    _log_debug(" !!! idle timeout timer ... ");
    /* if still idle - signal timeout occurred: */
    poolWrk->flags = (poolWrk->flags & ~TPWRK_FLAG_IDLTMR) | TPWRK_FLAG_SVC;
    if (poolWrk->flags & TPWRK_FLAG_IDLE) {
        poolWrk->flags |= TPWRK_FLAG_IDLTOUT;
    }
    /* we should repeat an idle interval/time events */
    Tcl_GetTime(&poolWrk->idleStartTm);
}
/*
 *----------------------------------------------------------------------
 */
static void
WorkerIdle(ClientData clientData) 
{
    TpoolWorker  *poolWrk = (TpoolWorker*)clientData;

    _log_debug(" !!! idle event (latency) ... ");
    if (poolWrk->idleScript != NULL && (poolWrk->flags & TPWRK_FLAG_IDLE)) {
        int oldMode = Tcl_SetServiceMode(TCL_SERVICE_NONE);
        _log_debug(" !!! exec idle command ... ");
        if (Tcl_EvalObjEx(poolWrk->interp, poolWrk->idleScript, TCL_EVAL_GLOBAL) != TCL_OK) {
            ThreadErrorProc(poolWrk->interp);
        }
        (void) Tcl_SetServiceMode(oldMode);
    }

    /* idle latency elapsed (don't reset/repeat until idle) + service flag */
    poolWrk->flags |= TPWRK_FLAG_SVC;
}
/*
 *----------------------------------------------------------------------
 */
static void
WorkerIdleInt(ClientData clientData) 
{
    TpoolWorker  *poolWrk = (TpoolWorker*)clientData;

    _log_debug(" !!! idle interval event ... ");
    if (poolWrk->idleIntScript != NULL && (poolWrk->flags & TPWRK_FLAG_IDLE)) {
        int oldMode = Tcl_SetServiceMode(TCL_SERVICE_NONE);
        _log_debug(" !!! exec idle int command ... ");
        if (Tcl_EvalObjEx(poolWrk->interp, poolWrk->idleIntScript, TCL_EVAL_GLOBAL) != TCL_OK) {
            ThreadErrorProc(poolWrk->interp);
        }
        (void) Tcl_SetServiceMode(oldMode);
    }

    /* repeateable interval, signal the idle was executed */
    poolWrk->flags = (poolWrk->flags & ~TPWRK_FLAG_IDLINT) | TPWRK_FLAG_SVC;
    /* we should repeat an idle interval */
    Tcl_GetTime(&poolWrk->idleIntTm);
}

/*
 *----------------------------------------------------------------------
 */
static void
WorkerSetMaxBlockTime(Tcl_Time *nowTime, Tcl_Time *startTime, Tcl_Time *offs)
{
    Tcl_Time blockTime;
    blockTime.sec = startTime->sec + offs->sec - nowTime->sec;
    blockTime.usec = startTime->usec + offs->usec - nowTime->usec;
    while (blockTime.usec < 0) {
        blockTime.sec--;
        blockTime.usec += 1000000;
    }
    while (blockTime.usec >= 1000000) {
        blockTime.sec++;
        blockTime.usec -= 1000000;
    }
    if (blockTime.sec < 0 || (blockTime.sec == 0 && blockTime.usec < 0)) {
        blockTime.sec = 0;
        blockTime.usec = 0;
    }
    _log_debug(" !!! block time ... %d.%d", blockTime.sec, blockTime.usec);
    Tcl_SetMaxBlockTime(&blockTime);
}
/*
 *----------------------------------------------------------------------
 */
static void
WorkerEventSetupProc(ClientData poolWrkCD, int flags)
{
    TpoolWorker *poolWrk = poolWrkCD;
    ThreadPool *tpoolPtr;
    Tcl_Time    nowTime;
    int         nf = 0;

    /* if don't wait or not exec idle events or not idle worker */
    if (
        (flags & TCL_DONT_WAIT) != 0 || (flags & TCL_IDLE_EVENTS) == 0
     || (poolWrk->flags & TPWRK_FLAG_IDLE) == 0
    ) {
        return;
    }

    /* setup or prolong idle "timers" - latency, interval, timeout */
    tpoolPtr = poolWrk->tpoolPtr;
    if ( 
      (poolWrk->flags & TPWRK_FLAG_IDLLAT) == 0
    ) {
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        WorkerSetMaxBlockTime(&nowTime, &poolWrk->idleStartTm, &tpoolPtr->idleLatency);
    }
    else
    if (
      poolWrk->idleIntScript != NULL && (poolWrk->flags & TPWRK_FLAG_IDLINT) == 0
    ) {
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        WorkerSetMaxBlockTime(&nowTime, &poolWrk->idleIntTm, &tpoolPtr->idleIntTime);
    }

    if (
        (poolWrk->flags & TPWRK_FLAG_IDLTMR) == 0 &&
        TimeIsNonZero(tpoolPtr->idleTime)
    ) {
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        WorkerSetMaxBlockTime(&nowTime, &poolWrk->idleStartTm, &tpoolPtr->idleTime);
    }
}

/*
 *----------------------------------------------------------------------
 */
static void
WorkerEventCheckProc(ClientData poolWrkCD, int flags)
{
    TpoolWorker *poolWrk = poolWrkCD;
    ThreadPool *tpoolPtr;
    Tcl_Time    nowTime, endOfWait; 
    int         nf = 0;

    if ( 
        (flags & TCL_DONT_WAIT) != 0 || (flags & TCL_IDLE_EVENTS) == 0
     || (poolWrk->flags & TPWRK_FLAG_IDLE) == 0 
    ) {
        return;
    }

    /* create idle events if configured time (since idle start) reached */
    tpoolPtr = poolWrk->tpoolPtr;
    if ( (poolWrk->flags & TPWRK_FLAG_IDLLAT) == 0 ) {
        endOfWait = poolWrk->idleStartTm;
        TimeAddTime(endOfWait, tpoolPtr->idleLatency);
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        if ( TimeGreaterOrEqual(nowTime, endOfWait) ) {
            if (poolWrk->idleScript != NULL) {
              Tcl_DoWhenIdle(WorkerIdle, (ClientData)poolWrk);
            }
            /* idle latency elapsed */
            poolWrk->flags |= TPWRK_FLAG_IDLLAT;
            /* we should repeat an idle interval */
            Tcl_GetTime(&poolWrk->idleIntTm);

        }
    }
    else
    if ( 
        poolWrk->idleIntScript != NULL &&
        (poolWrk->flags & TPWRK_FLAG_IDLINT) == 0
    ) {
        endOfWait = poolWrk->idleIntTm;
        TimeAddTime(endOfWait, tpoolPtr->idleIntTime);
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        if ( TimeGreaterOrEqual(nowTime, endOfWait) ) {
            Tcl_DoWhenIdle(WorkerIdleInt, (ClientData)poolWrk);
            poolWrk->flags |= TPWRK_FLAG_IDLINT;
        }
    }

    if (
        (poolWrk->flags & TPWRK_FLAG_IDLTMR) == 0 &&
        TimeIsNonZero(tpoolPtr->idleTime) 
    ) {
        endOfWait = poolWrk->idleStartTm;
        TimeAddTime(endOfWait, tpoolPtr->idleTime);
        if (!nf) {nf = 1; Tcl_GetTime(&nowTime);}
        if ( TimeGreaterOrEqual(nowTime, endOfWait) ) {
            Tcl_DoWhenIdle(WorkerIdleTOut, (ClientData)poolWrk);
            poolWrk->flags |= TPWRK_FLAG_IDLTMR;
        }
    }
}

/*
 *----------------------------------------------------------------------
 */
static void
WorkerExitHandler(ClientData clientData)
{
    TpoolWorker  *poolWrk = (TpoolWorker*)clientData;
    ThreadPool   *tpoolPtr;

    _log_debug(" !!! worker exit handler ...");
    
    if (poolWrk == NULL) {
        return;
    }

    /* cleanup worker of pool */
    tpoolPtr = poolWrk->tpoolPtr;

    if (tpoolPtr != NULL) {

        _log_debug(" !!! worker delete events ... ");

        /* move distributed jobs to pool (if not tear down), release other event data */
        Tcl_DeleteEvents((Tcl_EventDeleteProc*)TpoolDeleteEvent, NULL);

        _log_debug(" !!! worker stop ... ");

        Tcl_MutexLock(&tpoolPtr->mutex);

        /* worker goes down - create replacement for this if expected (pool still alive) */
        if (
            poolWrk->interp && !(poolWrk->flags & TPWRK_FLAG_ERROR) &&
            !tpoolPtr->suspend && !tpoolPtr->tearDown 
          && (
               tpoolPtr->numWorkers < tpoolPtr->minWorkers
            || (HasWork(tpoolPtr) && tpoolPtr->numWorkers < tpoolPtr->maxWorkers)
          )
        ) {
            if (CreateWorker(poolWrk->interp, tpoolPtr, 1) != TCL_OK) {
                ThreadErrorProc(poolWrk->interp);
                /* bgerror can be an idle event, so process it now */
                while (Tcl_DoOneEvent(TCL_IDLE_EVENTS)) {};
            }

            /* signal idle (waiter) workers to be sure jobs will be acquired */
            SignalWorker(tpoolPtr, -1, TPOOL_SVC_EVENT, TCL_QUEUE_TAIL);
        }

        _log_debug(" !!! worker stopped");

        /* signal waiter if tear down */
        tpoolPtr->stopWorkers--;
        if (tpoolPtr->waiterId) {
            SignalWaiter(tpoolPtr, tpoolPtr->waiterId);
        }

        poolWrk->tpoolPtr = NULL;

        Tcl_MutexUnlock(&tpoolPtr->mutex);

        if (poolWrk->idleScript != NULL) {
            Tcl_DecrRefCount(poolWrk->idleScript);
        }
        if (poolWrk->idleIntScript != NULL) {
            Tcl_DecrRefCount(poolWrk->idleIntScript);
        }

    }

    if (poolWrk->interp) {
        Tcl_Interp * interp = poolWrk->interp;
        poolWrk->interp = NULL;

        _log_debug(" !!! worker delete interp ... ");

    #ifdef NS_AOLSERVER
        Ns_TclMarkForDelete(interp);
        Ns_TclDeAllocateInterp(interp);
    #else
        Tcl_DeleteInterp(interp);
    #endif

        Tcl_Release((ClientData)interp);
    }

    ckfree((char*)poolWrk);
    _log_debug(" !!! worker exit handler - done.");
}
/*
 *----------------------------------------------------------------------
 *
 * TpoolWorkerProc --
 *
 *  This is the main function of each of the threads in the pool.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_ThreadCreateType
TpoolWorkerProc(clientData)
    ClientData clientData;
{
    TpoolResult          *rPtr = (TpoolResult*)clientData;
    ThreadPool       *tpoolPtr = rPtr->tpoolPtr;
    Tcl_ThreadId startWaiterId;
    struct ThreadSpecificData * threadTsdPtr = Thread_TSD_Init();

    Tcl_Interp *interp;
    TpoolWorker  *poolWrk = NULL;
    int         ret, retcode = -1;

    poolWrk = (TpoolWorker*)ckalloc(sizeof(TpoolWorker));
    if (!poolWrk) {
        retcode = TCL_ERROR;
        goto end;
    }
    memset(poolWrk, 0, sizeof(*poolWrk));
    poolWrk->tpoolPtr = tpoolPtr;
    poolWrk->threadId = Tcl_GetCurrentThread();

    /* create exit handler for cleanups */
    Tcl_CreateThreadExitHandler(WorkerExitHandler, (ClientData)poolWrk);

    _log_debug(" !!! worker start ... ");

    Tcl_MutexLock(&tpoolPtr->startMutex);

    /*
     * Initialize the Tcl interpreter
     */

#ifdef NS_AOLSERVER
    poolWrk->interp = interp = (Tcl_Interp*)Ns_TclAllocateInterp(NULL);
    if (!interp) {
        retcode = TCL_ERROR;
    }
#else
    poolWrk->interp = interp = Tcl_CreateInterp();
    if (!interp) {
        retcode = TCL_ERROR;
    }

    if (interp) {
        Tcl_Preserve((ClientData)interp);

        if (Tcl_Init(interp) != TCL_OK) {
            retcode = TCL_ERROR;
        } else if (Thread_Init(interp) != TCL_OK) {
            retcode = TCL_ERROR;
        } else {
            /* (re)create svc command with poolWrk handle */
            if (Tcl_CreateObjCommand(interp, THREAD_CMD_PREFIX"svc", 
                TpoolWorkerServiceCmd, (ClientData)poolWrk, NULL) == NULL
            ) {
                retcode = TCL_ERROR;
            }
        }
    }
#endif

    /*
     * Initialize the worker (call init script)
     */

    if (interp && tpoolPtr->initScript) {
        Tcl_Obj *objPtr = Sv_DuplicateObj(tpoolPtr->initScript);
        Tcl_IncrRefCount(objPtr);
        retcode = TpoolEval(interp, objPtr, rPtr);
        Tcl_DecrRefCount(objPtr);
        if (retcode != TCL_OK) {
            retcode = TCL_ERROR;
        }
    }

    if (retcode == TCL_ERROR) {
        /* prevent pool from releasing of tpoolPtr before worker exit */
        tpoolPtr->stopWorkers++;
        /* mark worker in error state */
        poolWrk->flags |= TPWRK_FLAG_ERROR;
        /* set error if not yet in result */
        if (!rPtr->resultObj) {
            if (interp) {
                Tcl_InitObjRef(rPtr->resultObj, 
                    ThreadGetReturnInterpStateObj(interp, retcode));
            } else {
                Tcl_InitObjRef(rPtr->resultObj,
                    ThreadErrorInterpStateObj(TCL_ERROR, "init interp failed!", "TCL EFAULT"));
            }
        }
        /* tell caller and done */
        Tcl_ConditionNotify(&tpoolPtr->cond);
        Tcl_MutexUnlock(&tpoolPtr->startMutex);
        goto end;
    }

    RingAttachWorker(poolWrk);

    /*
     * Tell caller we've started successfull
     */

    retcode = TCL_OK;
    startWaiterId = rPtr->threadId;
    Tcl_SetObjRef(rPtr->resultObj, Tcl_NewObj());
    Tcl_ConditionNotify(&tpoolPtr->cond);
    Tcl_MutexUnlock(&tpoolPtr->startMutex);


    poolWrk->idleScript = Sv_DupIncrObj(tpoolPtr->idleScript);
    poolWrk->idleIntScript = Sv_DupIncrObj(tpoolPtr->idleIntScript);

    _log_debug(" !!! worker started ... ");
    Tcl_MutexLock(&tpoolPtr->mutex);

    RingAttachIdle(poolWrk);

    SignalWaiter(tpoolPtr, startWaiterId);

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (
         (tpoolPtr->idleScript != NULL)
      || (tpoolPtr->idleIntScript != NULL)
      || TimeIsNonZero(tpoolPtr->idleTime)
    ) {
        Tcl_CreateEventSource(WorkerEventSetupProc, WorkerEventCheckProc, poolWrk);
    }

    /*
     * Wait for jobs to arrive, simultaneous process events of tcl-core.
     */

    while ( !Thread_Stopped(threadTsdPtr) ) {
        int val;

        if (tpoolPtr->tearDown) {
            Tcl_MutexLock(&tpoolPtr->mutex);
            val = tpoolPtr->tearDown;
            Tcl_MutexUnlock(&tpoolPtr->mutex);
            if (val) {
                /* shutdown */
                break;
            }
        }

        if (tpoolPtr->suspend) {
            /* active or passive wait in suspend ? */
            if (tpoolPtr->suspend != TPOOL_SUSPEND_JOBS) {
                Tcl_MutexLock(&tpoolPtr->startMutex);
                while (tpoolPtr->suspend && tpoolPtr->suspend != TPOOL_SUSPEND_JOBS) {
                    _log_debug("  !!! suspend all: cond wait ...")
                    Tcl_ConditionWait(&tpoolPtr->cond, &tpoolPtr->startMutex, NULL);
                }
                Tcl_MutexUnlock(&tpoolPtr->startMutex);
            }
            if (tpoolPtr->suspend == TPOOL_SUSPEND_JOBS) {
                _log_debug("  !!! suspend work: wait within event ...")
            }
        }

        _log_debug(" !!! wait-cycle .  %s, flags: %02X ", "*", poolWrk->flags);
        poolWrk->flags &= ~TPWRK_FLAG_SVC;
        /* process events of tcl-core without wait (check idle) */
        ret = Tcl_DoOneEvent((TCL_ALL_EVENTS & ~TCL_IDLE_EVENTS) | TCL_DONT_WAIT);
        _log_debug(" !!! ev w/o wait = %d, flags: %02X ", ret, poolWrk->flags);

        /* no more events */
        if (!ret) {
            int lock = 0;
            
            /* if worker goes idle */
            if ( !(poolWrk->flags & TPWRK_FLAG_IDLE) && !tpoolPtr->suspend ) {

                /* 
                 * no events processed but pool has jobs, be sure all jobs will be processed:
                 * possible some worker missed a signal (or no signal because no idle workers), 
                 * or just resumed after suspend
                 */
                if (HasWork(tpoolPtr)) {
                    TpoolResult *rPtr;
                    if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);};
                    if (!tpoolPtr->suspend && !tpoolPtr->tearDown && (rPtr = PopWork(tpoolPtr))) {
                        /* send work to itself (via event) */
                        SignalWorkerInner(poolWrk, WorkEvent, rPtr, TCL_QUEUE_TAIL);
                        lock = 0; Tcl_MutexUnlock(&tpoolPtr->mutex);
                        /* don't wait - has job now - repeat process event right now */
                        continue;
                    }
                }

                poolWrk->flags |= TPWRK_FLAG_IDLE;
                Tcl_GetTime(&poolWrk->idleStartTm);
                poolWrk->idleIntTm = poolWrk->idleStartTm;

                /* attach worker to idle ring, if it not yet belong */
                if (!poolWrk->nextIdl) {
                    if (!lock) {lock=1; Tcl_MutexLock(&tpoolPtr->mutex);}
                    if (!poolWrk->nextIdl) {
                        RingAttachIdle(poolWrk);
                    }
                }
                /* pool still has job, check in lock again (if worker goes idle) */
                if (lock && !tpoolPtr->suspend && HasWork(tpoolPtr)) {
                    SignalWorkerInner(poolWrk, WorkEvent, NULL, TCL_QUEUE_TAIL);
                }

                if (lock) {lock=0; Tcl_MutexUnlock(&tpoolPtr->mutex);}
            }

            /* wait for job, simultaneous process events of tcl-core */
            _log_debug(" !!! wait 4 event  %s, flags: %02X ", "*", poolWrk->flags);
            poolWrk->flags &= ~TPWRK_FLAG_SVC;
            ret = Tcl_DoOneEvent(TCL_ALL_EVENTS);
            _log_debug(" !!! end of wait = %d, flags: %02X ", ret, poolWrk->flags);
        }
        /* service event - no work, still idle */
        if ((poolWrk->flags & TPWRK_FLAG_SVC)) {
            poolWrk->flags &= ~TPWRK_FLAG_SVC;
            ret = 0;
        }
        /* mark as busy if event was not a idle/service event */
        if (ret) {
            poolWrk->flags &= ~(TPWRK_FLAG_IDLE|TPWRK_FLAG_IDLTOUT);
        }

        /* if worker no more idle - reset timer/event if was set */
        if ( !(poolWrk->flags & TPWRK_FLAG_IDLE) ) {
            /* cancel events, reset signals to setup "timers" again */
            if (poolWrk->flags & TPWRK_FLAG_IDLLAT) {
                Tcl_CancelIdleCall(WorkerIdle, (ClientData)poolWrk);
                poolWrk->flags &= ~TPWRK_FLAG_IDLLAT;
            }
            if (poolWrk->flags & TPWRK_FLAG_IDLINT) {
                Tcl_CancelIdleCall(WorkerIdleInt, (ClientData)poolWrk);
                poolWrk->flags &= ~TPWRK_FLAG_IDLINT;
            }
            if (poolWrk->flags & TPWRK_FLAG_IDLTMR) {
                Tcl_CancelIdleCall(WorkerIdleTOut, (ClientData)poolWrk);
                poolWrk->flags &= ~TPWRK_FLAG_IDLTMR;
            }
        }

        /* if timeout, check min workers, etc. */
        if (poolWrk->flags & TPWRK_FLAG_IDLTOUT) {
            Tcl_MutexLock(&tpoolPtr->mutex);
            /* If worker count at min, leave this one alive */
            val = (tpoolPtr->numWorkers > tpoolPtr->minWorkers);
            Tcl_MutexUnlock(&tpoolPtr->mutex);
            if (val) {
                break; /* Enough workers, can safely shutdown this one */
            }
            _log_debug(" !!! ignore timeout, workers %d ... continue.", tpoolPtr->numWorkers);
            /* reset timeout and idle timer flag also to generate an idle timer for new timeout again */
            poolWrk->flags &= ~(TPWRK_FLAG_IDLTOUT|TPWRK_FLAG_IDLTMR);
        }

        /* repeat wait cycle */
    }

    _log_debug(" !!! worker shutdown ... ");

    /* mark thread as stopped (if someone wait for worker or check it with "thread::exists") */
    ThreadReserve(NULL, (Tcl_ThreadId)0, threadTsdPtr, THREAD_RELEASE, 0);

    Tcl_DeleteEventSource(WorkerEventSetupProc, WorkerEventCheckProc, poolWrk);

    Tcl_MutexLock(&tpoolPtr->mutex);

    /*
     * Tear down the worker
     */
    if (poolWrk->nextIdl)
        RingDetachIdle(poolWrk);
    tpoolPtr->stopWorkers++;
    if (poolWrk->nextWrk)
        RingDetachWorker(poolWrk);

    Tcl_MutexUnlock(&tpoolPtr->mutex);

    /* call exit script, that should also call "update", if some events should be processed */
    if (tpoolPtr->exitScript) {
        Tcl_Obj *objPtr = Sv_DuplicateObj(tpoolPtr->exitScript);
        Tcl_IncrRefCount(objPtr);
        TpoolEval(interp, objPtr, NULL);
        Tcl_DecrRefCount(objPtr);
    }

 end:

    _log_debug(" !!! worker finalization ... ");
    Tcl_ExitThread(0);

     _log_debug(" !!! worker exit. ")

    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 */
static int
TpoolReloadEvent(Tcl_Event *eventPtr, int mask)
{
    TpoolEvent  *poolEvent = (TpoolEvent*)(eventPtr+1);
    TpoolWorker *poolWrk   = poolEvent->poolWrk;
    ThreadPool  *tpoolPtr  = poolWrk->tpoolPtr;
    Tcl_Obj *reloadScript  = (Tcl_Obj*)poolEvent->rPtr;

    /* if worker still attached (alive) */
    if (poolWrk->nextWrk) {

        Sv_SetDupObj(&poolWrk->idleScript,    tpoolPtr->idleScript);
        Sv_SetDupObj(&poolWrk->idleIntScript, tpoolPtr->idleIntScript);

        /* execute reload script broadcasted to worker */
        if (reloadScript) {
            int ret;
            Tcl_IncrRefCount(reloadScript);
            ret = Tcl_EvalObjEx(poolWrk->interp, reloadScript, TCL_EVAL_GLOBAL);
            if (ret != TCL_OK) {
                ThreadErrorProc(poolWrk->interp);
            }
        }
    }

    if (reloadScript) {
        Tcl_DecrRefCount(reloadScript);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * RunStopEvent --
 *
 *  Signalizes the waiter thread to stop waiting.
 *
 * Results:
 *  1 (always)
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
static int
RunStopEvent(eventPtr, mask)
    Tcl_Event *eventPtr;
    int mask;
{
    TpoolSpecificData *tsdPtr = TPOOL_TSD_INIT();

    tsdPtr->stop = 1;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * PushWork --
 *
 *  Adds a worker thread to the end of the workers list.
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
PushWork(rPtr, tpoolPtr)
    TpoolResult *rPtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(rPtr, tpoolPtr->workHead);
    if (tpoolPtr->workTail == NULL) {
        tpoolPtr->workTail = rPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PopWork --
 *
 *  Pops the work ticket from the list
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static TpoolResult *
PopWork(tpoolPtr)
    ThreadPool *tpoolPtr;
{
    TpoolResult *rPtr = tpoolPtr->workTail;

    if (rPtr == NULL) {
        return NULL;
    }

    tpoolPtr->workTail = rPtr->prevPtr;
    SpliceOut(rPtr, tpoolPtr->workHead);

    rPtr->nextPtr = rPtr->prevPtr = NULL;

    return rPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetTpool
 *
 *  Parses the Tcl threadpool handle and locates the
 *  corresponding threadpool maintenance structure.
 *
 * Results:
 *  Pointer to the threadpool struct or NULL if none found,
 *
 * Side effects:
 *  Value of objRefCount for returned pool is incremented
 *
 *----------------------------------------------------------------------
 */
static ThreadPool*
GetTpool(tpoolName)
    const char *tpoolName;
{
    ThreadPool *tpoolPtr;

    Tcl_MutexLock(&listMutex);
    tpoolPtr = GetTpoolUnl(tpoolName);
    if (tpoolPtr != NULL)
        tpoolPtr->objRefCount++;
    Tcl_MutexUnlock(&listMutex);

    return tpoolPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetTpoolUnl
 *
 *  Parses the threadpool handle and locates the
 *  corresponding threadpool maintenance structure.
 *  Assumes caller holds the listMutex,
 *
 * Results:
 *  Pointer to the threadpool struct or NULL if none found,
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static ThreadPool*
GetTpoolUnl (tpoolName)
    const char *tpoolName;
{
    ThreadPool *tpool;
    ThreadPool *tpoolPtr = NULL;

    if (sscanf(tpoolName, TPOOL_HNDLPREFIX"%p", &tpool) != 1) {
        return NULL;
    }
    for (tpoolPtr = tpoolList; tpoolPtr; tpoolPtr = tpoolPtr->nextPtr) {
        if (tpoolPtr == tpool) {
            break;
        }
    }

    return tpoolPtr;
}

/*
 * Type definition.
 */

Tcl_ObjType TpoolObjType = {
    "threadpool",          /* name */
    TpoolObj_FreeInternalRep, /* freeIntRepProc */
    TpoolObj_DupInternalRep,  /* dupIntRepProc */
    TpoolObj_UpdateString,    /* updateStringProc */
    TpoolObj_SetFromAny       /* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 */
static ThreadPool*
GetTpoolFromObj(interp, objPtr, shutDownLevel)
    Tcl_Interp *interp;
    Tcl_Obj    *objPtr;
    int shutDownLevel;
{
    ThreadPool *tpoolPtr;
    if (objPtr->typePtr != &TpoolObjType &&
        TpoolObj_SetFromAny(interp, objPtr) != TCL_OK
    ) {
        return NULL;
    }

    tpoolPtr = (ThreadPool*)objPtr->internalRep.twoPtrValue.ptr1;

    if (tpoolPtr->tearDown > shutDownLevel || (!shutDownLevel && tpoolPtr->refCount <= 0)) {
        if (interp) {
            Tcl_AppendResult(interp, "threadpool is shut down", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ESHUTDOWN", NULL);
        }
        return NULL;
    }
    return tpoolPtr;
}
/*
 *----------------------------------------------------------------------
 */
static void
TpoolObj_DupInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;
    Tcl_Obj *copyPtr;
{
    ThreadPool *tpoolPtr = (ThreadPool*)srcPtr->internalRep.twoPtrValue.ptr1;

    Tcl_MutexLock(&listMutex);
    tpoolPtr->objRefCount++;
    Tcl_MutexUnlock(&listMutex);
    TpoolObj_SetObjIntRep(copyPtr, tpoolPtr);
}
/*
 *----------------------------------------------------------------------
 */
static void
TpoolObj_FreeInternalRep(objPtr)
    Tcl_Obj *objPtr;
{
    ThreadPool *tpoolPtr = (ThreadPool*)objPtr->internalRep.twoPtrValue.ptr1;
    Tcl_MutexLock(&listMutex);
    /* decrement reference count of pool */
    if (--tpoolPtr->objRefCount <= 0) {
        /* if not reserved also - free it */
        if (tpoolPtr->refCount <= 0) {
            TpoolRelease(tpoolPtr);
        }
    }
    Tcl_MutexUnlock(&listMutex);
    objPtr->typePtr = NULL;
};
/*
 *----------------------------------------------------------------------
 */
static int
TpoolObj_SetFromAny(interp, objPtr)
    Tcl_Interp *interp;
    Tcl_Obj    *objPtr;
{
    ThreadPool *tpoolPtr;
    char *tpoolName = objPtr->bytes;
    if (!tpoolName)
        tpoolName = Tcl_GetString(objPtr);
    if (!tpoolName || (tpoolPtr = GetTpool(tpoolName)) == NULL) {
        if (interp) {
            Tcl_AppendResult(interp, "can not find threadpool \"",
                tpoolName ? tpoolName : "", "\"", NULL);
            Tcl_SetErrorCode(interp, "TCL", "ENOENT", NULL);
        }
        return TCL_ERROR;
    }
    
    if (objPtr->typePtr && objPtr->typePtr->freeIntRepProc)
        objPtr->typePtr->freeIntRepProc(objPtr);
    TpoolObj_SetObjIntRep(objPtr, tpoolPtr);
    return TCL_OK;
};
/*
 *----------------------------------------------------------------------
 */
static void
TpoolObj_UpdateString(objPtr)
    Tcl_Obj  *objPtr;
{
    ThreadPool *tpoolPtr = (ThreadPool*)objPtr->internalRep.twoPtrValue.ptr1;
    char buf[64];
    int  len = sprintf(buf, "%s%p", TPOOL_HNDLPREFIX, tpoolPtr);
    objPtr->length = len,
    objPtr->bytes = ckalloc((size_t)++len);
    if (objPtr->bytes)
        memcpy(objPtr->bytes, buf, len);
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolEval
 *
 *  Evaluates the script and fills in the result structure.
 *
 * Results:
 *  Standard Tcl result,
 *
 * Side effects:
 *  Many, depending on the script.
 *
 *----------------------------------------------------------------------
 */
static int
TpoolEval(interp, script, rPtr)
    Tcl_Interp *interp;
    Tcl_Obj *script;
    TpoolResult *rPtr;
{
    int ret;

    /* prevent wrap return code, should be possible to return to the caller thread: return = 2, break = 3, etc... */
    ThreadIncrNumLevels(interp, 1);
    /* todo: make possible to eval in current context/scope/namesapace (not global) */
    ret = Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL);
    ThreadIncrNumLevels(interp, -1);

    /* process error with error proc or set it to result pointer */
    if (rPtr == NULL || rPtr->detached) {
        if (ret != TCL_OK) {
            ThreadErrorProc(interp);
        }
        return ret;
    } else {
        Tcl_SetObjRef(rPtr->resultObj,
            ThreadGetReturnInterpStateObj(interp, ret));
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * SetResult
 *
 *  Sets the result in current interpreter.
 *
 * Results:
 *  Standard Tcl result,
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
static int
SetResult(interp, rPtr)
    Tcl_Interp *interp;
    TpoolResult *rPtr;
{
    if (rPtr->resultObj) {
        int code;
        if (interp) {
            code = ThreadRestoreInterpStateFromObj(interp, rPtr->resultObj);
        } else {
            code = ThreadGetStatusOfInterpStateObj(rPtr->resultObj);
        }
        Tcl_DecrRefCount(rPtr->resultObj);
        rPtr->resultObj = NULL;
        return code;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReserve --
 *
 *  Does the pool preserve and/or release. Assumes caller holds
 *  the listMutex.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May tear-down the threadpool if refcount drops to 0 or below.
 *
 *----------------------------------------------------------------------
 */
static int
TpoolReserve(tpoolPtr)
    ThreadPool *tpoolPtr;
{
    return ++tpoolPtr->refCount;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolRelease --
 *
 *  Does the pool preserve and/or release. Assumes caller holds
 *  the listMutex.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May tear-down the threadpool if refcount drops to 0 or below.
 *
 *----------------------------------------------------------------------
 */
static int
TpoolRelease(tpoolPtr)
    ThreadPool *tpoolPtr;
{
        TpoolSpecificData *tsdPtr;
        TpoolResult *rPtr;
        Tcl_HashEntry *hPtr;
        Tcl_HashSearch search;

    if (tpoolPtr->refCount > 0 && tpoolPtr->tearDown == 0) {

        _log_debug(" !!! pool master: release %d", tpoolPtr->refCount);
        if (--tpoolPtr->refCount > 0) {
            return tpoolPtr->refCount;
        }

        /*
         * Pool is going away; remove from the list of pools,
         */

        tsdPtr = TPOOL_TSD_INIT();

        SpliceOut(tpoolPtr, tpoolList);

        tpoolPtr->waiterId = Tcl_GetCurrentThread();

        /*
         * Signal and wait for all workers to die.
         */

        tpoolPtr->tearDown = 1;
        Tcl_MutexLock(&tpoolPtr->mutex);
        /* if was suspended - resume, otherwise wake up workers */
        if (tpoolPtr->suspend) {
            TpoolResume(NULL, tpoolPtr);
        } else {
            /* first all idle workers (break Tcl_DoOneEvent) */
            SignalWorker(tpoolPtr, 0x7fffffff, TPOOL_SVC_EVENT, TCL_QUEUE_HEAD);
        }
        /* second all working threads (+ wait to their end) */
        while (tpoolPtr->numWorkers > 0 || tpoolPtr->stopWorkers > 0) {
            Tcl_ConditionNotify(&tpoolPtr->cond);
            tsdPtr->stop = -1;
            Tcl_MutexUnlock(&tpoolPtr->mutex);
            while (
                tsdPtr->stop == -1 && 
                (tpoolPtr->numWorkers > 0 || tpoolPtr->stopWorkers > 0)
            ) {
                _log_debug(" !!! wait for workers: (%d)/%d/%d", 
                    tpoolPtr->idleWorkers, tpoolPtr->numWorkers, tpoolPtr->stopWorkers);
                Tcl_DoOneEvent(TCL_ALL_EVENTS);
            }
            Tcl_MutexLock(&tpoolPtr->mutex);
        }

        tpoolPtr->waiterId = (Tcl_ThreadId)NULL;

        _log_debug(" !!! pool master: tear down the pool structure");
        /*
         * Tear down the pool structure
         */

        tpoolPtr->tearDown = 2;
        
        if (tpoolPtr->initScript) {
            Tcl_DecrRefCount(tpoolPtr->initScript);
            tpoolPtr->initScript = NULL;
        }
        if (tpoolPtr->exitScript) {
            Tcl_DecrRefCount(tpoolPtr->exitScript);
            tpoolPtr->exitScript = NULL;
        }
        if (tpoolPtr->idleScript) {
            Tcl_DecrRefCount(tpoolPtr->idleScript);
            tpoolPtr->idleScript = NULL;
        }
        if (tpoolPtr->idleIntScript) {
            Tcl_DecrRefCount(tpoolPtr->idleIntScript);
            tpoolPtr->idleIntScript = NULL;
        }

        /*
         * Cleanup completed but not collected jobs
         */

        hPtr = Tcl_FirstHashEntry(&tpoolPtr->jobsDone, &search);
        while (hPtr != NULL) {
            rPtr = (TpoolResult*)Tcl_GetHashValue(hPtr);
            SetResult(NULL, rPtr);
            ckfree((char*)rPtr);
            Tcl_DeleteHashEntry(hPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&tpoolPtr->jobsDone);

        /*
         * Cleanup jobs posted but never completed.
         */

        for (rPtr = tpoolPtr->workHead; rPtr; rPtr = rPtr->nextPtr) {
            Tcl_DecrRefCount(rPtr->script);
            ckfree((char*)rPtr);
        }
        Tcl_MutexUnlock(&tpoolPtr->mutex);
        Tcl_MutexFinalize(&tpoolPtr->mutex);
        Tcl_MutexFinalize(&tpoolPtr->startMutex);
        Tcl_ConditionFinalize(&tpoolPtr->cond);
    }

    /* remove tpoolPtr only if the last reference everywhere */
    if (tpoolPtr->refCount == 0 && tpoolPtr->objRefCount == 0) {
        ckfree((char*)tpoolPtr);
    }

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolSuspend --
 *
 *  Marks the pool as suspended. This prevents pool workers to drain
 *  the pool work queue.
 *
 * Results:
 *  Value of the suspend flag (TPOOL_SUSPEND_JOBS | TPOOL_SUSPEND_FULL).
 *
 * Side effects:
 *  During the suspended state, pool worker threads wlll not timeout
 *  even if the worker inactivity timer has been configured.
 *
 *----------------------------------------------------------------------
 */
static void
TpoolSuspend(tpoolPtr, mode)
    ThreadPool *tpoolPtr;
    int mode;
{
    Tcl_MutexLock(&tpoolPtr->mutex);
    Tcl_MutexLock(&tpoolPtr->startMutex);
    if (tpoolPtr->suspend != mode) {
        /* other suspend mode - notify workers */
        if (tpoolPtr->suspend) {
            tpoolPtr->suspend = mode;
            Tcl_ConditionNotify(&tpoolPtr->cond);
        } else {
            tpoolPtr->suspend = mode;
        }
        /* for mode "full" we should be sure are workers leaves waiting Tcl_DoOneEvent */
        if (mode == TPOOL_SUSPEND_FULL) {
            SignalWorker(tpoolPtr, 0x7fffffff, TPOOL_SVC_EVENT, TCL_QUEUE_HEAD);
        }
    }
    Tcl_MutexUnlock(&tpoolPtr->startMutex);
    Tcl_MutexUnlock(&tpoolPtr->mutex);
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolResume --
 *
 *  Clears the pool suspended state. This allows pool workers to drain
 *  the pool work queue again.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Pool workers may be started or awaken.
 *
 *----------------------------------------------------------------------
 */
static void
TpoolResume(interp, tpoolPtr)
    Tcl_Interp *interp;
    ThreadPool *tpoolPtr;
{
    /* worker use "startMutex" for conditional wait for resume */
    Tcl_MutexLock(&tpoolPtr->mutex);
    Tcl_MutexLock(&tpoolPtr->startMutex);

    tpoolPtr->suspend = 0;
    Tcl_ConditionNotify(&tpoolPtr->cond);

    /* if some worker goes down - start again ... */
    if (
        !tpoolPtr->tearDown 
    ) {
        int count = tpoolPtr->minWorkers - tpoolPtr->numWorkers;
        if (count <= 0 && HasWork(tpoolPtr))
            count = 1;
        while (
            count-- > 0
         && tpoolPtr->numWorkers < tpoolPtr->maxWorkers
        ) {
            int ret;
            ret = CreateWorker(interp, tpoolPtr, 0);
            if (ret != TCL_OK) {
                ThreadErrorProc(interp);
                break;
            }
        }
    }

    /* Wake all workers, that can have jobs to be processed */
    if (HasWork(tpoolPtr)) {
        /* signal using create events */
        SignalWorker(tpoolPtr, 0x7fffffff, TPOOL_SVC_EVENT, TCL_QUEUE_TAIL);
    } else {
        /* because could be distributed works was refused, just notify instead of signal with event */
        TpoolWorker *poolWrk, *lastHead;
        poolWrk = lastHead = tpoolPtr->workerRing;
        while (poolWrk) {
            Tcl_ThreadAlert(poolWrk->threadId);
            poolWrk = poolWrk->nextWrk;
            if (poolWrk == lastHead)
                break;
        }
    }

    Tcl_MutexUnlock(&tpoolPtr->startMutex);
    Tcl_MutexUnlock(&tpoolPtr->mutex);
}

/*
 *----------------------------------------------------------------------
 *
 * SignalWaiter --
 *
 *  Signals the waiter thread.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  The waiter thread will exit from the event loop.
 *
 *----------------------------------------------------------------------
 */
static void
SignalWaiter(tpoolPtr, threadId)
    ThreadPool  *tpoolPtr;
    Tcl_ThreadId threadId;
{
    Tcl_Event *evPtr;

    evPtr = (Tcl_Event*)ckalloc(sizeof(Tcl_Event));
    evPtr->proc = RunStopEvent;

    Tcl_ThreadQueueEvent(threadId,(Tcl_Event*)evPtr,TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(threadId);
}

/*
 *----------------------------------------------------------------------
 *
 * AppExitHandler
 *
 *  Deletes all threadpools on application exit.
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
AppExitHandler(clientData)
    ClientData clientData;
{
    ThreadPool *tpoolPtr;

    Tcl_MutexLock(&listMutex);
    /*
     * Restart with head of list each time until empty. [Bug 1427570]
     */
    for (tpoolPtr = tpoolList; tpoolPtr; tpoolPtr = tpoolList) {
        TpoolRelease(tpoolPtr);
    }
    Tcl_MutexUnlock(&listMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * Tpool_Init --
 *
 *  Create commands in current interpreter.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  On first load, creates application exit handler to clean up
 *  any threadpools left.
 *
 *----------------------------------------------------------------------
 */

int
Tpool_Init (interp)
    Tcl_Interp *interp;                 /* Interp where to create cmds */
{
    static int initialized;

    TCL_CMD(interp, TPOOL_CMD_PREFIX"create",   TpoolCreateObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"names",    TpoolNamesObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"post",     TpoolPostObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"wait",     TpoolWaitObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"cancel",   TpoolCancelObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"get",      TpoolGetObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"preserve", TpoolReserveObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"release",  TpoolReleaseObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"suspend",  TpoolSuspendObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"resume",   TpoolResumeObjCmd);
    TCL_CMD(interp, TPOOL_CMD_PREFIX"reload",   TpoolReloadObjCmd);

    TCL_CMD(interp, THREAD_CMD_PREFIX"svc",     TpoolWorkerServiceCmd);

    if (initialized == 0) {
        Tcl_MutexLock(&listMutex);
        if (initialized == 0) {
            Tcl_CreateExitHandler(AppExitHandler, (ClientData)-1);
            initialized = 1;
        }
        Tcl_MutexUnlock(&listMutex);
    }
    return TCL_OK;
}

/* EOF $RCSfile: threadPoolCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
