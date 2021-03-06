/*
 *  Copyright (C) RedHat inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Simple JVMTI Agent demo.
 *
 * Pavel Tisnovsky <ptisnovs@redhat.com>
 * Jakub Filak     <jfilak@redhat.com>
 *
 * It needs to be compiled as a shared native library (.so)
 * and then loaded into JVM using -agentlib command line parameter.
 *
 * Please look into Makefile how the compilation & loading
 * should be performed.
 */

/* System include files */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <errno.h>
#include <syslog.h>

#if HAVE_SYSTEMD_JOURNAL
#include <systemd/sd-journal.h>
#endif

/* Shared macros and so on */
#include "abrt-checker.h"

/* ABRT include file */
#include "internal_libabrt.h"

/* JVM TI include files */
#include <jni.h>
#include <jvmti.h>
#include <jvmticmlr.h>

/* Internal tool includes */
#include "jthread_map.h"
#include "jthrowable_circular_buf.h"


/* Configuration of processed JVMTI Events */

/* Enables checks based on JVMTI_EVENT_VM_DEATH */
/* #define ABRT_VM_DEATH_CHECK */

/* Enables checks based on JVMTI_EVENT_VM_OBJECT_ALLOC */
/* #define ABRT_OBJECT_ALLOCATION_SIZE_CHECK */

/* Enables checks based on JVMTI_EVENT_OBJECT_FREE */
/* #define ABRT_OBJECT_FREE_CHECK */

/* Enables checks based on:
 *   JVMTI_EVENT_GARBAGE_COLLECTION_START
 *   JVMTI_EVENT_GARBAGE_COLLECTION_FINISH */
/* #define ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK */

/* Enables checks based on JVMTI_EVENT_COMPILED_METHOD_LOAD */
/* #define ABRT_COMPILED_METHOD_LOAD_CHECK */


/* Basic settings */
#define VM_MEMORY_ALLOCATION_THRESHOLD 1024
#define GC_TIME_THRESHOLD 1

/* For debugging purposes */
#define PRINT_JVM_ENVIRONMENT_VARIABLES 1

/* Don't need to be changed */
#define MAX_THREAD_NAME_LENGTH 40

/* Max. length of reason message */
#define MAX_REASON_MESSAGE_STRING_LENGTH 255

/* Max. length of stack trace */
#define MAX_STACK_TRACE_STRING_LENGTH 10000

/* Depth of stack trace */
#define MAX_STACK_TRACE_DEPTH 5

#define DEFAULT_THREAD_NAME "DefaultThread"

/* Fields which needs to be filled when calling ABRT */
#define FILENAME_TYPE_VALUE      "Java"
#define FILENAME_ANALYZER_VALUE  "Java"

/* Name of two methods from URL class */
#define TO_EXTERNAL_FORM_METHOD_NAME "toExternalForm"
#define GET_PATH_METHOD_NAME "getPath"

/* Default main class name */
#define UNKNOWN_CLASS_NAME "*unknown*"

/* The standard stack trace caused by header */
#define CAUSED_STACK_TRACE_HEADER "Caused by: "

/* A number stored reported exceptions */
#ifndef REPORTED_EXCEPTION_STACK_CAPACITY
#define  REPORTED_EXCEPTION_STACK_CAPACITY 5
#endif


/*
 * This structure contains all useful information about JVM environment.
 * (note that these strings should be deallocated using jvmti_env->Deallocate()!)
 */
typedef struct {
    char * cwd;
    char * command_and_params;
    char * launcher;
    char * java_home;
    char * class_path;
    char * boot_class_path;
    char * library_path;
    char * boot_library_path;
    char * ext_dirs;
    char * endorsed_dirs;
    char * java_vm_version;
    char * java_vm_name;
    char * java_vm_info;
    char * java_vm_vendor;
    char * java_vm_specification_name;
    char * java_vm_specification_vendor;
    char * java_vm_specification_version;
} T_jvmEnvironment;



/*
 * This structure contains all usefull information about process
 * where Java virtual machine is started.
 */
typedef struct {
    int    pid;
    char * exec_command;
    char * executable;
    char * main_class;
} T_processProperties;



/*
 * This structure represents a pair of additional information.
 */
typedef struct {
    const char *label; ///< FQDN static method returning String
    char *data;        ///< Return value of the method's call
} T_infoPair;



/*
 * This structure is representation of a single report of an exception.
 */
typedef struct {
    char *message;
    char *stacktrace;
    char *executable;
    char *exception_type_name;
    T_infoPair *additional_info;
    jobject exception_object;
} T_exceptionReport;



/* Global monitor lock */
jrawMonitorID shared_lock;

#if ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK
/* GC checks monitor lock */
jrawMonitorID gc_lock;
#endif /* ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK */

/* Log file */
FILE * fout = NULL;

/* Variable used to measure GC delays */
clock_t gc_start_time;

/* Structure containing JVM environment variables. */
T_jvmEnvironment jvmEnvironment;

/* Structure containing process properties. */
T_processProperties processProperties;

/* Map of buffer for already reported exceptions to prevent re-reporting */
T_jthreadMap *threadMap;

/* Map of uncaught exceptions. There should be only 1 per thread.*/
T_jthreadMap *uncaughtExceptionMap;

/* Configuration */
T_configuration globalConfig;

/* forward headers */
static char* get_path_to_class(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jclass class, char *class_name, const char *stringize_method_name);
static void print_jvm_environment_variables_to_file(FILE *out);
static char* format_class_name(char *class_signature, char replace_to);
static int check_jvmti_error(jvmtiEnv *jvmti_env, jvmtiError error_code, const char *str);
static jclass find_class_in_loaded_class(jvmtiEnv *jvmti_env, JNIEnv *jni_env, const char *searched_class_name);



/*
 * Frees memory of given array terminated by empty entry.
 */
static void info_pair_vector_free(T_infoPair *pairs)
{
    if (NULL == pairs)
    {
        return;
    }

    for (T_infoPair *iter = pairs; iter->label; ++iter)
    {
        free(iter->data);
    }

    free(pairs);
}



/*
 * Converts given array terminated by empty entry into String.
 */
static char *info_pair_vector_to_string(T_infoPair *pairs)
{
    if (NULL == pairs)
    {
        return NULL;
    }

    size_t required_bytes = 0;
    for (T_infoPair *iter = pairs; NULL != iter->label; ++iter)
    {
        required_bytes += strlen(iter->label) + strlen(iter->data) + strlen(" = \n");
    }

    if (required_bytes == 0)
    {
        return NULL;
    }

    char *contents = (char *)malloc(required_bytes);
    if (NULL == contents)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory");
        return NULL;
    }

    size_t to_write = required_bytes;
    char *pointer = contents;
    for (T_infoPair *iter = pairs; NULL != iter->label; ++iter)
    {
        const int written = snprintf(pointer, to_write, "%s = %s\n", iter->label, iter->data);
        if (written < 0)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": snprintf() failed to write to already malloced memory");
            break;
        }

        pointer += written;
    }

    return contents;
}



/*
 * Frees memory of given report structure.
 */
static void exception_report_free(T_exceptionReport *report)
{
    if (NULL == report)
    {
        return;
    }

    free(report->message);
    free(report->stacktrace);
    free(report->executable);
    free(report->exception_type_name);

    info_pair_vector_free(report->additional_info);
}



/*
 * Returns a static memory with default log file name. Must not be released by free()!
 */
static const char *get_default_log_file_name()
{
    static const char DEFAULT_LOG_FILE_NAME_FORMAT[] = "abrt_checker_%d.log";
    /* A bit more than necessary but this is an optimization and few more Bytes can't kill us */
#define _AUX_LOG_FILE_NAME_MAX_LENGTH (sizeof(DEFAULT_LOG_FILE_NAME_FORMAT) + sizeof(int) * 3)
    static char log_file_name[_AUX_LOG_FILE_NAME_MAX_LENGTH];
    static int initialized = 0;

    if (initialized == 0)
    {
        initialized = 1;

        pid_t pid = getpid();
        /* snprintf() returns -1 on error */
        if (0 > snprintf(log_file_name, _AUX_LOG_FILE_NAME_MAX_LENGTH, DEFAULT_LOG_FILE_NAME_FORMAT, pid))
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": snprintf(): can't print default log file name\n");
            return NULL;
        }
    }
#undef _AUX_LOG_FILE_NAME_MAX_LENGTH
    return log_file_name;
}



/*
 * Appends file_name to *path and returns a pointer to result. Returns NULL on
 * error and leaves *path untouched.
 */
static char *append_file_to_path(char **path, const char *file_name)
{
    if (NULL == file_name)
    {
        return NULL;
    }

    const size_t outlen = strlen(*path);
    const int need_trailing = (*path)[outlen -1] != '/';
    char *result = malloc(outlen + strlen(file_name) + need_trailing + 1);
    if (NULL == result)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory\n");
        return NULL;
    }

    char *tmp = strcpy(result, *path);
    tmp += outlen;
    if (need_trailing)
    {
        *tmp = '/';
        ++tmp;
    }

    strcpy(tmp, file_name);

    free(*path);
    *path = result;
    return result;
}



/*
 * Gets the log file
 */
static FILE *get_log_file()
{
    /* Log file */

    if (NULL == fout
        && DISABLED_LOG_OUTPUT != globalConfig.outputFileName)
    {
        /* try to open output log file */
        const char *fn = globalConfig.outputFileName;
        if (NULL != fn)
        {
            struct stat sb;
            if (0 > stat(fn, &sb))
            {
                if (ENOENT != errno)
                {
                    fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": cannot stat log file %s: %s\n", fn, strerror(errno));
                    return NULL;
                }
            }
            else if (S_ISDIR(sb.st_mode))
            {
                fn = append_file_to_path(&globalConfig.outputFileName, get_default_log_file_name());
            }
        }
        else
        {
            fn = get_default_log_file_name();
        }

        if (NULL == fn)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": cannot build log file name.");
            return NULL;
        }

        VERBOSE_PRINT("Path to the log file: %s\n", fn);
        fout = fopen(fn, "wt");
        if (NULL == fout)
        {
            free(globalConfig.outputFileName);
            globalConfig.outputFileName = DISABLED_LOG_OUTPUT;
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": can not create output file %s. Disabling logging.\n", fn);
        }
    }

    return fout;
}



/*
 * Define a helper macro
 */
static int log_print(const char *fmt, ...)
{
    FILE *flog = get_log_file();
    int ret = 0;

    if(NULL != flog)
    {
        va_list ap;
        va_start(ap, fmt);
        ret = vfprintf(flog, fmt, ap);
        va_end(ap);
    }

    return ret;
}



/*
 * Return PID (process ID) as a string.
 */
static void get_pid_as_string(char * buffer)
{
    int pid = getpid();
    sprintf(buffer, "%d", pid);
    INFO_PRINT("%s\n", buffer);
}



/*
 * Return UID (user ID) as a string.
 */
static void get_uid_as_string(char * buffer)
{
    int uid = getuid();
    sprintf(buffer, "%d", uid);
    INFO_PRINT("%s\n", buffer);
}



/*
 * Return original string or "" if NULL
 * is passed in instead of string.
 */
static const char * null2empty(const char *str)
{
    if (str == NULL)
    {
        return "";
    }
    else
    {
        return str;
    }
}


static char *get_exception_type_name(
        jvmtiEnv *jvmti_env,
        JNIEnv *jni_env,
        jobject exception_object)
{
    jclass exception_class = (*jni_env)->GetObjectClass(jni_env, exception_object);

    char *exception_name_ptr = NULL;

    /* retrieve all required informations */
    jvmtiError error_code = (*jvmti_env)->GetClassSignature(jvmti_env, exception_class, &exception_name_ptr, NULL);
    if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
        return NULL;

    char *formated_exception_name_ptr = format_class_name(exception_name_ptr, '\0');
    if (formated_exception_name_ptr != exception_name_ptr)
    {
        char *dest = exception_name_ptr;
        char *src = formated_exception_name_ptr;

        while(src[0] != '\0')
        {
            *dest = *src;
            ++dest;
            ++src;
        }
        dest[0] = '\0';
    }

    return exception_name_ptr;
}



/*
 * Returns non zero value if exception's type is intended to be reported even
 * if the exception was caught.
 */
static int exception_is_intended_to_be_reported(
        jvmtiEnv *jvmti_env,
        JNIEnv *jni_env,
        jobject exception_object,
        char **exception_type)
{
    int retval = 0;

    if (globalConfig.reportedCaughExceptionTypes != NULL)
    {
        if (NULL == *exception_type)
        {
            *exception_type = get_exception_type_name(jvmti_env, jni_env, exception_object);
            if (NULL == *exception_type)
                return 0;
        }

        /* special cases for selected exceptions */
        for (char **cursor = globalConfig.reportedCaughExceptionTypes; *cursor; ++cursor)
        {
            if (strcmp(*cursor, *exception_type) == 0)
            {
                retval = 1;
                break;
            }
        }
    }

    return retval;
}



/*
 * Add JVM environment data into ABRT event message.
 */
static void add_jvm_environment_data(problem_data_t *pd)
{
    char *jvm_env = NULL;
    size_t sizeloc = 0;
    FILE *mem = open_memstream(&jvm_env, &sizeloc);

    if (NULL == mem)
    {
        perror("Skipping 'jvm_environment' problem element. open_memstream");
        return;
    }

    print_jvm_environment_variables_to_file(mem);
    fclose(mem);

    problem_data_add_text_editable(pd, "jvm_environment", jvm_env);
    free(jvm_env);
}



/*
 * Add process properties into ABRT event message.
 */
static void add_process_properties_data(problem_data_t *pd)
{
    pid_t pid = getpid();

    char *environ = get_environ(pid);
    problem_data_add_text_editable(pd, FILENAME_ENVIRON, environ ? environ : "");
    free(environ);

    char pidstr[20];
    get_pid_as_string(pidstr);
    problem_data_add_text_editable(pd, FILENAME_PID, pidstr);
    problem_data_add_text_editable(pd, FILENAME_CMDLINE, null2empty(processProperties.exec_command));
    if (!problem_data_get_content_or_NULL(pd, FILENAME_EXECUTABLE))
    {
        problem_data_add_text_editable(pd, FILENAME_EXECUTABLE, null2empty(processProperties.executable));
    }
    else
    {
        problem_data_add_text_editable(pd, "java_executable", null2empty(processProperties.executable));
    }
}



/*
 * Add additional debug info item.
 */
static void add_additional_info_data(problem_data_t *pd, T_infoPair *additional_info)
{
    char *contents = info_pair_vector_to_string(additional_info);
    if (NULL != contents)
    {
        problem_data_add_text_editable(pd, "java_custom_debug_info", contents);
        free(contents);
    }
}



/*
 * Register new ABRT event using given message and a method name.
 * If reportErrosTo global flags doesn't contain ED_ABRT, this function does nothing.
 */
static void register_abrt_event(
        const char *executable,
        const char *message,
        const char *backtrace,
        T_infoPair *additional_info)
{
    if ((globalConfig.reportErrosTo & ED_ABRT) == 0)
    {
        VERBOSE_PRINT("ABRT reporting is disabled\n");
        return;
    }

    char s[11];
    problem_data_t *pd = problem_data_new();

    /* fill in all required fields */
    problem_data_add_text_editable(pd, FILENAME_TYPE, FILENAME_TYPE_VALUE);
    problem_data_add_text_editable(pd, FILENAME_ANALYZER, FILENAME_ANALYZER_VALUE);

    get_uid_as_string(s);
    problem_data_add_text_editable(pd, FILENAME_UID, s);

    /* executable must belong to some package otherwise ABRT refuse it */
    problem_data_add_text_editable(pd, FILENAME_EXECUTABLE, executable);
    problem_data_add_text_editable(pd, FILENAME_BACKTRACE, backtrace);

    /* type and analyzer are the same for abrt, we keep both just for sake of comaptibility */
    problem_data_add_text_editable(pd, FILENAME_REASON, message);
    /* end of required fields */

    /* add optional fields */
    add_jvm_environment_data(pd);
    add_process_properties_data(pd);
    add_additional_info_data(pd, additional_info);
    problem_data_add_text_noteditable(pd, "abrt-java-connector", VERSION);

    /* sends problem data to abrtd over the socket */
    int res = problem_data_send_to_abrt(pd);
    fprintf(stderr, "ABRT problem creation: '%s'\n", res ? "failure" : "success");
    problem_data_free(pd);
}



/*
 * Report a stack trace to all systems
 */
static void report_stacktrace(
        const char *executable,
        const char *message,
        const char *stacktrace,
        T_infoPair *additional_info)
{
    if (globalConfig.reportErrosTo & ED_SYSLOG)
    {
        VERBOSE_PRINT("Reporting stack trace to syslog\n");
        syslog(LOG_ERR, "%s\n%s", message, stacktrace);
    }

#if HAVE_SYSTEMD_JOURNAL
    if (globalConfig.reportErrosTo & ED_JOURNALD)
    {
        VERBOSE_PRINT("Reporting stack trace to JournalD\n");
        sd_journal_send("MESSAGE=%s", message,
                        "PRIORITY=%d", LOG_ERR,
                        "STACK_TRACE=%s", stacktrace ? stacktrace : "no stack trace",
                        NULL);

    }
#endif

    log_print("%s\n", message);

    if (stacktrace)
    {
        log_print("%s", stacktrace);
    }
    if (executable)
    {
        log_print("executable: %s\n", executable);
    }
    if (additional_info)
    {
        char *info = info_pair_vector_to_string(additional_info);
        if (NULL != info)
        {
            log_print("%s\n", info);
        }
        free(info);
    }

    if (NULL != stacktrace)
    {
        VERBOSE_PRINT("Reporting stack trace to ABRT");
        register_abrt_event(executable, message, stacktrace, additional_info);
    }
}



/*
 * Returns logical true if exception occurred and clears it.
 */
static inline int check_and_clear_exception(
        JNIEnv     *jni_env)
{
        if ((*jni_env)->ExceptionOccurred(jni_env))
        {
#ifdef VERBOSE
            (*jni_env)->ExceptionDescribe(jni_env);
#endif
            (*jni_env)->ExceptionClear(jni_env);
            return 1;
        }

        return 0;
}



/*
 * Print a message when any JVM TI error occurs.
 */
static void print_jvmti_error(
            jvmtiEnv   *jvmti_env,
            jvmtiError  error_code,
            const char *str)
{
    char *errnum_str;
    const char *msg_str = str == NULL ? "" : str;
    char *msg_err = NULL;
    errnum_str = NULL;

    /* try to convert error number to string */
    (void)(*jvmti_env)->GetErrorName(jvmti_env, error_code, &errnum_str);
    msg_err = errnum_str == NULL ? "Unknown" : errnum_str;
    fprintf(stderr, "ERROR: JVMTI: %d(%s): %s\n", error_code, msg_err, msg_str);

    if (NULL != errnum_str)
        (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)errnum_str);
}


static int get_tid(
        JNIEnv   *jni_env,
        jthread  thr,
        jlong    *tid)
{
    jclass thread_class = (*jni_env)->GetObjectClass(jni_env, thr);
    if (NULL == thread_class)
    {
        VERBOSE_PRINT("Cannot get class of thread object\n");
        return 1;
    }

    jmethodID get_id = (*jni_env)->GetMethodID(jni_env, thread_class, "getId", "()J" );
    if (check_and_clear_exception(jni_env) || NULL == get_id)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of java/lang/Thread.getId()J\n");
        return 1;
    }

    /* Thread.getId() throws nothing */
    *tid = (*jni_env)->CallLongMethod(jni_env, thr, get_id);

    return 0;
}



/*
 * Takes information about an exception and returns human readable string
 * describing the exception's occurrence.
 *
 * Cuts the description to not exceed MAX_REASON_MESSAGE_STRING_LENGTH
 * characters.
 */
static char *format_exception_reason_message(
        int caught,
        const char *exception_fqdn,
        const char *class_fqdn,
        const char *method)
{
    const char *exception_name = exception_fqdn;
    const char *class_name = class_fqdn;
    const char *prefix = caught ? "Caught" : "Uncaught";

    char *message = (char*)calloc(MAX_REASON_MESSAGE_STRING_LENGTH + 1, sizeof(char));
    if (message == NULL)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": calloc(): out of memory");
        return NULL;
    }

    while (1)
    {
        const int message_len = snprintf(message, MAX_REASON_MESSAGE_STRING_LENGTH,
                "%s exception %s in method %s%s%s()", prefix,
                exception_name, class_name, ('\0' != class_name[0] ? "." : ""),
                method);

        if (message_len <= 0)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": snprintf(): can't print reason message to memory on stack\n");
            free(message);
            return NULL;
        }
        else if (message_len >= MAX_REASON_MESSAGE_STRING_LENGTH)
        {
            const char *ptr = NULL;
            if (NULL != (ptr = strrchr(class_name, '.')))
            {
                /* Drop name space from method signature */
                class_name = ptr + 1;
                continue;
            }
            else if(NULL != (ptr = strrchr(exception_name, '.')))
            {
                /* Drop name space from exception class signature */
                exception_name = ptr + 1;
                continue;
            }
            else if (class_name[0] != '\0')
            {
                /* Drop class name from method signature */
                class_name += strlen(class_name);
                continue;
            }
            /* No more place for shortening. The message will remain truncated. */
        }

        return message;
    }
}



/*
 * Format class signature into a printable form.
 * Class names have form "Ljava/lang/String;"
 * Requested form        "java.lang.String"
 */
static char* format_class_name(char *class_signature, char replace_to)
{
    char *output;
    /* make sure we don't end with NPE */
    if (class_signature != NULL)
    {
        /* replace 'L' from the beggining of class signature */
        output = class_signature;
        if (output[0] == 'L')
        {
            output++; /* goto to the next character */
        }

        /* replace all '/'s to '.'s */
        char *c;
        for (c = class_signature; *c != 0; c++)
        {
            if (*c == '/') *c = '.';
        }

        /* replace the last character in the class name */
        /* but inly if this character is ';' */
        /* c[0] == '\0' see the for loop above */
        if (c != class_signature && c[-1] == ';')
        {
            c[-1] = replace_to;
        }
    }
    else
    {
        return NULL;
    }
    return output;
}



/*
 * Format class signature into a form suitable for ClassLoader.getResource()
 * Class names have form "Ljava/lang/String;"
 * Requested form        "java/lang/String"
 */
char* format_class_name_for_JNI_call(char *class_signature)
{
    char *output;
    /* make sure we don't end with NPE */
    if (class_signature != NULL)
    {
        /* replace 'L' from the beggining of class signature */
        output = class_signature;
        if (output[0] == 'L')
        {
            output++; /* goto to the next character */
        }
        /* replace last character in the class name */
        /* if this character is ';' */
        char *last_char = output + strlen(output) - 1;
        if (*last_char == ';')
        {
            *last_char = '.';
        }
    }
    else
    {
        return NULL;
    }
    return output;
}



/*
 * Check if any JVM TI error have occured.
 */
static int check_jvmti_error(
            jvmtiEnv   *jvmti_env,
            jvmtiError  error_code,
            const char *str)
{
    if ( error_code != JVMTI_ERROR_NONE )
    {
        print_jvmti_error(jvmti_env, error_code, str);
        return 1;
    }

    return 0;
}



/*
 * Enter a critical section by doing a JVMTI Raw Monitor Enter
 */
static void enter_critical_section(
            jvmtiEnv *jvmti_env,
            jrawMonitorID monitor)
{
    jvmtiError error_code;

    error_code = (*jvmti_env)->RawMonitorEnter(jvmti_env, monitor);
    check_jvmti_error(jvmti_env, error_code, "Cannot enter with raw monitor");
}



/*
 * Exit a critical section by doing a JVMTI Raw Monitor Exit
 */
static void exit_critical_section(
            jvmtiEnv *jvmti_env,
            jrawMonitorID monitor)
{
    jvmtiError error_code;

    error_code = (*jvmti_env)->RawMonitorExit(jvmti_env, monitor);
    check_jvmti_error(jvmti_env, error_code, "Cannot exit with raw monitor");
}



/*
 * Get a name for a given jthread.
 */
static void get_thread_name(
            jvmtiEnv *jvmti_env,
            jthread   thread,
            char     *tname,
            int       maxlen)
{
    jvmtiThreadInfo info;
    jvmtiError      error;

    /* Make sure the stack variables are garbage free */
    (void)memset(&info, 0, sizeof(info));

    /* Assume the name is unknown for now */
    (void)strcpy(tname, DEFAULT_THREAD_NAME);

    /* Get the thread information, which includes the name */
    error = (*jvmti_env)->GetThreadInfo(jvmti_env, thread, &info);
    check_jvmti_error(jvmti_env, error, "Cannot get thread info");

    /* The thread might not have a name, be careful here. */
    if (info.name != NULL)
    {
        int len;

        /* Copy the thread name into tname if it will fit */
        len = (int)strlen(info.name);
        if ( len < maxlen )
        {
            (void)strcpy(tname, info.name);
        }

        error = ((*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)info.name));
        if (error != JVMTI_ERROR_NONE)
        {
            INFO_PRINT("(get_thread_name) Error expected: %d, got: %d\n", JVMTI_ERROR_NONE, error);
            INFO_PRINT("\n");
        }
    }
}



/*
 * Read executable name from the special file /proc/${PID}/exe
 */
char *get_executable(int pid)
{
    /* be sure we allocated enough memory for path to a file /proc/${PID}/exe */
    char buf[sizeof("/proc/%lu/exe") + sizeof(long)*3];

    sprintf(buf, "/proc/%lu/exe", (long)pid);
    char *executable = malloc_readlink(buf);
    if (!executable)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": can't read executable name from /proc/${PID}/exe");
        return NULL;
    }

    /* find and cut off " (deleted)" from the path */
    char *deleted = executable + strlen(executable) - strlen(" (deleted)");
    if (deleted > executable && strcmp(deleted, " (deleted)") == 0)
    {
        *deleted = '\0';
        /*log("File '%s' seems to be deleted", executable);*/
    }

    /* find and cut off prelink suffixes from the path */
    char *prelink = executable + strlen(executable) - strlen(".#prelink#.XXXXXX");
    if (prelink > executable && strncmp(prelink, ".#prelink#.", strlen(".#prelink#.")) == 0)
    {
        /*log("File '%s' seems to be a prelink temporary file", executable);*/
        *prelink = '\0';
    }
    return executable;
}



/*
 * Read command parameters from /proc/${PID}/cmdline
 */
char *get_command(int pid)
{
    char file_name[32];
    FILE *fin;
    size_t size = 0;
    char *out;
    char buffer[2048];

    /* name of /proc/${PID}/cmdline */
    sprintf(file_name, "/proc/%d/cmdline", pid);

    /* read first 2047 bytes from this file */
    fin = fopen(file_name, "rb");
    if (NULL == fin)
    {
        return NULL;
    }

    size = fread(buffer, sizeof(char), 2048, fin);
    fclose(fin);

    /* parameters are divided by \0, get rid of it */
    for (size_t i=0; i<size-1; i++)
    {
        if (buffer[i] == 0) buffer[i] = ' ';
    }

    /* defensive copy */
    out = (char*)calloc(strlen(buffer)+1, sizeof(char));
    strcpy(out, buffer);
    return out;
}



/*
 * Replace all old_chars by new_chars
 */
static void string_replace(char *string_to_replace, char old_char, char new_char)
{
    char *c = string_to_replace;
    for (; *c; c++)
    {
        if (*c==old_char) *c=new_char;
    }
}



/*
 * Appends '.' and returns the result a newly mallocated memory
 */
static char * create_updated_class_name(char *class_name)
{
    char *upd_class_name = (char*)malloc(strlen(class_name)+2);
    if (NULL == upd_class_name)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory");
        return NULL;
    }
    strcpy(upd_class_name, class_name);
    strcat(upd_class_name, ".");
    return upd_class_name;
}



/*
 * Solution for JAR-style URI:
 * file:/home/tester/abrt_connector/bin/JarTest.jar!/SimpleTest.class
 */
static char *extract_fs_path(char *url_path)
{
    char *jar_sfx = strstr(url_path, ".jar!");
    if (NULL != jar_sfx)
        jar_sfx[sizeof(".jar") - 1] = '\0';

    if (strncmp("file:", url_path, sizeof("file:") - 1) == 0)
        memmove(url_path, url_path + (sizeof("file:") - 1), 2 + strlen(url_path) - sizeof("file:"));

    return url_path;
}



/*
 * Get name and path to main class.
 */
static char *get_main_class(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env)
{
    jvmtiError error_code;
    char *class_name;

    error_code = (*jvmti_env)->GetSystemProperty(jvmti_env, "sun.java.command", &class_name);
    if (error_code != JVMTI_ERROR_NONE || NULL == class_name)
    {
        return UNKNOWN_CLASS_NAME;
    }

    /* strip the second part of sun.java.command property */
    char *space = strchrnul(class_name, ' ');
    *space = '\0';

    /* check whether the executed entity is a jar file */
    if (strlen(class_name) > 4 &&
            (space[-4] == '.' && space[-3] == 'j' && space[-2] == 'a' && space[-1] == 'r'))
    {
        char jarpath[PATH_MAX + 1] = { '\0' };
        if (realpath(class_name, jarpath) == NULL)
        {
            fprintf(stderr, "Error %d: Could get real path of '%s'\n", errno, class_name);
            strncpy(jarpath, class_name, sizeof(jarpath));
        }

        char *executable = strdup(jarpath);
        if (NULL == executable)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": strdup(): out of memory");
            return NULL;
        }

        return executable;
    }

    /* the executed entity is a Java class */

    /* replace all '.' to '/' */
    string_replace(class_name, '.', '/');

    jclass cls = (*jni_env)->FindClass(jni_env, class_name);
    if (check_and_clear_exception(jni_env) || cls == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of %s\n", class_name);
        (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)class_name);
        return UNKNOWN_CLASS_NAME;
    }

    /* add '.' at the end of class name */
    char *upd_class_name = create_updated_class_name(class_name);

    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)class_name);

    if (upd_class_name == NULL)
    {
        (*jni_env)->DeleteLocalRef(jni_env, cls);
        return NULL;
    }

    char *path_to_class = get_path_to_class(jvmti_env, jni_env, cls, upd_class_name, GET_PATH_METHOD_NAME);

    free(upd_class_name);

    (*jni_env)->DeleteLocalRef(jni_env, cls);

    if (path_to_class == NULL)
    {
        return UNKNOWN_CLASS_NAME;
    }

    return extract_fs_path(path_to_class);
}



/*
 * Fill in the structure processProperties with JVM process info.
 */
static void fill_process_properties(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env)
{
    int pid = getpid();
    processProperties.pid = pid;
    processProperties.executable = get_executable(pid);
    processProperties.exec_command = get_command(pid);
    processProperties.main_class = get_main_class(jvmti_env, jni_env);
}



/*
 * Fill in the structure jvmEnvironment with JVM info.
 */
static void fill_jvm_environment(
            jvmtiEnv *jvmti_env)
{
    (*jvmti_env)->GetSystemProperty(jvmti_env, "sun.java.command", &(jvmEnvironment.command_and_params));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "sun.java.launcher", &(jvmEnvironment.launcher));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.home", &(jvmEnvironment.java_home));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.class.path", &(jvmEnvironment.class_path));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.library.path", &(jvmEnvironment.library_path));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "sun.boot.class.path", &(jvmEnvironment.boot_class_path));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "sun.boot.library.path", &(jvmEnvironment.boot_library_path));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.ext.dirs", &(jvmEnvironment.ext_dirs));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.endorsed.dirs", &(jvmEnvironment.endorsed_dirs));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.version", &(jvmEnvironment.java_vm_version));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.name", &(jvmEnvironment.java_vm_name));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.info", &(jvmEnvironment.java_vm_info));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.vendor", &(jvmEnvironment.java_vm_vendor));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.specification.name", &(jvmEnvironment.java_vm_specification_name));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.specification.vendor", &(jvmEnvironment.java_vm_specification_vendor));
    (*jvmti_env)->GetSystemProperty(jvmti_env, "java.vm.specification.version", &(jvmEnvironment.java_vm_specification_version));

    jvmEnvironment.cwd = getcwd(NULL, 0);
}



/*
 * Print process properties.
 */
static void print_process_properties(void)
{
    INFO_PRINT("%-30s: %d\n", "pid", processProperties.pid);
    INFO_PRINT("%-30s: %s\n", "executable", processProperties.executable);
    INFO_PRINT("%-30s: %s\n", "exec_command", processProperties.exec_command);
    INFO_PRINT("%-30s: %s\n", "main_class", processProperties.main_class);
}



/*
 * Print JVM environment
 */
static void print_jvm_environment_variables(void)
{
#ifndef SILENT
    print_jvm_environment_variables_to_file(stdout);
#endif
}



static void print_jvm_environment_variables_to_file(FILE *out)
{
    fprintf(out, "%-30s: %s\n", "sun.java.command", null2empty(jvmEnvironment.command_and_params));
    fprintf(out, "%-30s: %s\n", "sun.java.launcher", null2empty(jvmEnvironment.launcher));
    fprintf(out, "%-30s: %s\n", "java.home", null2empty(jvmEnvironment.java_home));
    fprintf(out, "%-30s: %s\n", "java.class.path", null2empty(jvmEnvironment.class_path));
    fprintf(out, "%-30s: %s\n", "java.library.path", null2empty(jvmEnvironment.library_path));
    fprintf(out, "%-30s: %s\n", "sun.boot.class.path", null2empty(jvmEnvironment.boot_class_path));
    fprintf(out, "%-30s: %s\n", "sun.boot.library.path", null2empty(jvmEnvironment.boot_library_path));
    fprintf(out, "%-30s: %s\n", "java.ext.dirs", null2empty(jvmEnvironment.ext_dirs));
    fprintf(out, "%-30s: %s\n", "java.endorsed.dirs", null2empty(jvmEnvironment.endorsed_dirs));
    fprintf(out, "%-30s: %s\n", "cwd", null2empty(jvmEnvironment.cwd));
    fprintf(out, "%-30s: %s\n", "java.vm.version", null2empty(jvmEnvironment.java_vm_version));
    fprintf(out, "%-30s: %s\n", "java.vm.name", null2empty(jvmEnvironment.java_vm_name));
    fprintf(out, "%-30s: %s\n", "java.vm.info", null2empty(jvmEnvironment.java_vm_info));
    fprintf(out, "%-30s: %s\n", "java.vm.vendor", null2empty(jvmEnvironment.java_vm_vendor));
    fprintf(out, "%-30s: %s\n", "java.vm.specification_name", null2empty(jvmEnvironment.java_vm_specification_name));
    fprintf(out, "%-30s: %s\n", "java.vm.specification.vendor", null2empty(jvmEnvironment.java_vm_specification_vendor));
    fprintf(out, "%-30s: %s\n", "java.vm.specification.version", null2empty(jvmEnvironment.java_vm_specification_version));
}



/*
 * Goes throw the list of FQDN static methods returning java.Lang.String, tries
 * to call them and returns their results in an array terminated by empty
 * entry.
 */
static T_infoPair *collect_additional_debug_information(
        jvmtiEnv *jvmti_env,
        JNIEnv   *jni_env)
{
    if (NULL == globalConfig.fqdnDebugMethods)
    {
        return NULL;
    }

    size_t cnt = 0;
    const char *const *iter = (const char *const *)globalConfig.fqdnDebugMethods;
    for ( ; NULL != *iter; ++iter)
    {
        ++cnt;
    }

    T_infoPair *ret_val = (T_infoPair *)malloc(sizeof(*ret_val) * (cnt + 1));
    if (NULL == ret_val)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory");
        return NULL;
    }

    T_infoPair *info = ret_val;
    iter = (const char *const *)globalConfig.fqdnDebugMethods;
    for( ; NULL != *iter; ++iter)
    {
        char *debug_class_name_str = strdup(*iter);
        if (debug_class_name_str == NULL)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": strdup(): out of memory");
            /* We want to finish this method call */
            break;
        }

        /* name.space.class.method -> name.space.class + method
         */
        char *debug_method_name = strrchr(debug_class_name_str, '.');
        if (NULL == debug_method_name)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": Debug method '%s' is not in FQDN format\n", debug_class_name_str);
            goto next_debug_method;
        }

        debug_method_name[0] = '\0';
        ++debug_method_name;

        /* Try to find instance of Class class for the debug class name.
         * Looks only in the list of already loaded classes as we don't
         * want to use Class Loader to find the class on disk. This
         * approache ensures that the debug method is called only for
         * relevant applications.
         */
        jclass debug_class = find_class_in_loaded_class(jvmti_env, jni_env, debug_class_name_str);
        if (NULL == debug_class)
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not find class of '%s'\n", *iter);
            goto next_debug_method;
        }

        jmethodID debug_method = (*jni_env)->GetStaticMethodID(jni_env, debug_class, debug_method_name, "()Ljava/lang/String;");
        if (check_and_clear_exception(jni_env) || NULL == debug_method)
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not find debug method '%s'\n", *iter);
            goto next_debug_method;
        }

        jstring debug_string = (*jni_env)->CallStaticObjectMethod(jni_env, debug_class, debug_method);
        if (check_and_clear_exception(jni_env))
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Exception occurred in debug method '%s'\n", *iter);
            goto next_debug_method;
        }

        info->label = *iter;
        {
            char *tmp = (char*)(*jni_env)->GetStringUTFChars(jni_env, debug_string, NULL);
            info->data = strdup(tmp);
            (*jni_env)->ReleaseStringUTFChars(jni_env, debug_string, tmp);
        }

        if (NULL == info->data)
        {
            fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": strdup(): out of memory");
            /* We want to finish this method call */
            break;
        }

        ++info;

next_debug_method:
        free(debug_class_name_str);
    }

    /* stop */
    info->label = NULL;
    info->data = NULL;

    return ret_val;
}



/*
 * Called right after JVM started up.
 */
static void JNICALL callback_on_vm_init(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            jthread   thread)
{
    char tname[MAX_THREAD_NAME_LENGTH];

    enter_critical_section(jvmti_env, shared_lock);

    INFO_PRINT("Got VM init event\n");
    get_thread_name(jvmti_env , thread, tname, sizeof(tname));
    INFO_PRINT("callbackVMInit:  %s thread\n", tname);

    fill_jvm_environment(jvmti_env);
    fill_process_properties(jvmti_env, jni_env);
#if PRINT_JVM_ENVIRONMENT_VARIABLES == 1
    print_jvm_environment_variables();
    print_process_properties();
#endif
    exit_critical_section(jvmti_env, shared_lock);
}



#if ABRT_VM_DEATH_CHECK
/*
 * Called before JVM shuts down.
 */
static void JNICALL callback_on_vm_death(
            jvmtiEnv *jvmti_env,
            JNIEnv   *env __UNUSED_VAR)
{
    enter_critical_section(jvmti_env, shared_lock);
    INFO_PRINT("Got VM Death event\n");
    exit_critical_section(jvmti_env, shared_lock);
}
#endif /* ABRT_VM_DEATH_CHECK */



/*
 * Former callback_on_thread_start but it is not necessary to create an empty
 * structures and waste CPU time because it is more likely that no exception
 * will occur during the thread's lifetime. So, we converted the callback to a
 * function which can be used for initialization of the internal structures.
 */
static T_jthrowableCircularBuf *create_exception_buf_for_thread(
            JNIEnv   *jni_env,
            jlong tid)
{
    T_jthrowableCircularBuf *threads_exc_buf = jthrowable_circular_buf_new(jni_env, REPORTED_EXCEPTION_STACK_CAPACITY);
    if (NULL == threads_exc_buf)
    {
        fprintf(stderr, "Cannot enable check for already reported exceptions. Disabling reporting to ABRT in current thread!");
        return NULL;
    }

    jthread_map_push(threadMap, tid, (void *)threads_exc_buf);
    return threads_exc_buf;
}



/*
 * Called before thread end.
 */
static void JNICALL callback_on_thread_end(
            jvmtiEnv *jvmti_env __UNUSED_VAR,
            JNIEnv   *jni_env,
            jthread  thread)
{
    INFO_PRINT("ThreadEnd\n");
    if (NULL == threadMap)
    {
        return;
    }

    if (!jthread_map_empty(threadMap) || !jthread_map_empty(uncaughtExceptionMap))
    {
        jlong tid = 0;

        if (get_tid(jni_env, thread, &tid))
        {
            VERBOSE_PRINT("Cannot free thread's exception buffer because cannot get TID");
            return;
        }

        T_exceptionReport *rpt = (T_exceptionReport *)jthread_map_pop(uncaughtExceptionMap, tid);
        T_jthrowableCircularBuf *threads_exc_buf = (T_jthrowableCircularBuf *)jthread_map_pop(threadMap, tid);

        if (NULL != rpt)
        {
            if (NULL == threads_exc_buf || NULL == jthrowable_circular_buf_find(threads_exc_buf, rpt->exception_object))
            {
                report_stacktrace(NULL != rpt->executable ? rpt->executable : processProperties.main_class,
                                  NULL != rpt->message ? rpt->message : "Uncaught exception",
                                  rpt->stacktrace, rpt->additional_info);
            }

            exception_report_free(rpt);
        }

        if (threads_exc_buf != NULL)
        {
            jthrowable_circular_buf_free(threads_exc_buf);
        }
    }
}


#ifdef GENERATE_JVMTI_STACK_TRACE
/*
 * Get line number for given method and location in this method.
 */
static int get_line_number(
            jvmtiEnv  *jvmti_env,
            jmethodID  method,
            jlocation  location)
{
    int count;
    int line_number = 0;
    int i;
    jvmtiLineNumberEntry *location_table;
    jvmtiError error_code;

    /* how can we recognize an illegal value of the location variable.
     * Documentation says:
     *   A 64 bit value, representing a monotonically increasing executable
     *   position within a method. -1 indicates a native method.
     *
     * we use 0 for now.
     */
    if (NULL == method || 0 == location)
    {
        return -1;
    }

    /* read table containing line numbers and instruction indexes */
    error_code = (*jvmti_env)->GetLineNumberTable(jvmti_env, method, &count, &location_table);
    /* it is possible, that we are unable to read the table -> missing debuginfo etc. */
    if (error_code != JVMTI_ERROR_NONE)
    {
        if (location_table != NULL)
        {
            (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)location_table);
        }
        return -1;
    }

    /* iterate over the read table */
    for (i = 0; i < count - 1; i++)
    {
        jvmtiLineNumberEntry entry1 = location_table[i];
        jvmtiLineNumberEntry entry2 = location_table[i+1];
        /* entry1 and entry2 are copies allocated on the stack:          */
        /*   how can we recognize that location_table[i] is valid value? */
        /*                                                               */
        /* we hope that all array values are valid for now               */

        /* if location is between entry1 (including) and entry2 (excluding), */
        /* we are on the right line */
        if (location >= entry1.start_location && location < entry2.start_location)
        {
            line_number = entry1.line_number;
            break;
        }
    }

    /* last instruction is handled specifically */
    if (location >= location_table[count-1].start_location)
    {
        line_number = location_table[count-1].line_number;
    }

    /* memory deallocation */
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)location_table);
    return line_number;
}
#endif /* GENERATE_JVMTI_STACK_TRACE */



/*
 * Return path to given class using given class loader.
 */
static char* get_path_to_class_class_loader(
            jvmtiEnv *jvmti_env __UNUSED_VAR,
            JNIEnv   *jni_env,
            jclass    class_loader,
            char     *class_name,
            const char *stringize_method_name)
{
    char *out = NULL;
    jclass class_loader_class = NULL;

    char *upd_class_name = (char*)malloc(strlen(class_name) + sizeof("class") + 1);
    if (NULL == upd_class_name)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory");
        return NULL;
    }

    strcpy(upd_class_name, class_name);
    strcat(upd_class_name, "class");

    /* find ClassLoader class */
    class_loader_class = (*jni_env)->FindClass(jni_env, "java/lang/ClassLoader");
    if (check_and_clear_exception(jni_env) || class_loader_class ==  NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of java/lang/ClassLoader\n");
        free(upd_class_name);
        return NULL;
    }

    /* find method ClassLoader.getResource() */
    jmethodID get_resource = (*jni_env)->GetMethodID(jni_env, class_loader_class, "getResource", "(Ljava/lang/String;)Ljava/net/URL;");
    if (check_and_clear_exception(jni_env) || get_resource ==  NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of java/lang/ClassLoader.getResource(Ljava/lang/String;)Ljava/net/URL;\n");
        free(upd_class_name);
        (*jni_env)->DeleteLocalRef(jni_env, class_loader_class);
        return NULL;
    }

    /* convert new class name into a Java String */
    jstring j_class_name = (*jni_env)->NewStringUTF(jni_env, upd_class_name);
    free(upd_class_name);
    if (check_and_clear_exception(jni_env))
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not allocate a new UTF string for '%s'\n", upd_class_name);
        goto get_path_to_class_class_loader_lcl_refs_cleanup;
    }

    /* call method ClassLoader.getResource(className) */
    jobject url = (*jni_env)->CallObjectMethod(jni_env, class_loader, get_resource, j_class_name);
    if (check_and_clear_exception(jni_env) || NULL == url)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get a resource of %s\n", class_name);
        goto get_path_to_class_class_loader_lcl_refs_cleanup;
    }

    /* find method URL.toString() */
    jclass url_class = (*jni_env)->FindClass(jni_env, "java/net/URL");
    if (check_and_clear_exception(jni_env) || NULL == url_class)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of java/net/URL\n");
        goto get_path_to_class_class_loader_lcl_refs_cleanup;
    }

    jmethodID to_external_form = (*jni_env)->GetMethodID(jni_env, url_class, stringize_method_name, "()Ljava/lang/String;");
    if (check_and_clear_exception(jni_env) || NULL == to_external_form)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of java/net/URL.%s()Ljava/lang/String;\n", stringize_method_name);
        goto get_path_to_class_class_loader_lcl_refs_cleanup;
    }

    /* call method URL.toString() */
    jstring jstr = (jstring)(*jni_env)->CallObjectMethod(jni_env, url, to_external_form);
    if (check_and_clear_exception(jni_env) || jstr ==  NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Failed to convert an URL object to a string\n");
        goto get_path_to_class_class_loader_lcl_refs_cleanup;
    }

    /* convert Java String into C char* */
    char *str = (char*)(*jni_env)->GetStringUTFChars(jni_env, jstr, NULL);
    out = strdup(str);
    if (out == NULL)
    {
        fprintf(stderr, "strdup(): out of memory");
    }

    /* cleanup */
    (*jni_env)->ReleaseStringUTFChars(jni_env, jstr, str);

get_path_to_class_class_loader_lcl_refs_cleanup:
    (*jni_env)->DeleteLocalRef(jni_env, class_loader_class);
    (*jni_env)->DeleteLocalRef(jni_env, j_class_name);
    return out;
}



/*
 * Wraps java.lang.ClassLoader.getSystemClassLoader()
 */
static jobject get_system_class_loader(
            jvmtiEnv *jvmti_env __UNUSED_VAR,
            JNIEnv   *jni_env)
{
    jobject system_class_loader = NULL;

    jclass class_loader_class = (*jni_env)->FindClass(jni_env, "java/lang/ClassLoader");
    if (check_and_clear_exception(jni_env) || NULL == class_loader_class)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of java/lang/ClassLoader\n");
        return NULL;
    }

    jmethodID get_system_class_loader_smethod =(*jni_env)->GetStaticMethodID(jni_env, class_loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (check_and_clear_exception(jni_env) || NULL == get_system_class_loader_smethod)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not find method java.lang.ClassLoader.getSystemClassLoader()Ljava/lang/ClassLoader;\n");
        goto get_system_class_loader_cleanup;
    }

    system_class_loader = (*jni_env)->CallStaticObjectMethod(jni_env, class_loader_class, get_system_class_loader_smethod);
    if (check_and_clear_exception(jni_env))
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Exception occurred: Cannot get the system class loader\n");
        goto get_system_class_loader_cleanup;
    }

get_system_class_loader_cleanup:
    (*jni_env)->DeleteLocalRef(jni_env, class_loader_class);
    return system_class_loader;
}



/*
 * Return path to given class.
 */
static char* get_path_to_class(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            jclass    class,
            char     *class_name,
            const char *stringize_method_name)
{
    jobject class_loader = NULL;
    (*jvmti_env)->GetClassLoader(jvmti_env, class, &class_loader);

    /* class is loaded using boot classloader */
    if (class_loader == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": A class has not been loaded by a ClassLoader. Going to use the system class loader.\n");

        class_loader = get_system_class_loader(jvmti_env, jni_env);
        if (NULL == class_loader)
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Cannot get the system class loader.");
            return NULL;
        }
    }

    return get_path_to_class_class_loader(jvmti_env, jni_env, class_loader, class_name, stringize_method_name);
}



/*
 * Looks up a Class instance of given class name in the list of already loaded
 * classes.
 *
 * Calls toString() method for each entry in the result to
 * JVMTI::GetLoadedClasses() and compares its result to the given class name.
 */
static jclass find_class_in_loaded_class(
            jvmtiEnv   *jvmti_env,
            JNIEnv     *jni_env,
            const char *searched_class_name)
{
    jclass result = NULL;
    jint num_classes = 0;
    jclass *loaded_classes;
    jvmtiError error = (*jvmti_env)->GetLoadedClasses(jvmti_env, &num_classes, &loaded_classes);
    if (check_jvmti_error(jvmti_env, error, "jvmtiEnv::GetLoadedClasses()"))
    {
        return NULL;
    }

    jclass class_class = (*jni_env)->FindClass(jni_env, "java/lang/Class");
    if (check_and_clear_exception(jni_env) || NULL == class_class)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of java/lang/Class\n");
        goto find_class_in_loaded_class_cleanup;
    }

    jmethodID get_name_method = (*jni_env)->GetMethodID(jni_env, class_class, "getName", "()Ljava/lang/String;");
    if (check_and_clear_exception(jni_env) || NULL == get_name_method)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of java/lang/Class.getName()Ljava/lang/String;\n");
        (*jni_env)->DeleteLocalRef(jni_env, class_class);
        goto find_class_in_loaded_class_cleanup;
    }

    for (jint i = 0; NULL == result && i < num_classes; ++i)
    {
        jobject class_name = (*jni_env)->CallObjectMethod(jni_env, loaded_classes[i], get_name_method);
        if (check_and_clear_exception(jni_env) || NULL == class_name)
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get name of a loaded class\n");
            continue;
        }

        char *class_name_cstr = (char*)(*jni_env)->GetStringUTFChars(jni_env, class_name, NULL);
        if (strcmp(searched_class_name, class_name_cstr) == 0)
        {
            VERBOSE_PRINT("The class was found in the array of loaded classes\n");
            result = loaded_classes[i];
        }

        (*jni_env)->ReleaseStringUTFChars(jni_env, class_name, class_name_cstr);
        (*jni_env)->DeleteLocalRef(jni_env, class_name);
    }

find_class_in_loaded_class_cleanup:
    if (NULL != loaded_classes)
        (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)loaded_classes);

    return result;
}



/*
 * Print one method from stack frame.
 */
static int print_stack_trace_element(
            jvmtiEnv       *jvmti_env,
            JNIEnv         *jni_env,
            jobject         stack_frame,
            char           *stack_trace_str,
            unsigned        max_length,
            char           **class_fs_path)
{
    jclass stack_frame_class = (*jni_env)->GetObjectClass(jni_env, stack_frame);
    jmethodID get_class_name_method = (*jni_env)->GetMethodID(jni_env, stack_frame_class, "getClassName", "()Ljava/lang/String;");
    if (check_and_clear_exception(jni_env) || get_class_name_method == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of $(Frame class).getClassName()Ljava/lang/String;\n");
        (*jni_env)->DeleteLocalRef(jni_env, stack_frame_class);
        return -1;
    }

    jstring class_name_of_frame_method = (*jni_env)->CallObjectMethod(jni_env, stack_frame, get_class_name_method);
    if (check_and_clear_exception(jni_env) || class_name_of_frame_method == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class name of a class on a frame\n");
        (*jni_env)->DeleteLocalRef(jni_env, stack_frame_class);
        return -1;
    }

    char *cls_name_str = (char*)(*jni_env)->GetStringUTFChars(jni_env, class_name_of_frame_method, NULL);
    string_replace(cls_name_str, '.', '/');
    jclass class_of_frame_method = (*jni_env)->FindClass(jni_env, cls_name_str);
    char *class_location = NULL;

    if (check_and_clear_exception(jni_env) || NULL == class_of_frame_method)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get class of %s. Try more hard by searching in the loaded classes.\n", cls_name_str);
        string_replace(cls_name_str, '/', '.');
        class_of_frame_method = find_class_in_loaded_class(jvmti_env, jni_env, cls_name_str);
        string_replace(cls_name_str, '.', '/');
    }

    if (NULL != class_of_frame_method)
    {
        char *updated_cls_name_str = create_updated_class_name(cls_name_str);
        if (updated_cls_name_str != NULL)
        {
            class_location = get_path_to_class(jvmti_env, jni_env, class_of_frame_method, updated_cls_name_str, TO_EXTERNAL_FORM_METHOD_NAME);

            if (NULL != class_fs_path)
            {
                *class_fs_path = get_path_to_class(jvmti_env, jni_env, class_of_frame_method, updated_cls_name_str, GET_PATH_METHOD_NAME);
                if (NULL != *class_fs_path)
                    *class_fs_path = extract_fs_path(*class_fs_path);
            }

            free(updated_cls_name_str);
        }
        (*jni_env)->DeleteLocalRef(jni_env, class_of_frame_method);
    }
    (*jni_env)->ReleaseStringUTFChars(jni_env, class_name_of_frame_method, cls_name_str);

    jmethodID to_string_method = (*jni_env)->GetMethodID(jni_env, stack_frame_class, "toString", "()Ljava/lang/String;");
    (*jni_env)->DeleteLocalRef(jni_env, stack_frame_class);
    if (check_and_clear_exception(jni_env) || to_string_method == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of $(Frame class).toString()Ljava/lang/String;\n");
        return -1;
    }

    jobject orig_str = (*jni_env)->CallObjectMethod(jni_env, stack_frame, to_string_method);
    if (check_and_clear_exception(jni_env) || NULL == orig_str)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get a string representation of a class on a frame\n");
        (*jni_env)->DeleteLocalRef(jni_env, orig_str);
        return -1;
    }

    char *str = (char*)(*jni_env)->GetStringUTFChars(jni_env, orig_str, NULL);
    int wrote = snprintf(stack_trace_str, max_length, "\tat %s [%s]\n", str, class_location == NULL ? "unknown" : class_location);
    if (wrote > 0 && stack_trace_str[wrote-1] != '\n')
    {   /* the length limit was reached and frame is printed only partially */
        /* so in order to not show partial frames clear current frame's data */
        VERBOSE_PRINT("Too many frames or too long frame. Finishing stack trace generation.");
        stack_trace_str[0] = '\0';
        wrote = 0;
    }
    (*jni_env)->ReleaseStringUTFChars(jni_env, orig_str, str);
    (*jni_env)->DeleteLocalRef(jni_env, orig_str);
    return wrote;
}



/*
 * Generates standard Java exception stack trace with file system path to the file
 */
static int print_exception_stack_trace(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            jobject   exception,
            char     *stack_trace_str,
            size_t    max_stack_trace_lenght,
            char     **executable)
{

    jclass exception_class = (*jni_env)->GetObjectClass(jni_env, exception);
    jmethodID to_string_method = (*jni_env)->GetMethodID(jni_env, exception_class, "toString", "()Ljava/lang/String;");
    if (check_and_clear_exception(jni_env) || to_string_method == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of $(Exception class).toString()Ljava/lang/String;\n");
        (*jni_env)->DeleteLocalRef(jni_env, exception_class);
        return -1;
    }

    jobject exception_str = (*jni_env)->CallObjectMethod(jni_env, exception, to_string_method);
    if (check_and_clear_exception(jni_env) || exception_str == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get a string representation of a class on a frame\n");
        (*jni_env)->DeleteLocalRef(jni_env, exception_class);
        return -1;
    }

    char *str = (char*)(*jni_env)->GetStringUTFChars(jni_env, exception_str, NULL);
    int wrote = snprintf(stack_trace_str, max_stack_trace_lenght, "%s\n", str);
    if (wrote < 0 )
    {   /* this should never happen, snprintf() usually works w/o errors */
        return -1;
    }
    if (wrote > 0 && stack_trace_str[wrote-1] != '\n')
    {
        VERBOSE_PRINT("Too long exception string. Not generating stack trace at all.");
        /* in order to not show partial exception clear current frame's data */
        stack_trace_str[0] = '\0';
        return 0;
    }

    (*jni_env)->ReleaseStringUTFChars(jni_env, exception_str, str);
    (*jni_env)->DeleteLocalRef(jni_env, exception_str);

    jmethodID get_stack_trace_method = (*jni_env)->GetMethodID(jni_env, exception_class, "getStackTrace", "()[Ljava/lang/StackTraceElement;");
    (*jni_env)->DeleteLocalRef(jni_env, exception_class);

    if (check_and_clear_exception(jni_env) || get_stack_trace_method == NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of $(Exception class).getStackTrace()[Ljava/lang/StackTraceElement;\n");
        return wrote;
    }

    jobject stack_trace_array = (*jni_env)->CallObjectMethod(jni_env, exception, get_stack_trace_method);
    if (check_and_clear_exception(jni_env) || stack_trace_array ==  NULL)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get a stack trace from an exception object\n");
        return wrote;
    }

    jint array_size = (*jni_env)->GetArrayLength(jni_env, stack_trace_array);
    for (jint i = 0; i < array_size; ++i)
    {
        /* Throws only ArrayIndexOutOfBoundsException and this should not happen */
        jobject frame_element = (*jni_env)->GetObjectArrayElement(jni_env, stack_trace_array, i);

        const int frame_wrote = print_stack_trace_element(jvmti_env,
                jni_env,
                frame_element,
                stack_trace_str + wrote,
                max_stack_trace_lenght - wrote,
                ((NULL != executable && array_size - 1 == i) ? executable : NULL));

        (*jni_env)->DeleteLocalRef(jni_env, frame_element);

        if (frame_wrote <= 0)
        {   /* <  0 : this should never happen, snprintf() usually works w/o errors */
            /* == 0 : wrote nothing: the length limit was reached and no more */
            /* frames can be added to the stack trace */
            break;
        }

        wrote += frame_wrote;
    }

    (*jni_env)->DeleteLocalRef(jni_env, stack_trace_array);

    return wrote;
}

static char *generate_thread_stack_trace(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            char     *thread_name,
            jobject  exception,
            char     **executable)
{
    char  *stack_trace_str;
    /* allocate string which will contain stack trace */
    stack_trace_str = (char*)calloc(MAX_STACK_TRACE_STRING_LENGTH + 1, sizeof(char));
    if (stack_trace_str == NULL)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": calloc(): out of memory");
        return NULL;
    }

    int wrote = snprintf(stack_trace_str, MAX_STACK_TRACE_STRING_LENGTH, "Exception in thread \"%s\" ", thread_name);
    int exception_wrote = print_exception_stack_trace(jvmti_env,
            jni_env,
            exception,
            stack_trace_str + wrote,
            MAX_STACK_TRACE_STRING_LENGTH - wrote,
            executable);

    if (exception_wrote <= 0)
    {
        free(stack_trace_str);
        return NULL;
    }

    wrote += exception_wrote;

    /* GetObjectClass() throws nothing */
    jclass exception_class = (*jni_env)->GetObjectClass(jni_env, exception);
    if (NULL == exception_class)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Cannot get class of an object\n");
        return stack_trace_str;
    }

    jmethodID get_cause_method = (*jni_env)->GetMethodID(jni_env, exception_class, "getCause", "()Ljava/lang/Throwable;");
    (*jni_env)->DeleteLocalRef(jni_env, exception_class);

    if (check_and_clear_exception(jni_env) || NULL == get_cause_method)
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Could not get methodID of $(Exception class).getCause()Ljava/lang/Throwable;\n");
        return stack_trace_str;
    }

    jobject cause = (*jni_env)->CallObjectMethod(jni_env, exception, get_cause_method);
    if (check_and_clear_exception(jni_env))
    {
        VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Failed to get an inner exception of the top most one;\n");
        return stack_trace_str;
    }

    while (NULL != cause)
    {
        if ((size_t)(MAX_STACK_TRACE_STRING_LENGTH - wrote) < (sizeof(CAUSED_STACK_TRACE_HEADER) - 1))
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Full exception stack trace buffer. Cannot add a cause.");
            (*jni_env)->DeleteLocalRef(jni_env, cause);
            break;
        }

        strcat(stack_trace_str + wrote, CAUSED_STACK_TRACE_HEADER);
        wrote += sizeof(CAUSED_STACK_TRACE_HEADER) - 1;

        const int cause_wrote = print_exception_stack_trace(jvmti_env,
                jni_env,
                cause,
                stack_trace_str + wrote,
                MAX_STACK_TRACE_STRING_LENGTH - wrote,
                /*No executable*/NULL);

        if (cause_wrote <= 0)
        {   /* <  0 : this should never happen, snprintf() usually works w/o errors */
            /* == 0 : wrote nothing: the length limit was reached and no more */
            /* cause can be added to the stack trace */
            break;
        }

        wrote += cause_wrote;

        jobject next_cause = (*jni_env)->CallObjectMethod(jni_env, cause, get_cause_method);
        (*jni_env)->DeleteLocalRef(jni_env, cause);
        if (check_and_clear_exception(jni_env))
        {
            VERBOSE_PRINT(__FILE__ ":" STRINGIZE(__LINE__)": Failed to get an inner exception of another inner one;\n");
            return stack_trace_str;
        }
        cause = next_cause;
    }

    return stack_trace_str;
}

#ifdef GENERATE_JVMTI_STACK_TRACE
/*
 * Print one method from stack frame.
 */
static void print_one_method_from_stack(
            jvmtiEnv       *jvmti_env,
            JNIEnv         *jni_env,
            jvmtiFrameInfo  stack_frame,
            char           *stack_trace_str)
{
    jvmtiError  error_code;
    jclass      declaring_class;
    char       *method_name = NULL;
    char       *declaring_class_name = NULL;
    char       *source_file_name = NULL;

    error_code = (*jvmti_env)->GetMethodName(jvmti_env, stack_frame.method, &method_name, NULL, NULL);
    if (error_code != JVMTI_ERROR_NONE)
    {
        return;
    }
    error_code = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, stack_frame.method, &declaring_class);
    if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
        goto print_one_method_from_stack_cleanup;

    error_code = (*jvmti_env)->GetClassSignature(jvmti_env, declaring_class, &declaring_class_name, NULL);
    if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
        goto print_one_method_from_stack_cleanup;

    if (error_code != JVMTI_ERROR_NONE)
    {
        return;
    }
    char *updated_class_name = format_class_name_for_JNI_call(declaring_class_name);
    int line_number = get_line_number(jvmti_env, stack_frame.method, stack_frame.location);
    if (declaring_class != NULL)
    {
        error_code = (*jvmti_env)->GetSourceFileName(jvmti_env, declaring_class, &source_file_name);
        if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
            goto print_one_method_from_stack_cleanup;
    }

    char buf[1000];
    char line_number_buf[20];
    if (line_number >= 0)
    {
        sprintf(line_number_buf, "%d", line_number);
    }
    else
    {
        strcpy(line_number_buf, "Unknown location");
    }

    char *class_location = get_path_to_class(jvmti_env, jni_env, declaring_class, updated_class_name, TO_EXTERNAL_FORM_METHOD_NAME);
    sprintf(buf, "\tat %s%s(%s:%s) [%s]\n", updated_class_name, method_name, source_file_name, line_number_buf, class_location == NULL ? "unknown" : class_location);
    free(class_location);
    strncat(stack_trace_str, buf, MAX_STACK_TRACE_STRING_LENGTH - strlen(stack_trace_str) - 1);

#ifdef VERBOSE
    if (line_number >= 0)
    {
        printf("\tat %s%s(%s:%d location)\n", updated_class_name, method_name, source_file_name, line_number);
    }
    else
    {
        printf("\tat %s%s(%s:Unknown location)\n", updated_class_name, method_name, source_file_name);
    }
#endif

print_one_method_from_stack_cleanup:
    /* cleanup */
    if (NULL != method_name)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)method_name);
        check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
    }
    if (NULL != declaring_class_name)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)declaring_class_name);
        check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
    }
    if (NULL != source_file_name)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)source_file_name);
        check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
    }
}
#endif /* GENERATE_JVMTI_STACK_TRACE */


#ifdef GENERATE_JVMTI_STACK_TRACE
/*
 * Print stack trace for given thread.
 */
static char *generate_stack_trace(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            jthread   thread,
            char     *thread_name,
            char     *exception_class_name)
{
    jvmtiError     error_code;
    jvmtiFrameInfo stack_frames[MAX_STACK_TRACE_DEPTH];

    char  *stack_trace_str;
    char  buf[1000];
    int count = -1;
    int i;

    /* allocate string which will contain stack trace */
    stack_trace_str = (char*)calloc(MAX_STACK_TRACE_STRING_LENGTH + 1, sizeof(char));
    if (stack_trace_str == NULL)
    {
        fprintf(stderr, "calloc(): out of memory");
        return NULL;
    }

    /* get stack trace */
    error_code = (*jvmti_env)->GetStackTrace(jvmti_env, thread, 0, MAX_STACK_TRACE_DEPTH, stack_frames, &count);
    VERBOSE_PRINT("Number of records filled: %d\n", count);
    /* error or is stack trace empty? */
    if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__))
            || count < 1)
    {
        free(stack_trace_str);
        return NULL;
    }

    sprintf(buf, "Exception in thread \"%s\" %s\n", thread_name, exception_class_name);
    strncat(stack_trace_str, buf, MAX_STACK_TRACE_STRING_LENGTH - strlen(stack_trace_str) - 1);

    /* print content of stack frames */
    for (i = 0; i < count; i++) {
        jvmtiFrameInfo stack_frame = stack_frames[i];
        print_one_method_from_stack(jvmti_env, jni_env, stack_frame, stack_trace_str);
    }

    VERBOSE_PRINT(
    "Exception Stack Trace\n"
    "=====================\n"
    "Stack Trace Depth: %d\n"
    "%s\n", count, stack_trace_str);


    return stack_trace_str;
}
#endif /* GENERATE_JVMTI_STACK_TRACE */



/**
 * Called when an exception is thrown.
 */
static void JNICALL callback_on_exception(
            jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thr,
            jmethodID method,
            jlocation location __UNUSED_VAR,
            jobject exception_object,
            jmethodID catch_method,
            jlocation catch_location __UNUSED_VAR)
{
    /* This is caught exception and no caught exception is to be reported */
    if (NULL != catch_method && NULL == globalConfig.reportedCaughExceptionTypes)
        return;

    char *exception_type_name = NULL;

    /* all operations should be processed in critical section */
    enter_critical_section(jvmti_env, shared_lock);

    /* readable class names */
    if (catch_method == NULL || exception_is_intended_to_be_reported(jvmti_env, jni_env, exception_object, &exception_type_name))
    {
        char tname[MAX_THREAD_NAME_LENGTH];
        get_thread_name(jvmti_env, thr, tname, sizeof(tname));

        jlong tid = 0;
        T_jthrowableCircularBuf *threads_exc_buf = NULL;

        if (NULL != threadMap && 0 == get_tid(jni_env, thr, &tid))
        {
            threads_exc_buf = (T_jthrowableCircularBuf *)jthread_map_get(threadMap, tid);
            VERBOSE_PRINT("Got circular buffer for thread %p\n", (void *)threads_exc_buf);
        }
        else
        {
            VERBOSE_PRINT("Cannot get thread's ID. Disabling reporting to ABRT.");
        }

        if (NULL == threads_exc_buf || NULL == jthrowable_circular_buf_find(threads_exc_buf, exception_object))
        {
            jvmtiError error_code;
            jclass method_class;
            char *method_name_ptr = NULL;
            char *method_signature_ptr = NULL;
            char *class_name_ptr = NULL;
            char *class_signature_ptr = NULL;

            error_code = (*jvmti_env)->GetMethodName(jvmti_env, method, &method_name_ptr, &method_signature_ptr, NULL);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_cleanup;

            error_code = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, method, &method_class);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_cleanup;

            error_code = (*jvmti_env)->GetClassSignature(jvmti_env, method_class, &class_signature_ptr, NULL);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_cleanup;

            class_name_ptr = format_class_name(class_signature_ptr, '.');

            /* Remove trailing '.' */
            const ssize_t class_name_len = strlen(class_name_ptr);
            if (class_name_len > 0)
                class_name_ptr[class_name_len - 1] = '\0';

            if (NULL == exception_type_name)
                exception_type_name = get_exception_type_name(jvmti_env, jni_env, exception_object);

            char *message = format_exception_reason_message(/*caught?*/NULL != catch_method,
                    exception_type_name, class_name_ptr, method_name_ptr);

            char *executable = NULL;
            char *stack_trace_str = generate_thread_stack_trace(jvmti_env, jni_env, tname, exception_object,
                    (globalConfig.executableFlags & ABRT_EXECUTABLE_THREAD) ? &executable : NULL);

            T_infoPair *additional_info = collect_additional_debug_information(jvmti_env, jni_env);

            const char *report_message = message;
            if (NULL == report_message)
                report_message = (NULL != catch_method) ? "Caught exception" : "Uncaught exception";

            if (NULL == catch_method)
            {   /* Postpone reporting of uncaught exceptions as they may be caught by a native function */
                T_exceptionReport *rpt = malloc(sizeof(*rpt));
                if (NULL == rpt)
                {
                    fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": malloc(): out of memory");
                }
                else
                {
                    rpt->message = message;
                    message = NULL;

                    rpt->exception_type_name = exception_type_name;
                    exception_type_name = NULL;

                    rpt->stacktrace = stack_trace_str;
                    stack_trace_str = NULL;

                    rpt->executable = executable;
                    executable = NULL;

                    rpt->additional_info = additional_info;
                    additional_info = NULL;

                    rpt->exception_object = exception_object;

                    jthread_map_push(uncaughtExceptionMap, tid, (T_exceptionReport *)rpt);
                }
            }
            else
            {
                report_stacktrace(NULL != executable ? executable : processProperties.main_class,
                        report_message,
                        stack_trace_str,
                        additional_info);

                if (NULL == threads_exc_buf)
                    threads_exc_buf = create_exception_buf_for_thread(jni_env, tid);

                if (NULL != threads_exc_buf)
                {
                    VERBOSE_PRINT("Pushing to circular buffer\n");
                    jthrowable_circular_buf_push(threads_exc_buf, exception_object);
                }
            }

            free(executable);
            free(message);
            free(stack_trace_str);
            info_pair_vector_free(additional_info);

callback_on_exception_cleanup:
        /* cleapup */
            if (method_name_ptr != NULL)
            {
                error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)method_name_ptr);
                check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
            }
            if (method_signature_ptr != NULL)
            {
                error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)method_signature_ptr);
                check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
            }
            if (class_signature_ptr != NULL)
            {
                error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)class_signature_ptr);
                check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
            }
        }
        else
        {
            VERBOSE_PRINT("The exception was already reported!\n");
        }
    }

    if (NULL != exception_type_name)
    {
        free(exception_type_name);
    }

    exit_critical_section(jvmti_env, shared_lock);
}


/*
 * This function is called when an exception is catched.
 */
static void JNICALL callback_on_exception_catch(
            jvmtiEnv *jvmti_env,
            JNIEnv   *jni_env,
            jthread   thread,
            jmethodID method,
            jlocation location __UNUSED_VAR,
            jobject   exception_object)
{
    if (jthread_map_empty(uncaughtExceptionMap))
        return;

    /* all operations should be processed in critical section */
    enter_critical_section(jvmti_env, shared_lock);

    jclass class;

    jlong tid = 0;

    if (get_tid(jni_env, thread, &tid))
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": Cannot clear uncaught exceptions");
        goto callback_on_exception_catch_exit;
    }

    T_exceptionReport *rpt = (T_exceptionReport *)jthread_map_get(uncaughtExceptionMap, tid);
    if (NULL == rpt)
    {
        goto callback_on_exception_catch_exit;
    }

    jclass object_class = (*jni_env)->FindClass(jni_env, "java/lang/Object");
    if (check_and_clear_exception(jni_env) || NULL == object_class)
    {
        VERBOSE_PRINT("Cannot find java/lang/Object class");
        goto callback_on_exception_catch_exit;
    }

    jmethodID equal_method = (*jni_env)->GetMethodID(jni_env, object_class, "equals", "(Ljava/lang/Object;)Z");
    if (check_and_clear_exception(jni_env) || NULL == equal_method)
    {
        VERBOSE_PRINT("Cannot find java.lang.Object.equals(Ljava/lang/Object;)Z method");
        (*jni_env)->DeleteLocalRef(jni_env, object_class);
        goto callback_on_exception_catch_exit;
    }

    jboolean equal_objects = (*jni_env)->CallBooleanMethod(jni_env, exception_object, equal_method, rpt->exception_object);
    if (check_and_clear_exception(jni_env) || !equal_objects)
    {
        VERBOSE_PRINT("Cannot determine whether the caught exception is also the uncaught exception");
        (*jni_env)->DeleteLocalRef(jni_env, object_class);
        goto callback_on_exception_catch_exit;
    }

    /* Faster than get()-pop() approach is faster because it is search-and-search-free but
     * pop()-push() approach is search-free-and-search-malloc
     *
     * JVM always catches java.security.PrivilegedActionException while
     * handling uncaught java.lang.ClassNotFoundException throw by
     * initialization of the system (native) class loader.
     */
    jthread_map_pop(uncaughtExceptionMap, tid);

    if (exception_is_intended_to_be_reported(jvmti_env, jni_env, rpt->exception_object, &(rpt->exception_type_name)))
    {
        jlong tid = 0;
        T_jthrowableCircularBuf *threads_exc_buf = NULL;

        if (NULL != threadMap && 0 == get_tid(jni_env, thread, &tid))
        {
            threads_exc_buf = (T_jthrowableCircularBuf *)jthread_map_get(threadMap, tid);
            VERBOSE_PRINT("Got circular buffer for thread %p\n", (void *)threads_exc_buf);
        }
        else
        {
            VERBOSE_PRINT("Cannot get thread's ID. Disabling reporting to ABRT.");
        }

        if (NULL == threads_exc_buf || NULL == jthrowable_circular_buf_find(threads_exc_buf, rpt->exception_object))
        {
            char *method_name_ptr = NULL;
            char *method_signature_ptr = NULL;
            char *class_signature_ptr = NULL;

            jvmtiError error_code;

            /* retrieve all required informations */
            error_code = (*jvmti_env)->GetMethodName(jvmti_env, method, &method_name_ptr, &method_signature_ptr, NULL);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_catch_cleanup;

            error_code = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, method, &class);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_catch_cleanup;

            error_code = (*jvmti_env)->GetClassSignature(jvmti_env, class, &class_signature_ptr, NULL);
            if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
                goto callback_on_exception_catch_cleanup;

            /* readable class name */
            char *class_name_ptr = format_class_name(class_signature_ptr, '\0');
            char *message = format_exception_reason_message(/*caught*/1, rpt->exception_type_name,  class_name_ptr, method_name_ptr);
            report_stacktrace(NULL != rpt->executable ? rpt->executable : processProperties.main_class,
                              NULL != message ? message : "Caught exception",
                              rpt->stacktrace, rpt->additional_info);

            if (NULL == threads_exc_buf)
                threads_exc_buf = create_exception_buf_for_thread(jni_env, tid);

            if (NULL != threads_exc_buf)
            {
                VERBOSE_PRINT("Pushing to circular buffer\n");
                jthrowable_circular_buf_push(threads_exc_buf, rpt->exception_object);
            }

callback_on_exception_catch_cleanup:
            /* cleapup */
            if (method_name_ptr != NULL)
            {
                error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)method_name_ptr);
                check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
            }
            if (class_signature_ptr != NULL)
            {
                error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)class_signature_ptr);
                check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__));
            }
        }
    }

    exception_report_free(rpt);

callback_on_exception_catch_exit:
    exit_critical_section(jvmti_env, shared_lock);
}



#if ABRT_OBJECT_ALLOCATION_SIZE_CHECK
/**
 * Called when an object is allocated.
 */
static void JNICALL callback_on_object_alloc(
            jvmtiEnv *jvmti_env,
            JNIEnv* jni_env __UNUSED_VAR,
            jthread thread __UNUSED_VAR,
            jobject object __UNUSED_VAR,
            jclass object_klass,
            jlong size)
{
    char *signature_ptr = NULL;

    enter_critical_section(jvmti_env, shared_lock);
    jvmtiError error_code = (*jvmti_env)->GetClassSignature(jvmti_env, object_klass, &signature_ptr, NULL);
    if (check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
        return;

    if (size >= VM_MEMORY_ALLOCATION_THRESHOLD)
    {
        INFO_PRINT("object allocation: instance of class %s, allocated %ld bytes\n", signature_ptr, (long int)size);
    }

    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)signature_ptr);
    exit_critical_section(jvmti_env, shared_lock);
}
#endif /* ABRT_OBJECT_ALLOCATION_SIZE_CHECK */



#if ABRT_OBJECT_FREE_CHECK
/**
 * Called when an object is freed.
 */
static void JNICALL callback_on_object_free(
            jvmtiEnv *jvmti_env,
            jlong tag __UNUSED_VAR)
{
    enter_critical_section(jvmti_env, shared_lock);
    VERBOSE_PRINT("object free\n");
    exit_critical_section(jvmti_env, shared_lock);
}
#endif /* ABRT_OBJECT_FREE_CHECK */



#if ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK
/**
 * Called on GC start.
 */
static void JNICALL callback_on_gc_start(
            jvmtiEnv *jvmti_env)
{
    enter_critical_section(jvmti_env, gc_lock);
    gc_start_time = clock();
    VERBOSE_PRINT("GC start\n");
    exit_critical_section(jvmti_env, gc_lock);
}



/**
 * Called on GC finish.
 */
static void JNICALL callback_on_gc_finish(
            jvmtiEnv *jvmti_env)
{
    clock_t gc_end_time = clock();
    int diff;
    enter_critical_section(jvmti_env, gc_lock);
    INFO_PRINT("GC end\n");
    diff = (gc_end_time - (gc_start_time))/CLOCKS_PER_SEC;
    if (diff > GC_TIME_THRESHOLD)
    {
        char str[100];
        sprintf(str, "GC took more time than expected: %d\n", diff);
        INFO_PRINT("%s\n", str);
        register_abrt_event(processProperties.main_class, str, (unsigned char *)"GC thread", "no stack trace");
    }
    exit_critical_section(jvmti_env, gc_lock);
}
#endif /* ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK */



#if ABRT_COMPILED_METHOD_LOAD_CHECK
/**
 * Called when some method is about to be compiled.
 */
static void JNICALL callback_on_compiled_method_load(
            jvmtiEnv   *jvmti_env,
            jmethodID   method,
            jint        code_size __UNUSED_VAR,
            const void *code_addr __UNUSED_VAR,
            jint        map_length __UNUSED_VAR,
            const jvmtiAddrLocationMap* map __UNUSED_VAR,
            const void  *compile_info __UNUSED_VAR)
{
    jvmtiError error_code;
    char* name = NULL;
    char* signature = NULL;
    char* generic_ptr = NULL;
    char* class_signature = NULL;
    jclass class;

    enter_critical_section(jvmti_env, shared_lock);

    error_code = (*jvmti_env)->GetMethodName(jvmti_env, method, &name, &signature, &generic_ptr);
    if (check_jvmti_error(jvmti_env, error_code, "get method name"))
        goto callback_on_compiled_method_load_cleanup;

    error_code = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, method, &class);
    if (check_jvmti_error(jvmti_env, error_code, "get method declaring class"))
        goto callback_on_compiled_method_load_cleanup;

    error_code = (*jvmti_env)->GetClassSignature(jvmti_env, class, &class_signature, NULL);
    if (check_jvmti_error(jvmti_env, error_code, "get method name"))
        goto callback_on_compiled_method_load_cleanup;

    INFO_PRINT("Compiling method: %s.%s with signature %s %s   Code size: %5d\n",
        class_signature == NULL ? "" : class_signature,
        name, signature,
        generic_ptr == NULL ? "" : generic_ptr, (int)code_size);

callback_on_compiled_method_load_cleanup:
    if (name != NULL)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)name);
        check_jvmti_error(jvmti_env, error_code, "deallocate name");
    }
    if (signature != NULL)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)signature);
        check_jvmti_error(jvmti_env, error_code, "deallocate signature");
    }
    if (generic_ptr != NULL)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)generic_ptr);
        check_jvmti_error(jvmti_env, error_code, "deallocate generic_ptr");
    }
    if (class_signature != NULL)
    {
        error_code = (*jvmti_env)->Deallocate(jvmti_env, (unsigned char*)class_signature);
        check_jvmti_error(jvmti_env, error_code, "deallocate class_signature");
    }

    exit_critical_section(jvmti_env, shared_lock);
}
#endif /* ABRT_COMPILED_METHOD_LOAD_CHECK */



/*
 * Sel all required JVMTi capabilities.
 */
jvmtiError set_capabilities(jvmtiEnv *jvmti_env)
{
    jvmtiCapabilities capabilities;
    jvmtiError error_code;

    /* Add JVMTI capabilities */
    (void)memset(&capabilities, 0, sizeof(jvmtiCapabilities));
    capabilities.can_signal_thread = 1;
    capabilities.can_get_owned_monitor_info = 1;
    capabilities.can_generate_method_entry_events = 1;
    capabilities.can_generate_method_exit_events = 1;
    capabilities.can_generate_frame_pop_events = 1;
    capabilities.can_generate_exception_events = 1;
    capabilities.can_generate_vm_object_alloc_events = 1;
    capabilities.can_generate_object_free_events = 1;
    capabilities.can_generate_garbage_collection_events = 1;
    capabilities.can_generate_compiled_method_load_events = 1;
    capabilities.can_get_line_numbers = 1;
    capabilities.can_get_source_file_name = 1;
    capabilities.can_tag_objects = 1;

    error_code = (*jvmti_env)->AddCapabilities(jvmti_env, &capabilities);
    check_jvmti_error(jvmti_env, error_code, "Unable to get necessary JVMTI capabilities.");
    return error_code;
}



/*
 * Register all callback functions.
 */
jvmtiError register_all_callback_functions(jvmtiEnv *jvmti_env)
{
    jvmtiEventCallbacks callbacks;
    jvmtiError error_code;

    /* Initialize callbacks structure */
    (void)memset(&callbacks, 0, sizeof(callbacks));

    /* JVMTI_EVENT_VM_INIT */
    callbacks.VMInit = &callback_on_vm_init;

#if ABRT_VM_DEATH_CHECK
    /* JVMTI_EVENT_VM_DEATH */
    callbacks.VMDeath = &callback_on_vm_death;
#endif /* ABRT_VM_DEATH_CHECK */

    /* JVMTI_EVENT_THREAD_END */
    callbacks.ThreadEnd = &callback_on_thread_end;

    /* JVMTI_EVENT_EXCEPTION */
    callbacks.Exception = &callback_on_exception;

    /* JVMTI_EVENT_EXCEPTION_CATCH */
    callbacks.ExceptionCatch = &callback_on_exception_catch;

#if ABRT_OBJECT_ALLOCATION_SIZE_CHECK
    /* JVMTI_EVENT_VM_OBJECT_ALLOC */
    callbacks.VMObjectAlloc = &callback_on_object_alloc;
#endif

#if ABRT_OBJECT_FREE_CHECK
    /* JVMTI_EVENT_OBJECT_FREE */
    callbacks.ObjectFree = &callback_on_object_free;
#endif

#if ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK
    /* JVMTI_EVENT_GARBAGE_COLLECTION_START */
    callbacks.GarbageCollectionStart  = &callback_on_gc_start;

    /* JVMTI_EVENT_GARBAGE_COLLECTION_FINISH */
    callbacks.GarbageCollectionFinish = &callback_on_gc_finish;
#endif /* ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK*/

#if ABRT_COMPILED_METHOD_LOAD_CHECK
    /* JVMTI_EVENT_COMPILED_METHOD_LOAD */
    callbacks.CompiledMethodLoad = &callback_on_compiled_method_load;
#endif /* ABRT_COMPILED_METHOD_LOAD_CHECK */

    error_code = (*jvmti_env)->SetEventCallbacks(jvmti_env, &callbacks, (jint)sizeof(callbacks));
    check_jvmti_error(jvmti_env, error_code, "Cannot set jvmti callbacks");
    return error_code;
}



/*
 * Set given event notification mode.
 */
jvmtiError set_event_notification_mode(jvmtiEnv* jvmti_env, int event)
{
    jvmtiError error_code;

    error_code = (*jvmti_env)->SetEventNotificationMode(jvmti_env, JVMTI_ENABLE, event, (jthread)NULL);
    check_jvmti_error(jvmti_env, error_code, "Cannot set event notification");
    return error_code;
}



/*
 * Configure all event notification modes.
 */
jvmtiError set_event_notification_modes(jvmtiEnv* jvmti_env)
{
    jvmtiError error_code;

    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_VM_INIT)) != JNI_OK)
    {
        return error_code;
    }

#if ABRT_VM_DEATH_CHECK
    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_VM_DEATH)) != JNI_OK)
    {
        return error_code;
    }
#endif /* ABRT_VM_DEATH_CHECK */

    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_THREAD_END)) != JNI_OK)
    {
        return error_code;
    }

    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_EXCEPTION)) != JNI_OK)
    {
        return error_code;
    }

    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_EXCEPTION_CATCH)) != JNI_OK)
    {
        return error_code;
    }

#if ABRT_OBJECT_ALLOCATION_SIZE_CHECK
    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_VM_OBJECT_ALLOC)) != JNI_OK)
    {
        return error_code;
    }
#endif /* ABRT_OBJECT_ALLOCATION_SIZE_CHECK */

#if ABRT_OBJECT_FREE_CHECK
    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_OBJECT_FREE)) != JNI_OK)
    {
        return error_code;
    }
#endif /* ABRT_OBJECT_FREE_CHECK */

#if ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK
    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_GARBAGE_COLLECTION_START)) != JNI_OK)
    {
        return error_code;
    }

    if ((error_code= set_event_notification_mode(jvmti_env, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH)) != JNI_OK)
    {
        return error_code;
    }
#endif /* ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK */

#if ABRT_COMPILED_METHOD_LOAD_CHECK
    if ((error_code = set_event_notification_mode(jvmti_env, JVMTI_EVENT_COMPILED_METHOD_LOAD)) != JNI_OK)
    {
        return error_code;
    }
#endif /* ABRT_COMPILED_METHOD_LOAD_CHECK */

    return error_code;
}



/*
 * Create monitor used to acquire and free global lock (mutex).
 */
jvmtiError create_raw_monitor(jvmtiEnv *jvmti_env, const char *name, jrawMonitorID *monitor)
{
    jvmtiError error_code;

    error_code = (*jvmti_env)->CreateRawMonitor(jvmti_env, name, monitor);
    check_jvmti_error(jvmti_env, error_code, "Cannot create raw monitor");

    return error_code;
}



/*
 * Print major, minor and micro version of JVM TI.
 */
jvmtiError print_jvmti_version(jvmtiEnv *jvmti_env __UNUSED_VAR)
{
#ifndef SILENT
    jvmtiError error_code;

    jint version;
    jint cmajor, cminor, cmicro;

    error_code = (*jvmti_env)->GetVersionNumber(jvmti_env, &version);
    if (!check_jvmti_error(jvmti_env, error_code, __FILE__ ":" STRINGIZE(__LINE__)))
    {
        cmajor = (version & JVMTI_VERSION_MASK_MAJOR) >> JVMTI_VERSION_SHIFT_MAJOR;
        cminor = (version & JVMTI_VERSION_MASK_MINOR) >> JVMTI_VERSION_SHIFT_MINOR;
        cmicro = (version & JVMTI_VERSION_MASK_MICRO) >> JVMTI_VERSION_SHIFT_MICRO;
        printf("Compile Time JVMTI Version: %d.%d.%d (0x%08x)\n", cmajor, cminor, cmicro, version);
    }

    return error_code;
#else
    return 0;
#endif
}



/*
 * Called when agent is loading into JVM.
 */
JNIEXPORT jint JNICALL Agent_OnLoad(
        JavaVM *jvm,
        char *options,
        void *reserved __UNUSED_VAR)
{
    static int already_called = 0;
    jvmtiEnv  *jvmti_env = NULL;
    jvmtiError error_code = JVMTI_ERROR_NONE;
    jint       result;

    /* we need to make sure the agent is initialized once */
    if (already_called) {
        return JNI_OK;
    }

    already_called = 1;
    pthread_mutex_init(&abrt_print_mutex, /*attr*/NULL);

    INFO_PRINT("Agent_OnLoad\n");
    VERBOSE_PRINT("VERBOSE OUTPUT ENABLED\n");

    configuration_initialize(&globalConfig);
    parse_commandline_options(&globalConfig, options);
    if (globalConfig.configurationFileName)
    {
        parse_configuration_file(&globalConfig, globalConfig.configurationFileName);
    }

    /* check if JVM TI version is correct */
    result = (*jvm)->GetEnv(jvm, (void **) &jvmti_env, JVMTI_VERSION_1_0);
    if (result != JNI_OK || jvmti_env == NULL)
    {
        fprintf(stderr, "ERROR: Unable to access JVMTI Version 1 (0x%x),"
                " is your J2SE a 1.5 or newer version? JNIEnv's GetEnv() returned %d which is wrong.\n",
                JVMTI_VERSION_1, (int)result);
        return result;
    }
    INFO_PRINT("JVM TI version is correct\n");

    print_jvmti_version(jvmti_env);

    /* set required JVM TI agent capabilities */
    if ((error_code = set_capabilities(jvmti_env)) != JNI_OK)
    {
        return error_code;
    }

    /* register all callback functions */
    if ((error_code = register_all_callback_functions(jvmti_env)) != JNI_OK)
    {
        return error_code;
    }

    /* set notification modes for all callback functions */
    if ((error_code = set_event_notification_modes(jvmti_env)) != JNI_OK)
    {
        return error_code;
    }

    /* create global mutex */
    if ((error_code = create_raw_monitor(jvmti_env, "Shared Agent Lock", &shared_lock)) != JNI_OK)
    {
        return error_code;
    }

#if ABRT_GARBAGE_COLLECTION_TIMEOUT_CHECK
    /* create GC checks mutex */
    if ((error_code = create_raw_monitor(jvmti_env, "GC Checks Lock", &gc_lock)) != JNI_OK)
    {
        return error_code;
    }
#endif

    threadMap = jthread_map_new();
    if (NULL == threadMap)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": can not create a set of reported exceptions\n");
        return -1;
    }

    uncaughtExceptionMap = jthread_map_new();
    if (NULL == uncaughtExceptionMap)
    {
        fprintf(stderr, __FILE__ ":" STRINGIZE(__LINE__) ": can not create a set of uncaught exceptions\n");
        return -1;
    }
    return JNI_OK;
}



/*
 * Called when agent is unloading from JVM.
 */
JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm __UNUSED_VAR)
{
    static int already_called = 0;

    /* we need to make sure the agent is initialized once */
    if (already_called) {
        return;
    }

    already_called = 1;

    pthread_mutex_destroy(&abrt_print_mutex);

    INFO_PRINT("Agent_OnUnLoad\n");

    configuration_destroy(&globalConfig);

    if (fout != NULL)
    {
        fclose(fout);
    }

    jthread_map_free(uncaughtExceptionMap);
    jthread_map_free(threadMap);
}



/*
 * finito
 */

