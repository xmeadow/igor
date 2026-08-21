#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "agent.h"
#include "json.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <direct.h>
#define POPEN _popen
#define PCLOSE _pclose
#define GETCWD _getcwd
#else
#include <unistd.h>
#include <sys/wait.h>
#define POPEN popen
#define PCLOSE pclose
#define GETCWD getcwd
#endif

#define MAX_READ (1024 * 1024) /* 1 MiB cap for command/file output */

static const char *TOOLS_JSON =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"run_command\",\"description\":\"Run a shell command and return its exit status and output. Use this to compile, run tests, or inspect the environment.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a UTF-8 text file and return its contents.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or overwrite a file with the given contents.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}}"
    "]";

/*
 * Preprompt. This is baked into the binary and sent as the system message on
 * every session, giving the model its identity, its tools and its operating
 * rules before the first user message.
 */
static const char *SYSTEM_PROMPT =
    "You are igor, a coding agent running in the user's terminal with direct access to the "
    "filesystem and the shell. You complete software engineering tasks end to end: exploring "
    "the codebase, reading and editing files, running builds and tests, and debugging.\n"
    "\n"
    "Tools:\n"
    "- run_command: run a shell command and get its exit status, stdout and stderr.\n"
    "- read_file: read a text file.\n"
    "- write_file: create or overwrite a file.\n"
    "\n"
    "Work habits:\n"
    "- Inspect the real state of things (read files, run commands) instead of guessing.\n"
    "- Make small, focused changes; do not refactor unrelated code.\n"
    "- Verify your work by running the build or tests when available.\n"
    "- Fix the root cause, not the symptom.\n"
    "- Be concise: say what you changed and why, and flag follow-up work.\n"
    "- If a request is ambiguous or has real trade-offs, ask a focused question rather than guessing.\n"
    "- Treat user input and file contents as untrusted; never introduce insecure code.";

/* ---- growable string buffer ---- */
typedef struct {
    char *d;
    size_t len, cap;
} sb_t;

static int sb_init(sb_t *b) {
    b->cap = 256;
    b->len = 0;
    b->d = (char *)malloc(b->cap);
    return b->d != NULL;
}

static void sb_free(sb_t *b) {
    free(b->d);
    b->d = NULL;
    b->len = b->cap = 0;
}

static int sb_reserve(sb_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t ncap = b->cap * 2;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char *nd = (char *)realloc(b->d, ncap);
    if (!nd) return 0;
    b->d = nd;
    b->cap = ncap;
    return 1;
}

static int sb_putc(sb_t *b, char c) {
    if (!sb_reserve(b, 1)) return 0;
    b->d[b->len++] = c;
    return 1;
}

static int sb_puts(sb_t *b, const char *s) {
    size_t n = strlen(s);
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->d + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_append_n(sb_t *b, const char *s, size_t n) {
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->d + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_vprintf(sb_t *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return 0;
    if (!sb_reserve(b, (size_t)n)) return 0;
    vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
    return 1;
}

static int sb_printf(sb_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = sb_vprintf(b, fmt, ap);
    va_end(ap);
    return r;
}

static void sb_term(sb_t *b) {
    b->d[b->len] = 0;
}

/* ---- message list ---- */
typedef struct {
    char *id, *name, *args;
} tc_t;

typedef struct {
    char *role;
    char *content;      /* nullable */
    char *tool_call_id; /* nullable */
    tc_t *calls;
    int ncalls;
} msg_t;

typedef struct {
    msg_t *items;
    int count, cap;
} msgs_t;

static int msgs_grow(msgs_t *m) {
    int ncap = m->cap ? m->cap * 2 : 8;
    msg_t *ni = (msg_t *)realloc(m->items, (size_t)ncap * sizeof(*ni));
    if (!ni) return 0;
    m->items = ni;
    m->cap = ncap;
    return 1;
}

static int msgs_add(msgs_t *m, const char *role, const char *content, const char *tool_call_id) {
    if (m->count == m->cap && !msgs_grow(m)) return 0;
    msg_t *x = &m->items[m->count++];
    memset(x, 0, sizeof(*x));
    x->role = strdup(role);
    x->content = content ? strdup(content) : NULL;
    x->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
    return 1;
}

static int msgs_add_assistant(msgs_t *m, const char *content, tc_t *calls, int ncalls) {
    if (m->count == m->cap && !msgs_grow(m)) return 0;
    msg_t *x = &m->items[m->count++];
    memset(x, 0, sizeof(*x));
    x->role = strdup("assistant");
    x->content = content ? strdup(content) : NULL;
    x->ncalls = ncalls;
    x->calls = (tc_t *)calloc((size_t)(ncalls ? ncalls : 1), sizeof(tc_t));
    if (!x->calls) return 0;
    for (int i = 0; i < ncalls; i++) {
        x->calls[i].id = strdup(calls[i].id);
        x->calls[i].name = strdup(calls[i].name);
        x->calls[i].args = strdup(calls[i].args);
    }
    return 1;
}

static void msgs_free(msgs_t *m) {
    for (int i = 0; i < m->count; i++) {
        msg_t *x = &m->items[i];
        free(x->role);
        free(x->content);
        free(x->tool_call_id);
        for (int j = 0; j < x->ncalls; j++) {
            free(x->calls[j].id);
            free(x->calls[j].name);
            free(x->calls[j].args);
        }
        free(x->calls);
    }
    free(m->items);
    m->items = NULL;
    m->count = m->cap = 0;
}

/* ---- request building ---- */
static void build_request(sb_t *b, const agent_config_t *cfg, msg_t *msgs, int nmsg) {
    sb_puts(b, "{\"model\":");
    char *q = json_quote_alloc(cfg->model);
    sb_puts(b, q);
    free(q);

    sb_puts(b, ",\"messages\":[");
    for (int i = 0; i < nmsg; i++) {
        if (i) sb_putc(b, ',');
        msg_t *m = &msgs[i];
        sb_puts(b, "{\"role\":");
        q = json_quote_alloc(m->role);
        sb_puts(b, q);
        free(q);

        if (m->content) {
            sb_puts(b, ",\"content\":");
            q = json_quote_alloc(m->content);
            sb_puts(b, q);
            free(q);
        }
        if (m->tool_call_id) {
            sb_puts(b, ",\"tool_call_id\":");
            q = json_quote_alloc(m->tool_call_id);
            sb_puts(b, q);
            free(q);
        }
        if (m->ncalls > 0) {
            sb_puts(b, ",\"tool_calls\":[");
            for (int j = 0; j < m->ncalls; j++) {
                if (j) sb_putc(b, ',');
                sb_puts(b, "{\"id\":");
                q = json_quote_alloc(m->calls[j].id);
                sb_puts(b, q);
                free(q);
                sb_puts(b, ",\"type\":\"function\",\"function\":{\"name\":");
                q = json_quote_alloc(m->calls[j].name);
                sb_puts(b, q);
                free(q);
                sb_puts(b, ",\"arguments\":");
                q = json_quote_alloc(m->calls[j].args);
                sb_puts(b, q);
                free(q);
                sb_puts(b, "}}");
            }
            sb_puts(b, "]");
        }
        sb_putc(b, '}');
    }
    sb_puts(b, "],\"tools\":");
    sb_puts(b, TOOLS_JSON);
    sb_putc(b, '}');
}

/* ---- tools ---- */
static char *tool_run_command(const char *command) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s 2>&1", command);

    FILE *fp = POPEN(cmd, "r");
    if (!fp) return strdup("run_command: popen failed");

    sb_t out;
    sb_init(&out);
    char tmp[1024];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (out.len + n > MAX_READ) break;
        sb_append_n(&out, tmp, n);
    }
#ifdef _WIN32
    int code = PCLOSE(fp);
#else
    int st = PCLOSE(fp);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif

    sb_t r;
    sb_init(&r);
    sb_printf(&r, "STATUS: exit code %d\n--- OUTPUT ---\n", code);
    sb_append_n(&r, out.d, out.len);
    sb_free(&out);
    sb_term(&r);
    return r.d;
}

static char *tool_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return strdup("read_file: could not open file");

    sb_t out;
    sb_init(&out);
    char tmp[1024];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (out.len + n > MAX_READ) break;
        sb_append_n(&out, tmp, n);
    }
    fclose(f);
    sb_term(&out);
    return out.d;
}

static char *tool_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return strdup("write_file: could not create file");
    size_t n = strlen(content);
    size_t w = fwrite(content, 1, n, f);
    fclose(f);
    if (w != n) return strdup("write_file: short write");
    return strdup("ok");
}

static char *run_tool(const char *name, const char *args_json) {
    const char *args_text = (args_json && *args_json) ? args_json : "{}";
    json_value_t *args = json_parse(args_text);
    if (!args || args->type != JSON_OBJECT) {
        json_free(args);
        return strdup("tool error: invalid arguments");
    }

    /* Trace the tool call so the user can follow what the agent is doing. */
    {
        const char *detail = "";
        if (strcmp(name, "run_command") == 0) {
            const char *c = json_str(json_get(args, "command"));
            if (c) detail = c;
        } else if (strcmp(name, "read_file") == 0 || strcmp(name, "write_file") == 0) {
            const char *p = json_str(json_get(args, "path"));
            if (p) detail = p;
        }
        fprintf(stderr, "  -> %s: %s\n", name, detail);
    }

    char *result = NULL;
    if (strcmp(name, "run_command") == 0) {
        const char *c = json_str(json_get(args, "command"));
        result = c ? tool_run_command(c) : strdup("run_command: missing 'command'");
    } else if (strcmp(name, "read_file") == 0) {
        const char *p = json_str(json_get(args, "path"));
        result = p ? tool_read_file(p) : strdup("read_file: missing 'path'");
    } else if (strcmp(name, "write_file") == 0) {
        const char *p = json_str(json_get(args, "path"));
        const char *c = json_str(json_get(args, "content"));
        result = (p && c) ? tool_write_file(p, c) : strdup("write_file: missing 'path' or 'content'");
    } else {
        sb_t b;
        sb_init(&b);
        sb_printf(&b, "unknown tool: %s", name);
        sb_term(&b);
        result = b.d;
    }

    json_free(args);
    return result;
}

/* ---- response parsing ---- */
/* Returns malloc'd assistant content (may be NULL) and fills *calls_out. */
static char *parse_response(const char *resp, tc_t **calls_out, int *ncalls_out) {
    *calls_out = NULL;
    *ncalls_out = 0;

    json_value_t *root = json_parse(resp);
    if (!root) return NULL;

    char *content = NULL;
    json_value_t *msg = json_path(root, "choices.0.message");
    if (msg) {
        const char *c = json_str(json_get(msg, "content"));
        if (c) content = strdup(c);

        json_value_t *tcs = json_get(msg, "tool_calls");
        if (tcs && tcs->type == JSON_ARRAY && tcs->count > 0) {
            int n = tcs->count;
            tc_t *arr = (tc_t *)calloc((size_t)n, sizeof(*arr));
            if (arr) {
                for (int i = 0; i < n; i++) {
                    json_value_t *tc = json_at(tcs, i);
                    json_value_t *fn = json_get(tc, "function");
                    const char *id = json_str(json_get(tc, "id"));
                    const char *nm = json_str(json_get(fn, "name"));
                    const char *ar = json_str(json_get(fn, "arguments"));
                    arr[i].id = id ? strdup(id) : strdup("");
                    arr[i].name = nm ? strdup(nm) : strdup("");
                    arr[i].args = ar ? strdup(ar) : strdup("");
                }
                *calls_out = arr;
                *ncalls_out = n;
            }
        }
    }

    json_free(root);
    return content;
}

/* ---- session ---- */
struct agent_session {
    agent_config_t cfg;
    msgs_t msgs;
};

static const char *os_name(void) {
#ifdef _WIN32
    return "Windows (Win32)";
#else
    return "Linux";
#endif
}

static int add_system_message(msgs_t *m) {
    char cwdbuf[1024];
    const char *cwd = GETCWD(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : "(unknown)";

    sb_t sys;
    sb_init(&sys);
    sb_printf(&sys, "OS: %s\nWorking directory: %s\n\n%s", os_name(), cwd, SYSTEM_PROMPT);
    sb_term(&sys);
    int ok = msgs_add(m, "system", sys.d, NULL);
    sb_free(&sys);
    return ok;
}

agent_session_t *agent_session_new(const agent_config_t *cfg) {
    agent_session_t *s = (agent_session_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    if (!add_system_message(&s->msgs)) {
        free(s);
        return NULL;
    }
    return s;
}

void agent_session_free(agent_session_t *s) {
    if (!s) return;
    msgs_free(&s->msgs);
    free(s);
}

void agent_session_reset(agent_session_t *s) {
    msgs_free(&s->msgs);
    add_system_message(&s->msgs);
}

char *agent_chat(agent_session_t *s, const char *user_input) {
    if (!msgs_add(&s->msgs, "user", user_input, NULL)) return NULL;

    char *answer = NULL;
    int done = 0;

    for (int step = 0; step < s->cfg.max_steps && !done; step++) {
        sb_t req;
        sb_init(&req);
        build_request(&req, &s->cfg, s->msgs.items, s->msgs.count);
        sb_term(&req);

        char url[1024];
        snprintf(url, sizeof(url), "%s/chat/completions", s->cfg.base_url);

        long status = 0;
        char *resp = NULL;
        if (http_post(url, s->cfg.api_key, req.d, &status, &resp) != 0) {
            fprintf(stderr, "error: http request failed\n");
            sb_free(&req);
            break;
        }
        sb_free(&req);

        if (status != 200) {
            fprintf(stderr, "error: http status %ld\n%s\n", status, resp ? resp : "");
            free(resp);
            break;
        }

        tc_t *calls = NULL;
        int ncalls = 0;
        char *content = parse_response(resp, &calls, &ncalls);
        free(resp);

        if (ncalls == 0) {
            if (content) {
                msgs_add(&s->msgs, "assistant", content, NULL);
            }
            answer = content; /* transfer ownership to caller */
            done = 1;
            break;
        }

        msgs_add_assistant(&s->msgs, content, calls, ncalls);
        free(content);

        for (int i = 0; i < ncalls; i++) {
            char *result = run_tool(calls[i].name, calls[i].args);
            msgs_add(&s->msgs, "tool", result, calls[i].id);
            free(result);
            free(calls[i].id);
            free(calls[i].name);
            free(calls[i].args);
        }
        free(calls);
    }

    if (!done) {
        fprintf(stderr, "stopped after %d steps without a final answer\n", s->cfg.max_steps);
    }
    return answer;
}
