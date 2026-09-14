#ifndef IGOR_AGENT_H
#define IGOR_AGENT_H

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    int max_steps;
    /* Show the answer as it is written. When NULL the answer is only returned. */
    void (*out)(const char *text);
    /* Ask the API to stream; the answer then reaches out() while it arrives. */
    int stream;
} agent_config_t;

/* Opaque conversation session; keeps the message history across turns. */
typedef struct agent_session agent_session_t;

agent_session_t *agent_session_new(const agent_config_t *cfg);
void agent_session_free(agent_session_t *s);
void agent_session_reset(agent_session_t *s);

/* Sends one user message through the loop and returns the final answer
 * (malloc'd, caller frees), or NULL on error. The answer has already been given
 * to cfg->out by then, so callers normally just free it. */
char *agent_chat(agent_session_t *s, const char *user_input);

#endif
