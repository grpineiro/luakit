/*
 * log.c - logging functions
 *
 * Copyright © 2016 Aidan Holm <aidanholm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "globalconf.h"
#include "common/log.h"
#include "common/luaserialize.h"
#include "common/ipc.h"

#include <glib/gprintf.h>
#include <stdlib.h>
#include <unistd.h>

static GHashTable *group_levels;

void
log_set_verbosity(const char *group, log_level_t lvl)
{
    group_levels = group_levels ?: g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert(group_levels, g_strdup(group), GINT_TO_POINTER(lvl+1));
}

/* Will modify the group name passed to it, unless that name is "all" */
log_level_t
log_get_verbosity(char *group)
{
    if (!group_levels) return LOG_LEVEL_info;

    while (TRUE) {
        log_level_t lvl = GPOINTER_TO_UINT(g_hash_table_lookup(group_levels, (gpointer)group));
        if (lvl > 0)
            return lvl-1;
        char *slash = strrchr(group, '/');
        if (slash)
            *slash = '\0';
        else
            group = "all";
    }
}

static char *
log_group_from_fct(const char *fct)
{
    int len = strlen(fct);
    gboolean core = !strcmp(&fct[len-2], ".c"), lua = !strcmp(&fct[len-4], ".lua");
    g_assert_cmpint(core,!=,lua);

    if (core) /* Strip .c off the end */
        return g_strdup_printf("core/%.*s", len-2, fct);
    else {
        if (!strncmp(fct, "./", 2)) {
            fct += 2;
            len -= 2;
        }
        return g_strdup_printf("lua/%.*s", len-4, fct);
    }
}

int
log_level_from_string(log_level_t *out, const char *str)
{
#define X(name) if (!strcmp(#name, str)) { \
    *out = LOG_LEVEL_##name; \
    return 0; \
}
LOG_LEVELS
#undef X
    return 1;
}

void
_log(log_level_t lvl, const gchar *line, const gchar *fct, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_log(lvl, line, fct, fmt, ap);
    va_end(ap);
}

void
va_log(log_level_t lvl, const gchar *line, const gchar *fct, const gchar *fmt, va_list ap)
{
    char *group = log_group_from_fct(fct);
    log_level_t verbosity = log_get_verbosity(group);
    g_free(group);
    if (lvl > verbosity)
        return;

    gchar *msg = g_strdup_vprintf(fmt, ap);
    gint log_fd = STDERR_FILENO;

    /* Determine logging style */
    /* TODO: move to X-macro generated table? */

    gchar prefix_char, *style = "";
    switch (lvl) {
        case LOG_LEVEL_fatal:   prefix_char = 'F'; style = ANSI_COLOR_BG_RED; break;
        case LOG_LEVEL_error:   prefix_char = 'E'; style = ANSI_COLOR_RED; break;
        case LOG_LEVEL_warn:    prefix_char = 'W'; style = ANSI_COLOR_YELLOW; break;
        case LOG_LEVEL_info:    prefix_char = 'I'; break;
        case LOG_LEVEL_verbose: prefix_char = 'V'; break;
        case LOG_LEVEL_debug:   prefix_char = 'D'; break;
        default: g_assert_not_reached();
    }

    /* Log format: [timestamp] prefix: fct:line msg */
#define LOG_FMT "[%#12f] %c: %s:%s: %s"
#define LOG_IND "                  "

    /* Indent new lines within the message */
    static GRegex *indent_lines_reg;
    if (!indent_lines_reg) {
        GError *err = NULL;
        indent_lines_reg = g_regex_new("\n", G_REGEX_OPTIMIZE, 0, &err);
        g_assert_no_error(err);
    }
    gchar *wrapped = g_regex_replace_literal(indent_lines_reg, msg, -1, 0, "\n" LOG_IND, 0, NULL);
    g_free(msg);
    msg = wrapped;

    if (!isatty(log_fd)) {
        gchar *stripped = strip_ansi_escapes(msg);
        g_free(msg);
        msg = stripped;

        g_fprintf(stderr, LOG_FMT "\n",
                l_time() - globalconf.starttime,
                prefix_char, fct, line, msg);
    } else {
        g_fprintf(stderr, "%s" LOG_FMT ANSI_COLOR_RESET "\n",
                style,
                l_time() - globalconf.starttime,
                prefix_char, fct, line, msg);
    }

    g_free(msg);

    if (lvl == LOG_LEVEL_fatal)
        exit(EXIT_FAILURE);
}

void
ipc_recv_log(ipc_endpoint_t *UNUSED(ipc), const guint8 *lua_msg, guint length)
{
    lua_State *L = common.L;
    gint n = lua_deserialize_range(L, lua_msg, length);
    g_assert_cmpint(n, ==, 4);

    log_level_t lvl = lua_tointeger(L, -4);
    const gchar *line = lua_tostring(L, -3);
    const gchar *fct = lua_tostring(L, -2);
    const gchar *msg = lua_tostring(L, -1);
    _log(lvl, line, fct, "%s", msg);
    lua_pop(L, 4);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
