#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <wchar.h>
#include <wctype.h>
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

/* ---- UTF-8 aware console I/O ----
 *
 * On Windows the console is in the OEM code page, not UTF-8. We read and write
 * through the wide-character APIs and convert to/from UTF-8, so non-ASCII
 * (umlauts, ...) round-trips correctly to the LLM API.
 */

#ifdef _WIN32

static char *wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return strdup("");
    char *out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    return out; /* n includes the NUL terminator */
}

static char *ansi_to_utf8(const char *s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) return strdup(s);
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return strdup(s);
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    char *out = wide_to_utf8(w);
    free(w);
    return out ? out : strdup(s);
}

static char *read_line_utf8(FILE *f) {
    size_t cap = 256, len = 0;
    wchar_t *w = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!w) return NULL;
    wint_t c;
    while ((c = fgetwc(f)) != WEOF) {
        if (c == L'\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            wchar_t *nw = (wchar_t *)realloc(w, cap * sizeof(wchar_t));
            if (!nw) { free(w); return NULL; }
            w = nw;
        }
        w[len++] = (wchar_t)c;
    }
    if (c == WEOF && len == 0) { free(w); return NULL; }
    w[len] = 0;
    char *out = wide_to_utf8(w);
    free(w);
    return out;
}

static void out_puts(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    fputws(w, stdout);
    free(w);
}

#else /* POSIX */

static char *read_line_utf8(FILE *f) {
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

static void out_puts(const char *s) {
    fputs(s, stdout);
}

#endif /* _WIN32 */

/* Read the entire remaining stdin (piped one-shot task). */
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

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void print_help(void) {
    out_puts("Commands:\n"
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
    char *ans = agent_chat(s, task);
    agent_session_free(s);
    if (ans) {
        out_puts(ans);
        out_puts("\n");
        free(ans);
        fflush(stdout);
        return 0;
    }
    return 1;
}

static int interactive_loop(const agent_config_t *cfg) {
    agent_session_t *s = agent_session_new(cfg);
    if (!s) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    out_puts("igor - coding agent. Type a task, or /help, /clear, /exit.\n\n");
    fflush(stdout);

    int rc = 0;
    for (;;) {
        out_puts("you> ");
        fflush(stdout);

        char *line = read_line_utf8(stdin);
        if (!line) {
            out_puts("\n");
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
            out_puts("conversation cleared\n");
            fflush(stdout);
            free(line);
            continue;
        }

        char *ans = agent_chat(s, line);
        free(line);

        if (ans) {
            out_puts("igor> ");
            out_puts(ans);
            out_puts("\n\n");
            free(ans);
            fflush(stdout);
        }
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
#ifdef _WIN32
        char *task = ansi_to_utf8(argv[1]);
        int rc = run_once(&cfg, task);
        free(task);
        return rc;
#else
        return run_once(&cfg, argv[1]);
#endif
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
