#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
#define KB (1 << 10)
#define MB (1 << 20)
#define GB (1 << 30)
#define BUFFER_SIZE 4096
#define UPDATE_INTERVAL 1000000

struct program_options {
    const char *filename_in;
    const char *filename_out;
    size_t block_size;
    size_t count;
    const char *status;
};
struct program_state {
    int in_file;
    int out_file;
    size_t buffer_size;
    char *buffer;
    int out_file_is_device;
    int started_copying;
    long long start_time;
    size_t num_bytes_in;
    size_t num_bytes_out;
    size_t num_blocks_copied;
};
static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s if=<in_file> of=<out_file> [bs=N] [count=N] [status=progress] [--about]\n", program_name);
}
static void print_about(void) {
    printf("This is a reimplementation of the classic Linux `dd` command. It may not be 100%% perfect like the original command, as it's based on an understanding of how the original command works, so errors may exist.\n");
    printf("This reimplementation was done by: Nikki\n");
}
static long long get_time_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}
static void format_size(char *buffer, size_t buffer_size, size_t size) {
    if (size >= GB) {
        snprintf(buffer, buffer_size, "%.1f GB", (double)size / (double)GB);
    } else if (size >= MB) {
        snprintf(buffer, buffer_size, "%.1f MB", (double)size / (double)MB);
    } else if (size >= KB) {
        snprintf(buffer, buffer_size, "%.1f KB", (double)size / (double)KB);
    } else {
        snprintf(buffer, buffer_size, "%zu bytes", size);
    }
}
static void format_speed(char *buffer, size_t buffer_size, double speed) {
    if (speed >= (double)GB) {
        snprintf(buffer, buffer_size, "%.1f GB/s", speed / (double)GB);
    } else if (speed >= (double)MB) {
        snprintf(buffer, buffer_size, "%.1f MB/s", speed / (double)MB);
    } else if (speed >= (double)KB) {
        snprintf(buffer, buffer_size, "%.1f KB/s", speed / (double)KB);
    } else {
        snprintf(buffer, buffer_size, "%.1f bytes/s", speed);
    }
}
static void print_progress(size_t num_bytes_copied,
                           size_t last_bytes_copied,
                           long long start_time,
                           long long last_time) {
    long long current_time;
    long long elapsed_time;
    double speed;
    char bytes_str[16];
    char speed_str[16];
    current_time = get_time_usec();
    elapsed_time = current_time - start_time;
    if (elapsed_time >= 1000000) {
        speed = last_bytes_copied
            / ((double)(current_time - last_time) / 1000000);
    } else {
        speed = (double)last_bytes_copied;
    }
    format_size(bytes_str, sizeof(bytes_str), num_bytes_copied);
    format_speed(speed_str, sizeof(speed_str), speed);
    printf("%zu bytes (%s) copied, %.1f s, %s\n",
        num_bytes_copied,
        bytes_str,
        (double)elapsed_time / 1000000.0,
        speed_str);
}
static void print_status(size_t num_bytes_copied, long long start_time) {
    print_progress(
        num_bytes_copied,
        num_bytes_copied,
        start_time,
        start_time);
}
static void clear_output(void) {
    printf("\033[2K\r");
    fflush(stdout);
}
static void cleanup(const struct program_state *s) {
    if (s->buffer != NULL) {
        free(s->buffer);
    }
    if (s->in_file != -1) {
        close(s->in_file);
    }
    if (s->out_file != -1) {
        close(s->out_file);
    }
}
static void exit_on_error(const struct program_state *s,
                          int error_code,
                          char *format,
                          ...) {
    va_list arg_list;
    char error_buffer[256];
    va_start(arg_list, format);
    vfprintf(stderr, format, arg_list);
    va_end(arg_list);
    fprintf(stderr, ": ");
    fprintf(stderr, "%s\n", strerror(error_code));
    if (s->started_copying) {
        print_status(s->num_bytes_out, s->start_time);
    }
    cleanup(s);
    exit(EXIT_FAILURE);
}
static size_t parse_size(const char *str) {
    char *end = NULL;
    size_t size = (size_t)strtoll(str, &end, 10);
    if (end != NULL && *end != '\0') {
        switch (*end) {
            case 'k':
            case 'K':
                size *= 1 << 10;
                break;
            case 'm':
            case 'M':
                size *= 1 << 20;
                break;
            case 'g':
            case 'G':
                size *= 1 << 30;
                break;
        }
    }
    return size;
}
static int is_empty_string(const char *s) {
    return s == NULL || *s == '\0';
}
static int parse_options(int argc,
                         char **argv,
                         struct program_options *options) {
    int i;
    int about_found = 0;
    int other_params_found = 0;
    options->filename_in = NULL;
    options->filename_out = NULL;
    options->block_size = 0;
    options->count = (size_t)-1;
    options->status = NULL;
    for (i = 1; i < argc; i++) {
        char *value = NULL;
        char *saveptr = NULL;
        char *name = strtok_r(argv[i], "=", &saveptr);
        value = strtok_r(NULL, "=", &saveptr);
        if (name == NULL) continue;
        if (strcmp(name, "--about") == 0) {
            about_found = 1;
            continue;
        }
        other_params_found = 1;
        if (strcmp(name, "if") == 0) {
            options->filename_in = strdup(value);
        } else if (strcmp(name, "of") == 0) {
            options->filename_out = strdup(value);
        } else if (strcmp(name, "bs") == 0) {
            options->block_size = parse_size(value);
        } else if (strcmp(name, "count") == 0) {
            options->count = (size_t)strtoll(value, NULL, 10);
        } else if (strcmp(name, "status") == 0) {
            options->status = strdup(value);
        } else {
            return 0;
        }
    }
    if (about_found && other_params_found) {
        fprintf(stderr, "Error: --about cannot be combined with other parameters\n");
        return -1;
    }
    if (about_found && !other_params_found) {
        print_about();
        exit(EXIT_SUCCESS);
    }
    return !is_empty_string(options->filename_in)
        && !is_empty_string(options->filename_out);
}
static int is_block_device(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return S_ISBLK(st.st_mode);
    }
    return 0;
}
static int check_root_permissions(const char *filename_in, const char *filename_out) {
    struct stat st_in, st_out;
    int needs_root = 0;
    if (stat(filename_in, &st_in) == 0) {
        if (S_ISBLK(st_in.st_mode)) {
            needs_root = 1;
        }
    }
    if (stat(filename_out, &st_out) == 0) {
        if (S_ISBLK(st_out.st_mode)) {
            needs_root = 1;
        }
        if (strncmp(filename_out, "/dev/", 5) == 0) {
            needs_root = 1;
        }
    } else {
        if (strncmp(filename_out, "/dev/", 5) == 0) {
            needs_root = 1;
        }
    }
    return needs_root;
}
static int try_open_with_root_check(const char *filename, int flags, struct program_state *s) {
    int fd = open(filename, flags, 0644);
    if (fd == -1) {
        if (errno == EACCES || errno == EPERM) {
            if (strncmp(filename, "/dev/", 5) == 0 || is_block_device(filename)) {
                fprintf(stderr, "Error: Permission denied for %s\n", filename);
                fprintf(stderr, "This operation might require root privileges.\n");
                fprintf(stderr, "Try running with 'sudo' or as root user.\n");
                cleanup(s);
                exit(EXIT_FAILURE);
            }
        }
    }
    return fd;
}
int main(int argc, char **argv) {
    struct program_options options;
    struct program_state s;
    int show_progress = 0;
    size_t last_bytes_copied = 0;
    long long last_time = 0;
    if (argc == 2 && strcmp(argv[1], "--about") == 0) {
        print_about();
        return EXIT_SUCCESS;
    }
    memset(&options, 0, sizeof(options));
    int parse_result = parse_options(argc, argv, &options);
    if (parse_result == -1) {
        return EXIT_FAILURE;
    }
    if (parse_result == 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (check_root_permissions(options.filename_in, options.filename_out) && geteuid() != 0) {
        fprintf(stderr, "Warning: This operation involves block devices or system files\n");
        fprintf(stderr, "that typically require root privileges. If you get permission\n");
        fprintf(stderr, "errors, try running with 'sudo'.\n\n");
    }
    memset(&s, 0, sizeof(s));
    s.in_file = -1;
    s.out_file = -1;
    s.start_time = get_time_usec();
    s.out_file_is_device = 0;
    s.started_copying = 0;
    s.num_bytes_in = 0;
    s.num_bytes_out = 0;
    s.num_blocks_copied = 0;
    s.in_file = try_open_with_root_check(options.filename_in, O_RDONLY, &s);
    if (s.in_file == -1) {
        exit_on_error(
            &s,
            errno,
            "Could not open input file or device %s for reading",
            options.filename_in);
    }
    s.out_file = try_open_with_root_check(options.filename_out, O_WRONLY | O_CREAT | O_TRUNC, &s);
    if (s.out_file == -1) {
        s.out_file = try_open_with_root_check(options.filename_out, O_WRONLY, &s);
        if (s.out_file == -1) {
            exit_on_error(
                &s,
                errno,
                "Could not open output file or device %s for writing",
                options.filename_out);
        }
    }
    s.out_file_is_device = is_block_device(options.filename_out);
    s.buffer_size = BUFFER_SIZE;
    if (options.block_size > 0) {
        s.buffer_size = options.block_size;
    }
    if (s.out_file_is_device) {
        if (s.buffer_size < 4096) {
            s.buffer_size = 4096;
        }
    }
    s.buffer = malloc(s.buffer_size);
    if (s.buffer == NULL) {
        exit_on_error(&s, errno, "Failed to allocate buffer");
    }
    show_progress =
        (options.status != NULL && strcmp(options.status, "progress") == 0);
    s.started_copying = 1;
    for (;;) {
        ssize_t num_block_bytes_in;
        ssize_t num_block_bytes_out;
        long long current_time;
        if (options.count != (size_t)-1 && s.num_blocks_copied >= options.count) {
            break;
        }
        if (show_progress) {
            current_time = get_time_usec();
            if (last_time == 0) {
                last_time = current_time;
            } else {
                if (current_time - last_time >= UPDATE_INTERVAL) {
                    clear_output();
                    print_progress(
                        s.num_bytes_out,
                        s.num_bytes_out - last_bytes_copied,
                        s.start_time,
                        last_time);
                    last_time = current_time;
                    last_bytes_copied = s.num_bytes_out;
                }
            }
        }
        num_block_bytes_in = read(s.in_file, s.buffer, s.buffer_size);
        if (num_block_bytes_in == 0) {
            break;
        }
        if (num_block_bytes_in == -1) {
            exit_on_error(&s, errno, "Error reading from file");
        }
        s.num_bytes_in += num_block_bytes_in;
        num_block_bytes_out = write(s.out_file, s.buffer, num_block_bytes_in);
        if (num_block_bytes_out == -1) {
            exit_on_error(&s, errno, "Error writing to file");
        }
        if (num_block_bytes_out != num_block_bytes_in) {
            ssize_t remaining = num_block_bytes_in - num_block_bytes_out;
            ssize_t written;
            char *ptr = s.buffer + num_block_bytes_out;
            while (remaining > 0) {
                written = write(s.out_file, ptr, remaining);
                if (written == -1) {
                    exit_on_error(&s, errno, "Error writing to file");
                }
                ptr += written;
                remaining -= written;
                num_block_bytes_out += written;
            }
        }
        s.num_bytes_out += num_block_bytes_out;
        s.num_blocks_copied++;
    }
    cleanup(&s);
    if (show_progress) {
        clear_output();
    }
    print_status(s.num_bytes_out, s.start_time);
    if (options.filename_in) free((void*)options.filename_in);
    if (options.filename_out) free((void*)options.filename_out);
    if (options.status) free((void*)options.status);
    return EXIT_SUCCESS;
}