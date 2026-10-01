#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "config.h"

#define MAX_ENTRIES 16

static struct {
    char *name;
    char *value;
} entries[MAX_ENTRIES];
static int entry_count;
static int have_read;

/* The settings sit where the platform keeps per-user configuration rather than
 * in the working directory: the file holds an API key, and the working
 * directory is often a repository that would carry it away. */
int config_path(char *buf, size_t size) {
    const char *base;
    int n;

#ifdef _WIN32
    base = getenv("APPDATA");
    if (base && *base) {
        n = snprintf(buf, size, "%s\\igor\\config", base);
        return n > 0 && (size_t)n < size;
    }
    base = getenv("USERPROFILE");
    if (base && *base) {
        n = snprintf(buf, size, "%s\\.igor\\config", base);
        return n > 0 && (size_t)n < size;
    }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        n = snprintf(buf, size, "%s/igor/config", base);
        return n > 0 && (size_t)n < size;
    }
    base = getenv("HOME");
    if (base && *base) {
        n = snprintf(buf, size, "%s/.config/igor/config", base);
        return n > 0 && (size_t)n < size;
    }
#endif

    /* No home directory to speak of - ReactOS can be set up that way. Keeping
     * it beside the working directory is worse, but it still works. */
#ifdef _WIN32
    n = snprintf(buf, size, ".igor\\config");
#else
    n = snprintf(buf, size, ".igor/config");
#endif
    return n > 0 && (size_t)n < size;
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

/* Create every directory on the way to the file. An intermediate that cannot be
 * made is not reported here - the open that follows is the real test, and on
 * Windows a leading "C:" is a component that no mkdir can create. */
static void make_parents(const char *file) {
    char tmp[512];
    char *p;

    if (snprintf(tmp, sizeof(tmp), "%s", file) >= (int)sizeof(tmp)) return;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = 0;
            if (tmp[0]) make_dir(tmp);
            *p = sep;
        }
    }
}

static void forget(void) {
    for (int i = 0; i < entry_count; i++) {
        free(entries[i].name);
        free(entries[i].value);
    }
    entry_count = 0;
    have_read = 0;
}

static void trim_tail(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void read_file(void) {
    char path[512];
    char line[1024];
    FILE *f;

    have_read = 1;
    if (!config_path(path, sizeof(path))) return;
    f = fopen(path, "r");
    if (!f) return;

    while (entry_count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        char *name = line, *value, *eq;

        while (*name == ' ' || *name == '\t') name++;
        if (*name == '#' || *name == '\n' || *name == '\r' || *name == 0) continue;

        eq = strchr(name, '=');
        if (!eq) continue;
        *eq = 0;
        value = eq + 1;

        trim_tail(name);
        while (*value == ' ' || *value == '\t') value++;
        trim_tail(value);
        if (!*name || !*value) continue;

        entries[entry_count].name = strdup(name);
        entries[entry_count].value = strdup(value);
        if (!entries[entry_count].name || !entries[entry_count].value) {
            free(entries[entry_count].name);
            free(entries[entry_count].value);
            break;
        }
        entry_count++;
    }
    fclose(f);
}

const char *config_get(const char *name) {
    if (!have_read) read_file();
    for (int i = 0; i < entry_count; i++)
        if (strcmp(entries[i].name, name) == 0) return entries[i].value;
    return NULL;
}

int config_save(const char *api_key, const char *base_url, const char *model,
                const char **err) {
    char path[512];
    FILE *f;

    if (err) *err = NULL;
    if (!config_path(path, sizeof(path))) {
        if (err) *err = "the path to the settings file is too long";
        return 0;
    }
    make_parents(path);

    f = fopen(path, "w");
    if (!f) {
        if (err) *err = strerror(errno);
        return 0;
    }
    fputs("# igor settings, written by `igor --setup`.\n"
          "# An environment variable of the same name wins over what is here.\n",
          f);
    if (api_key && *api_key)   fprintf(f, "LLM_API_KEY = %s\n", api_key);
    if (base_url && *base_url) fprintf(f, "LLM_BASE_URL = %s\n", base_url);
    if (model && *model)       fprintf(f, "LLM_MODEL = %s\n", model);

    if (fclose(f) != 0) {
        if (err) *err = strerror(errno);
        return 0;
    }

#ifndef _WIN32
    /* It holds an API key, so it is the writer's business and nobody else's. */
    if (chmod(path, S_IRUSR | S_IWUSR) != 0 && err) *err = NULL;
#endif

    /* What was just written is what the next lookup should see. */
    forget();
    return 1;
}
