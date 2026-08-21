#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    char *task = NULL;
    if (argc > 1) {
        task = argv[1];
    } else {
        task = read_stdin();
    }

    if (!task || !*task) {
        fprintf(stderr, "usage: igor \"task\"   (or pipe the task on stdin)\n");
        if (argc <= 1) free(task);
        return 1;
    }

    int rc = agent_run(&cfg, task);
    if (argc <= 1) free(task);
    return rc;
}
