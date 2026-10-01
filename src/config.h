#ifndef IGOR_CONFIG_H
#define IGOR_CONFIG_H

#include <stddef.h>

/* Settings saved by the first-run setup, so a key typed once survives a restart.
 *
 * The file is a plain list of `NAME = value` lines using the same names as the
 * environment variables, because a user who has read about `LLM_MODEL` should
 * find it here under that name and nowhere else. Lines starting with `#` are
 * comments.
 */

/* Where the file lives, written into buf. Returns 0 if it does not fit. */
int config_path(char *buf, size_t size);

/* The saved value for a name, or NULL. The file is read once; later calls are
 * answered from memory, so asking for every setting at startup costs one open. */
const char *config_get(const char *name);

/* Replace the saved settings with these three, creating the directory on the
 * way. Returns 0 on failure, and then err (when given) names what went wrong. */
int config_save(const char *api_key, const char *base_url, const char *model,
                const char **err);

#endif
