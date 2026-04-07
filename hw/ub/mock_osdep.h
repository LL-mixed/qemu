#ifndef MOCK_OSDEP_H
#define MOCK_OSDEP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

// Mock GLib
#define g_autofree
#define g_autoptr(type) type*
#define g_free(p) free(p)
#define g_malloc(s) malloc(s)
#define g_malloc0(s) calloc(1, s)
#define g_strdup(s) strdup(s)
#define g_strdup_printf(fmt, ...) ({ char *p; asprintf(&p, fmt, ##__VA_ARGS__); p; })
#define g_mkdir_with_parents(path, mode) mkdir(path, mode)
#define g_getenv(s) getenv(s)
#define g_remove(path) unlink(path)
#define g_usleep(u) usleep(u)
#define g_ascii_isalnum(c) ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
#define g_file_test(path, flag) (access(path, F_OK) == 0)
#define G_FILE_TEST_EXISTS 1

// Mock QEMU
#define qemu_log(fmt, ...) printf(fmt, ##__VA_ARGS__)

typedef struct Error Error;
void error_setg(Error **errp, const char *fmt, ...) { (void)errp; (void)fmt; }

#endif
