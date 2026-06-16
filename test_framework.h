#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include "test_util.h"

#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"

typedef void (*test_func_t)(void);

typedef struct test_case {
    const char *name;
    test_func_t func;
    struct test_case *next;
} test_case_t;

static test_case_t *test_list_head = NULL;
static int current_test_failed = 0;

typedef struct assert_signal_ctx {
    int expected_signal;
    const char *file;
    int line;
    pid_t pid;
    int done;
} assert_signal_ctx_t;

#define REGISTER_TEST(test_name) \
    void test_name(void); \
    __attribute__((constructor)) static void register_##test_name(void) { \
        static test_case_t test = { #test_name, test_name, NULL }; \
        test.next = test_list_head; \
        test_list_head = &test; \
    }

static inline assert_signal_ctx_t assert_signal_start(int expected_signal,
                                                      const char *file,
                                                      int line) {
    assert_signal_ctx_t ctx = {
        .expected_signal = expected_signal,
        .file = file,
        .line = line,
        .pid = -1,
        .done = 0,
    };

    fflush(NULL);
    ctx.pid = fork();
    if (ctx.pid < 0) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_SIGNAL(%d): fork failed\n",
                file, line, expected_signal);
        current_test_failed = 1;
        ctx.done = 1;
    }

    return ctx;
}

static inline void assert_signal_finish(assert_signal_ctx_t *ctx) {
    int status;

    if (ctx->pid == 0)
        _exit(EXIT_SUCCESS);

    if (waitpid(ctx->pid, &status, 0) < 0) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_SIGNAL(%d): waitpid failed\n",
                ctx->file, ctx->line, ctx->expected_signal);
        current_test_failed = 1;
        ctx->done = 1;
        return;
    }

    if (!WIFSIGNALED(status)) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_SIGNAL(%d): no signal thrown\n",
                ctx->file, ctx->line, ctx->expected_signal);
        current_test_failed = 1;
    } else if (WTERMSIG(status) != ctx->expected_signal) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_SIGNAL(%d): got signal %d\n",
                ctx->file, ctx->line, ctx->expected_signal, WTERMSIG(status));
        current_test_failed = 1;
    }

    ctx->done = 1;
}

static inline const char *anon_vma_source_name(enum anon_vma_info_source source) {
    switch (source) {
    case ANON_VMA_FOLIO:
        return "folio";
    case ANON_VMA_VMA:
        return "vma";
    default:
        return "unknown";
    }
}

static inline struct anon_vma_info_args get_anon_vma_info_from_source(
        enum anon_vma_info_source source, void *addr) {
    switch (source) {
    case ANON_VMA_FOLIO:
        return get_anon_vma_info(addr);
    case ANON_VMA_VMA:
        return get_anon_vma_info_from_vma(addr);
    default: {
        struct anon_vma_info_args empty = {0};
        return empty;
    }
    }
}

static inline void assert_eq_anon_vma_impl(
        enum anon_vma_info_source left_source, void *left_addr,
        enum anon_vma_info_source right_source, void *right_addr,
        const char *left_expr, const char *right_expr,
        const char *file, int line) {
    struct anon_vma_info_args left =
        get_anon_vma_info_from_source(left_source, left_addr);
    struct anon_vma_info_args right =
        get_anon_vma_info_from_source(right_source, right_addr);

    if (left.anon_vma != right.anon_vma ||
        left.root != right.root ||
        left.parent != right.parent ||
        left.refcount != right.refcount ||
        left.num_children != right.num_children ||
        left.num_active_vmas != right.num_active_vmas ||
        left.named_swap_file != right.named_swap_file) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_EQ_ANON_VMA(%s, %s)\n"
                "  left source:       %s\n"
                "  right source:      %s\n"
                "  left anon_vma:     %p\n"
                "  right anon_vma:    %p\n"
                "  left root:         %p\n"
                "  right root:        %p\n"
                "  left parent:       %p\n"
                "  right parent:      %p\n"
                "  left refcount:     %lu\n"
                "  right refcount:    %lu\n"
                "  left children:     %lu\n"
                "  right children:    %lu\n"
                "  left active vmas:  %lu\n"
                "  right active vmas: %lu\n"
                "  left swap file:    %p\n"
                "  right swap file:   %p\n",
                file, line, left_expr, right_expr,
                anon_vma_source_name(left_source),
                anon_vma_source_name(right_source),
                left.anon_vma, right.anon_vma,
                left.root, right.root,
                left.parent, right.parent,
                left.refcount, right.refcount,
                left.num_children, right.num_children,
                left.num_active_vmas, right.num_active_vmas,
                left.named_swap_file, right.named_swap_file);
        current_test_failed = 1;
    }
}

static inline void assert_neq_anon_vma_impl(
        enum anon_vma_info_source left_source, void *left_addr,
        enum anon_vma_info_source right_source, void *right_addr,
        const char *left_expr, const char *right_expr,
        const char *file, int line) {
    struct anon_vma_info_args left =
        get_anon_vma_info_from_source(left_source, left_addr);
    struct anon_vma_info_args right =
        get_anon_vma_info_from_source(right_source, right_addr);

    if (left.anon_vma == right.anon_vma) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] ASSERT_NEQ_ANON_VMA(%s, %s)\n"
                "  left source:    %s\n"
                "  right source:   %s\n"
                "  left anon_vma:  %p\n"
                "  right anon_vma: %p\n",
                file, line, left_expr, right_expr,
                anon_vma_source_name(left_source),
                anon_vma_source_name(right_source),
                left.anon_vma, right.anon_vma);
        current_test_failed = 1;
    }
}

#define ASSERT_SIGNAL(expected_signal) \
    for (assert_signal_ctx_t _assert_signal_ctx = \
             assert_signal_start((expected_signal), __FILE__, __LINE__); \
         !_assert_signal_ctx.done; \
         assert_signal_finish(&_assert_signal_ctx)) \
        if (_assert_signal_ctx.pid == 0)

#define ASSERT_EQ_ANON_VMA(left_source, left_addr, right_source, right_addr) \
    assert_eq_anon_vma_impl((left_source), (void *)(left_addr), \
                            (right_source), (void *)(right_addr), \
                            #left_addr, #right_addr, __FILE__, __LINE__)

#define ASSERT_NEQ_ANON_VMA(left_source, left_addr, right_source, right_addr) \
    assert_neq_anon_vma_impl((left_source), (void *)(left_addr), \
                             (right_source), (void *)(right_addr), \
                             #left_addr, #right_addr, __FILE__, __LINE__)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s\n", \
                __FILE__, __LINE__, #expr); \
        current_test_failed = 1; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    __auto_type _assert_a = (a); \
    __auto_type _assert_b = (b); \
    unsigned long long _assert_a_hex = (unsigned long long)_assert_a; \
    unsigned long long _assert_b_hex = (unsigned long long)_assert_b; \
    if (_assert_a_hex != _assert_b_hex) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET \
                " [%s:%d] %s == %s\n" \
                "  left:  0x%llx\n" \
                "  right: 0x%llx\n", \
                __FILE__, __LINE__, #a, #b, \
                _assert_a_hex, _assert_b_hex); \
        current_test_failed = 1; \
    } \
} while (0)

#define ASSERT_EQ_AT(p, expected) do { \
    __auto_type _assert_p = (p); \
    __auto_type _assert_val = *_assert_p; \
    __auto_type _assert_exp = (expected); \
    unsigned long long _assert_v = (unsigned long long)_assert_val; \
    unsigned long long _assert_e = (unsigned long long)_assert_exp; \
    if (_assert_v != _assert_e) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET \
                " [%s:%d] *(%s) == %s\n" \
                "  address: %p\n" \
                "  read:    0x%llx\n" \
                "  expect:  0x%llx\n", \
                __FILE__, __LINE__, #p, #expected, \
                (void *)_assert_p, _assert_v, _assert_e); \
        fflush(stderr); \
        current_test_failed = 1; \
    } \
} while (0)

static inline unsigned long assert_named_swap_file_for_addr_impl(
        void *addr, const char *addr_expr, const char *file, int line) {
    struct swap_file_info swap_file_info = get_swap_file_info(addr);
    unsigned long index = 0;
    char expected_path[PATH_MAX];
    int parsed;
    int access_ret;
    int access_errno;

    parsed = parse_named_swap_index(swap_file_info.path, &index);
    named_swap_path_for_index(expected_path, sizeof(expected_path), index);
    errno = 0;
    access_ret = access(expected_path, F_OK);
    access_errno = errno;

    if (swap_file_info.path[0] == '\0' ||
        !parsed ||
        strcmp(swap_file_info.path, expected_path) != 0 ||
        access_ret != 0) {
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET
                " [%s:%d] named swap file for %s\n"
                "  address:            %p\n"
                "  returned path:      \"%s\"\n"
                "  expected prefix:    \"" NAMED_SWAP_PREFIX "\"\n"
                "  parsed index:       %s\n"
                "  index value:        %lu\n"
                "  reconstructed path: \"%s\"\n"
                "  access result:      %d\n"
                "  access errno:       %d (%s)\n",
                file, line, addr_expr, addr,
                swap_file_info.path, parsed ? "yes" : "no", index,
                expected_path, access_ret, access_errno,
                strerror(access_errno));
        current_test_failed = 1;
    }

    return index;
}

#define assert_named_swap_file_for_addr(addr) \
    assert_named_swap_file_for_addr_impl((addr), #addr, __FILE__, __LINE__)

#define ASSERT_NEQ(a, b) do { \
    __auto_type _assert_a = (a); \
    __auto_type _assert_b = (b); \
    unsigned long long _assert_a_hex = (unsigned long long)_assert_a; \
    unsigned long long _assert_b_hex = (unsigned long long)_assert_b; \
    if (_assert_a_hex == _assert_b_hex) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET \
                " [%s:%d] %s != %s\n" \
                "  left:  0x%llx\n" \
                "  right: 0x%llx\n", \
                __FILE__, __LINE__, #a, #b, \
                _assert_a_hex, _assert_b_hex); \
        current_test_failed = 1; \
    } \
} while (0)

#define ASSERT_ABOVE(a, b) do { \
    __auto_type _assert_a = (a); \
    __auto_type _assert_b = (b); \
    unsigned long long _assert_a_hex = (unsigned long long)_assert_a; \
    unsigned long long _assert_b_hex = (unsigned long long)_assert_b; \
    if (_assert_a_hex <= _assert_b_hex) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET \
                " [%s:%d] %s > %s\n" \
                "  left:  0x%llx\n" \
                "  right: 0x%llx\n", \
                __FILE__, __LINE__, #a, #b, \
                _assert_a_hex, _assert_b_hex); \
        current_test_failed = 1; \
    } \
} while (0)

static inline int run_all_tests(int enable_traces) {
    int count = 0;
    int passed = 0;
    test_case_t *t = test_list_head;

    while (t) {
        pid_t trace_pid = 0;
        fprintf(stderr, COLOR_YELLOW "RUNNING" COLOR_RESET " %s\n", t->name);
        fflush(stderr);
        current_test_failed = 0;

        if (enable_traces)
            trace_pid = start_ftrace();

        t->func();

        if (enable_traces)
            stop_ftrace((char *)t->name, trace_pid);

        if (current_test_failed) {
            fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET "    %s\n", t->name);
        } else {
            fprintf(stderr, COLOR_GREEN "PASS" COLOR_RESET "    %s\n", t->name);
            passed++;
        }

        count++;
        t = t->next;
    }

    fprintf(stderr, "Summary: %d/%d tests passed\n", passed, count);
    return (passed == count) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
