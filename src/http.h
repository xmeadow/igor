#ifndef IGOR_HTTP_H
#define IGOR_HTTP_H

#include <stddef.h>

/*
 * Called with body bytes as they arrive, before the whole response is known.
 * `data` is not NUL-terminated; read exactly len bytes. Used to show a streamed
 * answer while it is still being written.
 */
typedef void (*http_chunk_fn)(void *ctx, const char *data, size_t len);

/*
 * POST `body` (application/json) to `url` (must be https://host[:port]/path)
 * with the given bearer token.
 *
 * On success returns 0 and sets *status (HTTP code) and *resp (malloc'd body,
 * caller frees). On transport failure returns -1.
 *
 * When on_chunk is not NULL it is called for every chunk of the response body
 * as it arrives; the body is still accumulated and returned in *resp, so a
 * caller can show progress and parse the finished response either way.
 */
int http_post(const char *url, const char *api_key, const char *body,
              long *status, char **resp,
              http_chunk_fn on_chunk, void *ctx);

#endif
