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
#include <windows.h>
#include <tlhelp32.h>
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
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a text file, or a byte range of any file. Binary content is returned as a hex dump with absolute offsets.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"description\":\"first byte to read, defaults to 0\"},\"limit\":{\"type\":\"integer\",\"description\":\"how many bytes to read\"}},\"required\":[\"path\"]}}},"
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
    "- Stop when the task is done: once you have what was asked for, write it up rather than"
    " verifying further. If two approaches to the same sub-problem fail, say what is missing and"
    " report what you did establish, instead of trying more variants until the step budget is gone.\n"
    "- Be concise: say what you changed and why, and flag follow-up work.\n"
    "- If a request is ambiguous or has real trade-offs, ask a focused question rather than guessing.\n"
    "- Use the correct shell for the system (see the Shell field); on Windows that is cmd.exe, not PowerShell.\n"
    "- Check that a tool is installed before building a plan on one (Windows: `where <tool>`), and"
    " use the tools this machine actually has - where the platform has a system directory they are"
    " listed at the top of this message. Do not assume curl, wget, git, python or PowerShell exist.\n"
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
static void build_request(sb_t *b, const agent_config_t *cfg, msg_t *msgs, int nmsg, int with_tools) {
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
    sb_putc(b, ']');
    /* Left out for the wrap-up turn: without tools the model has to answer. */
    if (with_tools) {
        sb_puts(b, ",\"tools\":");
        sb_puts(b, TOOLS_JSON);
    }
    sb_putc(b, '}');
}

/* ---- text sanitising ----
 *
 * Tool output is a mixture: data a tool writes in UTF-8 sits next to cmd.exe's
 * own messages, which use the OEM code page. The API rejects a request body
 * that is not valid UTF-8, so well-formed sequences are passed through and
 * stray bytes are decoded from the OEM code page and re-encoded. Call this on
 * a complete buffer, never on a chunk, or a multi-byte sequence split across
 * the chunk boundary would be misread.
 */

/* Files are treated as binary when they contain a NUL byte - the same rule
 * git uses. Text stays text even when it is not valid UTF-8, because
 * sb_append_utf8 repairs that below. */
static int looks_binary(const char *s, size_t len) {
    return memchr(s, 0, len) != NULL;
}

#ifdef _WIN32
/* Length of the well-formed UTF-8 sequence at p, or 0 if there is none. */
static int utf8_seq_len(const unsigned char *p, size_t avail) {
    static const unsigned long min_cp[4] = {0, 0x80, 0x800, 0x10000};
    unsigned char c = p[0];
    int extra, i;
    unsigned long cp;

    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0;

    if ((size_t)extra + 1 > avail) return 0;
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (unsigned long)(p[i] & 0x3F);
    }
    if (cp < min_cp[extra] || cp > 0x10FFFFUL) return 0;
    if (cp >= 0xD800UL && cp <= 0xDFFFUL) return 0;
    return extra + 1;
}

static void sb_append_utf8(sb_t *b, const char *s, size_t len) {
    size_t i = 0;

    while (i < len) {
        int n = utf8_seq_len((const unsigned char *)s + i, len - i);
        if (n > 0 && (unsigned char)s[i] != 0) {
            sb_append_n(b, s + i, (size_t)n);
            i += (size_t)n;
            continue;
        }
        wchar_t w = 0;
        char u8[8];
        int m = MultiByteToWideChar(CP_OEMCP, 0, s + i, 1, &w, 1);
        if (m == 1) m = WideCharToMultiByte(CP_UTF8, 0, &w, 1, u8, sizeof(u8), NULL, NULL);
        if (m > 0) sb_append_n(b, u8, (size_t)m);
        else sb_putc(b, '?');
        i++;
    }
}
#else
static void sb_append_utf8(sb_t *b, const char *s, size_t len) {
    sb_append_n(b, s, len);
}
#endif

/* How much of a binary file a hex dump shows. Reading one is the only way to
 * look at bytes here: neither the target platform nor POSIX guarantees xxd,
 * od or hexdump. */
#define DUMP_MAX 2048

static void sb_put_hex_dump(sb_t *b, const char *s, size_t len, long base) {
    size_t i, j, shown = len < DUMP_MAX ? len : DUMP_MAX;

    for (i = 0; i < shown; i += 16) {
        sb_printf(b, "%08lx  ", (unsigned long)(base + (long)i));
        for (j = 0; j < 16; j++) {
            if (i + j < shown) sb_printf(b, "%02x ", (unsigned char)s[i + j]);
            else sb_puts(b, "   ");
        }
        sb_putc(b, ' ');
        for (j = 0; j < 16 && i + j < shown; j++) {
            unsigned char c = (unsigned char)s[i + j];
            sb_putc(b, (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        sb_putc(b, '\n');
    }
    if (shown < len)
        sb_printf(b, "... stopped after %lu bytes; ask for another offset to see more\n",
                  (unsigned long)shown);
}

/* ---- tools ---- */

#ifdef _WIN32
/* Command timeout: a command that never returns (a script waiting on a dead
 * network call, say) would otherwise hang the agent for good and leave a
 * process holding igor.exe. Override with IGOR_COMMAND_TIMEOUT (seconds). */
#define COMMAND_TIMEOUT_MS 120000

static int command_timeout_ms(void) {
    const char *v = getenv("IGOR_COMMAND_TIMEOUT");
    if (v && *v) {
        int secs = atoi(v);
        if (secs > 0) return secs * 1000;
    }
    return COMMAND_TIMEOUT_MS;
}

/* Terminate everything below pid: children, grandchildren, and so on. Needed
 * where job objects do not exist - ReactOS' AssignProcessToJobObject returns
 * ERROR_INVALID_FUNCTION - so terminating cmd.exe alone would leave the
 * command it launched running. */
#define KILL_TREE_MAX 64

static void kill_process_tree(DWORD pid) {
    DWORD victims[KILL_TREE_MAX];
    int n = 1, i, pass, added;

    victims[0] = pid;
    for (pass = 0; pass < 4; pass++) {
        HANDLE snap;
        PROCESSENTRY32 pe;

        added = 0;
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) break;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                for (i = 0; i < n; i++)
                    if (pe.th32ParentProcessID == victims[i]) break;
                if (i == n) continue; /* not a descendant */
                for (i = 0; i < n; i++)
                    if (pe.th32ProcessID == victims[i]) break;
                if (i < n || n >= KILL_TREE_MAX) continue; /* already collected */
                victims[n++] = pe.th32ProcessID;
                added = 1;
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        if (!added) break;
    }

    for (i = 0; i < n; i++) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, victims[i]);
        if (h) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }
}

/* A job object so that killing an overrunning command takes its whole process
 * tree with it. Works on Windows; on ReactOS every step may fail, which
 * kill_process_tree above then covers. */
static void note_job_unavailable(const char *step);

static HANDLE create_job(void) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    HANDLE job = CreateJobObjectA(NULL, NULL);

    if (!job) {
        note_job_unavailable("CreateJobObject");
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        note_job_unavailable("SetInformationJobObject");
    return job;
}

static void note_job_unavailable(const char *step) {
    static int reported = 0;
    if (reported) return;
    reported = 1;
    fprintf(stderr, "  -- job objects unavailable here (%s: error %lu); "
                    "killing timed-out command trees by hand\n",
            step, (unsigned long)GetLastError());
}

/* End the command, and everything it spawned. */
static void kill_child(HANDLE job, HANDLE process, DWORD pid) {
    if (job) {
        TerminateJobObject(job, 1);
        return;
    }
    TerminateProcess(process, 1);
    kill_process_tree(pid);
}

/* Run cmd through cmd.exe, capturing stdout+stderr. Returns 0 when the
 * process could not be started; otherwise fills *exit_code and *timed_out. */
static int run_child(const char *cmd, int timeout_ms, int *exit_code, int *timed_out, sb_t *out) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, job = NULL;
    const char *comspec = getenv("ComSpec");
    char tmp[4096], *cl;
    DWORD start, avail, got, code = 0;
    size_t n;
    int stop = 0;

    *exit_code = -1;
    *timed_out = 0;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    if (!comspec || !*comspec) comspec = "cmd.exe";
    n = strlen(comspec) + strlen(cmd) + 8;
    cl = (char *)malloc(n);
    if (!cl) { CloseHandle(rd); CloseHandle(wr); return 0; }
    snprintf(cl, n, "\"%s\" /c %s", comspec, cmd);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        free(cl);
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    free(cl);
    CloseHandle(wr);

    job = create_job();
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        note_job_unavailable("AssignProcessToJobObject");
        CloseHandle(job);
        job = NULL;
    }

    start = GetTickCount();
    while (!stop) {
        avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            got = 0;
            if (avail > sizeof(tmp)) avail = sizeof(tmp);
            if (ReadFile(rd, tmp, avail, &got, NULL) && got > 0) {
                sb_append_n(out, tmp, got);
                continue;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (out->len > MAX_READ) {
            kill_child(job, pi.hProcess, pi.dwProcessId);
            stop = 1;
            break;
        }
        if (GetTickCount() - start >= (DWORD)timeout_ms) {
            *timed_out = 1;
            kill_child(job, pi.hProcess, pi.dwProcessId);
            stop = 1;
            break;
        }
        Sleep(20);
    }
    WaitForSingleObject(pi.hProcess, 5000);

    /* Drain what the pipe still holds, now that nothing else writes to it. */
    while (out->len < MAX_READ) {
        avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
        got = 0;
        if (avail > sizeof(tmp)) avail = sizeof(tmp);
        if (!ReadFile(rd, tmp, avail, &got, NULL) || got == 0) break;
        sb_append_n(out, tmp, got);
    }

    if (GetExitCodeProcess(pi.hProcess, &code)) *exit_code = (int)code;
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return 1;
}
#endif /* _WIN32 */

static char *tool_run_command(const char *command) {
    char cmd[4096];
    char prefix[600] = "";
    const char *root = getenv("SystemRoot");
    if (!root || !*root) root = getenv("windir");
    /* ReactOS ships a PATH that points at a non-existent C:\Windows, so no
     * system tool (ping, findstr, where, certutil, dwnl, ...) can be called by
     * name. Put the real system directories in front of the inherited PATH,
     * the way a normal Windows installation has them. */
#ifdef _WIN32
    if (root && *root)
        snprintf(prefix, sizeof(prefix), "set \"PATH=%s\\system32;%s;%%PATH%%\" && ", root, root);
#else
    (void)root;
#endif
    snprintf(cmd, sizeof(cmd), "%s%s 2>&1", prefix, command);

#ifdef _WIN32
    sb_t raw, r;
    int code = -1, timed_out = 0, timeout_ms = command_timeout_ms();

    sb_init(&raw);
    if (!run_child(cmd, timeout_ms, &code, &timed_out, &raw)) {
        sb_free(&raw);
        return strdup("run_command: could not start the command");
    }

    sb_init(&r);
    if (timed_out)
        sb_printf(&r, "STATUS: killed after %d s, the command did not finish\n--- OUTPUT ---\n",
                  timeout_ms / 1000);
    else
        sb_printf(&r, "STATUS: exit code %d\n--- OUTPUT ---\n", code);
    sb_append_utf8(&r, raw.d, raw.len);
    sb_free(&raw);
    sb_term(&r);
    return r.d;
#else
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
    int st = PCLOSE(fp);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

    sb_t r;
    sb_init(&r);
    sb_printf(&r, "STATUS: exit code %d\n--- OUTPUT ---\n", code);
    sb_append_utf8(&r, out.d, out.len);
    sb_free(&out);
    sb_term(&r);
    return r.d;
#endif
}

/* Read a file, optionally a byte range. Binary content (a NUL byte) comes
 * back as a hex dump with absolute offsets, so the model can walk a file it
 * cannot otherwise inspect. */
static char *tool_read_file(const char *path, long offset, long limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return strdup("read_file: could not open file");

    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (offset < 0) offset = 0;
    if (offset > size) offset = size;
    if (limit <= 0 || limit > MAX_READ) limit = MAX_READ;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return strdup("read_file: could not seek there");
    }

    sb_t out;
    sb_init(&out);
    char tmp[1024];
    size_t n;
    long left = limit;
    while (left > 0) {
        size_t want = sizeof(tmp);
        if ((long)want > left) want = (size_t)left;
        if ((n = fread(tmp, 1, want, f)) == 0) break;
        sb_append_n(&out, tmp, n);
        left -= (long)n;
    }
    fclose(f);

    sb_t r;
    sb_init(&r);
    if (looks_binary(out.d, out.len)) {
        sb_printf(&r, "read_file: %s is %ld bytes, binary, here from offset %ld\n",
                  path, size, offset);
        sb_put_hex_dump(&r, out.d, out.len, offset);
    } else {
        if (offset || (long)out.len < size)
            sb_printf(&r, "read_file: bytes %ld..%ld of %ld\n",
                      offset, offset + (long)out.len, size);
        sb_append_utf8(&r, out.d, out.len);
    }
    sb_free(&out);
    sb_term(&r);
    return r.d;
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
        long offset = (long)json_num(json_get(args, "offset"), 0);
        long limit = (long)json_num(json_get(args, "limit"), 0);
        result = p ? tool_read_file(p, offset, limit) : strdup("read_file: missing 'path'");
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

static const char *shell_hint(void) {
#ifdef _WIN32
    return "cmd.exe (Windows Command Prompt; PowerShell is NOT available)";
#else
    return "/bin/sh (POSIX shell)";
#endif
}

/* The Windows installation directory (C:\Windows, or C:\ReactOS on ReactOS).
 * NULL when the platform does not define one. */
static const char *system_dir(void) {
    const char *d = getenv("SystemRoot");
    if (!d || !*d) d = getenv("windir");
    return (d && *d) ? d : NULL;
}

/* Where the system tools live (C:\Windows\System32, C:\ReactOS\System32).
 * Empty string when it cannot be determined. */
static void system_tools_dir(char *buf, size_t len) {
#ifdef _WIN32
    UINT n = GetSystemDirectoryA(buf, (UINT)len);
    if (n > 0 && n < len) return;
#endif
    const char *root = system_dir();
    if (root) snprintf(buf, len, "%s\\system32", root);
    else buf[0] = 0;
}

#ifdef _WIN32
#define TOOLS_LIST_MAX 300

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* List the programs in the system directory. Without this the model reasons
 * about a normal Windows machine and reaches for curl, wget or PowerShell -
 * none of which ReactOS has, while the tool it needs (dwnl.exe) is right
 * there. */
static void sb_put_installed_tools(sb_t *b, const char *dir) {
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *names[TOOLS_LIST_MAX];
    int n = 0, i;

    snprintf(pattern, sizeof(pattern), "%s\\*.exe", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names[n] = strdup(fd.cFileName);
        if (names[n]) n++;
    } while (n < TOOLS_LIST_MAX && FindNextFileA(h, &fd));
    FindClose(h);

    if (!n) return;
    qsort(names, (size_t)n, sizeof(names[0]), name_cmp);
    for (i = 0; i < n; i++) {
        sb_printf(b, "%s%s", i ? " " : "", names[i]);
        free(names[i]);
    }
    if (n == TOOLS_LIST_MAX) sb_puts(b, " ...");
}
#endif /* _WIN32 */

static int add_system_message(msgs_t *m) {
    char cwdbuf[1024], tools_dir[1024];
    const char *cwd = GETCWD(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : "(unknown)";

    sb_t sys;
    sb_init(&sys);
    sb_printf(&sys, "OS: %s\nShell: %s\nWorking directory: %s\n",
              os_name(), shell_hint(), cwd);

    system_tools_dir(tools_dir, sizeof(tools_dir));
    if (*tools_dir) {
        sb_printf(&sys, "System directory: %s (on PATH for run_command)\n", tools_dir);
#ifdef _WIN32
        sb_puts(&sys, "Programs installed there: ");
        sb_put_installed_tools(&sys, tools_dir);
        sb_puts(&sys, "\n");
#endif
    }

    sb_printf(&sys, "\n%s", SYSTEM_PROMPT);
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

/* Drop the newest message again, freeing it. */
static void msgs_drop_last(msgs_t *m) {
    if (m->count == 0) return;
    msg_t *x = &m->items[--m->count];
    free(x->role);
    free(x->content);
    free(x->tool_call_id);
    for (int j = 0; j < x->ncalls; j++) {
        free(x->calls[j].id);
        free(x->calls[j].name);
        free(x->calls[j].args);
    }
    free(x->calls);
    memset(x, 0, sizeof(*x));
}

/* One request/response round. Returns 1 on success and hands back the
 * assistant text plus any tool calls; nothing is written to the history here,
 * the caller decides what to do with the result. */
static int http_turn(const agent_config_t *cfg, msgs_t *msgs, int with_tools,
                     char **content_out, tc_t **calls_out, int *ncalls_out) {
    sb_t req;
    char url[1024];
    long status = 0;
    char *resp = NULL;

    *content_out = NULL;
    *calls_out = NULL;
    *ncalls_out = 0;

    if (!sb_init(&req)) return 0;
    build_request(&req, cfg, msgs->items, msgs->count, with_tools);
    sb_term(&req);

    snprintf(url, sizeof(url), "%s/chat/completions", cfg->base_url);
    if (http_post(url, cfg->api_key, req.d, &status, &resp) != 0) {
        fprintf(stderr, "error: http request failed\n");
        sb_free(&req);
        return 0;
    }
    sb_free(&req);

    if (status != 200) {
        fprintf(stderr, "error: http status %ld\n%s\n", status, resp ? resp : "");
        free(resp);
        return 0;
    }

    *content_out = parse_response(resp, calls_out, ncalls_out);
    free(resp);
    return 1;
}

/* The step budget is gone but the user still deserves an answer, so ask for a
 * summary instead of stopping silently. The tools are left out of that request,
 * and the prompt that triggers it is not kept in the history - only the
 * summary is. */
static char *wrap_up(agent_session_t *s) {
    static const char *ask =
        "You have used up your step budget. Do not start anything new. Tell the user "
        "concisely what you did, what you found, what is still open, and what you would "
        "do next.";
    char *content = NULL, *answer = NULL;
    tc_t *calls = NULL;
    int ncalls = 0;

    fprintf(stderr, "  -- step budget of %d used up, asking for a summary\n",
            s->cfg.max_steps);

    if (!msgs_add(&s->msgs, "user", ask, NULL)) return NULL;
    if (http_turn(&s->cfg, &s->msgs, 0, &content, &calls, &ncalls)) {
        if (ncalls == 0) {
            answer = content;
        } else {
            free(content);
            for (int i = 0; i < ncalls; i++) {
                free(calls[i].id);
                free(calls[i].name);
                free(calls[i].args);
            }
            free(calls);
        }
    }
    msgs_drop_last(&s->msgs);
    if (answer && *answer) msgs_add(&s->msgs, "assistant", answer, NULL);
    return answer;
}

char *agent_chat(agent_session_t *s, const char *user_input) {
    if (!msgs_add(&s->msgs, "user", user_input, NULL)) return NULL;

    char *answer = NULL;
    int done = 0, step = 0;

    for (; step < s->cfg.max_steps && !done; step++) {
        char *content = NULL;
        tc_t *calls = NULL;
        int ncalls = 0;

        if (!http_turn(&s->cfg, &s->msgs, 1, &content, &calls, &ncalls)) break;

        if (ncalls == 0) {
            if (content) msgs_add(&s->msgs, "assistant", content, NULL);
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

    /* Out of steps rather than an error: end with a summary, never in silence. */
    if (!done && step == s->cfg.max_steps) answer = wrap_up(s);
    return answer;
}
