/*
 * lwan - simple web server
 * Copyright (c) 2014 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "lwan-cache.h"
#include "lwan-config.h"
#include "lwan-http-authorize.h"

struct realm_password_file_t {
    struct cache_entry_t base;
    struct hash *entries;
};

static struct cache_t *realm_password_cache = NULL;

static void fourty_two_and_free(void *str)
{
    if (LIKELY(str)) {
        char *s = str;
        while (*s)
            *s++ = 42;
        free(str);
    }
}

static struct cache_entry_t *create_realm_file(
          const char *key,
          void *context __attribute__((unused)))
{
    struct realm_password_file_t *rpf = malloc(sizeof(*rpf));
    config_t f;
    config_line_t l;

    if (UNLIKELY(!rpf))
        return NULL;

    rpf->entries = hash_str_new(fourty_two_and_free, fourty_two_and_free);
    if (UNLIKELY(!rpf->entries))
        goto error_no_close;

    if (!config_open(&f, key))
        goto error_no_close;

    while (config_read_line(&f, &l)) {
        /* FIXME: Storing plain-text passwords in memory isn't a good idea. */
        switch (l.type) {
        case CONFIG_LINE_TYPE_LINE: {
            char *username = strdup(l.line.key);
            if (!username)
                goto error;

            char *password = strdup(l.line.value);
            if (!password) {
                free(username);
                goto error;
            }

            int err = hash_add_unique(rpf->entries, username, password);
            if (LIKELY(!err))
                continue;

            free(username);
            free(password);

            if (err == -EEXIST) {
                lwan_status_warning(
                    "Username entry already exists, ignoring: \"%s\"",
                    l.line.key);
                continue;
            }

            goto error;
        }
        default:
            config_error(&f, "Expected username = password");
            break;
        }
    }

    if (f.error_message) {
        lwan_status_error("Error on password file \"%s\", line %d: %s",
              key, f.line, f.error_message);
        goto error;
    }

    config_close(&f);
    return (struct cache_entry_t *)rpf;

error:
    config_close(&f);
error_no_close:
    hash_free(rpf->entries);
    free(rpf);
    return NULL;
}

static void destroy_realm_file(struct cache_entry_t *entry,
                                void *context __attribute__((unused)))
{
    struct realm_password_file_t *rpf = (struct realm_password_file_t *)entry;
    hash_free(rpf->entries);
    free(rpf);
}

bool
lwan_http_authorize_init(void)
{
    realm_password_cache = cache_create(create_realm_file,
          destroy_realm_file, NULL, 60);

    return !!realm_password_cache;
}

void
lwan_http_authorize_shutdown(void)
{
    cache_destroy(realm_password_cache);
}

static bool
authorize(coro_t *coro,
           lwan_value_t *authorization,
           const char *password_file)
{
    struct realm_password_file_t *rpf;
    unsigned char *decoded;
    char *colon;
    char *password;
    char *looked_password;
    size_t decoded_len;
    bool password_ok = false;

    rpf = (struct realm_password_file_t *)cache_coro_get_and_ref_entry(
            realm_password_cache, coro, password_file);
    if (UNLIKELY(!rpf))
        return false;

    decoded = base64_decode((unsigned char *)authorization->value,
                            authorization->len, &decoded_len);
    if (UNLIKELY(!decoded))
        return false;

    if (UNLIKELY(decoded_len >= sizeof(((config_line_t *)0)->buffer)))
        goto out;

    colon = memchr(decoded, ':', decoded_len);
    if (UNLIKELY(!colon))
        goto out;

    *colon = '\0';
    password = colon + 1;

    looked_password = hash_find(rpf->entries, decoded);
    if (looked_password)
        password_ok = streq(password, looked_password);

out:
    free(decoded);
    return password_ok;
}

bool
lwan_http_authorize(lwan_request_t *request,
                    lwan_value_t *authorization,
                    const char *realm,
                    const char *password_file)
{
    static const char authenticate_tmpl[] = "Basic realm=\"%s\"";
    static const size_t basic_len = sizeof("Basic ") - 1;
    lwan_key_value_t *headers;

    if (!authorization->value)
        goto unauthorized;

    if (UNLIKELY(strncmp(authorization->value, "Basic ", basic_len)))
        goto unauthorized;

    authorization->value += basic_len;
    authorization->len -= basic_len;

    if (authorize(request->conn->coro, authorization, password_file))
        return true;

unauthorized:
    headers = coro_malloc(request->conn->coro, 2 * sizeof(*headers));
    if (UNLIKELY(!headers))
        return false;

    headers[0].key = "WWW-Authenticate";
    headers[0].value = coro_printf(request->conn->coro,
                authenticate_tmpl, realm);
    headers[1].key = headers[1].value = NULL;

    request->response.headers = headers;
    return false;
}
