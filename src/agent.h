#ifndef IGOR_AGENT_H
#define IGOR_AGENT_H

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    int max_steps;
} agent_config_t;

/* Runs the agent loop; prints the final answer to stdout. Returns 0 on success. */
int agent_run(const agent_config_t *cfg, const char *task);

#endif
