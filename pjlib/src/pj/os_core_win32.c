/* 
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/guid.h>
#include <pj/rand.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/except.h>
#include <pj/unicode.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include <windows.h>

#if defined(PJ_HAS_WINSOCK2_H) && PJ_HAS_WINSOCK2_H != 0
#  include <winsock2.h>
#endif

#if defined(PJ_HAS_WINSOCK_H) && PJ_HAS_WINSOCK_H != 0
#  include <winsock.h>
#endif

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
#   include "../../../third_party/threademulation/include/ThreadEmulation.h"
#endif

/* Activate mutex related logging if PJ_DEBUG_MUTEX is set, otherwise
 * use default level 6 logging.
 */
#if defined(PJ_DEBUG_MUTEX) && PJ_DEBUG_MUTEX
#   undef PJ_DEBUG
#   define PJ_DEBUG         1
#   define LOG_MUTEX(expr)  PJ_LOG(5,expr)
#else
#   define LOG_MUTEX(expr)  PJ_LOG(6,expr)
#endif
#   define LOG_MUTEX_WARN(expr)  PJ_PERROR(3,expr)
#define THIS_FILE       "os_core_win32.c"

/*
 * Implementation of pj_thread_t.
 */
struct pj_thread_t
{
    char            obj_name[PJ_MAX_OBJ_NAME];
    HANDLE          hthread;
    DWORD           idthread;
    pj_thread_proc *proc;
    void           *arg;

#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    pj_uint32_t     stk_size;
    pj_uint32_t     stk_max_usage;
    char           *stk_start;
    const char     *caller_file;
    int             caller_line;
#endif
};


/*
 * Implementation of pj_mutex_t.
 */
struct pj_mutex_t
{
#if PJ_WIN32_WINNT >= 0x0400
    CRITICAL_SECTION    crit;
#else
    HANDLE              hMutex;
#endif
    char                obj_name[PJ_MAX_OBJ_NAME];
#if PJ_DEBUG
    int                 nesting_level;
    pj_thread_t        *owner;
#endif
};

/*
 * Implementation of pj_sem_t.
 */
typedef struct pj_sem_t
{
    HANDLE              hSemaphore;
    char                obj_name[PJ_MAX_OBJ_NAME];
} pj_mem_t;


/*
 * Implementation of pj_event_t.
 */
struct pj_event_t
{
    HANDLE              hEvent;
    char                obj_name[PJ_MAX_OBJ_NAME];
};

/*
 * Implementation of pj_atomic_t.
 */
struct pj_atomic_t
{
    long value;
};

/*
 * Implementation of pj_barrier_t.
 */
#if PJ_WIN32_WINNT >= _WIN32_WINNT_WIN8
struct pj_barrier_t {
    SYNCHRONIZATION_BARRIER sync_barrier;
};
#elif PJ_WIN32_WINNT >= _WIN32_WINNT_VISTA
struct pj_barrier_t {
    CRITICAL_SECTION        mutex;
    CONDITION_VARIABLE      cond;
    unsigned                count;
    unsigned                waiting;
};
#else
struct pj_barrier_t {
    HANDLE                  cond;   /* Semaphore */
    LONG                    count;  /* Number of threads required to pass the barrier */
    LONG                    waiting;/* Number of threads waiting at the barrier */
};
#endif

/*
 * Flag and reference counter for PJLIB instance.
 */
static int initialized;

/*
 * Static global variables.
 */
static pj_thread_desc main_thread;
static long thread_tls_id = -1;
static pj_mutex_t critical_section_mutex;
static unsigned atexit_count;
static void (*atexit_func[32])(void);

/*
 * Some static prototypes.
 */
static pj_status_t init_mutex(pj_mutex_t *mutex, const char *name);


/*
 * pj_init(void).
 * Init PJLIB!
 */
PJ_DEF(pj_status_t) pj_init(void)
{
    WSADATA wsa;
    char dummy_guid[32]; /* use maximum GUID length */
    pj_str_t guid;
    pj_status_t rc;

    /* Check if PJLIB have been initialized */
    if (initialized) {
        ++initialized;
        return PJ_SUCCESS;
    }

    /* Init Winsock.. */
    if (WSAStartup(MAKEWORD(2,0), &wsa) != 0) {
        return PJ_RETURN_OS_ERROR(WSAGetLastError());
    }

    /* Init this thread's TLS. */
    if ((rc=pj_thread_init()) != PJ_SUCCESS) {
        return rc;
    }
    
    /* Init logging */
    pj_log_init();

    /* Init random seed. */
    /* Or probably not. Let application in charge of this */
    /* pj_srand( GetCurrentProcessId() ); */

    /* Initialize critical section. */
    if ((rc=init_mutex(&critical_section_mutex, "pj%p")) != PJ_SUCCESS)
        return rc;

    /* Startup GUID. */
    guid.ptr = dummy_guid;
    pj_generate_unique_string( &guid );

    /* Initialize exception ID for the pool. 
     * Must do so after critical section is configured.
     */
    rc = pj_exception_id_alloc("PJLIB/No memory", &PJ_NO_MEMORY_EXCEPTION);
    if (rc != PJ_SUCCESS)
        return rc;

    /* Startup timestamp */
#if defined(PJ_HAS_HIGH_RES_TIMER) && PJ_HAS_HIGH_RES_TIMER != 0
    {
        pj_timestamp dummy_ts;
        if ((rc=pj_get_timestamp_freq(&dummy_ts)) != PJ_SUCCESS) {
            return rc;
        }
        if ((rc=pj_get_timestamp(&dummy_ts)) != PJ_SUCCESS) {
            return rc;
        }
    }
#endif

    /* Flag PJLIB as initialized */
    ++initialized;
    pj_assert(initialized == 1);

    PJ_LOG(4,(THIS_FILE, "pjlib %s for win32 initialized",
              PJ_VERSION));

    return PJ_SUCCESS;
}

/*
 * pj_atexit()
 */
PJ_DEF(pj_status_t) pj_atexit(void (*func)(void))
{
    if (atexit_count >= PJ_ARRAY_SIZE(atexit_func))
        return PJ_ETOOMANY;

    atexit_func[atexit_count++] = func;
    return PJ_SUCCESS;
}


/*
 * pj_shutdown(void)
 */
PJ_DEF(void) pj_shutdown()
{
    int i;

    /* Only perform shutdown operation when 'initialized' reaches zero */
    pj_assert(initialized > 0);
    if (--initialized != 0)
        return;

    /* Display stack usage */
#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    {
        pj_thread_t *rec = (pj_thread_t*)main_thread;
        PJ_LOG(5,(rec->obj_name, "Main thread stack max usage=%u by %s:%d", 
                  rec->stk_max_usage, rec->caller_file, rec->caller_line));
    }
#endif

    /* Call atexit() functions */
    for (i=atexit_count-1; i>=0; --i) {
        (*atexit_func[i])();
    }
    atexit_count = 0;

    /* Free exception ID */
    if (PJ_NO_MEMORY_EXCEPTION != -1) {
        pj_exception_id_free(PJ_NO_MEMORY_EXCEPTION);
        PJ_NO_MEMORY_EXCEPTION = -1;
    }

    /* Destroy PJLIB critical section */
    pj_mutex_destroy(&critical_section_mutex);

    /* Free PJLIB TLS */
    if (thread_tls_id != -1) {
        pj_thread_local_free(thread_tls_id);
        thread_tls_id = -1;
    }

    /* Clear static variables */
    pj_errno_clear_handlers();

    /* Ticket #1132: Assertion when (re)starting PJLIB on different thread */
    pj_bzero(main_thread, sizeof(main_thread));

    /* Shutdown Winsock */
    WSACleanup();
}


/*
 * pj_getpid(void)
 */
PJ_DEF(pj_uint32_t) pj_getpid(void)
{
    PJ_CHECK_STACK();
    return GetCurrentProcessId();
}

/*
 * Check if this thread has been registered to PJLIB.
 */
PJ_DEF(pj_bool_t) pj_thread_is_registered(void)
{
    return pj_thread_local_get(thread_tls_id) != 0;
}


/*
 * Get thread priority value for the thread.
 */
PJ_DEF(int) pj_thread_get_prio(pj_thread_t *thread)
{
#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    PJ_UNUSED_ARG(thread);
    return -1;
#else
    return GetThreadPriority(thread->hthread);
#endif
}


/*
 * Set the thread priority.
 */
PJ_DEF(pj_status_t) pj_thread_set_prio(pj_thread_t *thread,  int prio)
{
#if PJ_HAS_THREADS
    PJ_ASSERT_RETURN(thread, PJ_EINVAL);
    PJ_ASSERT_RETURN(prio>=THREAD_PRIORITY_IDLE && 
                        prio<=THREAD_PRIORITY_TIME_CRITICAL,
                     PJ_EINVAL);

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    if (SetThreadPriorityRT(thread->hthread, prio) == FALSE)
#else
    if (SetThreadPriority(thread->hthread, prio) == FALSE)
#endif
        return PJ_RETURN_OS_ERROR(GetLastError());

    return PJ_SUCCESS;

#else
    PJ_UNUSED_ARG(thread);
    PJ_UNUSED_ARG(prio);
    pj_assert("pj_thread_set_prio() called in non-threading mode!");
    return PJ_EINVALIDOP;
#endif
}


/*
 * Get the lowest priority value available on this system.
 */
PJ_DEF(int) pj_thread_get_prio_min(pj_thread_t *thread)
{
    PJ_UNUSED_ARG(thread);
    return THREAD_PRIORITY_IDLE;
}


/*
 * Get the highest priority value available on this system.
 */
PJ_DEF(int) pj_thread_get_prio_max(pj_thread_t *thread)
{
    PJ_UNUSED_ARG(thread);
    return THREAD_PRIORITY_TIME_CRITICAL;
}


/*
 * Get native thread handle
 */
PJ_DEF(void*) pj_thread_get_os_handle(pj_thread_t *thread) 
{
    PJ_ASSERT_RETURN(thread, NULL);

#if PJ_HAS_THREADS
    return thread->hthread;
#else
    pj_assert("pj_thread_is_registered() called in non-threading mode!");
    return NULL;
#endif
}

/*
 * pj_thread_register(..)
 */
PJ_DEF(pj_status_t) pj_thread_register ( const char *cstr_thread_name,
                                         pj_thread_desc desc,
                                         pj_thread_t **thread_ptr)
{
    char stack_ptr;
    pj_status_t rc;
    pj_thread_t *thread = (pj_thread_t *)desc;
    pj_str_t thread_name = pj_str((char*)cstr_thread_name);

    /* Size sanity check. */
    if (sizeof(pj_thread_desc) < sizeof(pj_thread_t)) {
        pj_assert(!"Not enough pj_thread_desc size!");
        return PJ_EBUG;
    }

    /* If a thread descriptor has been registered before, just return it. */
    if (pj_thread_local_get (thread_tls_id) != 0) {
        // 2006-02-26 bennylp:
        //  This wouldn't work in all cases!.
        //  If thread is created by external module (e.g. sound thread),
        //  thread may be reused while the pool used for the thread descriptor
        //  has been deleted by application.
        //*thread_ptr = (pj_thread_t*)pj_thread_local_get (thread_tls_id);
        //return PJ_SUCCESS;
    }

    /* Initialize and set the thread entry. */
    pj_bzero(desc, sizeof(struct pj_thread_t));
    thread->hthread = GetCurrentThread();
    thread->idthread = GetCurrentThreadId();

#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    thread->stk_start = &stack_ptr;
    thread->stk_size = 0xFFFFFFFFUL;
    thread->stk_max_usage = 0;
#else
    stack_ptr = '\0';
#endif

    if (cstr_thread_name && pj_strlen(&thread_name) < sizeof(thread->obj_name)-1)
        pj_ansi_snprintf(thread->obj_name, sizeof(thread->obj_name), 
                         cstr_thread_name, thread->idthread);
    else
        pj_ansi_snprintf(thread->obj_name, sizeof(thread->obj_name), 
                         "thr%p", (void*)(pj_ssize_t)thread->idthread);
    
    rc = pj_thread_local_set(thread_tls_id, thread);
    if (rc != PJ_SUCCESS)
        return rc;

    *thread_ptr = thread;
    return PJ_SUCCESS;
}

/*
 * pj_thread_init(void)
 */
pj_status_t pj_thread_init(void)
{
    pj_status_t rc;
    pj_thread_t *thread;

    rc = pj_thread_local_alloc(&thread_tls_id);
    if (rc != PJ_SUCCESS)
        return rc;

    return pj_thread_register("thr%p", main_thread, &thread);
}

/*
 * Set current thread display name
 * MSDN document:
 * https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code
 */
#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO {
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags;    // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

// The SetThreadDescription API was brought in version 1607 of Windows 10.
typedef HRESULT(WINAPI *FnSetThreadDescription)(HANDLE hThread,
                                                PCWSTR lpThreadDescription);

static void set_thread_display_name(const char *name)
{
#if (defined(PJ_WIN32_UWP) && PJ_WIN32_UWP!=0) || \
      (defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8!=0)

    return;

#else
    /* Set thread name by SetThreadDescription (if support) */
    FnSetThreadDescription fn = (FnSetThreadDescription)GetProcAddress(
        GetModuleHandle(PJ_T("Kernel32.dll")), "SetThreadDescription");
    PJ_LOG(5, (THIS_FILE, "SetThreadDescription:%p, name:%s", fn, name));
    if (fn) {
        wchar_t wname[PJ_MAX_OBJ_NAME];
        pj_ansi_to_unicode(name, (int)pj_ansi_strlen(name), wname,
                           PJ_MAX_OBJ_NAME);
        fn(GetCurrentThread(), wname);
        return;
    }

    /* Set thread name by throwing an exception */
#if defined(__MINGW32__) || defined(__CYGWIN__)
#pragma message("Warning: Not support exception")
#else
    // The debugger needs to be around to catch the name in the exception.
    // If there isn't a debugger, we are needlessly throwing an exception.
    if (!IsDebuggerPresent()) {
        return;
    }

    {
        const DWORD MS_VC_EXCEPTION = 0x406D1388;
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;
        info.dwThreadID = (DWORD)-1;
        info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable : 6320 6322)
        __try {
            RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR),
                           (ULONG_PTR *)&info);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#pragma warning(pop)
    }
#  endif

#endif
}

static DWORD WINAPI thread_main(void *param)
{
    pj_thread_t *rec = param;
    DWORD result;

#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    rec->stk_start = (char*)&rec;
#endif

    if (pj_thread_local_set(thread_tls_id, rec) != PJ_SUCCESS) {
        pj_assert(!"TLS is not set (pj_init() error?)");
    }

    PJ_LOG(6,(rec->obj_name, "Thread started"));

    set_thread_display_name(rec->obj_name);

    result = (*rec->proc)(rec->arg);

    PJ_LOG(6,(rec->obj_name, "Thread quitting"));
#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    PJ_LOG(5,(rec->obj_name, "Thread stack max usage=%u by %s:%d", 
              rec->stk_max_usage, rec->caller_file, rec->caller_line));
#endif

    return (DWORD)result;
}

/*
 * pj_thread_create(...)
 */
PJ_DEF(pj_status_t) pj_thread_create( pj_pool_t *pool, 
                                      const char *thread_name,
                                      pj_thread_proc *proc, 
                                      void *arg,
                                      pj_size_t stack_size, 
                                      unsigned flags,
                                      pj_thread_t **thread_ptr)
{
    DWORD dwflags = 0;
    pj_thread_t *rec;

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    PJ_UNUSED_ARG(stack_size);
#endif

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(pool && proc && thread_ptr, PJ_EINVAL);

    /* Set flags */
    if (flags & PJ_THREAD_SUSPENDED)
        dwflags |= CREATE_SUSPENDED;

    /* Create thread record and assign name for the thread */
    rec = (struct pj_thread_t*) pj_pool_calloc(pool, 1, sizeof(pj_thread_t));
    if (!rec)
        return PJ_ENOMEM;

    /* Set name. */
    if (!thread_name)
        thread_name = "thr%p";

    if (strchr(thread_name, '%')) {
        pj_ansi_snprintf(rec->obj_name, PJ_MAX_OBJ_NAME, thread_name, rec);
    } else {
        pj_ansi_strxcpy(rec->obj_name, thread_name, PJ_MAX_OBJ_NAME);
    }

    PJ_LOG(6, (rec->obj_name, "Thread created"));

#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK!=0
    rec->stk_size = stack_size ? (pj_uint32_t)stack_size : 0xFFFFFFFFUL;
    rec->stk_max_usage = 0;
#endif

    /* Create the thread. */
    rec->proc = proc;
    rec->arg = arg;

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    rec->hthread = CreateThreadRT(NULL, 0,
                                  thread_main, rec,
                                  dwflags, NULL);
#else
    rec->hthread = CreateThread(NULL, stack_size,
                                thread_main, rec,
                                dwflags, &rec->idthread);
#endif

    if (rec->hthread == NULL)
        return PJ_RETURN_OS_ERROR(GetLastError());

    /* Success! */
    *thread_ptr = rec;
    return PJ_SUCCESS;
}

/*
 * pj_thread-get_name()
 */
PJ_DEF(const char*) pj_thread_get_name(pj_thread_t *p)
{
    pj_thread_t *rec = (pj_thread_t*)p;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(p, "");

    return rec->obj_name;
}

/*
 * pj_thread_resume()
 */
PJ_DEF(pj_status_t) pj_thread_resume(pj_thread_t *p)
{
    pj_thread_t *rec = (pj_thread_t*)p;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(p, PJ_EINVAL);

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    if (ResumeThreadRT(rec->hthread) == (DWORD)-1)
#else
    if (ResumeThread(rec->hthread) == (DWORD)-1)
#endif    
        return PJ_RETURN_OS_ERROR(GetLastError());
    else
        return PJ_SUCCESS;
}

/*
 * pj_thread_this()
 */
PJ_DEF(pj_thread_t*) pj_thread_this(void)
{
    pj_thread_t *rec = pj_thread_local_get(thread_tls_id);

    if (rec == NULL) {
        pj_assert(!"Calling pjlib from unknown/external thread. You must "
                   "register external threads with pj_thread_register() "
                   "before calling any pjlib functions.");
    }

    /*
     * MUST NOT check stack because this function is called
     * by PJ_CHECK_STACK() itself!!!
     *
     */

    return rec;
}

/*
 * pj_thread_join()
 */
PJ_DEF(pj_status_t) pj_thread_join(pj_thread_t *p)
{
    pj_thread_t *rec = (pj_thread_t *)p;
    DWORD rc;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(p, PJ_EINVAL);

    if (p == pj_thread_this())
        return PJ_ECANCELLED;

    PJ_LOG(6, (pj_thread_this()->obj_name, "Joining thread %s", p->obj_name));

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    rc = WaitForSingleObjectEx(rec->hthread, INFINITE, FALSE);
#else
    rc = WaitForSingleObject(rec->hthread, INFINITE);
#endif    

    if (rc==WAIT_OBJECT_0)
        return PJ_SUCCESS;
    else if (rc==WAIT_TIMEOUT)
        return PJ_ETIMEDOUT;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_thread_destroy()
 */
PJ_DEF(pj_status_t) pj_thread_destroy(pj_thread_t *p)
{
    pj_thread_t *rec = (pj_thread_t *)p;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(p, PJ_EINVAL);

    if (CloseHandle(rec->hthread) == TRUE)
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_thread_sleep()
 */
PJ_DEF(pj_status_t) pj_thread_sleep(unsigned msec)
{
    PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    SleepRT(msec);
#else
    Sleep(msec);
#endif

    return PJ_SUCCESS;
}

#if defined(PJ_OS_HAS_CHECK_STACK) && PJ_OS_HAS_CHECK_STACK != 0
/*
 * pj_thread_check_stack()
 * Implementation for PJ_CHECK_STACK()
 */
PJ_DEF(void) pj_thread_check_stack(const char *file, int line)
{
    char stk_ptr;
    pj_uint32_t usage;
    pj_thread_t *thread = pj_thread_this();

    pj_assert(thread);

    /* Calculate current usage. */
    usage = (&stk_ptr > thread->stk_start) ? 
                (pj_uint32_t)(&stk_ptr - thread->stk_start) :
                (pj_uint32_t)(thread->stk_start - &stk_ptr);

    /* Assert if stack usage is dangerously high. */
    pj_assert("STACK OVERFLOW!! " && (usage <= thread->stk_size - 128));

    /* Keep statistic. */
    if (usage > thread->stk_max_usage) {
        thread->stk_max_usage = usage;
        thread->caller_file = file;
        thread->caller_line = line;
    }

}

/*
 * pj_thread_get_stack_max_usage()
 */
PJ_DEF(pj_uint32_t) pj_thread_get_stack_max_usage(pj_thread_t *thread)
{
    return thread->stk_max_usage;
}

/*
 * pj_thread_get_stack_info()
 */
PJ_DEF(pj_status_t) pj_thread_get_stack_info( pj_thread_t *thread,
                                              const char **file,
                                              int *line )
{
    pj_assert(thread);

    *file = thread->caller_file;
    *line = thread->caller_line;
    return 0;
}

#endif  /* PJ_OS_HAS_CHECK_STACK */


///////////////////////////////////////////////////////////////////////////////

/*
 * pj_atomic_create()
 */
PJ_DEF(pj_status_t) pj_atomic_create( pj_pool_t *pool, 
                                      pj_atomic_value_t initial,
                                      pj_atomic_t **atomic_ptr)
{
    pj_atomic_t *atomic_var = pj_pool_alloc(pool, sizeof(pj_atomic_t));
    if (!atomic_var)
        return PJ_ENOMEM;

    atomic_var->value = initial;
    *atomic_ptr = atomic_var;

    return PJ_SUCCESS;
}

/*
 * pj_atomic_destroy()
 */
PJ_DEF(pj_status_t) pj_atomic_destroy( pj_atomic_t *var )
{
    PJ_UNUSED_ARG(var);
    PJ_ASSERT_RETURN(var, PJ_EINVAL);

    return 0;
}

/*
 * pj_atomic_set()
 */
PJ_DEF(void) pj_atomic_set( pj_atomic_t *atomic_var, pj_atomic_value_t value)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_ON_FAIL(atomic_var, return);

    InterlockedExchange(&atomic_var->value, value);
}

/*
 * pj_atomic_get()
 */
PJ_DEF(pj_atomic_value_t) pj_atomic_get(pj_atomic_t *atomic_var)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(atomic_var, 0);

    return atomic_var->value;
}

/*
 * pj_atomic_inc_and_get()
 */
PJ_DEF(pj_atomic_value_t) pj_atomic_inc_and_get(pj_atomic_t *atomic_var)
{
    PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINNT) && PJ_WIN32_WINNT >= 0x0400
    return InterlockedIncrement(&atomic_var->value);
#else
    return InterlockedIncrement(&atomic_var->value);
#endif
}

/*
 * pj_atomic_inc()
 */
PJ_DEF(void) pj_atomic_inc(pj_atomic_t *atomic_var)
{
    PJ_ASSERT_ON_FAIL(atomic_var, return);
    pj_atomic_inc_and_get(atomic_var);
}

/*
 * pj_atomic_dec_and_get()
 */
PJ_DEF(pj_atomic_value_t) pj_atomic_dec_and_get(pj_atomic_t *atomic_var)
{
    PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINNT) && PJ_WIN32_WINNT >= 0x0400
    return InterlockedDecrement(&atomic_var->value);
#else
    return InterlockedDecrement(&atomic_var->value);
#endif
}

/*
 * pj_atomic_dec()
 */
PJ_DEF(void) pj_atomic_dec(pj_atomic_t *atomic_var)
{
    PJ_ASSERT_ON_FAIL(atomic_var, return);
    pj_atomic_dec_and_get(atomic_var);
}

/*
 * pj_atomic_add()
 */
PJ_DEF(void) pj_atomic_add( pj_atomic_t *atomic_var,
                            pj_atomic_value_t value )
{
    PJ_ASSERT_ON_FAIL(atomic_var, return);
#if defined(PJ_WIN32_WINNT) && PJ_WIN32_WINNT >= 0x0400
    InterlockedExchangeAdd( &atomic_var->value, value );
#else
    InterlockedExchangeAdd( &atomic_var->value, value );
#endif
}

/*
 * pj_atomic_add_and_get()
 */
PJ_DEF(pj_atomic_value_t) pj_atomic_add_and_get( pj_atomic_t *atomic_var,
                                                 pj_atomic_value_t value)
{
#if defined(PJ_WIN32_WINNT) && PJ_WIN32_WINNT >= 0x0400
    long oldValue = InterlockedExchangeAdd( &atomic_var->value, value);
    return oldValue + value;
#else
    long oldValue = InterlockedExchangeAdd( &atomic_var->value, value);
    return oldValue + value;
#endif
}

///////////////////////////////////////////////////////////////////////////////
/*
 * pj_thread_local_alloc()
 */
PJ_DEF(pj_status_t) pj_thread_local_alloc(long *index)
{
    DWORD tls_index;
    PJ_ASSERT_RETURN(index != NULL, PJ_EINVAL);

    //Can't check stack because this function is called in the
    //beginning before main thread is initialized.
    //PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    tls_index = TlsAllocRT();
#else
    tls_index = TlsAlloc();
#endif

    if (tls_index == TLS_OUT_OF_INDEXES)
        return PJ_RETURN_OS_ERROR(GetLastError());
    else {
        *index = (long)tls_index;
        return PJ_SUCCESS;
    }
}

/*
 * pj_thread_local_free()
 */
PJ_DEF(void) pj_thread_local_free(long index)
{
    PJ_CHECK_STACK();
#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    TlsFreeRT(index);
#else
    TlsFree(index);
#endif    
}

/*
 * pj_thread_local_set()
 */
PJ_DEF(pj_status_t) pj_thread_local_set(long index, void *value)
{
    BOOL rc;

    //Can't check stack because this function is called in the
    //beginning before main thread is initialized.
    //PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    rc = TlsSetValueRT(index, value);
#else
    rc = TlsSetValue(index, value);
#endif
    
    return rc!=0 ? PJ_SUCCESS : PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_thread_local_get()
 */
PJ_DEF(void*) pj_thread_local_get(long index)
{
    //Can't check stack because this function is called
    //by PJ_CHECK_STACK() itself!!!
    //PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(index >= 0, NULL);
#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    return TlsGetValueRT(index);
#else
    return TlsGetValue(index);
#endif
}

///////////////////////////////////////////////////////////////////////////////
static pj_status_t init_mutex(pj_mutex_t *mutex, const char *name)
{

    PJ_CHECK_STACK();

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    InitializeCriticalSectionEx(&mutex->crit, 0, 0);
#elif PJ_WIN32_WINNT >= 0x0400
    InitializeCriticalSection(&mutex->crit);
#else
    mutex->hMutex = CreateMutex(NULL, FALSE, NULL);
    if (!mutex->hMutex) {
        return PJ_RETURN_OS_ERROR(GetLastError());
    }
#endif

#if PJ_DEBUG
    /* Set owner. */
    mutex->nesting_level = 0;
    mutex->owner = NULL;
#endif

    /* Set name. */
    if (!name) {
        name = "mtx%p";
    }
    if (strchr(name, '%')) {
        pj_ansi_snprintf(mutex->obj_name, PJ_MAX_OBJ_NAME, name, mutex);
    } else {
        pj_ansi_strxcpy(mutex->obj_name, name, PJ_MAX_OBJ_NAME);
    }

    PJ_LOG(6, (mutex->obj_name, "Mutex created"));
    return PJ_SUCCESS;
}

/*
 * pj_mutex_create()
 */
PJ_DEF(pj_status_t) pj_mutex_create(pj_pool_t *pool, 
                                    const char *name, 
                                    int type,
                                    pj_mutex_t **mutex_ptr)
{
    pj_status_t rc;
    pj_mutex_t *mutex;

    PJ_UNUSED_ARG(type);
    PJ_ASSERT_RETURN(pool && mutex_ptr, PJ_EINVAL);

    mutex = pj_pool_alloc(pool, sizeof(*mutex));
    if (!mutex)
        return PJ_ENOMEM;

    rc = init_mutex(mutex, name);
    if (rc != PJ_SUCCESS)
        return rc;

    *mutex_ptr = mutex;

    return PJ_SUCCESS;
}

/*
 * pj_mutex_create_simple()
 */
PJ_DEF(pj_status_t) pj_mutex_create_simple( pj_pool_t *pool, 
                                            const char *name,
                                            pj_mutex_t **mutex )
{
    return pj_mutex_create(pool, name, PJ_MUTEX_SIMPLE, mutex);
}

/*
 * pj_mutex_create_recursive()
 */
PJ_DEF(pj_status_t) pj_mutex_create_recursive( pj_pool_t *pool,
                                               const char *name,
                                               pj_mutex_t **mutex )
{
    return pj_mutex_create(pool, name, PJ_MUTEX_RECURSE, mutex);
}

/*
 * pj_mutex_lock()
 */
PJ_DEF(pj_status_t) pj_mutex_lock(pj_mutex_t *mutex)
{
    pj_status_t status;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(mutex, PJ_EINVAL);

    LOG_MUTEX((mutex->obj_name, "Mutex: thread %s is waiting", 
                                pj_thread_this()->obj_name));

#if PJ_WIN32_WINNT >= 0x0400
    EnterCriticalSection(&mutex->crit);
    status=PJ_SUCCESS;
#else
    if (WaitForSingleObject(mutex->hMutex, INFINITE)==WAIT_OBJECT_0)
        status = PJ_SUCCESS;
    else
        status = PJ_STATUS_FROM_OS(GetLastError());

#endif
    if (status == PJ_SUCCESS) {
        LOG_MUTEX((mutex->obj_name, 
                   "Mutex acquired by thread %s",
                   pj_thread_this()->obj_name));
    } else {
        LOG_MUTEX_WARN((mutex->obj_name, status,
                        "Failed to acquire mutex by thread %s",
                        pj_thread_this()->obj_name));
    }
#if PJ_DEBUG
    if (status == PJ_SUCCESS) {
        mutex->owner = pj_thread_this();
        ++mutex->nesting_level;
    }
#endif

    return status;
}

/*
 * pj_mutex_unlock()
 */
PJ_DEF(pj_status_t) pj_mutex_unlock(pj_mutex_t *mutex)
{
    pj_status_t status;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(mutex, PJ_EINVAL);

#if PJ_DEBUG
    pj_assert(mutex->owner == pj_thread_this());
    if (--mutex->nesting_level == 0) {
        mutex->owner = NULL;
    }
#endif

    LOG_MUTEX((mutex->obj_name, "Mutex released by thread %s", 
                                pj_thread_this()->obj_name));

#if PJ_WIN32_WINNT >= 0x0400
    LeaveCriticalSection(&mutex->crit);
    status=PJ_SUCCESS;
#else
    status = ReleaseMutex(mutex->hMutex) ? PJ_SUCCESS : 
                PJ_STATUS_FROM_OS(GetLastError());
#endif
    return status;
}

/*
 * pj_mutex_trylock()
 */
PJ_DEF(pj_status_t) pj_mutex_trylock(pj_mutex_t *mutex)
{
    pj_status_t status;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(mutex, PJ_EINVAL);

    LOG_MUTEX((mutex->obj_name, "Mutex: thread %s is trying", 
                                pj_thread_this()->obj_name));

#if PJ_WIN32_WINNT >= 0x0400
    status=TryEnterCriticalSection(&mutex->crit) ? PJ_SUCCESS : PJ_EUNKNOWN;
#else
    status = WaitForSingleObject(mutex->hMutex, 0)==WAIT_OBJECT_0 ? 
                PJ_SUCCESS : PJ_ETIMEDOUT;
#endif
    if (status==PJ_SUCCESS) {
        LOG_MUTEX((mutex->obj_name, "Mutex acquired by thread %s", 
                                  pj_thread_this()->obj_name));

#if PJ_DEBUG
        mutex->owner = pj_thread_this();
        ++mutex->nesting_level;
#endif
    } else {
        LOG_MUTEX((mutex->obj_name, "Mutex: thread %s's trylock() failed", 
                                    pj_thread_this()->obj_name));
    }

    return status;
}

/*
 * pj_mutex_destroy()
 */
PJ_DEF(pj_status_t) pj_mutex_destroy(pj_mutex_t *mutex)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(mutex, PJ_EINVAL);

    LOG_MUTEX((mutex->obj_name, "Mutex destroyed"));

#if PJ_WIN32_WINNT >= 0x0400
    DeleteCriticalSection(&mutex->crit);
    return PJ_SUCCESS;
#else
    return CloseHandle(mutex->hMutex) ? PJ_SUCCESS : 
            PJ_RETURN_OS_ERROR(GetLastError());
#endif
}

/*
 * pj_mutex_is_locked()
 */
PJ_DEF(pj_bool_t) pj_mutex_is_locked(pj_mutex_t *mutex)
{
#if PJ_DEBUG
    return mutex->owner == pj_thread_this();
#else
    PJ_UNUSED_ARG(mutex);
    pj_assert(!"PJ_DEBUG is not set!");
    return 1;
#endif
}

///////////////////////////////////////////////////////////////////////////////
/*
 * Win32 lacks Read/Write mutex, so include the emulation.
 */
#include "os_rwmutex.c"

///////////////////////////////////////////////////////////////////////////////
/*
 * pj_enter_critical_section()
 */
PJ_DEF(void) pj_enter_critical_section(void)
{
    pj_mutex_lock(&critical_section_mutex);
}


/*
 * pj_leave_critical_section()
 */
PJ_DEF(void) pj_leave_critical_section(void)
{
    pj_mutex_unlock(&critical_section_mutex);
}

///////////////////////////////////////////////////////////////////////////////
#if defined(PJ_HAS_SEMAPHORE) && PJ_HAS_SEMAPHORE != 0

/*
 * pj_sem_create()
 */
PJ_DEF(pj_status_t) pj_sem_create( pj_pool_t *pool, 
                                   const char *name,
                                   unsigned initial, 
                                   unsigned max,
                                   pj_sem_t **sem_ptr)
{
    pj_sem_t *sem;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(pool && sem_ptr, PJ_EINVAL);

    sem = pj_pool_alloc(pool, sizeof(*sem));    

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    /** SEMAPHORE_ALL_ACCESS **/
    sem->hSemaphore = CreateSemaphoreEx(NULL, initial, max, NULL, 0,
                                        SEMAPHORE_ALL_ACCESS);
#else
    sem->hSemaphore = CreateSemaphore(NULL, initial, max, NULL);
#endif
    
    if (!sem->hSemaphore)
        return PJ_RETURN_OS_ERROR(GetLastError());

    /* Set name. */
    if (!name) {
        name = "sem%p";
    }
    if (strchr(name, '%')) {
        pj_ansi_snprintf(sem->obj_name, PJ_MAX_OBJ_NAME, name, sem);
    } else {
        pj_ansi_strxcpy(sem->obj_name, name, PJ_MAX_OBJ_NAME);
    }

    LOG_MUTEX((sem->obj_name, "Semaphore created"));

    *sem_ptr = sem;
    return PJ_SUCCESS;
}

static pj_status_t pj_sem_wait_for(pj_sem_t *sem, unsigned timeout)
{
    DWORD result;
    pj_status_t status = PJ_SUCCESS;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(sem, PJ_EINVAL);

    LOG_MUTEX((sem->obj_name, "Semaphore: thread %s is waiting", 
                              pj_thread_this()->obj_name));
    
#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    result = WaitForSingleObjectEx(sem->hSemaphore, timeout, FALSE);
#else
    result = WaitForSingleObject(sem->hSemaphore, timeout);
#endif

    if (result == WAIT_OBJECT_0) {
        LOG_MUTEX((sem->obj_name, "Semaphore acquired by thread %s", 
                                  pj_thread_this()->obj_name));
    } else {
        if (result == WAIT_TIMEOUT)
            status = PJ_ETIMEDOUT;
        else
            status = PJ_RETURN_OS_ERROR(GetLastError());
        LOG_MUTEX_WARN((sem->obj_name, status, "Semaphore: thread %s failed "
                        "to acquire", pj_thread_this()->obj_name));
    }

    return status;
}

/*
 * pj_sem_wait()
 */
PJ_DEF(pj_status_t) pj_sem_wait(pj_sem_t *sem)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(sem, PJ_EINVAL);

    return pj_sem_wait_for(sem, INFINITE);
}

/*
 * pj_sem_trywait()
 */
PJ_DEF(pj_status_t) pj_sem_trywait(pj_sem_t *sem)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(sem, PJ_EINVAL);

    return pj_sem_wait_for(sem, 0);
}

/*
 * pj_sem_post()
 */
PJ_DEF(pj_status_t) pj_sem_post(pj_sem_t *sem)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(sem, PJ_EINVAL);

    LOG_MUTEX((sem->obj_name, "Semaphore released by thread %s",
                              pj_thread_this()->obj_name));

    if (ReleaseSemaphore(sem->hSemaphore, 1, NULL))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_sem_destroy()
 */
PJ_DEF(pj_status_t) pj_sem_destroy(pj_sem_t *sem)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(sem, PJ_EINVAL);

    LOG_MUTEX((sem->obj_name, "Semaphore destroyed by thread %s",
                              pj_thread_this()->obj_name));

    if (CloseHandle(sem->hSemaphore))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

#endif  /* PJ_HAS_SEMAPHORE */
///////////////////////////////////////////////////////////////////////////////


#if defined(PJ_HAS_EVENT_OBJ) && PJ_HAS_EVENT_OBJ != 0

/*
 * pj_event_create()
 */
PJ_DEF(pj_status_t) pj_event_create( pj_pool_t *pool, 
                                     const char *name,
                                     pj_bool_t manual_reset, 
                                     pj_bool_t initial,
                                     pj_event_t **event_ptr)
{
    pj_event_t *event;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(pool && event_ptr, PJ_EINVAL);

    event = pj_pool_alloc(pool, sizeof(*event));
    if (!event)
        return PJ_ENOMEM;

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    event->hEvent = CreateEventEx(NULL, NULL,
                                 (manual_reset? 0x1:0x0) | (initial? 0x2:0x0),
                                 EVENT_ALL_ACCESS);
#else
    event->hEvent = CreateEvent(NULL, manual_reset ? TRUE : FALSE,
                                initial ? TRUE : FALSE, NULL);
#endif

    if (!event->hEvent)
        return PJ_RETURN_OS_ERROR(GetLastError());

    /* Set name. */
    if (!name) {
        name = "evt%p";
    }
    if (strchr(name, '%')) {
        pj_ansi_snprintf(event->obj_name, PJ_MAX_OBJ_NAME, name, event);
    } else {
        pj_ansi_strxcpy(event->obj_name, name, PJ_MAX_OBJ_NAME);
    }

    PJ_LOG(6, (event->obj_name, "Event created"));

    *event_ptr = event;
    return PJ_SUCCESS;
}

static pj_status_t pj_event_wait_for(pj_event_t *event, unsigned timeout)
{
    DWORD result;

    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    PJ_LOG(6, (event->obj_name, "Event: thread %s is waiting", 
                                pj_thread_this()->obj_name));

#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    result = WaitForSingleObjectEx(event->hEvent, timeout, FALSE);
#else
    result = WaitForSingleObject(event->hEvent, timeout);
#endif

    if (result == WAIT_OBJECT_0) {
        PJ_LOG(6, (event->obj_name, "Event: thread %s is released", 
                                    pj_thread_this()->obj_name));
    } else {
        PJ_LOG(6, (event->obj_name, "Event: thread %s FAILED to acquire", 
                                    pj_thread_this()->obj_name));
    }

    if (result==WAIT_OBJECT_0)
        return PJ_SUCCESS;
    else if (result==WAIT_TIMEOUT)
        return PJ_ETIMEDOUT;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_event_wait()
 */
PJ_DEF(pj_status_t) pj_event_wait(pj_event_t *event)
{
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    return pj_event_wait_for(event, INFINITE);
}

/*
 * pj_event_trywait()
 */
PJ_DEF(pj_status_t) pj_event_trywait(pj_event_t *event)
{
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    return pj_event_wait_for(event, 0);
}

/*
 * pj_event_set()
 */
PJ_DEF(pj_status_t) pj_event_set(pj_event_t *event)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    PJ_LOG(6, (event->obj_name, "Setting event"));

    if (SetEvent(event->hEvent))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_event_pulse()
 */
PJ_DEF(pj_status_t) pj_event_pulse(pj_event_t *event)
{
#if defined(PJ_WIN32_WINPHONE8) && PJ_WIN32_WINPHONE8
    PJ_UNUSED_ARG(event);
    pj_assert(!"pj_event_pulse() not supported!");
    return PJ_ENOTSUP;
#else
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    PJ_LOG(6, (event->obj_name, "Pulsing event"));

    if (PulseEvent(event->hEvent))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
#endif
}

/*
 * pj_event_reset()
 */
PJ_DEF(pj_status_t) pj_event_reset(pj_event_t *event)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    PJ_LOG(6, (event->obj_name, "Event is reset"));

    if (ResetEvent(event->hEvent))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_event_destroy()
 */
PJ_DEF(pj_status_t) pj_event_destroy(pj_event_t *event)
{
    PJ_CHECK_STACK();
    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    PJ_LOG(6, (event->obj_name, "Event is destroying"));

    if (CloseHandle(event->hEvent))
        return PJ_SUCCESS;
    else
        return PJ_RETURN_OS_ERROR(GetLastError());
}

#endif  /* PJ_HAS_EVENT_OBJ */

///////////////////////////////////////////////////////////////////////////////

/*
 * pj_barrier_create()
 */
PJ_DEF(pj_status_t) pj_barrier_create(pj_pool_t *pool, unsigned trip_count, pj_barrier_t **p_barrier) 
{
    pj_barrier_t *barrier;
    PJ_ASSERT_RETURN(pool && p_barrier, PJ_EINVAL);
    barrier = (pj_barrier_t *)pj_pool_zalloc(pool, sizeof(pj_barrier_t));
    if (barrier == NULL)
        return PJ_ENOMEM;
#if PJ_WIN32_WINNT >= _WIN32_WINNT_WIN8
    if (InitializeSynchronizationBarrier(&barrier->sync_barrier, trip_count, -1)) {
        *p_barrier = barrier;
        return PJ_SUCCESS;
    } else
        return pj_get_os_error();
#elif PJ_WIN32_WINNT >= _WIN32_WINNT_VISTA
    InitializeCriticalSection(&barrier->mutex);
    InitializeConditionVariable(&barrier->cond);
    barrier->count = trip_count;
    barrier->waiting = 0;
    *p_barrier = barrier;
    return PJ_SUCCESS;
#else
    barrier->cond = CreateSemaphore(NULL,
                                    0,          /* initial count */
                                    trip_count, /* max count */
                                    NULL);
    if (!barrier->cond)
        return pj_get_os_error();
    barrier->count = trip_count;
    barrier->waiting = 0;
    *p_barrier = barrier;
    return PJ_SUCCESS;
#endif
}

/*
 * pj_barrier_destroy()
 */
PJ_DEF(pj_status_t) pj_barrier_destroy(pj_barrier_t *barrier) 
{
    PJ_ASSERT_RETURN(barrier, PJ_EINVAL);
#if PJ_WIN32_WINNT >= _WIN32_WINNT_WIN8
    DeleteSynchronizationBarrier(&barrier->sync_barrier);
    return PJ_SUCCESS;
#elif PJ_WIN32_WINNT >= _WIN32_WINNT_VISTA
    DeleteCriticalSection(&barrier->mutex);
    return PJ_SUCCESS;
#else
    if (CloseHandle(barrier->cond))
        return PJ_SUCCESS;
    else
        return pj_get_os_error();
#endif
}

/*
 * pj_barrier_wait()
 */
PJ_DEF(pj_int32_t) pj_barrier_wait(pj_barrier_t *barrier, pj_uint32_t flags) 
{
    PJ_ASSERT_RETURN(barrier, PJ_EINVAL);
#if PJ_WIN32_WINNT >= _WIN32_WINNT_WIN8
    DWORD dwFlags = ((flags & PJ_BARRIER_FLAGS_BLOCK_ONLY) ? SYNCHRONIZATION_BARRIER_FLAGS_BLOCK_ONLY : 0) |
        ((flags & PJ_BARRIER_FLAGS_SPIN_ONLY) ? SYNCHRONIZATION_BARRIER_FLAGS_SPIN_ONLY : 0) |
        ((flags & PJ_BARRIER_FLAGS_NO_DELETE) ? SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE : 0);
    return EnterSynchronizationBarrier(&barrier->sync_barrier, dwFlags);
#elif PJ_WIN32_WINNT >= _WIN32_WINNT_VISTA
    PJ_UNUSED_ARG(flags);
    EnterCriticalSection(&barrier->mutex);
    if (++barrier->waiting == barrier->count) {
        barrier->waiting = 0;
        LeaveCriticalSection(&barrier->mutex);
        WakeAllConditionVariable(&barrier->cond);
        return PJ_TRUE;
    } else {
        BOOL rc = SleepConditionVariableCS(&barrier->cond, &barrier->mutex, INFINITE);
        LeaveCriticalSection(&barrier->mutex);
        if (rc)
            /* Returning PJ_FALSE in this case (a successful return from 
             * SleepConditionVariableCS) is intentional.
             */
            return PJ_FALSE;
        else
            return pj_get_os_error();
    }
#else
    PJ_UNUSED_ARG(flags);

    if (InterlockedIncrement(&barrier->waiting) == barrier->count) {
        LONG previousCount = 0;
        barrier->waiting = 0;
        /* Release all threads waiting on the semaphore */
        if (barrier->count == 1 || 
            ReleaseSemaphore(barrier->cond, barrier->count-1, &previousCount))
        {
            PJ_ASSERT_RETURN(previousCount == 0, PJ_EBUG);
            return PJ_TRUE;
        } else {
            return pj_get_os_error();
        }
    }

    if (WaitForSingleObject(barrier->cond, INFINITE) == WAIT_OBJECT_0)
        return PJ_FALSE;
    else
        return pj_get_os_error();
#endif
}

///////////////////////////////////////////////////////////////////////////////
#if defined(PJ_TERM_HAS_COLOR) && PJ_TERM_HAS_COLOR != 0
/*
 * Terminal color
 */

static WORD pj_color_to_os_attr(pj_color_t color)
{
    WORD attr = 0;

    if (color & PJ_TERM_COLOR_R)
        attr |= FOREGROUND_RED;
    if (color & PJ_TERM_COLOR_G)
        attr |= FOREGROUND_GREEN;
    if (color & PJ_TERM_COLOR_B)
        attr |= FOREGROUND_BLUE;
    if (color & PJ_TERM_COLOR_BRIGHT)
        attr |= FOREGROUND_INTENSITY;

    return attr;
}

static pj_color_t os_attr_to_pj_color(WORD attr)
{
    int color = 0;

    if (attr & FOREGROUND_RED)
        color |= PJ_TERM_COLOR_R;
    if (attr & FOREGROUND_GREEN)
        color |= PJ_TERM_COLOR_G;
    if (attr & FOREGROUND_BLUE)
        color |= PJ_TERM_COLOR_B;
    if (attr & FOREGROUND_INTENSITY)
        color |= PJ_TERM_COLOR_BRIGHT;

    return color;
}


/*
 * pj_term_set_color()
 */
PJ_DEF(pj_status_t) pj_term_set_color(pj_color_t color)
{
    BOOL rc;
    WORD attr = 0;

    PJ_CHECK_STACK();

    attr = pj_color_to_os_attr(color);
    rc = SetConsoleTextAttribute( GetStdHandle(STD_OUTPUT_HANDLE), attr);
    return rc ? PJ_SUCCESS : PJ_RETURN_OS_ERROR(GetLastError());
}

/*
 * pj_term_get_color()
 * Get current terminal foreground color.
 */
PJ_DEF(pj_color_t) pj_term_get_color(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    PJ_CHECK_STACK();

    GetConsoleScreenBufferInfo( GetStdHandle(STD_OUTPUT_HANDLE), &info);
    return os_attr_to_pj_color(info.wAttributes);
}

#endif  /* PJ_TERM_HAS_COLOR */

/*
 * pj_run_app()
 */
PJ_DEF(int) pj_run_app(pj_main_func_ptr main_func, int argc, char *argv[],
                       unsigned flags)
{
    PJ_UNUSED_ARG(flags);
    return (*main_func)(argc, argv);
}

/*
 * Set file descriptor close-on-exec flag
 */
PJ_DEF(pj_status_t) pj_set_cloexec_flag(int fd)
{
    PJ_UNUSED_ARG(fd);
    return PJ_ENOTSUP;
}
