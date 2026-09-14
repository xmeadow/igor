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

/* Is s well-formed UTF-8 with at least one non-ASCII byte? */
static int looks_like_utf8(const char *s) {
    static const unsigned long min_cp[4] = {0, 0x80, 0x800, 0x10000};
    const unsigned char *p = (const unsigned char *)s;
    int non_ascii = 0;

    while (*p) {
        unsigned char c = *p;
        int extra;
        unsigned long cp;

        if (c < 0x80) { p++; continue; }
        non_ascii = 1;

        if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return 0;

        for (int i = 0; i < extra; i++) {
            if ((p[1] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (unsigned long)(p[1] & 0x3F);
            p++;
        }
        p++;

        if (cp < min_cp[extra] || cp > 0x10FFFFUL) return 0;
        if (cp >= 0xD800UL && cp <= 0xDFFFUL) return 0;
    }
    return non_ascii;
}

/* Command-line arguments arrive as ANSI (CP_ACP), which is right when the
 * user types at the console. When igor is driven from a UTF-8 environment
 * (ssh, a UTF-8 shell) the bytes are already UTF-8, and re-decoding them as
 * CP_ACP would double-encode every umlaut. Prefer UTF-8 when the bytes are
 * well-formed, fall back to CP_ACP otherwise.
 */
static char *arg_to_utf8(const char *s) {
    if (looks_like_utf8(s)) return strdup(s);
    return ansi_to_utf8(s);
}

static char *read_line_utf8(FILE *f) {
    (void)f;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    size_t cap = 256, len = 0;
    wchar_t *w = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!w) return NULL;

    for (;;) {
        wchar_t chunk[256];
        DWORD nread = 0;
        if (!ReadConsoleW(h, chunk, 256, &nread, NULL)) {
            free(w);
            return NULL;
        }
        int stop = 0;
        for (DWORD i = 0; i < nread; i++) {
            wchar_t c = chunk[i];
            if (c == L'\n') { stop = 1; break; }
            if (c == L'\r') continue;
            if (len + 1 >= cap) {
                cap *= 2;
                wchar_t *nw = (wchar_t *)realloc(w, cap * sizeof(wchar_t));
                if (!nw) { free(w); return NULL; }
                w = nw;
            }
            w[len++] = c;
        }
        if (stop || nread == 0) break;
    }

    if (len == 0) { free(w); return NULL; }
    w[len] = 0;
    char *out = wide_to_utf8(w);
    free(w);
    return out;
}

/* ---- output ----
 *
 * Everything the user sees goes through here, because the three kinds of text
 * have to stay apart: what the model thinks, what it did, and what it answers.
 * On a console the difference is colour; anywhere else it is nothing, so the
 * markers the caller adds carry it.
 */

static WORD default_attr;
static int have_default_attr;

static HANDLE out_handle(int to_stderr) {
    return GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}

static int is_console(HANDLE h) {
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

static WORD attr_for(int kind) {
    switch (kind) {
    case IGOR_THOUGHT: return FOREGROUND_INTENSITY; /* dim */
    case IGOR_NOTE:    return FOREGROUND_GREEN | FOREGROUND_BLUE; /* cyan */
    case IGOR_ERROR:   return FOREGROUND_RED | FOREGROUND_INTENSITY;
    default:           return have_default_attr ? default_attr : 7;
    }
}

static void put_text(const char *s, int kind, int to_stderr) {
    HANDLE h = out_handle(to_stderr);
    FILE *f = to_stderr ? stderr : stdout;

    if (is_console(h)) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        if (n <= 0) return;
        wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
        if (!w) return;
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
        if (kind != IGOR_TEXT) SetConsoleTextAttribute(h, attr_for(kind));
        DWORD written = 0;
        WriteConsoleW(h, w, (DWORD)(n - 1), &written, NULL);
        if (kind != IGOR_TEXT) SetConsoleTextAttribute(h, attr_for(IGOR_TEXT));
        free(w);
    } else {
        fwrite(s, 1, strlen(s), f);
    }
    /* Streamed answers arrive in fragments: flush so each one shows at once. */
    fflush(f);
}

#else /* POSIX */

#define IGOR_ATTR_RESET "\033[0m"

static int stdout_is_tty, stderr_is_tty;

static const char *attr_for(int kind) {
    switch (kind) {
    case IGOR_THOUGHT: return "\033[2m";  /* dim */
    case IGOR_NOTE:    return "\033[36m"; /* cyan */
    case IGOR_ERROR:   return "\033[31m"; /* red */
    default:           return "";
    }
}

static void put_text(const char *s, int kind, int to_stderr) {
    FILE *f = to_stderr ? stderr : stdout;
    int tty = to_stderr ? stderr_is_tty : stdout_is_tty;

    if (tty && kind != IGOR_TEXT) fputs(attr_for(kind), f);
    fputs(s, f);
    if (tty && kind != IGOR_TEXT) fputs(IGOR_ATTR_RESET, f);
    fflush(f);
}

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

#endif /* _WIN32 */

static void out_puts(const char *s) {
    put_text(s, IGOR_TEXT, 0);
}

static void out_kind(const char *s, int kind) {
    put_text(s, kind, 0);
}

static void note_kind(const char *s, int kind) {
    put_text(s, kind, 1);
}

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
    int rc = ans ? 0 : 1;
    free(ans);
    /* The answer itself was already written by the session, either streamed or
     * once it had arrived. */
    out_puts("\n");
    fflush(stdout);
    return rc;
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

        out_puts("igor> ");
        fflush(stdout);

        char *ans = agent_chat(s, line);
        free(line);
        free(ans);
        out_puts("\n\n");
        fflush(stdout);
    }

    agent_session_free(s);
    return rc;
}

int main(int argc, char **argv) {
    agent_config_t cfg;
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            default_attr = info.wAttributes;
            have_default_attr = 1;
        }
    }
#else
    stdout_is_tty = isatty(fileno(stdout));
    stderr_is_tty = isatty(fileno(stderr));
#endif

    cfg.api_key = env_or("LLM_API_KEY", LLM_API_KEY);
    if (!cfg.api_key) {
        fprintf(stderr, "error: LLM_API_KEY is not set\n");
        return 1;
    }
    cfg.base_url = env_or("LLM_BASE_URL", LLM_BASE_URL);
    cfg.model = env_or("LLM_MODEL", LLM_MODEL);
    cfg.out = out_kind;
    cfg.note = note_kind;
    cfg.stream = 1;
    cfg.show_thought = 1;
    {
        const char *v = getenv("LLM_STREAM");
        if (v && *v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'|| v[0] == 'f' || v[0] == 'F'))
            cfg.stream = 0;
        v = getenv("IGOR_SHOW_THINKING");
        if (v && *v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'|| v[0] == 'f' || v[0] == 'F'))
            cfg.show_thought = 0;
    }
    const char *ms = getenv("LLM_MAX_STEPS");
    cfg.max_steps = (ms && *ms) ? atoi(ms) : 16;
    if (cfg.max_steps <= 0) cfg.max_steps = 16;

    if (argc > 1) {
#ifdef _WIN32
        char *task = arg_to_utf8(argv[1]);
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
