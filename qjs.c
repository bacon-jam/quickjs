/*
 * QuickJS stand alone interpreter
 * 
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "cutils.h"
#include "quickjs-libc.h"

extern const uint8_t qjsc_repl[];
extern const uint32_t qjsc_repl_size;
#ifdef CONFIG_BIGNUM
extern const uint8_t qjsc_qjscalc[];
extern const uint32_t qjsc_qjscalc_size;
static int bignum_ext;
#endif

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags)
{
    JSValue val;
    int ret;

    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        /* for the modules, we compile then run to be able to set
           import.meta */
        val = JS_Eval(ctx, buf, buf_len, filename,
                      eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            js_module_set_import_meta(ctx, val, TRUE, TRUE);
            val = JS_EvalFunction(ctx, val);
        }
    } else {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename, int module)
{
    uint8_t *buf;
    int ret, eval_flags;
    size_t buf_len;
    
    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        perror(filename);
        exit(1);
    }

    if (module < 0) {
        module = (has_suffix(filename, ".mjs") ||
                  JS_DetectModule((const char *)buf, buf_len));
    }
    if (module)
        eval_flags = JS_EVAL_TYPE_MODULE;
    else
        eval_flags = JS_EVAL_TYPE_GLOBAL;
    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);
    return ret;
}

/* also used to initialize the worker context */
static JSContext *JS_NewCustomContext(JSRuntime *rt)
{
    JSContext *ctx;
    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
#ifdef CONFIG_BIGNUM
    if (bignum_ext) {
        JS_AddIntrinsicBigFloat(ctx);
        JS_AddIntrinsicBigDecimal(ctx);
        JS_AddIntrinsicOperators(ctx);
        JS_EnableBignumExt(ctx, TRUE);
    }
#endif
    /* system modules */
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD  0
#else
#define MALLOC_OVERHEAD  8
#endif

struct trace_malloc_data {
    uint8_t *base;
};

static inline unsigned long long js_trace_malloc_ptr_offset(uint8_t *ptr,
                                                struct trace_malloc_data *dp)
{
    return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static inline size_t js_trace_malloc_usable_size(void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize(ptr);
#elif defined(EMSCRIPTEN)
    return 0;
#elif defined(__linux__)
    return malloc_usable_size(ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size(ptr);
#endif
}

static void
#ifdef _WIN32
/* mingw printf is used */
__attribute__((format(gnu_printf, 2, 3)))
#else
__attribute__((format(printf, 2, 3)))
#endif
    js_trace_malloc_printf(JSMallocState *s, const char *fmt, ...)
{
    va_list ap;
    int c;

    va_start(ap, fmt);
    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            /* only handle %p and %zd */
            if (*fmt == 'p') {
                uint8_t *ptr = va_arg(ap, void *);
                if (ptr == NULL) {
                    printf("NULL");
                } else {
                    printf("H%+06lld.%zd",
                           js_trace_malloc_ptr_offset(ptr, s->opaque),
                           js_trace_malloc_usable_size(ptr));
                }
                fmt++;
                continue;
            }
            if (fmt[0] == 'z' && fmt[1] == 'd') {
                size_t sz = va_arg(ap, size_t);
                printf("%zd", sz);
                fmt += 2;
                continue;
            }
        }
        putc(c, stdout);
    }
    va_end(ap);
}

static void js_trace_malloc_init(struct trace_malloc_data *s)
{
    free(s->base = malloc(8));
}

static void *js_trace_malloc(JSMallocState *s, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    assert(size != 0);

    if (unlikely(s->malloc_size + size > s->malloc_limit))
        return NULL;
    ptr = malloc(size);
    js_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    }
    return ptr;
}

static void js_trace_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;

    js_trace_malloc_printf(s, "F %p\n", ptr);
    s->malloc_count--;
    s->malloc_size -= js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    free(ptr);
}

static void *js_trace_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_trace_malloc(s, size);
    }
    old_size = js_trace_malloc_usable_size(ptr);
    if (size == 0) {
        js_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    if (s->malloc_size + size - old_size > s->malloc_limit)
        return NULL;

    js_trace_malloc_printf(s, "R %zd %p", size, ptr);

    ptr = realloc(ptr, size);
    js_trace_malloc_printf(s, " -> %p\n", ptr);
    if (ptr) {
        s->malloc_size += js_trace_malloc_usable_size(ptr) - old_size;
    }
    return ptr;
}

static const JSMallocFunctions trace_mf = {
    js_trace_malloc,
    js_trace_free,
    js_trace_realloc,
#if defined(__APPLE__)
    malloc_size,
#elif defined(_WIN32)
    (size_t (*)(const void *))_msize,
#elif defined(EMSCRIPTEN)
    NULL,
#elif defined(__linux__)
    (size_t (*)(const void *))malloc_usable_size,
#else
    /* change this to `NULL,` if compilation fails */
    malloc_usable_size,
#endif
};

#define PROG_NAME "qjs"

/* REPRL and coverage support for Fuzzilli. */

#define CHECK(cond) do {                                  \
    if (!(cond)) {                                        \
        fprintf(stderr, "\"" #cond "\" failed\n");       \
        _exit(-1);                                        \
    }                                                     \
} while (0)

#ifdef _WIN32
static int reprl_crfd = -1;
static int reprl_cwfd = -1;
static int reprl_drfd = -1;

static int reprl_open_channel(const char *name, int flags)
{
    const char *value = getenv(name);
    char *end;
    unsigned long long handle;

    if (!value || !*value)
        return -1;
    errno = 0;
    handle = strtoull(value, &end, 10);
    if (errno || *end || handle > UINTPTR_MAX || !handle)
        return -1;
    return _open_osfhandle((intptr_t)(uintptr_t)handle, flags | _O_BINARY);
}

static void reprl_open_channels(void)
{
    reprl_crfd = reprl_open_channel("FUZZILLI_REPRL_CRFD", _O_RDONLY);
    reprl_cwfd = reprl_open_channel("FUZZILLI_REPRL_CWFD", _O_WRONLY);
    reprl_drfd = reprl_open_channel("FUZZILLI_REPRL_DRFD", _O_RDONLY);
    CHECK(reprl_crfd >= 0 && reprl_cwfd >= 0 && reprl_drfd >= 0);
}

#define REPRL_CRFD reprl_crfd
#define REPRL_CWFD reprl_cwfd
#define REPRL_DRFD reprl_drfd
#define reprl_read _read
#define reprl_write _write
#else
#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define reprl_read read
#define reprl_write write
#endif

#define SHM_SIZE 0x200000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)

struct shmem_data {
    uint32_t num_edges;
    unsigned char edges[];
};

struct shmem_data *__shmem;
uint32_t *__edges_start, *__edges_stop;
static int reprl_unhandled_rejections;

static void reprl_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                            JSValueConst reason, BOOL is_handled,
                                            void *opaque)
{
    (void)ctx;
    (void)promise;
    (void)reason;
    (void)opaque;
    if (is_handled) {
        if (reprl_unhandled_rejections > 0)
            reprl_unhandled_rejections--;
    } else {
        reprl_unhandled_rejections++;
    }
}

void __sanitizer_cov_reset_edgeguards(void)
{
    uint64_t next_edge = 0;
    uint32_t *guard;

    for (guard = __edges_start;
         guard < __edges_stop && next_edge < MAX_EDGES - 1; guard++)
        *guard = ++next_edge;
}

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    const char *shm_key;

    /* Avoid duplicate initialization. */
    if (start == stop || *start)
        return;

    if (__edges_start != NULL || __edges_stop != NULL) {
        fprintf(stderr, "Coverage instrumentation is only supported for a single module\n");
        _exit(-1);
    }

    __edges_start = start;
    __edges_stop = stop;

    /* Map the shared memory region. */
    shm_key = getenv("SHM_ID");
    if (!shm_key) {
        __shmem = (struct shmem_data *)calloc(1, SHM_SIZE);
        if (!__shmem)
            _exit(-1);
    } else {
#ifdef _WIN32
        HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_key);
        if (!mapping) {
            fprintf(stderr, "Failed to open shared memory region: %lu\n", GetLastError());
            _exit(-1);
        }
        __shmem = (struct shmem_data *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
                                                     0, 0, SHM_SIZE);
        CloseHandle(mapping);
        if (!__shmem) {
            fprintf(stderr, "Failed to map shared memory region: %lu\n", GetLastError());
            _exit(-1);
        }
#else
        int fd = shm_open(shm_key, O_RDWR, S_IREAD | S_IWRITE);
        if (fd <= -1) {
            fprintf(stderr, "Failed to open shared memory region: %s\n", strerror(errno));
            _exit(-1);
        }

        __shmem = (struct shmem_data *)mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
        close(fd);
        if (__shmem == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap shared memory region\n");
            _exit(-1);
        }
#endif
    }

    __sanitizer_cov_reset_edgeguards();

    __shmem->num_edges = stop - start < MAX_EDGES ? stop - start
                                                  : MAX_EDGES - 1;
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    uint32_t index = *guard;

    /* There is a small race if two threads execute the same edge at once:
       one may clear the guard before the other reads it. The first edge is
       ignored by libcoverage.c, so this does not affect coverage. */
    /* Ignore calls before coverage instrumentation is initialized. */
    if (!index)
        return;
    __shmem->edges[index / 8] |= 1 << (index % 8);
    *guard = 0;
}

void help(void)
{
    printf("QuickJS version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [file [args]]\n"
           "-h  --help         list options\n"
           "-e  --eval EXPR    evaluate EXPR\n"
           "-i  --interactive  go to interactive mode\n"
           "-m  --module       load as ES6 module (default=autodetect)\n"
           "    --script       load as ES6 script (default=autodetect)\n"
           "-I  --include file include an additional file\n"
           "    --std          make 'std' and 'os' available to the loaded script\n"
#ifdef CONFIG_BIGNUM
           "    --bignum       enable the bignum extensions (BigFloat, BigDecimal)\n"
           "    --qjscalc      load the QJSCalc runtime (default if invoked as qjscalc)\n"
#endif
           "-T  --trace        trace memory allocation\n"
           "-d  --dump         dump the memory usage stats\n"
           "    --memory-limit n       limit the memory usage to 'n' bytes\n"
           "    --stack-size n         limit the stack size to 'n' bytes\n"
           "    --unhandled-rejection  dump unhandled promise rejections\n"
           "-q  --quit         just instantiate the interpreter and quit\n");
    exit(1);
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    struct trace_malloc_data trace_data = { NULL };
    int optind;
    char *expr = NULL;
    int interactive = 0;
    int dump_memory = 0;
    int trace_memory = 0;
    int empty_run = 0;
    int module = -1;
    int load_std = 0;
    int dump_unhandled_promise_rejection = 0;
    size_t memory_limit = 0;
    char *include_list[32];
    int i, include_count = 0;
#ifdef CONFIG_BIGNUM
    int load_jscalc;
#endif
    size_t stack_size = 0;
    BOOL reprl_mode = FALSE;
    int status = 0;
    int result = EXIT_SUCCESS;
    
#ifdef CONFIG_BIGNUM
    /* load jscalc runtime if invoked as 'qjscalc' */
    {
        const char *p, *exename;
        exename = argv[0];
        p = strrchr(exename, '/');
        if (p)
            exename = p + 1;
        load_jscalc = !strcmp(exename, "qjscalc");
    }
#endif
    
    /* cannot use getopt because we want to pass the command line to
       the script */
    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        char *arg = argv[optind] + 1;
        const char *longopt = "";
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help();
                continue;
            }
            if (opt == 'e' || !strcmp(longopt, "eval")) {
                if (*arg) {
                    expr = arg;
                    break;
                }
                if (optind < argc) {
                    expr = argv[optind++];
                    break;
                }
                fprintf(stderr, "qjs: missing expression for -e\n");
                exit(2);
            }
            if (opt == 'I' || !strcmp(longopt, "include")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting filename");
                    exit(1);
                }
                if (include_count >= countof(include_list)) {
                    fprintf(stderr, "too many included files");
                    exit(1);
                }
                include_list[include_count++] = argv[optind++];
                continue;
            }
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                continue;
            }
            if (opt == 'm' || !strcmp(longopt, "module")) {
                module = 1;
                continue;
            }
            if (!strcmp(longopt, "script")) {
                module = 0;
                continue;
            }
            if (opt == 'd' || !strcmp(longopt, "dump")) {
                dump_memory++;
                continue;
            }
            if (opt == 'T' || !strcmp(longopt, "trace")) {
                trace_memory++;
                continue;
            }
            if (!strcmp(longopt, "std")) {
                load_std = 1;
                continue;
            }
            if (!strcmp(longopt, "unhandled-rejection")) {
                dump_unhandled_promise_rejection = 1;
                continue;
            }
#ifdef CONFIG_BIGNUM
            if (!strcmp(longopt, "bignum")) {
                bignum_ext = 1;
                continue;
            }
            if (!strcmp(longopt, "qjscalc")) {
                load_jscalc = 1;
                continue;
            }
#endif
            if (opt == 'q' || !strcmp(longopt, "quit")) {
                empty_run++;
                continue;
            }
            if (!strcmp(longopt, "memory-limit")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting memory limit");
                    exit(1);
                }
                memory_limit = (size_t)strtod(argv[optind++], NULL);
                continue;
            }
            if (!strcmp(longopt, "stack-size")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting stack size");
                    exit(1);
                }
                stack_size = (size_t)strtod(argv[optind++], NULL);
                continue;
            }
            if (opt == 'r' || !strcmp(longopt, "reprl")) {
                reprl_mode = TRUE;
                continue;
            }
            if (opt) {
                fprintf(stderr, "qjs: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "qjs: unknown option '--%s'\n", longopt);
            }
            help();
        }
    }

    if (reprl_mode) {
        char helo[] = "HELO";

#ifdef _WIN32
        reprl_open_channels();
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
        if (reprl_write(REPRL_CWFD, helo, 4) != 4 || reprl_read(REPRL_CRFD, helo, 4) != 4) {
            printf("Invalid HELO response from parent\n");
            reprl_mode = FALSE;
        }

        if (memcmp(helo, "HELO", 4) != 0) {
            printf("Invalid response from parent\n");
            _exit(-1);
        }
    }

    if (load_jscalc)
        bignum_ext = 1;

    if (trace_memory) {
        js_trace_malloc_init(&trace_data);
        rt = JS_NewRuntime2(&trace_mf, &trace_data);
    } else {
        rt = JS_NewRuntime();
    }
    if (!rt) {
        fprintf(stderr, "qjs: cannot allocate JS runtime\n");
        exit(2);
    }
    if (memory_limit != 0)
        JS_SetMemoryLimit(rt, memory_limit);
    if (stack_size != 0)
        JS_SetMaxStackSize(rt, stack_size);
    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);
    ctx = JS_NewCustomContext(rt);
    if (!ctx) {
        fprintf(stderr, "qjs: cannot allocate JS context\n");
        exit(2);
    }

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    if (reprl_mode) {
        JS_SetHostPromiseRejectionTracker(rt, reprl_promise_rejection_tracker, NULL);
    } else if (dump_unhandled_promise_rejection) {
        JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker,
                                          NULL);
    }
    
    if (!empty_run) {
#ifdef CONFIG_BIGNUM
        if (load_jscalc) {
            js_std_eval_binary(ctx, qjsc_qjscalc, qjsc_qjscalc_size, 0);
        }
#endif
        js_std_add_helpers(ctx, argc - optind, argv + optind);

        /* make 'std' and 'os' visible to non module code */
        if (load_std) {
            const char *str = "import * as std from 'std';\n"
                "import * as os from 'os';\n"
                "globalThis.std = std;\n"
                "globalThis.os = os;\n";
            eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE);
        }

        for(i = 0; i < include_count; i++) {
            if (eval_file(ctx, include_list[i], module))
                goto fail;
        }

        if (reprl_mode) {
            do {
                uint64_t wire_script_size = 0;
                size_t script_size, remaining;
                char action[4];
                char *script_src, *ptr;

                CHECK(reprl_read(REPRL_CRFD, action, 4) == 4);
                if (!memcmp(action, "exec", 4)) {
                    CHECK(reprl_read(REPRL_CRFD, &wire_script_size, 8) == 8);
                } else {
                    fprintf(stderr, "Unknown REPRL action\n");
                    _exit(-1);
                }
                CHECK(wire_script_size <= 0x1000000);
                script_size = (size_t)wire_script_size;
                script_src = js_malloc(ctx, script_size + 1);
                CHECK(script_src != NULL);

#ifdef _WIN32
                CHECK(_lseeki64(REPRL_DRFD, 0, SEEK_SET) == 0);
#endif
                ptr = script_src;
                remaining = script_size;
                while (remaining > 0) {
                    int rv = reprl_read(REPRL_DRFD, ptr, remaining);
                    if (rv <= 0) {
                        fprintf(stderr, "Failed to load script\n");
                        _exit(-1);
                    }
                    remaining -= rv;
                    ptr += rv;
                }
                script_src[script_size] = '\0';
                reprl_unhandled_rejections = 0;
                if ((result = eval_buf(ctx, script_src, script_size, "reprl", 0)) == 0) {
                    js_std_loop(ctx);
                    if (reprl_unhandled_rejections)
                        result = -1;
                } else {
                    fprintf(stderr, "Failed to eval_buf reprl\n");
                }
                js_free(ctx, script_src);
                js_std_free_handlers(rt);
                JS_FreeContext(ctx);
                JS_FreeRuntime(rt);

                /* Each execution starts with a fresh global object and heap. */
                rt = trace_memory ? JS_NewRuntime2(&trace_mf, &trace_data) : JS_NewRuntime();
                CHECK(rt != NULL);
                if (memory_limit != 0)
                    JS_SetMemoryLimit(rt, memory_limit);
                if (stack_size != 0)
                    JS_SetMaxStackSize(rt, stack_size);
                js_std_init_handlers(rt);
                ctx = JS_NewCustomContext(rt);
                CHECK(ctx != NULL);
                JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);
                JS_SetHostPromiseRejectionTracker(rt, reprl_promise_rejection_tracker, NULL);
#ifdef CONFIG_BIGNUM
                if (load_jscalc)
                    js_std_eval_binary(ctx, qjsc_qjscalc, qjsc_qjscalc_size, 0);
#endif
                js_std_add_helpers(ctx, argc - optind, argv + optind);
                if (load_std) {
                    const char *str = "import * as std from 'std';\n"
                        "import * as os from 'os';\n"
                        "globalThis.std = std;\n"
                        "globalThis.os = os;\n";
                    CHECK(eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE) == 0);
                }
                for (i = 0; i < include_count; i++)
                    CHECK(eval_file(ctx, include_list[i], module) == 0);
                fflush(stdout);
                fflush(stderr);
                /* Send the return code to the parent and reset edge counters. */
                status = (result & 0xff) << 8;
                CHECK(reprl_write(REPRL_CWFD, &status, 4) == 4);
                __sanitizer_cov_reset_edgeguards();
            } while (reprl_mode);
        } else {
            if (expr) {
                if (eval_buf(ctx, expr, strlen(expr), "<cmdline>", 0))
                    goto fail;
            } else
            if (optind >= argc) {
                /* interactive mode */
                interactive = 1;
            } else {
                const char *filename;
                filename = argv[optind];
                if (eval_file(ctx, filename, module))
                    goto fail;
            }
            if (interactive) {
                js_std_eval_binary(ctx, qjsc_repl, qjsc_repl_size, 0);
            }
            js_std_loop(ctx);
        }
    }
    
    if (dump_memory) {
        JSMemoryUsage stats;
        JS_ComputeMemoryUsage(rt, &stats);
        JS_DumpMemoryUsage(stdout, &stats, rt);
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (empty_run && dump_memory) {
        clock_t t[5];
        double best[5];
        int i, j;
        for (i = 0; i < 100; i++) {
            t[0] = clock();
            rt = JS_NewRuntime();
            t[1] = clock();
            ctx = JS_NewContext(rt);
            t[2] = clock();
            JS_FreeContext(ctx);
            t[3] = clock();
            JS_FreeRuntime(rt);
            t[4] = clock();
            for (j = 4; j > 0; j--) {
                double ms = 1000.0 * (t[j] - t[j - 1]) / CLOCKS_PER_SEC;
                if (i == 0 || best[j] > ms)
                    best[j] = ms;
            }
        }
        printf("\nInstantiation times (ms): %.3f = %.3f+%.3f+%.3f+%.3f\n",
               best[1] + best[2] + best[3] + best[4],
               best[1], best[2], best[3], best[4]);
    }
    return 0;
 fail:
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 1;
}
