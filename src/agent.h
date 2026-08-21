#ifndef IGOR_AGENT_H
#define IGOR_AGENT_H

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    int max_steps;
} agent_config_t;

/* Opaque conversation session; keeps the message history across turns. */
typedef struct agent_session agent_session_t;

agent_session_t *agent_session_new(const agent_config_t *cfg);
void agent_session_free(agent_session_t *s);
void agent_session_reset(agent_session_t *s);

/* Sends one user message through the loop and prints the final answer. */
int agent_chat(agent_session_t *s, const char *user_input);

#endif
