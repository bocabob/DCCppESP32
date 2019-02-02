/** \copyright
 * Copyright (c) 2012, Stuart W Baker
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file os.c
 * This file represents a C language abstraction of common operating
 * system calls.
 *
 * @author Stuart W. Baker
 * @date 13 August 2012
 */

#define _DEFAULT_SOURCE

/// Forces one definition of each inline function to be compiled.
#define OS_INLINE extern

#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#if !defined (GCC_MEGA_AVR)
#include <unistd.h>
#endif // !GCC_MEGA_AVR

#if defined (__FreeRTOS__)
#include "devtab.h"
#include "FreeRTOS.h"
#include "task.h"

#elif defined(__WIN32__)

#include <winsock2.h>
#include <ws2tcpip.h> /* socklen_t */
#include <time.h>
#include <signal.h>

#elif defined(ESP_NONOS)

#include <sys/select.h>
#include <sched.h>
#include <signal.h>
#include <user_interface.h>

#else

#include <sys/select.h>
#include <sched.h>
#include <time.h>
#include <signal.h>

#endif // switch by OS

#include "nmranet_config.h"

#include "utils/macros.h"
#include "os/os.h"

/** default stdin */
extern const char *STDIN_DEVICE;

/** default stdout */
extern const char *STDOUT_DEVICE;

/** default stderr */
extern const char *STDERR_DEVICE;

/** Captures point of death (line). */
int g_death_lineno;
/** Captures point of death (file). */
const char* g_death_file;

/** clock management **/
long long rtcOffset = 0;

/** This magic value is written to a task's taskList entry in order to signal
 * the idle task to pick it out of the taskList structure. */
#define DELETED_TASK_MAGIC 0xb5c5d5e5

/* This section of code is required because CodeSourcery's mips-gcc
 * distribution contains a strangely compiled NewLib (in the unhosted-libc.a
 * version) that does not forward these function calls to the implementations
 * we have. We are thus forced to override their weak definition of these
 * functions. */
#if defined(TARGET_PIC32MX) || defined(ESP_NONOS)
#include "reent.h"

#ifndef _READ_WRITE_RETURN_TYPE
#define _READ_WRITE_RETURN_TYPE ssize_t
#endif

int open(const char* b, int flags, ...) {
    return _open_r(_impure_ptr, b, flags, 0);
}
int close(int fd) {
    return _close_r(_impure_ptr, fd);
}
_READ_WRITE_RETURN_TYPE read(int fd, void* buf, size_t count) {
    return _read_r(_impure_ptr, fd, buf, count);
}
_READ_WRITE_RETURN_TYPE write(int fd, const void* buf, size_t count) {
    return _write_r(_impure_ptr, fd, buf, count);
}
off_t lseek(int fd, off_t offset, int whence) {
    return _lseek_r(_impure_ptr, fd, offset, whence);
}
int fstat(int fd, struct stat* buf) {
    return _fstat_r(_impure_ptr, fd, buf);
}

#endif


#if defined (__FreeRTOS__)
/** Task list entriy */
typedef struct task_list
{
    xTaskHandle task; /**< list entry data */
    char * name; /**< name of task */
    size_t unused; /**< number of bytes left unused in the stack */
    struct task_list *next; /**< next link in the list */
} TaskList;

/** List of all the tasks in the system */
static TaskList *taskList = NULL;

/** Mutex for os_thread_once. */
static os_mutex_t onceMutex = OS_MUTEX_INITIALIZER;

/** Default hardware initializer.  This function is defined weak so that
 * a given board can stub in an intiailization specific to it.
 */
void hw_init(void) __attribute__ ((weak));
void hw_init(void)
{
}

/** Default hardware post-initializer.  This function is called from the main
 * task, after the scheduler is started, but before appl_main is invoked. This
 * function is defined weak so that a given board can stub in an intiailization
 * specific to it.
 */
void hw_postinit(void) __attribute__ ((weak));
void hw_postinit(void)
{
}

__attribute__ ((weak))
struct _reent* allocate_reent(void)
{
    struct _reent* data = malloc(sizeof(struct _reent));
    _REENT_INIT_PTR(data);
    return data;
}

/** One time intialization routine
 * @param once one time instance
 * @param routine method to call once
 * @return 0 upon success
 */
int os_thread_once(os_thread_once_t *once, void (*routine)(void))
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        /* The scheduler has started so we should use locking */
        os_mutex_lock(&onceMutex);
        if (once->state == OS_THREAD_ONCE_NEVER)
        {
            once->state = OS_THREAD_ONCE_INPROGRESS;
            os_mutex_unlock(&onceMutex);
            routine();
            os_mutex_lock(&onceMutex);
            once->state = OS_THREAD_ONCE_DONE;
        }

        while (once->state == OS_THREAD_ONCE_INPROGRESS)
        {
            /* avoid dead lock waiting for PTHREAD_ONCE_DONE state */
            os_mutex_unlock(&onceMutex);
            usleep(MSEC_TO_USEC(10));
            os_mutex_lock(&onceMutex);
        }
        os_mutex_unlock(&onceMutex);
    }
    else
    {
        /* this is for static constructures before the scheduler is started */
        if (once->state == OS_THREAD_ONCE_NEVER)
        {
            once->state = OS_THREAD_ONCE_INPROGRESS;
            routine();
            once->state = OS_THREAD_ONCE_DONE;
        }
    }

    return 0;
}
#elif defined (__WIN32__)
/** Windows does not support pipes, so we made our own with a pseudo socketpair.
 * @param fildes fildes[0] is open for reading, filedes[1] is open for writing
 * @return 0 upon success, else -1 with errno set to indicate error
 */
int pipe(int fildes[2])
{
    struct sockaddr_in addr;  
    int listener, connector, acceptor;
    socklen_t addrlen = sizeof(addr);

    if ((listener = socket(AF_INET, SOCK_STREAM, 0)) <= 0)
    {
        errno = EMFILE;
        return -1;
    }
    if ((connector = socket(AF_INET, SOCK_STREAM, 0)) <= 0)
    {
        closesocket(listener);
        errno = EMFILE;
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; 

    int reuse = 0;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, 
                   (char*)&reuse, (socklen_t)sizeof(reuse)) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return -1;
    }

    if (bind(listener, (const struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return -1;
    }
    
    if  (getsockname(listener, (struct sockaddr*)&addr, &addrlen) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return -1;
    }

    if (listen(listener, 1) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return -1;
    }

    if (connect(connector, (const struct sockaddr*)&addr, addrlen) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return -1;
    }
   
    if ((acceptor = accept(listener, NULL, NULL)) < 0)
    {
        closesocket(listener);
        closesocket(connector);
        errno = EMFILE;
        return  -1;
    }

    int flag = 1;
    setsockopt(connector, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
    setsockopt(acceptor, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    fildes[0] = connector;
    fildes[1] = acceptor;
    closesocket(listener);
    return 0;
}
#endif

#if defined (__FreeRTOS__)
extern const void* stack_malloc(unsigned long length);

#endif  // FreeRTOS

/** Entry point to a thread.
 * @param arg metadata for entering the thread
 */
#if defined (__FreeRTOS__)
static void os_thread_start(void *arg)
{
    ThreadPriv *priv = arg;
    vTaskSetApplicationTaskTag(NULL, arg);
    _impure_ptr = priv->reent;
    (*priv->entry)(priv->arg);

    vTaskSuspendAll();
    TaskList *tl;
    for (tl = taskList; tl != NULL && tl->task != xTaskGetCurrentTaskHandle();
         tl = tl->next)
    {
    }
    if (tl)
    {
        tl->task = NULL;
        tl->unused = DELETED_TASK_MAGIC;
    }
    xTaskResumeAll();

    free(priv->reent);
    free(priv);
    vTaskDelete(NULL);
}
#endif

#if !(defined(__EMSCRIPTEN__) || defined(ESP_NONOS) || defined(ARDUINO))

#if defined(__FreeRTOS__)
#if (configSUPPORT_STATIC_ALLOCATION == 1)
/** Static memory allocators for idle system thread.
 * @param pxIdleTaskTCBBuffer pointer to pointer to TCB
 * @param pxIdelTaskStackBuffer pointer to pointer to Stack
 * @param ulIdleTaskStackSize pointer to stack size
 */

void vApplicationGetIdleTaskMemory(StaticTask_t **pxIdleTaskTCBBuffer,
                                   StackType_t **pxIdleTaskStackBuffer,
                                   uint32_t *ulIdleTaskStackSize);

void vApplicationGetIdleTaskMemory(StaticTask_t **pxIdleTaskTCBBuffer,
                                   StackType_t **pxIdleTaskStackBuffer,
                                   uint32_t *ulIdleTaskStackSize)
{
    const uint32_t stksz = configMINIMAL_STACK_SIZE*sizeof(StackType_t);
    *pxIdleTaskTCBBuffer = (StaticTask_t *) malloc(sizeof(StaticTask_t));
    HASSERT(*pxIdleTaskTCBBuffer);
    *pxIdleTaskStackBuffer = (StackType_t *) malloc(stksz);
    HASSERT(*pxIdleTaskStackBuffer);
    *ulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/** Static memory allocators for timer system thread.
 * @param pxTimerTaskTCBBuffer pointer to pointer to TCB
 * @param pxIdelTaskStackBuffer pointer to pointer to Stack
 * @param ulTimerTaskStackSize pointer to stack size
 */

void vApplicationGetTimeraskMemory(StaticTask_t **pxTimerTaskTCBBuffer,
                                   StackType_t **pxTimerTaskStackBuffer,
                                   uint32_t *ulTimerTaskStackSize);

void vApplicationGetTimerTaskMemory(StaticTask_t **pxTimerTaskTCBBuffer,
                                   StackType_t **pxTimerTaskStackBuffer,
                                   uint32_t *ulTimerTaskStackSize)
{
    const uint32_t stksz = configMINIMAL_STACK_SIZE*sizeof(StackType_t);
    *pxTimerTaskTCBBuffer = (StaticTask_t *) malloc(sizeof(StaticTask_t));
    HASSERT(*pxTimerTaskTCBBuffer);
    *pxTimerTaskStackBuffer = (StackType_t *) malloc(stksz);
    HASSERT(*pxTimerTaskStackBuffer);
    *ulTimerTaskStackSize = configMINIMAL_STACK_SIZE;
}
#endif // configSUPPORT_STATIC_ALLOCATION

/** Add a thread to the task list for tracking.
 * @param task_new metadata for new task
 */
static void add_thread_to_task_list(TaskList *task_new)
{
    vTaskSuspendAll();
    task_new->next = taskList;
    taskList = task_new;
    xTaskResumeAll();
}
#endif // FreeRTOS

/** Create a thread.
 * @param thread handle to the created thread
 * @param name name of thread, NULL for an auto generated name
 * @param priority priority of created thread, 0 means default,
 *        lower numbers means lower priority, higher numbers mean higher priority
 * @param stack_size size in bytes of the created thread's stack
 * @param start_routine entry point of the thread
 * @param arg entry parameter to the thread
 * @return 0 upon success or error number upon failure
 */
int os_thread_create(os_thread_t *thread, const char *name, int priority,
                     size_t stack_size,
                     void *(*start_routine) (void *), void *arg)
{
    static unsigned int count = 0;
    char auto_name[10];

    if (name == NULL)
    {
        strcpy(auto_name, "thread.");
        auto_name[9] = '\0';
        auto_name[8] = '0' + (count % 10);
        auto_name[7] = '0' + (count / 10);
        count++;
        name = auto_name;
    }

#if defined (__FreeRTOS__)
    ThreadPriv *priv = malloc(sizeof(ThreadPriv));
    
    priv->entry = start_routine;
    priv->selectEventBit = 0;
    priv->arg = arg;
    priv->reent = allocate_reent();
    
    if (priority == 0)
    {
        priority = configMAX_PRIORITIES / 2;
    }
    else if (priority >= configMAX_PRIORITIES)
    {
        priority = configMAX_PRIORITIES - 1;
    }
    
    if (stack_size == 0)
    {
        stack_size = 2048;
    }

    TaskList *task_new = malloc(sizeof(TaskList));
    task_new->unused = stack_size;
    
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (thread)
    {
        *thread = xTaskCreateStatic(os_thread_start,
                                    (const char *const)name,
                                    stack_size/sizeof(portSTACK_TYPE),
                                    priv,
                                    priority,
                                    (StackType_t *)stack_malloc(stack_size),
                                    (StaticTask_t *) malloc(sizeof(StaticTask_t)));
        task_new->task = *thread;
        task_new->name = (char*)pcTaskGetTaskName(*thread);
    }
    else
    {
        xTaskHandle task_handle;
        task_handle = xTaskCreateStatic(os_thread_start,
                                        (const char *const)name,
                                        stack_size/sizeof(portSTACK_TYPE),
                                        priv,
                                        priority,
                                        (StackType_t *) stack_malloc(stack_size),
                                        (StaticTask_t *) malloc(sizeof(StaticTask_t)));
        task_new->task = task_handle;
        task_new->name = (char*)pcTaskGetTaskName(task_handle);
    }
#elif (configSUPPORT_DYNAMIC_ALLOCATION == 1)
    if (thread)
    {
        xTaskCreate(os_thread_start,
                    (const char *const)name,
                    stack_size/sizeof(portSTACK_TYPE),
                    priv,
                    priority,
                    thread);
        task_new->task = *thread;
        task_new->name = (char*)pcTaskGetTaskName(*thread);
    }
    else
    {
        xTaskHandle task_handle;
        xTaskCreate(os_thread_start,
                    (const char *const)name,
                    stack_size/sizeof(portSTACK_TYPE),
                    priv,
                    priority,
                    &task_handle);
        task_new->task = task_handle;
        task_new->name = (char*)pcTaskGetTaskName(task_handle);
    }
#else  // prior to v9.0.0
    if (thread)
    {
        xTaskGenericCreate(os_thread_start,
                           (const char *const)name,
                           stack_size/sizeof(portSTACK_TYPE),
                           priv,
                           priority,
                           (xTaskHandle*)thread,
                           (long unsigned int*)stack_malloc(stack_size),
                           NULL);
        task_new->task = *thread;
        task_new->name = (char*)pcTaskGetTaskName(*thread);
    }
    else
    {
        xTaskHandle task_handle;
        xTaskGenericCreate(os_thread_start,
                           (const char *const)name,
                           stack_size/sizeof(portSTACK_TYPE),
                           priv,
                           priority,
                           (xTaskHandle*)&task_handle,
                           (long unsigned int*) stack_malloc(stack_size),
                           NULL);
        task_new->task = task_handle;
        task_new->name = (char*)pcTaskGetTaskName(task_handle);
    }
#endif
    add_thread_to_task_list(task_new);

    return 0;
#else // not freertos
    pthread_attr_t attr;

    int result = pthread_attr_init(&attr);
    if (result != 0)
    {
        return result;
    }
    result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (result != 0)
    {
        return result;
    }

#if !defined(__linux__) && !defined(__MACH__) /* Linux allocates stack as needed */
    struct sched_param sched_param;
    result = pthread_attr_setstacksize(&attr, stack_size);
    if (result != 0)
    {
        return result;
    }

    sched_param.sched_priority = 0; /* default priority */
    result = pthread_attr_setschedparam(&attr, &sched_param);
    if (result != 0)
    {
        return result;
    }

    result = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (result != 0)
    {
        return result;
    }

    result = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    if (result != 0)
    {
        return result;
    }
#endif // not linux and not mac
    result = pthread_create(thread, &attr, start_routine, arg);

#if !defined (__MINGW32__) && !defined (__MACH__)
    if (!result) pthread_setname_np(*thread, name);
#endif

    return result;
#endif // freertos or not
}
#endif // __EMSCRIPTEN__

/// Implement this function to read timing more accurately than 1 msec in
/// FreeRTOS.
extern long long hw_get_partial_tick_time_nsec(void);
/// Default implementation does not provide more accuracy.
long long __attribute__((weak)) hw_get_partial_tick_time_nsec() { return 0; }

long long os_get_time_monotonic(void)
{
    static long long last = 0;
    long long time;
#if defined (__FreeRTOS__)
    portTickType tick = xTaskGetTickCount();
    time = ((long long)tick) << NSEC_TO_TICK_SHIFT;
    time += hw_get_partial_tick_time_nsec();
#elif defined (__MACH__)
    /* get the timebase info */
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    
    /* get the timestamp */
    time = (long long)mach_absolute_time();
    
    /* convert to nanoseconds */
    time *= info.numer;
    time /= info.denom;
#elif defined (__WIN32__)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time = ((long long)tv.tv_sec * 1000LL * 1000LL * 1000LL) +
           ((long long)tv.tv_usec * 1000LL);
#elif defined(ARDUINO)
    static uint32_t last_millis = 0;
    auto new_millis = millis();
    static uint32_t overflow_millis = 0;
    if (new_millis < last_millis)
    {
        ++overflow_millis;
    }
    time = overflow_millis;
    time <<= 32;
    time += new_millis;
    time *= 1000000;
#elif defined(ESP_NONOS)
    static uint32_t clockmul = 0;
    if (clockmul == 0) {
        clockmul = system_rtc_clock_cali_proc();
        clockmul *= 1000;
        clockmul >>= 10;
    }
    time = system_get_rtc_time();
    time *= clockmul;
    time >>= 2;
#else
    struct timespec ts;
#if defined (__nuttx__)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    time = ((long long)ts.tv_sec * 1000000000LL) + ts.tv_nsec;
    
#endif
    /* This logic ensures that every successive call is one value larger
     * than the last.  Each call returns a unique value.
     */
    if (time <= last)
    {
        last++;
    }
    else
    {
        last = time;
    }

    return last;
}

#if defined(__EMSCRIPTEN__)
int os_thread_once(os_thread_once_t *once, void (*routine)(void))
{
    if (once->state == OS_THREAD_ONCE_NEVER)
    {
        once->state = OS_THREAD_ONCE_INPROGRESS;
        routine();
        once->state = OS_THREAD_ONCE_DONE;
    }
    else if (once->state == OS_THREAD_ONCE_INPROGRESS)
    {
        DIE("Recursive call to os_thread_once.");
    }
    return 0;
}
#endif

#if defined (__FreeRTOS__)
/* standard C library hooks for multi-threading */

/** Lock access to malloc.
 */
void __malloc_lock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        vTaskSuspendAll();
    }
}

/** Unlock access to malloc.
 */
void __malloc_unlock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        xTaskResumeAll();
    }
}

#if defined (_REENT_SMALL)
void *__real__malloc_r(size_t size);
void __real__free_r(void *address);

/** malloc() wrapper for newlib-nano
 * @param size size of malloc in bytes
 * @return pointer to newly malloc'd space
 */
void *__wrap__malloc_r(size_t size)
{
    void *result;
    __malloc_lock();
    result = __real__malloc_r(size);
    __malloc_unlock();
    return result;
}

/** free() wrapper for newlib-nano
 * @param address pointer to previously malloc'd address
 */
void __wrap__free_r(void *address)
{
    __malloc_lock();
    __real__free_r(address);
    __malloc_unlock();
}
#endif

/** Implementation of standard sleep().
 * @param seconds number of seconds to sleep
 */
unsigned sleep(unsigned seconds)
{
    vTaskDelay(seconds * configTICK_RATE_HZ);
    return 0;
}

/** Implementation of standard usleep().
 * @param usec number of microseconds to sleep
 */
int usleep(useconds_t usec)
{
    long long nsec = usec;
    nsec *= 1000;
    vTaskDelay(nsec >> NSEC_TO_TICK_SHIFT);
    return 0;
}

void abort(void)
{
#if defined(TARGET_LPC2368) || defined(TARGET_LPC11Cxx) || defined(TARGET_LPC1768) || defined(GCC_ARMCM3) || defined (GCC_ARMCM0) || defined(TARGET_PIC32MX)
    diewith(BLINK_DIE_ABORT);
#endif
    for (;;)
    {
    }
}

/* magic that allows for an optional second heap region */
char __attribute__((weak)) __heap2_start_alias;
extern char __heap2_start __attribute__((weak, alias ("__heap2_start_alias")));
extern char __heap2_end __attribute__((weak, alias ("__heap2_start_alias")));

extern char *heap_end;
char *heap_end = 0;
extern char *heap2_end;
char *heap2_end = 0;
void* _sbrk_r(struct _reent *reent, ptrdiff_t incr)
{
    /** @todo (Stuart Baker) change naming to remove "cs3" convention */
    extern char __cs3_heap_start;
    extern char __cs3_heap_end; /* Defined by the linker */
    char *prev_heap_end;
    if (heap_end == 0)
    {
        heap_end = &__cs3_heap_start;
    }
    if (heap2_end == 0)
    {
        heap2_end = &__heap2_start;
    }
    prev_heap_end = heap_end;
    if ((heap_end + incr) > &__cs3_heap_end)
    {
        if (&__heap2_start != &__heap2_end)
        {
            /* there is a second heap */
            char *prev_heap2_end;
            prev_heap2_end = heap2_end;
            if ((heap2_end + incr) <= &__heap2_end)
            {
                heap2_end += incr;
                return (caddr_t) prev_heap2_end;
            }
        }
        /* Heap and stack collistion */
        diewith(BLINK_DIE_OUTOFMEM);
        return 0;
    }
    heap_end += incr;
    return (caddr_t) prev_heap_end;
}

xTaskHandle volatile overflowed_task = 0;
signed portCHAR * volatile overflowed_task_name = 0;

/** This method is called if a stack overflows its boundries.
 * @param task task handle for violating task
 * @param name name of violating task
 */
void vApplicationStackOverflowHook(xTaskHandle task, signed portCHAR *name)
{
    overflowed_task = task;
    overflowed_task_name = name;
    diewith(BLINK_DIE_STACKOVERFLOW);
}

/** This method will be called repeatedly from the idle task. If needed, it can
 * be overridden in hw_init.c.
 */
void hw_idle_hook(void) __attribute__((weak));

void hw_idle_hook(void)
{
}

/** Here we will monitor the other tasks.
 */
void vApplicationIdleHook( void )
{
    hw_idle_hook();
    vTaskSuspendAll();
    // First we clean up all deleted tasks.
    for (TaskList **ptl = &taskList; *ptl != NULL;)
    {
        if ((*ptl)->unused == DELETED_TASK_MAGIC)
        {
            TaskList *tl = *ptl;
            *ptl = tl->next;
            free(tl);
        }
        else
        {
            ptl = &((*ptl)->next);
        }
    }
    // Then we scan through the tasks and update the free stack values.
    for (TaskList *tl = taskList; tl != NULL; tl = tl->next)
    {
        if (tl->task)
        {
            tl->unused = uxTaskGetStackHighWaterMark(tl->task) * sizeof(portSTACK_TYPE);
        }
        xTaskResumeAll();
        vTaskSuspendAll();
    }
    xTaskResumeAll();
}

#ifdef TARGET_PIC32MX
static void __attribute__((nomips16)) os_yield_trampoline(void) {
    taskYIELD();
}

void __attribute__((nomips16)) os_isr_exit_yield_test(int woken) {
   portEND_SWITCHING_ISR(woken); 
}

#else
static inline void __attribute__((always_inline)) os_yield_trampoline(void) {
    taskYIELD();
}
#endif

/** Entry point to the main thread.
 * @param arg unused argument
 * @return NULL;
 */
void main_thread(void *arg)
{
    ThreadPriv *priv = arg;
    char *argv[2] = {"nmranet", NULL};
    vTaskSetApplicationTaskTag(NULL, arg);
    _impure_ptr = priv->reent;

    /* setup the monitoring entries for the timer and idle tasks */
#if configUSE_TIMERS
    TaskList *task_new = malloc(sizeof(TaskList) * 2);
    task_new->task = xTimerGetTimerDaemonTaskHandle();
    task_new->name = (char*)pcTaskGetTaskName(task_new->task);
    task_new->unused = uxTaskGetStackHighWaterMark(task_new->task);
    add_thread_to_task_list(task_new);
    ++task_new;
#else
    TaskList *task_new = malloc(sizeof(TaskList));
#endif
    task_new->task = xTaskGetIdleTaskHandle();
    task_new->name = (char*)pcTaskGetTaskName(task_new->task);
    task_new->unused = uxTaskGetStackHighWaterMark(task_new->task);
    add_thread_to_task_list(task_new);

    /* Allow any library threads to run that must run ahead of main */
    os_yield_trampoline();

    /* Give another chance to the board file to do work, this time coordinating
     * between application and library threads. */
    hw_postinit();

    appl_main(1, argv);
    // If the main thread returns, FreeRTOS usually crashes the CPU in a
    // hard-to-debug state. Let's avoid that.
    abort();
}
#endif

/** This function does nothing. It can be used to alias other symbols to it via
 * linker flags, such as atexit(). @return 0. */
int ignore_fn(void)
{
    return 0;
}

#if !defined(ESP32)

#if !defined (__MINGW32__)
int main(int argc, char *argv[]) __attribute__ ((weak));
#endif

/** Entry point to program.
 * @param argc number of command line arguments
 * @param argv array of command line aguments
 * @return 0, should never return
 */
int main(int argc, char *argv[])
{
#if defined (__FreeRTOS__)
    /* initialize the processor hardware */
    hw_init();

    ThreadPriv *priv = malloc(sizeof(ThreadPriv));
    xTaskHandle task_handle;
    int priority;
    priv->reent = _impure_ptr;
    priv->selectEventBit = 0;
    priv->entry = NULL;
    priv->arg = NULL;
    
    if (config_main_thread_priority() == 0xdefa01)
    {
        priority = configMAX_PRIORITIES / 2;
    }
    else
    {
        priority = config_main_thread_priority();
    }
    
#ifndef TARGET_LPC11Cxx
    /* stdin */
    if (open(STDIN_DEVICE, O_RDWR) < 0)
    {
        open("/dev/null", O_RDWR);
    }
    /* stdout */
    if (open(STDOUT_DEVICE, O_RDWR) < 0)
    {
        open("/dev/null", O_RDWR);
    }
    /* stderr */
    if (open(STDERR_DEVICE, O_WRONLY) < 0)
    {
        open("/dev/null", O_WRONLY);
    }
#endif

    /* start the main thread */
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    task_handle = xTaskCreateStatic(main_thread, (char *)"thread.main",
                                    config_main_thread_stack_size() / sizeof(portSTACK_TYPE), priv,
                                    priority,
                                    (StackType_t *)stack_malloc(config_main_thread_stack_size()),
                                    (StaticTask_t *) malloc(sizeof(StaticTask_t)));
#elif (configSUPPORT_DYNAMIC_ALLOCATION == 1)
    xTaskCreate(main_thread, (char *)"thread.main",
                config_main_thread_stack_size() / sizeof(portSTACK_TYPE), priv,
                priority,
                &task_handle);
#else
    xTaskGenericCreate(main_thread, (char *)"thread.main",
                       config_main_thread_stack_size() / sizeof(portSTACK_TYPE), priv,
                       priority, &task_handle,
                       (long unsigned int *)stack_malloc(config_main_thread_stack_size()),
                       NULL);
#endif
    TaskList *task_new = malloc(sizeof(TaskList));
    task_new->task = task_handle;
    task_new->unused = config_main_thread_stack_size();
    task_new->name = "thread.main";
    add_thread_to_task_list(task_new);

    vTaskStartScheduler();
#else
#if defined (__WIN32__)
    /* enable Windows networking */
    WSADATA wsa_data;
    WSAStartup(WINSOCK_VERSION, &wsa_data);
#endif
    return appl_main(argc, argv);
#endif
}

#endif // ESP32

#if defined(ARDUINO)
unsigned critical_nesting;
#endif

#if 0 && defined(ESP_NONOS)
struct _reent *_impure_ptr = NULL;
static int my_errno;
int* __errno(void) {
    return &my_errno;
}
#endif