// test_util.c
#include "test_util.h"
#include <string.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <sys/swap.h>
#include <sys/mman.h>
#include <signal.h>
#include <limits.h>
#include <time.h>


struct swap_info_args {
    void *virtual_address;     // Input: User-space virtual address
    unsigned long offset;      // Output: Swap offset
    int has_swap_info;         // Output: Swap info presence
};
struct folio_info_args {
    unsigned int is_seq;
    void *virtual_address;     // Input: User-space virtual address
    unsigned int is_anon;
    unsigned int is_file;
    unsigned int has_mapping;
    unsigned short memory_cgroup;
};

struct swap_path_args {
    void *virtual_address;
    char path[256];
};

struct anon_vma_cow_folio_args {
    void *virtual_address;
    unsigned long page_anon_vma;
};

struct anon_vma_cow_vma_args {
    void *virtual_address;
    unsigned long vma_anon_vma;
};

struct rmap_count_args {
    void *virtual_address;
    int rmap_count;
};

#define PAGE_SIZE 4096
#define TOTAL_SWAPFILES 232
static int free_swapfile_index = 1;
int is_measuring = 0;
struct timespec start_time;


void set_minimal_swapfile_num(int num){
    if (num < 1 || num > TOTAL_SWAPFILES) {
        fprintf(stderr, "Invalid number of swapfiles: %d. Must be between 1 and %d.\n", num, TOTAL_SWAPFILES);
        exit(EXIT_FAILURE);
    }
    free_swapfile_index = num;
}

pid_t start_ftrace(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork failed");
        return -1;
    }
    char* command = malloc(256);
    snprintf(command, 256, "sudo trace-cmd reset");
    system(command);
    free(command);
    if (pid == 0) {
        // close(STDOUT_FILENO); // Close stdout in the child process
        // close(STDERR_FILENO); // Close stderr in the child process
        // Child process - exec trace-cmd
        char *args[] = {
            "trace-cmd",
            "record",
            "-e",
            "swap:*",
            "-e",
            "vmscan:*",
            NULL
            // "-e",
            // "kmem:folio_get",
            // "-e",
            // "kmem:folio_put",
            // "-e",
            // "kmem:folio_ref_add_unless",
            // "-e",
            // "mmap:*",
            // "-e",
            // "vmalloc:*",
        };
        
        execvp("trace-cmd", args);
        perror("execvp failed");
        exit(1);
    }
    sleep(10); // Give trace-cmd some time to start
    return pid;
}
void stop_ftrace(char* test_name, pid_t pid) {
    kill(pid, SIGINT); // Send SIGINT to stop trace-cmd
    sleep(10); // Wait for trace-cmd to finish
    kill(pid, SIGKILL); // Ensure it is killed
    char* command = malloc(256);
    snprintf(command, 256, "trace-cmd report > %s.trace", test_name);
    system(command);
    free(command);
}


int get_swapfile_path(void *addr, char *path_out) {
    struct swap_path_args args = {0};
    args.virtual_address = addr;
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    if (ioctl(fd, IOCTL_GET_SWAPFILE_PATH, &args) < 0) {
        perror("Failed to get swapfile path");
        close(fd);
        return -1;
    }
    close(fd);
    strncpy(path_out, args.path, 256);
    return 0;
}

unsigned long get_anon_vma_folio(void *addr) {
    struct anon_vma_cow_folio_args args = {0};
    args.virtual_address = addr;
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 0; // Or handle error appropriately
    }
    if (ioctl(fd, IOCTL_GET_ANON_VMA_FOLIO, &args) < 0) {
        perror("Failed to get VMA info");
        close(fd);
        return 0; // Or handle error appropriately
    }
    close(fd);
    return args.page_anon_vma; // Return the unsigned long directly
}

unsigned long get_anon_vma_vma(void *addr) {
    struct anon_vma_cow_vma_args args = {0};
    args.virtual_address = addr;
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 0; // Or handle error appropriately
    }
    if (ioctl(fd, IOCTL_GET_ANON_VMA_VMA, &args) < 0) {
        perror("Failed to get VMA info");
        close(fd);
        return 0; // Or handle error appropriately
    }
    close(fd);
    return args.vma_anon_vma; // Return the unsigned long directly
}
int get_rmap_count(void *addr) {
    struct rmap_count_args args = {0};
    args.virtual_address = addr;
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    if (ioctl(fd, IOCTL_GET_RMAP_COUNT, &args) < 0) {
        perror("Failed to get rmap count");
        close(fd);
        return -1;
    }
    close(fd);
    return args.rmap_count; // Return the rmap count directly
}


void mkswap(const char *filename){
    char command[256];
    snprintf(command, sizeof(command), "mkswap %s > /dev/null 2>&1", filename);
    system(command);
    // check for errors
}
void enable_swap(const char *filename, int swap_flags) {
    // printf("Enabling swap on %s\n", filename);
    int ret = syscall(SYS_swapon, filename, swap_flags);
    if (ret < 0) {
        perror("swapon");
    }
}
void disable_swap(const char *filename) {
    // printf("Disabling swap on %s\n", filename);
    if (swapoff(filename) < 0) {
        perror("Failed to disable swap");
        exit(EXIT_FAILURE);
    }
}
void make_swaps(int num_swapfiles, int swap_flags) {
    if (free_swapfile_index == TOTAL_SWAPFILES) {
        fprintf(stderr, "Reached maximum number of swapfiles (%d)\n", TOTAL_SWAPFILES);
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < num_swapfiles; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/scratch/vma_swaps/swapfile_%d.swap", i+free_swapfile_index);
        // char* filename = "/tmp/tempfile.1073741824";
        mkswap(filename);
        enable_swap(filename,swap_flags);
    }
}


#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// Simple version matching user's original intent
int evict_mem(int pages) {
    printf("Simple eviction test - stopping at first error...\n");
    int cgroup_id = get_current_memcg_id_fs();
    
    usleep(50000); // 50ms delay
    for (int node = 0; node<2; node++){
        // age the working set
        char command[512];
        
        // Build command exactly like the user's manual test
        snprintf(command, sizeof(command), 
                "echo \"+ %d %d %d\" > /sys/kernel/debug/lru_gen", cgroup_id, node, 3);
        
        printf("Testing aging ");
        fflush(stdout);
        
        int status = system(command);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("SUCCESS\n");
        } else {
            printf("FAILED (Invalid argument)\n");
        }
        for (int gen = 0; gen < 100; gen++) {
            char command[512];
            
            // Build command exactly like the user's manual test
            snprintf(command, sizeof(command), 
                    "echo \"- %d %d %d 200\" > /sys/kernel/debug/lru_gen", cgroup_id, node, gen);
            
            printf("Testing generation %d... ", gen);
            fflush(stdout);
            
            int status = system(command);
            
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                printf("SUCCESS\n");
            } else {
                printf("FAILED (Invalid argument)\n");
                printf("Maximum valid generation: %d\n", gen - 1);
                return gen;
            }
            
            usleep(50000); // 50ms delay
        }
    }
    return 100;
}

int swapout_page(void *addr) {
    void* aligned_page = (void*)((unsigned long)addr - (unsigned long)addr % PAGE_SIZE);
    // printf("Swapping out page at address %p aligned page %p\n", addr, aligned_page);
    if (madvise(aligned_page, PAGE_SIZE, MADV_PAGEOUT) < 0) {
        perror("madvise");
        return -1;
    }
    return 0;
}

int swapout_pages(void *addr, unsigned long long pages) {
    void* aligned_page = (void*)((unsigned long)addr - (unsigned long)addr % PAGE_SIZE);
    // printf("Swapping out page at address %p aligned page %p\n", addr, aligned_page);
    if (madvise(aligned_page, PAGE_SIZE * pages, MADV_PAGEOUT) < 0) {
        perror("madvise");
        return -1;
    }
    return 0;
}
void* map_large_anon_region(unsigned long long size) {
    if (size % (unsigned long long)PAGE_SIZE != 0) {
        fprintf(stderr, "Size must be a multiple of PAGE_SIZE (%d)\n", PAGE_SIZE);
        return NULL;
    }
    unsigned long long sz_in_pages = (size / (unsigned long long)PAGE_SIZE);
    char *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    for (unsigned long long i = 0; i < sz_in_pages; i++) {
        *(unsigned long long *)(addr+(i * PAGE_SIZE)) = i;
    }
    return addr;
}
void* map_anon_region(size_t size) {
    if (size % PAGE_SIZE != 0) {
        fprintf(stderr, "Size must be a multiple of PAGE_SIZE (%d)\n", PAGE_SIZE);
        return NULL;
    }
    char sz_in_pages = (size / PAGE_SIZE);
    char *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = (i+1)*42; //should we not do on i = 0? the KSM problem
    }
    return addr;
}
struct vma_info_args get_vma_info(void *addr) {
    struct vma_info_args args = {0};
    args.virtual_address = addr;
    if (ioctl(open(DEVICE, O_RDONLY), IOCTL_VMA_INFO, &args) < 0) {
        perror("Failed to get VMA info");
        return args; // Return empty args on error
    }
    return args;
}

int disable_swaps() {
    FILE *fp;
    char line[256];
    char filename[256];
    char type[32];
    int size, used, priority;
    int min_available_index = 1;
    
    // Open /proc/swaps
    fp = fopen("/proc/swaps", "r");
    if (fp == NULL) {
        perror("Failed to open /proc/swaps");
        return -1;
    }
    
    // Skip the header line
    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "Error reading /proc/swaps header\n");
        fclose(fp);
        return -1;
    }
    
    // Process each swap entry
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Parse the line
        if (sscanf(line, "%255s %31s %d %d %d", filename, type, &size, &used, &priority) != 5) {
            continue; // Skip malformed lines
        }
        // printf("line: %s\n", line);
        // printf("Processing: %s (used: %d)\n", filename, used);
        char *underscore = strrchr(filename, '_');
        char *dot = strrchr(filename, '.');
        int index = INT_MAX;
        if (underscore && dot && underscore < dot) {
            index = atoi(underscore + 1);
        }
        
        // If used is 0, turn off the swapfile
        if (used == 0) {
            // printf("Turning off unused swapfile: %s\n", filename);
            disable_swap(filename);
            // Extract index from filename
            // Look for pattern like "swapfile_X.swap"
            
        }
        else{
            // printf("Swapfile %s is in use, index: %d. min_available_index: %d\n", filename, index, min_available_index);

            if (index+1 > min_available_index) {
                min_available_index = index + 1; // Increment to find the next available index
            }
        }
    }
    
    fclose(fp);
    // printf("Minimal available swapfile index: %d\n", min_available_index);
    return min_available_index;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Step 1: Read /proc/self/cgroup and extract cgroup path
int get_cgroup_path(char *cgroup_path, size_t max_len) {
    FILE *fp;
    char line[1024];
    int pid = getpid();
    char cgroup_file[256];
    snprintf(cgroup_file, sizeof(cgroup_file), "/proc/%d/cgroup", pid);
    fp = fopen(cgroup_file, "r");
    if (!fp) {
        perror("Failed to open cgroup file: /proc/self/cgroup");
        return -1;
    }
    
    // printf("=== Reading %s ===\n",cgroup_file);
    
    while (fgets(line, sizeof(line), fp)) {
        // printf("Raw: %s", line);
        
        // Parse format: hierarchy-ID:controller-list:cgroup-path
        char *first_colon = strchr(line, ':');
        if (!first_colon) continue;
        
        char *second_colon = strchr(first_colon + 1, ':');
        if (!second_colon) continue;
        
        // Extract everything after the second colon
        char *path_start = second_colon + 1;
        
        // Remove trailing newline
        char *newline = strchr(path_start, '\n');
        if (newline) *newline = '\0';
        
        // Copy the path
        strncpy(cgroup_path, path_start, max_len - 1);
        cgroup_path[max_len - 1] = '\0';
        
        // printf("Extracted cgroup path: '%s'\n", cgroup_path);
        fclose(fp);
        return 0;
    }
    
    fclose(fp);
    printf("ERROR: Could not find cgroup path\n");
    return -1;
}

// Step 2: Parse /sys/kernel/debug/lru_gen to find matching memcg ID
int find_memcg_id(const char *target_path) {
    FILE *fp;
    char line[1024];
    int memcg_id = -1;
    
    fp = fopen("/sys/kernel/debug/lru_gen", "r");
    if (!fp) {
        perror("Failed to open /sys/kernel/debug/lru_gen");
        return -1;
    }
    
    // printf("\n=== Parsing /sys/kernel/debug/lru_gen ===\n");
    // printf("Looking for path: '%s'\n\n", target_path);
    
    while (fgets(line, sizeof(line), fp)) {
        // Look for lines starting with "memcg"
        if (strncmp(line, "memcg", 5) == 0) {
            int parsed_id;
            char parsed_path[512];
            
            // Parse format: "memcg   <ID> <path>"
            if (sscanf(line, "memcg %d %s", &parsed_id, parsed_path) == 2) {
                // printf("Found memcg %d %s\n", parsed_id, parsed_path);
                
                // Check if this path matches our target
                if (strcmp(parsed_path, target_path) == 0) {
                    // printf("*** MATCH FOUND ***\n");
                    // printf("Memcg ID: %d\n", parsed_id);
                    // printf("Path: %s\n", parsed_path);
                    memcg_id = parsed_id;
                    // Don't break - continue to show all entries
                }
            }
        }
    }
    
    fclose(fp);
    
    if (memcg_id == -1) {
        printf("\nERROR: No matching memcg found for path '%s'\n", target_path);
        printf("Available memcg entries were listed above.\n");
    }
    
    return memcg_id;
}

// Complete function that does steps 1-4
int get_current_memcg_id_fs(void) {
    char cgroup_path[512];
    
    // Step 1 & 2: Get cgroup path from /proc/self/cgroup
    if (get_cgroup_path(cgroup_path, sizeof(cgroup_path)) < 0) {
        return -1;
    }
    
    // Step 3 & 4: Find matching memcg ID in lru_gen
    int memcg_id = find_memcg_id(cgroup_path);
    printf("Current memcg ID: %d\n", memcg_id);
    return memcg_id;
}
int create_tempfile(size_t size) {
    char template[] = "/tmp/tempfile.XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    // Unlink the file so it is removed after closing
    unlink(template);
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    // drop caches
    return fd;
}
void drop_caches() {
    system("echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null");
}