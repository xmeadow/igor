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
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/time.h>
#define stdin_is_tty() isatty(fileno(stdin))
#endif

#include "agent.h"
#include "config.h"

#define IGOR_VERSION "0.8.1"

/* The one line that says what is happening, drawn on stderr and overwritten in
 * place; defined with the rest of the output code below. */
static void status_show(const char *text);

/* The model's answer is marked with `igor> ` where it starts, not where the
 * request starts - otherwise the trace scrolls in under the marker and the
 * answer has nothing in front of it. This is 0 while a request runs: the next
 * answer text begins a segment. status_mark() sets it back whenever igor starts
 * a new activity, so each answer block after a thought or a tool call is marked
 * again. */
static int answer_started = 1;
static int stdout_tty;

#ifndef LLM_API_KEY
#define LLM_API_KEY NULL
#endif
#ifndef LLM_BASE_URL
#define LLM_BASE_URL "https://api.openai.com/v1"
#endif
#ifndef LLM_MODEL
#define LLM_MODEL "gpt-4o-mini"
#endif

/* Where an interactive session keeps its conversation, relative to the working
 * directory - the same place the project's skills live. */
#define HISTORY_FILE ".igor/history.jsonl"

/* Ceiling for one request, in tokens. DeepSeek reports its model windows in the
 * tens of thousands, so this sits well inside every model igor is likely to be
 * pointed at, and it keeps the cost of a long session from climbing on every
 * step. Raise it for a model with a bigger window; igor trims to whatever this
 * says. */
#define DEFAULT_CONTEXT_TOKENS 32000

/* A setting is looked for in the environment first, then in the file the setup
 * wrote, then in the built-in default. The environment wins so a single run can
 * be pointed somewhere else without disturbing what was set up once. */
static const char *setting(const char *name, const char *def) {
    const char *v = getenv(name);
    if (v && *v) return v;
    v = config_get(name);
    if (v && *v) return v;
    return def;
}

/* A switch is on unless it is explicitly off, so `IGOR_X=1` and `IGOR_X=yes`
 * enable it; unset or empty falls back to the default. */
static int setting_on(const char *name, int def) {
    const char *v = setting(name, NULL);
    if (!v || !*v) return def;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

/* For a setting that otherwise names a path: exactly these turn it off, and
 * anything else is a path. Compared whole, so a file called notes.jsonl is not
 * mistaken for "no". */
static int value_off(const char *v) {
    return strcmp(v, "off") == 0 || strcmp(v, "none") == 0 ||
           strcmp(v, "-") == 0 || strcmp(v, "0") == 0;
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
    case IGOR_PROMPT:  return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    default:           return have_default_attr ? default_attr : 7;
    }
}

static void put_text(const char *s, int kind, int to_stderr) {
    HANDLE h = out_handle(to_stderr);
    FILE *f = to_stderr ? stderr : stdout;

    status_show(NULL); /* never write into the line the status is using */
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
    case IGOR_THOUGHT: return "\033[2m";   /* dim */
    case IGOR_NOTE:    return "\033[36m";  /* cyan */
    case IGOR_ERROR:   return "\033[1;31m"; /* red */
    case IGOR_PROMPT:  return "\033[1;32m"; /* green */
    default:           return "";
    }
}

static void put_text(const char *s, int kind, int to_stderr) {
    FILE *f = to_stderr ? stderr : stdout;
    int tty = to_stderr ? stderr_is_tty : stdout_is_tty;

    status_show(NULL); /* never write into the line the status is using */
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
    if (kind == IGOR_TEXT && !answer_started && stdout_tty) {
        const char *p = s, *q = s;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q) {
            answer_started = 1;
            while (*p == '\n' || *p == '\r') p++; /* not before the mark */
            put_text("\n", IGOR_TEXT, 0);
            put_text("igor> ", IGOR_PROMPT, 0);
            s = p;
        }
    }
    put_text(s, kind, 0);
}

static void note_kind(const char *s, int kind) {
    put_text(s, kind, 1);
}

/* ---- the status line ----
 *
 * One line on stderr that is overwritten in place while a request runs, so the
 * screen says what is happening without filling up with it. Drawn on stderr so
 * a piped answer stays clean, and only on a terminal - everywhere else it would
 * be noise in a log.
 */
static int status_drawn; /* a line is on screen right now */
static int status_tty;

static int status_columns(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
        int w = info.dwSize.X;
        if (w > 4) return w < 100 ? w : 100;
    }
#else
    {
        struct winsize ws;
        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 4)
            return ws.ws_col < 100 ? ws.ws_col : 100;
    }
#endif
    return 80;
}

/* igor is about to do something; whatever it answers next is a new block. */
static void status_mark(const char *text) {
    if (text) answer_started = 0;
    status_show(text);
}

/* NULL clears the line. Called often, so it must be cheap when nothing is up. */
static void status_show(const char *text) {
    static int width;
    char line[128];
    int n, i;

    if (!status_tty) return;
    if (!text) {
        if (!status_drawn) return;
        status_drawn = 0;
        text = "";
    }
    if (!width) width = status_columns();

    n = snprintf(line, sizeof(line), "%s", text ? text : "");
    if (n > width - 1) {
        n = width - 1;
        line[n] = 0;
    }
    fputc('\r', stderr);
    fputs(line, stderr);
    for (i = n; i < width - 1; i++) fputc(' ', stderr);
    fputc('\r', stderr);
    fflush(stderr);
    status_drawn = (n > 0);
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

/* ---- first-run setup ----
 *
 * Without a key igor used to print one line and stop, which tells somebody who
 * has just built it nothing about what to do next. Run from a terminal it now
 * asks instead, and writes the answers where the next start will find them.
 */

/* Typing a key into a screen that echoes it is how keys end up in screenshots
 * and scrollback. Turning the echo off is best-effort: a console that will not
 * do it still gets to finish the setup, it just shows what is typed. */
static int echo_was_off;

#ifdef _WIN32
static DWORD saved_console_mode;

static int echo_off(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(h, &mode)) return 0;
    saved_console_mode = mode;
    if (!SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT)) return 0;
    echo_was_off = 1;
    return 1;
}

static void echo_restore(void) {
    if (!echo_was_off) return;
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_console_mode);
    echo_was_off = 0;
}
#else
static struct termios saved_termios;

/* TCSADRAIN, not TCSAFLUSH: flushing would throw away input that is already
 * queued, which is how a setup driven from a script loses the key it was fed.
 * What that would buy - discarding a key typed before the prompt - is worth
 * little, since the echo it was meant to prevent has happened by then. */
static int echo_off(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return 0;
    saved_termios = t;
    t.c_lflag &= ~(tcflag_t)ECHO;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &t) != 0) return 0;
    echo_was_off = 1;
    return 1;
}

static void echo_restore(void) {
    if (!echo_was_off) return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_termios);
    echo_was_off = 0;
}
#endif

/* Ask one question and return the answer, or a copy of def when the line is
 * left empty. NULL means end-of-input: the caller gives up rather than looping
 * on a stdin that has nothing left to give. */
static char *ask(const char *question, const char *def, int hidden) {
    char *line;

    out_puts(question);
    if (def && *def) {
        out_puts(" [");
        out_puts(def);
        out_puts("]");
    }
    out_puts(": ");
    fflush(stdout);

    if (hidden) echo_off();
    line = read_line_utf8(stdin);
    if (hidden) {
        echo_restore();
        out_puts("\n"); /* the user's Return was swallowed with the echo */
        fflush(stdout);
    }
    if (!line) return NULL;

    trim(line);
    if (!*line) {
        free(line);
        return def ? strdup(def) : strdup("");
    }
    return line;
}

/* The endpoints people are most likely to have an account with, so the common
 * case is one keypress. Anything speaking the OpenAI protocol works, which is
 * what the last entry is for. */
static const struct {
    const char *name;
    const char *base_url;
    const char *model;
} providers[] = {
    { "OpenAI",     "https://api.openai.com/v1",    "gpt-4o-mini" },
    { "DeepSeek",   "https://api.deepseek.com",     "deepseek-chat" },
    { "Mistral",    "https://api.mistral.ai/v1",    "mistral-small-latest" },
    { "OpenRouter", "https://openrouter.ai/api/v1", "openai/gpt-4o-mini" },
    { "A local server (Ollama, llama.cpp, ...)", "http://localhost:11434/v1", "llama3.2" },
};
#define PROVIDER_COUNT ((int)(sizeof(providers) / sizeof(providers[0])))

/* Returns 1 when settings were written, 0 when the user gave up. */
static int run_setup(void) {
    char path[512];
    char line[64];
    const char *err = NULL;
    const char *base_default, *model_default;
    char *key = NULL, *base_url = NULL, *model = NULL;
    int choice = 0, local;

    out_puts("\nigor needs an API key for a language model before it can do\n"
             "anything. This asks once and remembers the answers.\n\n");

    for (int i = 0; i < PROVIDER_COUNT; i++) {
        snprintf(line, sizeof(line), "  %d) ", i + 1);
        out_puts(line);
        out_puts(providers[i].name);
        out_puts("\n");
    }
    snprintf(line, sizeof(line), "  %d) Something else\n\n", PROVIDER_COUNT + 1);
    out_puts(line);

    for (;;) {
        char *answer = ask("Which one", "1", 0);
        if (!answer) return 0;
        choice = atoi(answer);
        free(answer);
        if (choice >= 1 && choice <= PROVIDER_COUNT + 1) break;
        out_puts("Pick one of the numbers above.\n");
        fflush(stdout);
    }

    if (choice <= PROVIDER_COUNT) {
        base_default = providers[choice - 1].base_url;
        model_default = providers[choice - 1].model;
    } else {
        base_default = NULL;
        model_default = NULL;
    }
    /* A server on this machine usually wants no key at all. */
    local = (choice <= PROVIDER_COUNT &&
             strncmp(providers[choice - 1].base_url, "http://localhost", 16) == 0);

    base_url = ask("API base URL", base_default, 0);
    if (!base_url || !*base_url) goto give_up;

    out_puts("\nThe key is not shown while you type it.\n");
    key = ask("API key", local ? "none" : NULL, 1);
    if (!key || !*key) goto give_up;

    model = ask("Model", model_default, 0);
    if (!model || !*model) goto give_up;

    if (!config_save(key, base_url, model, &err)) {
        out_kind("\ncould not save the settings", IGOR_ERROR);
        if (err) {
            out_kind(": ", IGOR_ERROR);
            out_kind(err, IGOR_ERROR);
        }
        out_puts("\n");
        goto give_up;
    }

    out_puts("\nSaved");
    if (config_path(path, sizeof(path))) {
        out_puts(" to ");
        out_puts(path);
    }
    out_puts(".\nChange it later with `igor --setup`, or set LLM_API_KEY,\n"
             "LLM_BASE_URL and LLM_MODEL in the environment to override it.\n\n");
    fflush(stdout);

    free(key);
    free(base_url);
    free(model);
    return 1;

give_up:
    free(key);
    free(base_url);
    free(model);
    return 0;
}

static void print_help(void) {
    out_puts("Commands:\n"
             "  /help    show this help\n"
             "  /setup   change the API key, base URL and model\n"
             "  /clear   forget the conversation, history file included\n"
             "  /exit    quit (also /quit or 'exit')\n"
             "\n"
             "Anything else is sent to the agent.\n");
    fflush(stdout);
}

static void print_usage(void) {
    printf("igor " IGOR_VERSION " - a small coding agent\n"
           "\n"
           "usage:\n"
           "  igor                 interactive chat\n"
           "  igor \"task\"          run one task and exit\n"
           "  echo task | igor     run one task read from stdin\n"
           "\n"
           "options:\n"
           "  --setup              set the API key, base URL and model, and save them\n"
           "  --version            print the version\n"
           "  --help               print this\n"
           "\n"
           "Settings come from the environment (LLM_API_KEY, LLM_BASE_URL,\n"
           "LLM_MODEL) first, then from the file written by --setup.\n");
}

static int run_once(const agent_config_t *cfg, const char *task) {
    agent_session_t *s = agent_session_new(cfg);
    if (!s) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    answer_started = 1;
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

/* Copy the three provider settings out of the environment and the saved file.
 * They are copied because --setup rewrites the file and drops what was read
 * from it, and a pointer into that would not survive the rewrite. */
static void load_provider_settings(agent_config_t *cfg) {
    const char *v;

    free((void *)cfg->api_key);
    free((void *)cfg->base_url);
    free((void *)cfg->model);

    v = setting("LLM_API_KEY", LLM_API_KEY);
    cfg->api_key = v ? strdup(v) : NULL;
    cfg->base_url = strdup(setting("LLM_BASE_URL", LLM_BASE_URL));
    cfg->model = strdup(setting("LLM_MODEL", LLM_MODEL));
}

static int interactive_loop(agent_config_t *cfg) {
    agent_session_t *s = agent_session_new(cfg);
    if (!s) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    out_puts("igor " IGOR_VERSION " - coding agent. Type a task, or /help, /setup, /clear, /exit.\n\n");
    fflush(stdout);

    int rc = 0;
    for (;;) {
        out_kind("you> ", IGOR_PROMPT);
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
            answer_started = 1; /* a plain notice, not an answer block */
            print_help();
            free(line);
            continue;
        }
        if (strcmp(line, "/clear") == 0) {
            agent_session_reset(s);
            answer_started = 1; /* a plain notice, not an answer block */
            out_puts("conversation cleared\n");
            fflush(stdout);
            free(line);
            continue;
        }
        if (strcmp(line, "/setup") == 0) {
            answer_started = 1; /* a plain notice, not an answer block */
            free(line);
            if (run_setup()) {
                /* The session holds a copy of the settings, so it is built
                 * again to pick the new ones up. The conversation is on disk
                 * and comes back with it. */
                agent_session_t *fresh;
                load_provider_settings(cfg);
                fresh = agent_session_new(cfg);
                if (!fresh) {
                    out_kind("error: out of memory\n", IGOR_ERROR);
                    break;
                }
                agent_session_free(s);
                s = fresh;
            }
            continue;
        }

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
    agent_config_t cfg = {0};
    int history_off = 0;
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            default_attr = info.wAttributes;
            have_default_attr = 1;
        }
        status_tty = is_console(GetStdHandle(STD_ERROR_HANDLE));
        stdout_tty = is_console(GetStdHandle(STD_OUTPUT_HANDLE));
    }
#else
    stdout_is_tty = isatty(fileno(stdout));
    stderr_is_tty = isatty(fileno(stderr));
    status_tty = stderr_is_tty;
    stdout_tty = stdout_is_tty;
#endif

    if (argc > 1) {
        if (strcmp(argv[1], "--setup") == 0)
            return run_setup() ? 0 : 1;
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            printf("igor " IGOR_VERSION "\n");
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return 0;
        }
        /* Anything else is the task - but a mistyped option is not a task, and
         * sending it to the model would hide the typo behind an answer. */
        if (argv[1][0] == '-' && argv[1][1] == '-') {
            fprintf(stderr, "error: unknown option %s\n\n", argv[1]);
            print_usage();
            return 1;
        }
    }

    load_provider_settings(&cfg);
    if (!cfg.api_key || !*cfg.api_key) {
        /* Nothing in the environment and nothing saved. Somebody sitting at a
         * terminal can be asked for it; a script cannot, and is told where to
         * put it instead. */
        if (stdin_is_tty() && stdout_tty && run_setup())
            load_provider_settings(&cfg);
    }
    if (!cfg.api_key || !*cfg.api_key) {
        fprintf(stderr, "error: no API key. Run `igor --setup`, "
                        "or set LLM_API_KEY in the environment.\n");
        return 1;
    }
    cfg.out = out_kind;
    cfg.note = note_kind;
    cfg.status = status_mark;
    cfg.stream = setting_on("LLM_STREAM", 1);
    /* Off by default: shown in full, the reasoning buries the answer. It is
     * still reachable for debugging with IGOR_SHOW_THINKING=1. */
    cfg.show_thought = setting_on("IGOR_SHOW_THINKING", 0);
    cfg.context_tokens = DEFAULT_CONTEXT_TOKENS;
    cfg.history_path = NULL;
    {
        const char *v = setting("IGOR_CONTEXT_TOKENS", NULL);
        if (v && *v) {
            long n = atol(v);
            if (n > 0) cfg.context_tokens = n;
        }
    }

    /* Where the conversation is kept. IGOR_HISTORY names a file - useful for
     * scripted runs and for keeping several conversations apart - or turns
     * persistence off with off/none/-/0. Left unset, an interactive session
     * keeps one and a one-shot task keeps nothing. */
    {
        const char *v = setting("IGOR_HISTORY", NULL);
        history_off = (v && *v) ? value_off(v) : 0;
        if (v && *v && !history_off) cfg.history_path = v;
    }
    const char *ms = setting("LLM_MAX_STEPS", NULL);
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
        /* A chat that is gone when the process exits is not a chat, so an
         * interactive session keeps one unless told otherwise. A one-shot task
         * has nothing to come back to. */
        if (!cfg.history_path && !history_off) cfg.history_path = HISTORY_FILE;
        return interactive_loop(&cfg);
    }

    char *task = read_stdin();
    if (!task || !*task) {
        print_usage();
        if (task) free(task);
        return 1;
    }
    int rc = run_once(&cfg, task);
    free(task);
    return rc;
}
