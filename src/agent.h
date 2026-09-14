#ifndef IGOR_AGENT_H
#define IGOR_AGENT_H

/* What a piece of output is, so the caller can make the difference visible. */
enum {
    IGOR_TEXT = 0, /* the model's answer */
    IGOR_THOUGHT,  /* the model's reasoning, before it answers */
    IGOR_NOTE,     /* igor's own trace: what it is doing */
    IGOR_ERROR,    /* something went wrong */
    IGOR_PROMPT    /* the conversation skeleton: you> and igor> */
};

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    int max_steps;
    /* Answer text, with the kind from above. NULL in tests and library use. */
    void (*out)(const char *text, int kind);
    /* Trace and diagnostics; these belong on stderr so a piped answer stays
     * clean. Falls back to stderr when NULL. */
    void (*note)(const char *text, int kind);
    /* One line, overwritten in place, saying what igor is doing right now.
     * NULL clears it. Ignored when there is no terminal. */
    void (*status)(const char *text);
    /* Ask the API to stream; the answer then reaches out() while it arrives. */
    int stream;
    /* Show the model's reasoning, dimmed, instead of hiding it. */
    int show_thought;
    /* Ceiling for one request - prompt plus answer - in tokens. The oldest
     * turns are dropped to stay under it. 0 turns trimming off. */
    long context_tokens;
    /* Where the conversation is written between turns, or NULL to keep it in
     * memory only. Set for interactive sessions: a one-shot task has nothing
     * to come back to. */
    const char *history_path;
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
