#ifndef IGOR_HTTP_H
#define IGOR_HTTP_H

/*
 * POST `body` (application/json) to `url` (must be https://host[:port]/path)
 * with the given bearer token.
 *
 * On success returns 0 and sets *status (HTTP code) and *resp (malloc'd body,
 * caller frees). On transport failure returns -1.
 */
int http_post(const char *url, const char *api_key, const char *body,
              long *status, char **resp);

#endif
