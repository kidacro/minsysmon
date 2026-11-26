#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>

#define SYSMON_VERSION "0.1.0"
#define MAX_PROCS 100
#define MIN_DISPLAY_PROCS 1
#define MAX_DISPLAY_PROCS 20
#define DEFAULT_DISPLAY_PROCS 3

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int sig) {
    (void)sig;
    stop_requested = 1;
}

// Function to get the hostname
char* get_hostname(void) {
    struct utsname name;
    if (uname(&name) == 0) {
        return strdup(name.nodename);
    }
    return strdup("Unknown Hostname");
}

// Function to get uptime in D/H/M/S from /proc/uptime
void get_uptime(long long* days, long long* hours, long long* minutes, long long* seconds) {
    FILE *fp;
    double uptime_seconds_double;
    
    // /proc/uptime contains two numbers: total uptime, and idle time. We only need the first one.
    fp = fopen("/proc/uptime", "r");
    if (fp == NULL) {
        *days = *hours = *minutes = *seconds = 0;
        return;
    }
    
    if (fscanf(fp, "%lf", &uptime_seconds_double) != 1) {
        fclose(fp);
        *days = *hours = *minutes = *seconds = 0;
        return;
    }
    fclose(fp);

    long long total_seconds = (long long)uptime_seconds_double;

    // Calculate D/H/M/S
    *days = total_seconds / 86400;
    total_seconds %= 86400;
    
    *hours = total_seconds / 3600;
    total_seconds %= 3600;
    
    *minutes = total_seconds / 60;
    *seconds = total_seconds % 60;
}
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CPUStats;

CPUStats get_cpu_stats(void) {
    CPUStats stats;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        stats.user = stats.nice = stats.system = stats.idle = 
        stats.iowait = stats.irq = stats.softirq = stats.steal = 0;
        return stats;
    }
    if (fscanf(fp, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
               &stats.user, &stats.nice, &stats.system, &stats.idle,
               &stats.iowait, &stats.irq, &stats.softirq, &stats.steal) != 8) {
        memset(&stats, 0, sizeof(stats));
    }
    fclose(fp);
    return stats;
}

static CPUStats prev_stats;
static double last_process_sample_interval = 0.0;

double get_cpu_usage(void) {
    CPUStats curr_stats = get_cpu_stats();
    
    unsigned long long prev_idle = prev_stats.idle + prev_stats.iowait;
    unsigned long long curr_idle = curr_stats.idle + curr_stats.iowait;
    
    unsigned long long prev_non_idle = prev_stats.user + prev_stats.nice + prev_stats.system + prev_stats.irq + prev_stats.softirq + prev_stats.steal;
    unsigned long long curr_non_idle = curr_stats.user + curr_stats.nice + curr_stats.system + curr_stats.irq + curr_stats.softirq + curr_stats.steal;
    
    unsigned long long prev_total = prev_idle + prev_non_idle;
    unsigned long long curr_total = curr_idle + curr_non_idle;
    
    unsigned long long total_diff = curr_total - prev_total;
    unsigned long long idle_diff = curr_idle - prev_idle;
    
    prev_stats = curr_stats;
    
    if (total_diff == 0) return 0.0;
    
    return 100.0 * (1.0 - (double)idle_diff / (double)total_diff);
}

typedef struct {
    unsigned long long total;
    unsigned long long available;
    double usage;
} MemoryStats;

typedef struct {
    int pid;
    char *comm;
    unsigned long long utime;
    unsigned long long stime;
    unsigned long long prev_utime;
    unsigned long long prev_stime;
    double cpu_usage;
} ProcessCPUStats;

MemoryStats get_memory_stats(void) {
    MemoryStats stats = {0, 0, 0.0};
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) return stats;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %llu kB", &stats.total);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line, "MemAvailable: %llu kB", &stats.available);
        }
    }
    fclose(fp);

    if (stats.total > 0) {
        stats.usage = (double)(stats.total - stats.available) / stats.total * 100.0;
    }

    return stats;
}

// Returns 1 for kernel threads, 0 for user-space processes.
static int is_kernel_thread(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *fp = fopen(path, "r");
    if (fp != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Kthread:", 8) == 0) {
                int kthread = 0;
                sscanf(line + 8, "%d", &kthread);
                fclose(fp);
                return kthread != 0;
            }
        }
        fclose(fp);
    }

    // Fallback for kernels without the Kthread field: kernel threads have
    // no executable linked in /proc/<pid>/exe.
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    char exe[PATH_MAX];
    return readlink(path, exe, sizeof(exe) - 1) < 0;
}

// Function to get process CPU stats from /proc/[pid]/stat
ProcessCPUStats *get_process_info(int max_procs) {
    ProcessCPUStats *processes = calloc(max_procs, sizeof(ProcessCPUStats));
    if (processes == NULL) return NULL;
    
    int count = 0;
    char line[256];
    char stat_path[256];
    char comm_buf[64];
    int pid;

    // Remember the previous sample of each process so CPU usage is measured
    // between refreshes rather than accumulated since the process started.
    static int prev_pids[MAX_PROCS];
    static unsigned long long prev_utime[MAX_PROCS];
    static unsigned long long prev_stime[MAX_PROCS];
    static int prev_count = 0;

    static struct timespec prev_sample_time;
    static int have_prev_sample = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double sample_interval = 0.0;
    if (have_prev_sample) {
        sample_interval = (double)(now.tv_sec - prev_sample_time.tv_sec)
                        + (double)(now.tv_nsec - prev_sample_time.tv_nsec) / 1e9;
    }
    prev_sample_time = now;
    have_prev_sample = 1;

    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        free(processes);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_procs) {
        // Skip non-numeric entries (not process directories)
        pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        // Only include real user-space programs, not kernel threads.
        if (is_kernel_thread(pid)) continue;

        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE *fp = fopen(stat_path, "r");
        if (fp == NULL) continue;

        // Read the entire line
        if (fgets(line, sizeof(line), fp) == NULL) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        // Parse the line to extract PID, comm, and utime/stime
        // Format: PID (comm) state ppid pgrp ... utime stime
        char line_copy[256];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';
        
        // Remove trailing newline
        size_t len = strlen(line_copy);
        if (len > 0 && line_copy[len-1] == '\n') {
            line_copy[len-1] = '\0';
        }

        // Find PID (first number before space)
        char *pid_ptr = line_copy;
        char *end_ptr = strchr(pid_ptr, ' ');
        if (!end_ptr) continue;
        pid = atoi(pid_ptr);
        if (pid <= 0) continue;

        // Find the command name between parentheses. The last ')' in the
        // line is the real end of comm: names may contain '(' or ')'.
        char *comm_start = strchr(pid_ptr, '(');
        if (!comm_start) continue;
        char *comm_end = strrchr(pid_ptr, ')');
        if (!comm_end || comm_end <= comm_start) continue;

        // Copy comm verbatim, including any parentheses and spaces.
        int comm_len = comm_end - comm_start - 1;
        if (comm_len <= 0 || comm_len >= (int)sizeof(comm_buf)) continue;
        memcpy(comm_buf, comm_start + 1, (size_t)comm_len);
        comm_buf[comm_len] = '\0';

        // Parse fields after the command name
        // After ")", the rest of the line contains: state ppid pgrp ... utime stime
        // utime is field 14, stime is field 15 (1-indexed in proc format)
        
        unsigned long long utime = 0;
        unsigned long long stime = 0;
        int got_utime = 0, got_stime = 0;
        
        char *rest = comm_end + 1;
        // Skip to the end of the command name part (after the closing paren)
        // and parse the remaining fields
        
        char *saveptr;
        char *token = strtok_r(rest, " \t", &saveptr);
        int field_idx = 0;
        
        // Format: PID (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime
        // After comm the first token is state, so utime (field 14) is token 11
        // and stime (field 15) is token 12.
        
        while (token && field_idx < 15) {
            if (field_idx == 11) {
                got_utime = (sscanf(token, "%llu", &utime) == 1);
            } else if (field_idx == 12) {
                got_stime = (sscanf(token, "%llu", &stime) == 1);
            }
            field_idx++;
            token = strtok_r(NULL, " \t", &saveptr);
        }

        // Only skip processes whose CPU fields could not be parsed; a
        // genuine value of zero is still a valid process.
        if (!got_utime || !got_stime) {
            continue;
        }

        // Use the previous sample for this PID if we have one; a process
        // seen for the first time starts from its current values so it
        // reports 0% until the next refresh.
        unsigned long long last_utime = utime;
        unsigned long long last_stime = stime;
        for (int k = 0; k < prev_count; k++) {
            if (prev_pids[k] == pid) {
                last_utime = prev_utime[k];
                last_stime = prev_stime[k];
                break;
            }
        }

        processes[count].prev_utime = last_utime;
        processes[count].prev_stime = last_stime;
        processes[count].cpu_usage = 0.0;

        processes[count].pid = pid;
        processes[count].comm = strdup(comm_buf);
        if (processes[count].comm == NULL) {
            // Free already allocated entries and exit
            for (int j = 0; j < count; j++) {
                free(processes[j].comm);
            }
            free(processes);
            return NULL;
        }
        processes[count].utime = utime;
        processes[count].stime = stime;
        
        count++;
    }

    closedir(dir);

    // Save this sample so the next refresh can compute per-interval usage.
    prev_count = (count < MAX_PROCS) ? count : MAX_PROCS;
    for (int k = 0; k < prev_count; k++) {
        prev_pids[k] = processes[k].pid;
        prev_utime[k] = processes[k].utime;
        prev_stime[k] = processes[k].stime;
    }
    last_process_sample_interval = sample_interval;

    return processes;
}


typedef struct {
    unsigned long long total;
    unsigned long long free;
    double usage;
} SwapStats;
SwapStats get_swap_stats(void) {
    SwapStats stats = {0, 0, 0.0};
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) return stats;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "SwapTotal:", 10) == 0) {
            sscanf(line, "SwapTotal: %llu kB", &stats.total);
        } else if (strncmp(line, "SwapFree:", 9) == 0) {
            sscanf(line, "SwapFree: %llu kB", &stats.free);
        }
    }
    fclose(fp);

    if (stats.total > 0) {
        stats.usage = (double)(stats.total - stats.free) / stats.total * 100.0;
    }
    return stats;
}

// --- Video memory (VRAM) stats ---
// Sources are probed once at startup in this order: NVIDIA (nvidia-smi),
// then any Vulkan-capable GPU (vulkaninfo). The first one that reports a
// usable total is used for the rest of the run.
typedef enum {
    VRAM_SOURCE_NONE = 0,
    VRAM_SOURCE_NVIDIA,
    VRAM_SOURCE_VULKAN
} VramSource;

typedef struct {
    unsigned long long total;   // bytes
    unsigned long long used;    // bytes
    double usage;               // percentage
} VramStats;

static void vram_finalize_usage(VramStats *stats) {
    if (stats->total > 0) {
        stats->usage = (double)stats->used / (double)stats->total * 100.0;
        if (stats->usage < 0.0) stats->usage = 0.0;
        if (stats->usage > 100.0) stats->usage = 100.0;
    } else {
        stats->usage = 0.0;
    }
}

// nvidia-smi reports MiB per GPU; aggregate all GPUs into one total.
// Returns 1 if at least one GPU was reported successfully.
static int get_nvidia_vram(VramStats *stats) {
    FILE *fp = popen("nvidia-smi --query-gpu=memory.total,memory.used "
                     "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp == NULL) return 0;

    unsigned long long total_mib = 0, used_mib = 0;
    int found = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long total = 0, used = 0;
        if (sscanf(line, " %llu , %llu", &total, &used) == 2) {
            total_mib += total;
            used_mib += used;
            found = 1;
        }
    }
    int status = pclose(fp);
    if (!found || status != 0 || total_mib == 0) return 0;

    stats->total = total_mib * 1024ULL * 1024ULL;
    stats->used = used_mib * 1024ULL * 1024ULL;
    vram_finalize_usage(stats);
    return 1;
}

static const char *vram_skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static unsigned long long vram_number_after_equals(const char *line) {
    const char *eq = strchr(line, '=');
    if (eq == NULL) return 0;
    eq = vram_skip_spaces(eq + 1);
    if (!isdigit((unsigned char)*eq)) return 0;
    return strtoull(eq, NULL, 10);
}

// vulkaninfo prints one VkPhysicalDeviceMemoryProperties section per GPU.
// All DEVICE_LOCAL heaps count as VRAM; their sizes/usages are summed across
// heaps and GPUs. Returns 1 if any usable heap was found.
static int get_vulkan_vram(VramStats *stats) {
    FILE *fp = popen("vulkaninfo 2>/dev/null", "r");
    if (fp == NULL) return 0;

    int in_memory = 0;
    unsigned long long heap_total = 0, heap_used = 0;
    int heap_device_local = 0;
    unsigned long long device_total = 0, device_used = 0;

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *t = vram_skip_spaces(line);

        if (strncmp(t, "VkPhysicalDeviceMemoryProperties:", 33) == 0) {
            if (in_memory && heap_device_local) {
                device_total += heap_total;
                device_used += heap_used;
            }
            in_memory = 1;
            heap_total = heap_used = 0;
            heap_device_local = 0;
            continue;
        }
        if (!in_memory) continue;

        if (strncmp(t, "memoryHeaps[", 12) == 0) {
            if (heap_device_local) {
                device_total += heap_total;
                device_used += heap_used;
            }
            heap_total = heap_used = 0;
            heap_device_local = 0;
            continue;
        }
        if (strncmp(t, "memoryTypes:", 12) == 0) {
            if (heap_device_local) {
                device_total += heap_total;
                device_used += heap_used;
            }
            in_memory = 0;
            continue;
        }

        if (strncmp(t, "size", 4) == 0 && strchr(t, '=') != NULL) {
            heap_total = vram_number_after_equals(t);
        } else if (strncmp(t, "usage", 5) == 0 && strchr(t, '=') != NULL) {
            heap_used = vram_number_after_equals(t);
        } else if (strstr(t, "MEMORY_HEAP_DEVICE_LOCAL_BIT") != NULL) {
            heap_device_local = 1;
        }
    }

    if (in_memory && heap_device_local) {
        device_total += heap_total;
        device_used += heap_used;
    }

    int status = pclose(fp);
    if (status != 0 || device_total == 0) return 0;

    stats->total = device_total;
    stats->used = device_used;
    vram_finalize_usage(stats);
    return 1;
}

static VramSource detect_vram_source(void) {
    VramStats stats = {0, 0, 0.0};
    if (get_nvidia_vram(&stats)) return VRAM_SOURCE_NVIDIA;
    if (get_vulkan_vram(&stats)) return VRAM_SOURCE_VULKAN;
    return VRAM_SOURCE_NONE;
}

static VramStats get_vram_stats(VramSource source) {
    VramStats stats = {0, 0, 0.0};
    if (source == VRAM_SOURCE_NVIDIA) {
        get_nvidia_vram(&stats);
    } else if (source == VRAM_SOURCE_VULKAN) {
        get_vulkan_vram(&stats);
    }
    return stats;
}

// Write text without wrapping past the right edge or writing off-screen.
// Keeps the display usable in narrow terminals and small tmux panes.
static void print_clipped(WINDOW *win, int y, int x, const char *text) {
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    if (max_x <= 0 || max_y <= 0 || y < 0 || y >= max_y ||
        x < 0 || x >= max_x) return;

    int len = (int)strlen(text);
    int avail = max_x - x;
    if (len > avail) len = avail;
    if (len > 0) mvwaddnstr(win, y, x, text, len);
}

void display_swap_usage(WINDOW *win, double usage) {
    int bar_len = 20;
    int filled = (int)(usage / 100.0 * bar_len);
    if (filled > bar_len) filled = bar_len;
    if (filled < 0) filled = 0;

    char bar[21];
    memset(bar, '-', bar_len + 1);
    for (int i = 0; i < filled; i++) bar[i] = '#';
    bar[bar_len] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "Swap: [%s] %.1f%%", bar, usage);
    print_clipped(win, 4, 0, line);
}

void display_vram_usage(WINDOW *win, double usage, int y) {
    int bar_len = 20;
    int filled = (int)(usage / 100.0 * bar_len);
    if (filled > bar_len) filled = bar_len;
    if (filled < 0) filled = 0;

    char bar[21];
    memset(bar, '-', bar_len + 1);
    for (int i = 0; i < filled; i++) bar[i] = '#';
    bar[bar_len] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "VRAM: [%s] %.1f%%", bar, usage);
    print_clipped(win, y, 0, line);
}

void display_memory_usage(WINDOW *win, double usage) {
    int bar_len = 20;
    int filled = (int)(usage / 100.0 * bar_len);
    if (filled > bar_len) filled = bar_len;
    if (filled < 0) filled = 0;

    char bar[21];
    memset(bar, '-', bar_len + 1);
    for (int i = 0; i < filled; i++) bar[i] = '#';
    bar[bar_len] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "Memory: [%s] %.1f%%", bar, usage);
    print_clipped(win, 3, 0, line);
}

void display_cpu_usage(WINDOW *win, double usage) {
    int bar_len = 20;
    int filled = (int)(usage / 100.0 * bar_len);
    if (filled > bar_len) filled = bar_len;
    if (filled < 0) filled = 0;

    char bar[21];
    memset(bar, '-', bar_len + 1);
    for (int i = 0; i < filled; i++) bar[i] = '#';
    bar[bar_len] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "CPU: [%s] %.2f%%", bar, usage);
    print_clipped(win, 2, 0, line);
}

// Function to display the top CPU-consuming processes (up to max_display)
// row_offset shifts the section down when the VRAM line is shown.
void display_top_processes(WINDOW *win, ProcessCPUStats *processes, int count, int row_offset, int max_display) {
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);

    // Nothing to display when no processes were collected.
    if (count < 1) {
        return;
    }
    
    // Display header directly under the system stats
    char header[100];
    snprintf(header, sizeof(header), "Top %d CPU Processes:", max_display);
    int header_len = strlen(header);
    int header_start_x = (max_x > header_len) ? (max_x - header_len) / 2 : 0;
    print_clipped(win, 5 + row_offset, header_start_x, header);
    
    // Display the most CPU-hungry processes first. Reserve space for the
    // rank, PID and percentage so the name is what gets shortened.
    int y_pos = 6 + row_offset;
    for (int i = 0; i < max_display && i < count; i++) {
        if (y_pos >= max_y) break;

        char process_line[150];
        int name_width = max_x - 28;
        if (name_width >= 1) {
            snprintf(process_line, sizeof(process_line),
                     "  %2d. %.*s (%d): %.2f%%",
                     i + 1, name_width, processes[i].comm,
                     processes[i].pid, processes[i].cpu_usage);
        } else {
            // Very narrow panes: drop the PID and decimals.
            int compact_name = max_x - 10;
            if (compact_name < 1) compact_name = 1;
            snprintf(process_line, sizeof(process_line), "%d.%.*s %.0f%%",
                     i + 1, compact_name, processes[i].comm,
                     processes[i].cpu_usage);
        }

        print_clipped(win, y_pos, 0, process_line);
        y_pos++;
    }
}

#define DEFAULT_TAIL_LINES 10
#define MIN_TAIL_LINES 1
#define MAX_TAIL_LINES 100
#define LOG_LINE_WIDTH 200

// Read the last lines_wanted lines from a file. Returns the number of
// lines read, or -1 if the file could not be opened/read.
static int get_log_tail(const char *path, int lines_wanted,
                        char lines[MAX_TAIL_LINES][LOG_LINE_WIDTH]) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return -1;
    }

    char *buf = NULL;
    size_t buf_len = 0;
    long pos = size;
    int newline_count = 0;

    // Read backwards in chunks until enough line breaks are found.
    while (pos > 0 && newline_count <= lines_wanted) {
        size_t chunk = 4096;
        if ((long)chunk > pos) chunk = (size_t)pos;
        pos -= (long)chunk;

        char *chunk_buf = malloc(chunk);
        if (chunk_buf == NULL) {
            free(buf);
            fclose(fp);
            return -1;
        }
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(chunk_buf, 1, chunk, fp) != chunk) {
            free(chunk_buf);
            free(buf);
            fclose(fp);
            return -1;
        }

        for (size_t i = 0; i < chunk; i++) {
            if (chunk_buf[i] == '\n') newline_count++;
        }

        char *new_buf = realloc(buf, buf_len + chunk);
        if (new_buf == NULL) {
            free(chunk_buf);
            free(buf);
            fclose(fp);
            return -1;
        }
        memmove(new_buf + chunk, new_buf, buf_len);
        memcpy(new_buf, chunk_buf, chunk);
        free(chunk_buf);
        buf = new_buf;
        buf_len += chunk;
    }
    fclose(fp);

    if (buf == NULL) return 0;

    int count = 0;
    char *p = buf;
    char *end = buf + buf_len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 0 && p[len - 1] == '\r') len--;
        if (len >= LOG_LINE_WIDTH) len = LOG_LINE_WIDTH - 1;

        if (count < lines_wanted) {
            memcpy(lines[count], p, len);
            lines[count][len] = '\0';
            count++;
        } else {
            for (int i = 1; i < lines_wanted; i++) {
                memcpy(lines[i - 1], lines[i], LOG_LINE_WIDTH);
            }
            memcpy(lines[lines_wanted - 1], p, len);
            lines[lines_wanted - 1][len] = '\0';
        }

        if (nl == NULL) break;
        p = nl + 1;
    }

    free(buf);
    return count;
}

// Shared renderer for the log-file and shell-command tail viewers.
// A negative count means the label already explains why there is no output.
static void display_tail(WINDOW *win, const char *label,
                         char lines[MAX_TAIL_LINES][LOG_LINE_WIDTH],
                         int count, int start_y) {
    print_clipped(win, start_y, 0, label);
    if (count < 0) return;

    if (count == 0) {
        print_clipped(win, start_y + 1, 0, "(empty)");
        return;
    }

    for (int i = 0; i < count; i++) {
        print_clipped(win, start_y + 1 + i, 0, lines[i]);
    }
}

void display_log_tail(WINDOW *win, const char *path, int start_y, int lines_wanted) {
    if (path == NULL) return;

    char lines[MAX_TAIL_LINES][LOG_LINE_WIDTH];
    int count = get_log_tail(path, lines_wanted, lines);

    char label[256];
    if (count < 0) {
        snprintf(label, sizeof(label), "Log (%s): cannot read file", path);
    } else {
        snprintf(label, sizeof(label), "Log (%s):", path);
    }
    display_tail(win, label, lines, count, start_y);
}

// Read the output of a shell command and keep the last lines_wanted lines.
// Long lines are truncated (rather than split) to match the file viewer.
// Returns -1 if the command could not be run or failed without output.
static int get_command_tail(const char *command, int lines_wanted,
                            char lines[MAX_TAIL_LINES][LOG_LINE_WIDTH]) {
    FILE *fp = popen(command, "r");
    if (fp == NULL) return -1;

    int count = 0;
    char buf[LOG_LINE_WIDTH];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t len = strlen(buf);
        int complete = (len > 0 && buf[len - 1] == '\n');
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }

        if (count < lines_wanted) {
            memcpy(lines[count], buf, len);
            lines[count][len] = '\0';
            count++;
        } else {
            for (int i = 1; i < lines_wanted; i++) {
                memcpy(lines[i - 1], lines[i], LOG_LINE_WIDTH);
            }
            memcpy(lines[lines_wanted - 1], buf, len);
            lines[lines_wanted - 1][len] = '\0';
        }

        // Discard the rest of an overlong line so it occupies one display row.
        if (!complete) {
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF) { }
        }
    }

    int status = pclose(fp);
    if (status != 0 && count == 0) return -1;
    return count;
}

void display_command_tail(WINDOW *win, const char *command, int start_y,
                          int lines_wanted) {
    if (command == NULL) return;

    char lines[MAX_TAIL_LINES][LOG_LINE_WIDTH];
    int count = get_command_tail(command, lines_wanted, lines);

    char label[256];
    if (count < 0) {
        snprintf(label, sizeof(label), "Shell command failed: %s", command);
    } else {
        snprintf(label, sizeof(label), "Command (%s):", command);
    }
    display_tail(win, label, lines, count, start_y);
}

// Function to sort processes by CPU usage (descending)
void sort_processes_by_cpu(ProcessCPUStats *processes, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (processes[j].cpu_usage > processes[i].cpu_usage) {
                ProcessCPUStats temp = processes[i];
                processes[i] = processes[j];
                processes[j] = temp;
            }
        }
    }
}

// Function to calculate CPU usage for each process
void calculate_process_cpu_usage(ProcessCPUStats *processes, int count) {
    if (count == 0) return;
    
    // Allocate temporary array to store current state
    ProcessCPUStats *temp = malloc(count * sizeof(ProcessCPUStats));
    if (!temp) return;

    long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) ticks_per_second = 100;

    double interval = last_process_sample_interval;
    if (interval <= 0.0) interval = 5.0;
    
    // Store current state
    for (int i = 0; i < count; i++) {
        temp[i].utime = processes[i].utime;
        temp[i].stime = processes[i].stime;
        temp[i].prev_utime = processes[i].prev_utime;
        temp[i].prev_stime = processes[i].prev_stime;
        temp[i].pid = processes[i].pid;
        temp[i].comm = processes[i].comm;
    }
    
    // Calculate CPU usage for each process
    for (int i = 0; i < count; i++) {
        unsigned long long utime_diff = temp[i].utime - temp[i].prev_utime;
        unsigned long long stime_diff = temp[i].stime - temp[i].prev_stime;
        unsigned long long total_diff = utime_diff + stime_diff;
        
        if (total_diff > 0) {
            // Convert CPU ticks to seconds and divide by the actual time
            // elapsed since the previous sample.
            double cpu_seconds = (double)total_diff / (double)ticks_per_second;
            // Update prev values for next iteration
            temp[i].prev_utime = temp[i].utime;
            temp[i].prev_stime = temp[i].stime;
            temp[i].cpu_usage = (cpu_seconds / interval) * 100.0;
        } else {
            temp[i].cpu_usage = 0.0;
        }
    }
    
    // Update processes array with calculated values
    for (int i = 0; i < count; i++) {
        processes[i].utime = temp[i].utime;
        processes[i].stime = temp[i].stime;
        processes[i].prev_utime = temp[i].prev_utime;
        processes[i].prev_stime = temp[i].prev_stime;
        processes[i].cpu_usage = temp[i].cpu_usage;
    }
    free(temp);
}


// Function to center text on the screen
void center_text(WINDOW *win, int y, const char *text) {
    int max_x, max_y;
    getmaxyx(win, max_y, max_x);
    (void)max_y;
    int len = strlen(text);
    int start_x = (max_x > len) ? (max_x - len) / 2 : 0;
    print_clipped(win, y, start_x, text);
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out,
            "Usage: %s [-d|--delay <seconds>] [-n|--processes <count>]\n"
            "          [-l|--log <logfile> | -s|--shell <command>]\n"
            "          [-t|--taillines <count>] [-h|--help] [-V|--version]\n"
            "  -d, --delay <seconds>    Refresh delay in whole seconds (default: 5)\n"
            "  -n, --processes <count>  Number of top CPU processes to display\n"
            "                           (%d-%d, default: %d)\n"
            "  -l, --log <logfile>      Show the last <taillines> lines of <logfile>\n"
            "                           under the top processes, refreshed with stats\n"
            "  -s, --shell <command>    Run <command> via /bin/sh -c on each refresh and\n"
            "                           show its last <taillines> lines instead of a log\n"
            "                           (mutually exclusive with -l/--log)\n"
            "                           Caveats: pass a quoted, trusted command; it must\n"
            "                           be non-interactive and exit (e.g. 'journalctl -n 10'),\n"
            "                           or the display will freeze; add '2>&1' or '2>/dev/null'\n"
            "                           so child stderr does not overwrite the screen\n"
            "                           Example: sysmon -s 'journalctl -n 10 --no-pager 2>&1'\n"
            "  -t, --taillines <count>  Tail lines for -l or -s (%d-%d, default: %d)\n"
            "  -h, --help               Show this help\n"
            "  -V, --version            Show version and exit\n",
            prog, MIN_DISPLAY_PROCS, MAX_DISPLAY_PROCS, DEFAULT_DISPLAY_PROCS,
            MIN_TAIL_LINES, MAX_TAIL_LINES, DEFAULT_TAIL_LINES);
}

// Wait for the refresh interval while still reacting to 'q' or a signal
// promptly. Returns 1 when it is time to quit.
static int wait_for_refresh(unsigned int delay) {
    const unsigned long poll_ms = 100;
    unsigned long long remaining_ms = (unsigned long long)delay * 1000ULL;

    while (remaining_ms > 0 && !stop_requested) {
        timeout((int)poll_ms);
        if (getch() == 'q') return 1;
        remaining_ms = (remaining_ms > poll_ms) ? remaining_ms - poll_ms : 0;
    }

    timeout(0);
    return stop_requested != 0;
}

int main(int argc, char *argv[]) {
    const char *logfile = NULL;
    const char *shell_command = NULL;
    int tail_lines = DEFAULT_TAIL_LINES;
    unsigned int delay = 5;
    int num_processes = DEFAULT_DISPLAY_PROCS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            logfile = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--shell") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            shell_command = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--taillines") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            char *value = argv[++i];
            char *end = NULL;
            long count = strtol(value, &end, 10);
            if (end == value || *end != '\0' ||
                count < MIN_TAIL_LINES || count > MAX_TAIL_LINES) {
                fprintf(stderr, "Invalid tail line count: %s (expected a whole number from %d to %d)\n",
                        value, MIN_TAIL_LINES, MAX_TAIL_LINES);
                print_usage(stderr, argv[0]);
                return 1;
            }
            tail_lines = (int)count;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delay") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            char *value = argv[++i];
            char *end = NULL;
            long seconds = strtol(value, &end, 10);
            if (end == value || *end != '\0' || seconds <= 0 || seconds > INT_MAX) {
                fprintf(stderr, "Invalid delay: %s (expected a positive whole number of seconds)\n", value);
                print_usage(stderr, argv[0]);
                return 1;
            }
            delay = (unsigned int)seconds;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--processes") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            char *value = argv[++i];
            char *end = NULL;
            long count = strtol(value, &end, 10);
            if (end == value || *end != '\0' ||
                count < MIN_DISPLAY_PROCS || count > MAX_DISPLAY_PROCS) {
                fprintf(stderr, "Invalid process count: %s (expected a whole number from %d to %d)\n",
                        value, MIN_DISPLAY_PROCS, MAX_DISPLAY_PROCS);
                print_usage(stderr, argv[0]);
                return 1;
            }
            num_processes = (int)count;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("sysmon %s\n", SYSMON_VERSION);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(stderr, argv[0]);
            return 1;
        }
    }

    if (logfile != NULL && shell_command != NULL) {
        fprintf(stderr, "-l/--log and -s/--shell cannot be used together; they share the same display area\n");
        print_usage(stderr, argv[0]);
        return 1;
    }

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    // --- Ncurses Setup ---
    initscr();          // Start curses mode
    cbreak();           // Line buffering disabled, pass characters immediately
    noecho();           // Don't echo characters
    curs_set(0);        // Hide cursor
    keypad(stdscr, TRUE); // Enable special keys

    WINDOW *main_win = stdscr;

    // --- Initialization ---
    char *hostname = get_hostname();
    long long days, hours, minutes, seconds;
    
    get_uptime(&days, &hours, &minutes, &seconds);

    // Probe for a GPU once; if none is found the VRAM line is omitted and
    // all sections below it keep their original positions.
    VramSource vram_source = detect_vram_source();
    int vram_row_offset = (vram_source != VRAM_SOURCE_NONE) ? 1 : 0;

    // --- Main Loop ---
    int running = 1;
    
    // Initial display
    center_text(main_win, 0, hostname);
    char uptime_str[100];
    snprintf(uptime_str, sizeof(uptime_str), 
             "Uptime: %lld days, %lld hours, %lld minutes, %lld seconds", 
             days, hours, minutes, seconds);
    center_text(main_win, 1, uptime_str);
    
    prev_stats = get_cpu_stats();
    // Set up the refresh loop
    while (running && !stop_requested) {
        // 1. Non-blocking check for quit command 'q'
        nodelay(main_win, TRUE);
        int ch = getch();

        if (ch == 'q' || stop_requested) {
            running = 0;
            break;
        }
        
        // 2. Update data
        get_uptime(&days, &hours, &minutes, &seconds);
        double cpu_usage = get_cpu_usage();

        // Get process info and calculate CPU usage
        ProcessCPUStats *processes = get_process_info(MAX_PROCS);
        int count = 0;
        if (processes) {
            // Count non-NULL comm entries
            for (int i = 0; i < MAX_PROCS; i++) {
                if (processes[i].comm != NULL) count++;
            }
            calculate_process_cpu_usage(processes, count);
            
            // Sort processes by CPU usage (descending)
            sort_processes_by_cpu(processes, count);
        }

        // 3. Redraw screen
        werase(main_win); // Clear the window
        
        // Display Hostname (Line 0)
        center_text(main_win, 0, hostname);

        // Display Uptime (Line 1)
        snprintf(uptime_str, sizeof(uptime_str), 
                 "Uptime: %lld days, %lld hours, %lld minutes, %lld seconds", 
                 days, hours, minutes, seconds);
        center_text(main_win, 1, uptime_str);

        // Display CPU Usage (Line 2)
        display_cpu_usage(main_win, cpu_usage);

        // Display Memory Usage (Line 3)        
        display_memory_usage(main_win, get_memory_stats().usage);

        // Display Swap Usage (Line 4)
        display_swap_usage(main_win, get_swap_stats().usage);

        // Display VRAM Usage (Line 5, only when a GPU is detected)
        if (vram_source != VRAM_SOURCE_NONE) {
            VramStats vram = get_vram_stats(vram_source);
            display_vram_usage(main_win, vram.usage, 5);
        }

        // Display the configured number of top processes (starting at line 6,
        // or 7 with the VRAM line)
        if (processes) {
            display_top_processes(main_win, processes, count, vram_row_offset, num_processes);
        }

        // Display the monitored log tail, or the shell command output, below
        // the process list. With the defaults this starts at line 9 (10 with
        // VRAM); both viewers refresh on this same loop iteration.
        int tail_y = 6 + vram_row_offset + num_processes;
        if (shell_command) {
            display_command_tail(main_win, shell_command, tail_y, tail_lines);
        } else if (logfile) {
            display_log_tail(main_win, logfile, tail_y, tail_lines);
        }

        // Refresh the screen buffer
        refresh(); 

        if (processes) {
            for (int i = 0; i < MAX_PROCS; i++) {
                free(processes[i].comm);
            }
            free(processes);
        }

        // 4. Wait for the configured delay (default: 5 seconds), but keep
        // checking for 'q' so the display exits promptly.
        if (wait_for_refresh(delay)) break;
    }


    // --- Cleanup ---
    free(hostname);
    endwin(); // End curses mode
    return 0;
}