#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define stdin_is_tty() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define stdin_is_tty() isatty(fileno(stdin))
#endif

#include "agent.h"

#ifndef LLM_API_KEY
#define LLM_API_KEY NULL
#endif
#ifndef LLM_BASE_URL
#define LLM_BASE_URL "https://api.openai.com/v1"
#endif
#ifndef LLM_MODEL
#define LLM_MODEL "gpt-4o-mini"
#endif

static const char *env_or(const char *name, const char *def) {
    const char *v = getenv(name);
    return (v && *v) ? v : def;
}

/* Read the entire remaining stdin (used for a piped one-shot task). */
static char *read_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    buf[len] = 0;
    return buf;
}

/* Read one line of arbitrary length; returns NULL on EOF with no input. */
static char *read_line(FILE *f) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  /help    show this help\n"
        "  /clear   clear the conversation history\n"
        "  /exit    quit (also /quit or 'exit')\n"
        "\n"
        "Anything else is sent to the agent.\n");
    fflush(stdout);
}

static int run_once(const agent_config_t *cfg, const char *task) {
    agent_session_t *s = agent_session_new(cfg);
    if (!s) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    int rc = agent_chat(s, task);
    agent_session_free(s);
    return rc;
}

static int interactive_loop(const agent_config_t *cfg) {
    agent_session_t *s = agent_session_new(cfg);
    if (!s) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    printf("igor - coding agent. Type a task, or /help, /clear, /exit.\n");
    fflush(stdout);

    int rc = 0;
    for (;;) {
        printf("igor> ");
        fflush(stdout);

        char *line = read_line(stdin);
        if (!line) {
            printf("\n");
            break;
        }
        trim(line);

        if (*line == 0) {
            free(line);
            continue;
        }
        if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0 || strcmp(line, "exit") == 0) {
            free(line);
            break;
        }
        if (strcmp(line, "/help") == 0) {
            print_help();
            free(line);
            continue;
        }
        if (strcmp(line, "/clear") == 0) {
            agent_session_reset(s);
            printf("conversation cleared\n");
            free(line);
            continue;
        }

        rc = agent_chat(s, line);
        free(line);
        printf("\n");
        fflush(stdout);
    }

    agent_session_free(s);
    return rc;
}

int main(int argc, char **argv) {
    agent_config_t cfg;
    cfg.api_key = env_or("LLM_API_KEY", LLM_API_KEY);
    if (!cfg.api_key) {
        fprintf(stderr, "error: LLM_API_KEY is not set\n");
        return 1;
    }
    cfg.base_url = env_or("LLM_BASE_URL", LLM_BASE_URL);
    cfg.model = env_or("LLM_MODEL", LLM_MODEL);
    const char *ms = getenv("LLM_MAX_STEPS");
    cfg.max_steps = (ms && *ms) ? atoi(ms) : 8;
    if (cfg.max_steps <= 0) cfg.max_steps = 8;

    if (argc > 1) {
        return run_once(&cfg, argv[1]);
    }

    if (stdin_is_tty()) {
        return interactive_loop(&cfg);
    }

    char *task = read_stdin();
    if (!task || !*task) {
        fprintf(stderr, "usage: igor \"task\"   (or run interactively: igor)\n");
        if (task) free(task);
        return 1;
    }
    int rc = run_once(&cfg, task);
    free(task);
    return rc;
}
