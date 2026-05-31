#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h" // for make_swaps()

#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"

typedef void (*test_func_t)(void);

typedef struct test_case {
    char *name;
    test_func_t func;
    struct test_case *next;
} test_case_t;

static test_case_t *test_list_head = NULL;
static test_case_t *perf_test_list_head = NULL; // For performance tests
static test_case_t *memory_test_list_head = NULL; // For memory-limited tests
static int current_test_failed = 0;  // Track current test failure state

#define REGISTER_TEST(test_name) \
    void test_name(void); \
    __attribute__((constructor)) static void register_##test_name(void) { \
        static test_case_t test = { #test_name, test_name, NULL }; \
        test.next = test_list_head; \
        test_list_head = &test; \
    }
#define REGISTER_PERF_TEST(test_name) \
    void test_name(void); \
    __attribute__((constructor)) static void register_perf_##test_name(void) { \
        static test_case_t test = { #test_name, test_name, NULL }; \
        test.next = perf_test_list_head; \
        perf_test_list_head = &test; \
    }

#define REGISTER_MEMORY_TEST(test_name, memory_limit) \
    void test_name(void); \
    __attribute__((constructor)) static void register_memory_##test_name(void) { \
        static test_case_t test = { #test_name ":" memory_limit, test_name, NULL }; \
        test.next = memory_test_list_head; \
        memory_test_list_head = &test; \
    }

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s\n", \
                __FILE__, __LINE__, #expr); \
        current_test_failed = 1; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s == %s (%lx != %lx)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long)(a), (unsigned long)(b)); \
        current_test_failed = 1; \
    } else { \
        fprintf(stderr, COLOR_GREEN "PASS" COLOR_RESET " [%s:%d] %s == %s (%lx == %lx)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long)(a), (unsigned long)(b)); \
    } \
    fflush(stderr); \
} while (0)

#define ASSERT_NEQ(a, b) do { \
    if ((a) == (b)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s != %s (%lx == %lx)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long)(a), (unsigned long)(b)); \
        current_test_failed = 1; \
    } else { \
        fprintf(stderr, COLOR_GREEN "PASS" COLOR_RESET " [%s:%d] %s != %s (%lx != %lx)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long)(a), (unsigned long)(b)); \
    } \
    fflush(stderr); \
} while (0)
// should work on uint_64
#define ASSERT_ABOVE(a, b) do { \
    if ((a) <= (b)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s > %s (%llu <= %llu)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long long)(a), (unsigned long long)(b)); \
        current_test_failed = 1; \
    } \
} while (0)
#define ASSERT_BELOW(a, b) do { \
    if ((a) >= (b)) { \
        fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET " [%s:%d] %s < %s (%lu >= %lu)\n", \
                __FILE__, __LINE__, #a, #b, (unsigned long long)(a), (unsigned long long)(b)); \
        current_test_failed = 1; \
    } \
} while (0)

static inline int run_all_tests(int enable_traces) {
    int count = 0;
    int passed = 0;
    test_case_t *t = test_list_head;
    pid_t pid = 0;
    set_minimal_swapfile_num(disable_swaps()); // Disable all swaps before running tests
    
    while (t) {
        fprintf(stderr, COLOR_YELLOW "RUNNING" COLOR_RESET " %s\n", t->name);
        fflush(stdout);
        
        // Reset failure state for this test
        current_test_failed = 0;
        
        if (enable_traces) 
            pid = start_ftrace(); // Start ftrace if needed
            
        t->func();  
        
        set_minimal_swapfile_num(disable_swaps()); // Disable all swaps before running tests
        
        if (enable_traces) 
            stop_ftrace(t->name, pid); // Stop ftrace if needed
        
        // Check if test passed or failed
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

static inline void run_perf_tests(int enable_traces) {
    fprintf(stderr, "Running performance tests...\n");
    int count = 0;
    int passed = 0;
    test_case_t *t = perf_test_list_head;
    pid_t pid = 0;
    while (t) {
        fprintf(stderr, COLOR_YELLOW "RUNNING" COLOR_RESET " %s\n", t->name);
        fflush(stdout);
        
        // Reset failure state for this test
        current_test_failed = 0;
        if(enable_traces) {
            pid = start_ftrace(); // Start ftrace if needed
        }
        t->func();
        if(enable_traces) {
            stop_ftrace(t->name, pid); // Stop ftrace if needed
        }
        
        // Check if test passed or failed
        if (current_test_failed) {
            fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET "    %s\n", t->name);
        } else {
            fprintf(stderr, COLOR_GREEN "PASS" COLOR_RESET "    %s\n", t->name);
            passed++;
        }
        
        count++;
        t = t->next;
    }
    fprintf(stderr, "Performance Summary: %d/%d tests passed\n", passed, count);

}

// Create a temporary C file that contains only one memory test
static inline int create_temp_test_file(const char* test_name, char* temp_filename) {
    test_case_t *t = memory_test_list_head;
    
    // Find the specific test
    while (t && strcmp(t->name, test_name) != 0) {
        t = t->next;
        printf("Looking for test %s, currently at %s\n", test_name, t ? t->name : "NULL");
    }
    
    if (!t) {
        fprintf(stderr, COLOR_RED "ERROR" COLOR_RESET " Memory test '%s' not found\n", test_name);
        return -1;
    }
    
    // Create temporary filename
    snprintf(temp_filename, 256, "./memory_test_%s_%d.c", test_name, getpid());
    
    FILE *temp_file = fopen(temp_filename, "w");
    if (!temp_file) {
        perror("Failed to create temporary test file");
        return -1;
    }
    char clean_test_name[256];
    strncpy(clean_test_name, test_name, sizeof(clean_test_name));
    char *colon_pos = strchr(clean_test_name, ':');
    if (colon_pos) {
        *colon_pos = '\0';  // Null-terminate test name
    }
    printf("Creating temp test file %s for test %s\n", temp_filename, clean_test_name);
    // Write the minimal test file
    fprintf(temp_file, 
        "#include \"test_framework.h\"\n"
        "#include \"test_util.h\"\n"
        "#include <sys/mman.h>\n"
        "#include <unistd.h>\n"
        "#include <string.h>\n"
        "\n"
        "// External declaration - the actual test function will be linked\n"
        "extern void %s(void);\n"
        "\n"
        "int main(int argc, char *argv[]) {\n"
        "    \n"
        "    %s();\n"
        "    \n"
        "    return 0;\n"
        "}\n",
        clean_test_name, clean_test_name, clean_test_name);
    
    fclose(temp_file);
    return 0;
}

static inline void run_memory_tests(int enable_traces) {
    int count = 0;
    int passed = 0;
    test_case_t *t = memory_test_list_head;
    set_minimal_swapfile_num(disable_swaps()); // Disable all swaps before running tests

    while (t) {
        // Extract memory limit from test name (format: "test_name:memory_limit")
        char *test_name_copy = strdup(t->name);
        char *test_name = test_name_copy;
        char *memory_limit = strchr(test_name_copy, ':');
        if (memory_limit) {
            memory_limit++;        // Point to memory limit string
        } else {
            memory_limit = "64M";  // Default fallback
        }
        
        fprintf(stderr, COLOR_YELLOW "RUNNING" COLOR_RESET " %s (memory limit: %s)\n", test_name, memory_limit);
        fflush(stdout);
        
        // Create temporary test file
        char temp_filename[256];
        char temp_binary[256];
        if (create_temp_test_file(test_name, temp_filename) != 0) {
            fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET "    %s (failed to create temp file)\n", test_name);
            count++;
            free(test_name_copy);
            t = t->next;
            continue;
        }
        
        // Create temporary binary name
        snprintf(temp_binary, sizeof(temp_binary), "./memory_test_%s_%d", test_name, getpid());
        
        // Compile the temporary test
        char compile_cmd[1024];
        snprintf(compile_cmd, sizeof(compile_cmd), 
                "cc -Wall -Wextra -g -c -DCOMPILE_TESTS_ONLY test.c -o test_funcs.o 2>/dev/null && "
                "cc -Wall -Wextra -g -o %s %s test_funcs.o test_util.c -I. 2>/dev/null",
                temp_binary, temp_filename);
        
        int compile_result = system(compile_cmd);
        if (compile_result != 0) {
            fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET "    %s (compilation failed)\n", test_name);
            unlink(temp_filename);
            count++;
            free(test_name_copy);
            t = t->next;
            continue;
        }
        
        // Create systemd slice for memory limitation
        char slice_name[256];
        snprintf(slice_name, sizeof(slice_name), "test-%s.slice", test_name);
        
        // Build the full command to run the test under systemd
        char full_cmd[1024];
        snprintf(full_cmd, sizeof(full_cmd),
                "sudo systemd-run --slice=%s --property=MemoryMax=%s --property=MemorySwapMax=20G --wait --pty -- %s",
                slice_name, memory_limit, temp_binary);
        
        fprintf(stderr, "Executing: %s\n", full_cmd);
        int pid;
        if (enable_traces) 
            pid = start_ftrace(); // Start ftrace if needed
        make_swaps(100, 0); 
        drop_caches();
        sleep(1);
        int result = system(full_cmd);
        sleep(1);
        drop_caches();
        set_minimal_swapfile_num(disable_swaps()); // Disable all swaps before running tests
        if (enable_traces) 
            stop_ftrace(test_name, pid); // Stop ftrace if needed
        // Clean up the slice
        char cleanup_cmd[256];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "sudo systemctl stop %s 2>/dev/null", slice_name);
        system(cleanup_cmd);
        
        // Clean up temporary files
        unlink(temp_filename);
        unlink(temp_binary);
        
        // Check if test passed or failed
        if (result != 0) {
            fprintf(stderr, COLOR_RED "FAIL" COLOR_RESET "    %s (exit code: %d)\n", test_name, result);
        } else {
            fprintf(stderr, COLOR_GREEN "PASS" COLOR_RESET "    %s\n", test_name);
            passed++;
        }
        
        count++;
        free(test_name_copy);
        t = t->next;
    }
    fprintf(stderr, "Memory Test Summary: %d/%d tests passed\n", passed, count);
}

#endif // TEST_FRAMEWORK_H