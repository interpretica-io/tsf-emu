/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The protocol QEMU listens to
 */

#define TE_LGR_USER "TAPI EMU QMP"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "tapi_cfg_base.h"
#include "tapi_file.h"
#include "te_alloc.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_emu_qmp.h"
#include "tapi_emu_internal.h"

/** What the helper exits with when it could not reach the socket. */
#define EMU_QMP_EXIT_NOCONN 2

/**
 * The QMP client, as a python3 script.
 *
 * Written out whole because it is the thing between a test and the
 * machine, and a reader who does not trust a result has to be able to
 * read all of it.
 *
 * Three things in it are not obvious and all three are measured
 * against QEMU 10.2.0:
 *
 *  - the greeting comes first, unasked, and has to be read before
 *    anything can be sent;
 *  - no command works until `qmp_capabilities` has been sent, which is
 *    the handshake;
 *  - events arrive on the same socket whenever QEMU likes, including
 *    in the middle of waiting for a reply - `quit` gets a SHUTDOWN
 *    event before its own answer - so anything with an "event" key is
 *    skipped and reading continues.
 */
static const char emu_qmp_script[] =
"import sys, json, socket\n"
"path, command = sys.argv[1], sys.argv[2]\n"
"arguments = sys.argv[3] if len(sys.argv) > 3 else ''\n"
"timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0\n"
"try:\n"
"    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
"    s.settimeout(timeout)\n"
"    s.connect(path)\n"
"except OSError as e:\n"
"    sys.stderr.write('cannot reach %s: %s\\n' % (path, e))\n"
"    sys.exit(2)\n"
"f = s.makefile('rw', encoding='utf-8', newline='\\n')\n"
"\n"
"def reply():\n"
"    while True:\n"
"        line = f.readline()\n"
"        if not line:\n"
"            sys.stderr.write('the machine closed the socket\\n')\n"
"            sys.exit(3)\n"
"        obj = json.loads(line)\n"
"        if 'event' in obj:\n"
"            continue\n"
"        return obj\n"
"\n"
"def send(name, args):\n"
"    obj = {'execute': name}\n"
"    if args:\n"
"        obj['arguments'] = json.loads(args)\n"
"    f.write(json.dumps(obj) + '\\n')\n"
"    f.flush()\n"
"    return reply()\n"
"\n"
"greeting = reply()\n"
"if 'QMP' not in greeting:\n"
"    sys.stderr.write('not a QMP socket\\n')\n"
"    sys.exit(3)\n"
"send('qmp_capabilities', '')\n"
"sys.stdout.write(json.dumps(send(command, arguments)) + '\\n')\n";

/* See description in tapi_emu_internal.h */
te_errno
tapi_emu_put_helper(tapi_emu_vm *vm)
{
    te_string path = TE_STRING_INIT;
    char *tmp_dir;
    te_errno rc;

    if (vm->helper != NULL)
        return 0;

    tmp_dir = tapi_cfg_base_get_ta_dir(vm->ta, TAPI_CFG_BASE_TA_DIR_TMP);
    if (tmp_dir == NULL)
    {
        ERROR("Failed to get the temporary directory of TA %s", vm->ta);
        return TE_RC(TE_TAPI, TE_EFAIL);
    }

    tapi_file_make_custom_pathname(&path, tmp_dir, "-qmp.py");
    free(tmp_dir);

    rc = tapi_file_create_ta(vm->ta, path.ptr, "%s", emu_qmp_script);
    if (rc != 0)
    {
        ERROR("Failed to put the QMP helper on TA %s: %r", vm->ta, rc);
        te_string_free(&path);
        return rc;
    }

    vm->helper = path.ptr;

    return 0;
}

/* See description in tapi_emu_qmp.h */
te_errno
tapi_emu_qmp(tapi_emu_vm *vm, const char *command, const char *arguments,
             int timeout_ms, te_string *reply)
{
    te_vec args = TE_VEC_INIT(char *);
    te_string out = TE_STRING_INIT;
    te_string err = TE_STRING_INIT;
    const char *text;
    int code = 0;
    te_errno rc;

    rc = tapi_emu_put_helper(vm);
    if (rc != 0)
        return rc;

    tapi_emu_arg(&args, "%s", vm->helper);
    tapi_emu_arg(&args, "%s", vm->qmp_socket);
    tapi_emu_arg(&args, "%s", command);
    tapi_emu_arg(&args, "%s", arguments != NULL ? arguments : "");
    tapi_emu_arg(&args, "%d", timeout_ms > 0 ? timeout_ms / 1000 : 30);

    rc = tapi_emu_cmd(vm->factory, "qmp", vm->python, &args, timeout_ms,
                      &out, &err, &code);

    te_vec_deep_free(&args);

    if (rc != 0)
        goto out;

    if (code == EMU_QMP_EXIT_NOCONN)
    {
        ERROR("Nothing is listening on %s: %s", vm->qmp_socket,
              te_string_value(&err));
        rc = TE_RC(TE_TAPI, TE_ECONNREFUSED);
        goto out;
    }

    if (code != 0)
    {
        ERROR("The QMP helper failed (status %d): %s", code,
              te_string_value(&err));
        rc = TE_RC(TE_TAPI, TE_EFAIL);
        goto out;
    }

    text = te_string_value(&out);

    /*
     * An error is a reply, not a failure to reply: QEMU answers
     * {"error": {"class": ..., "desc": ...}} and the description is
     * what a reader needs. It is logged here and turned into a status
     * code, rather than handed back as if it were a result.
     */
    if (strstr(text, "\"error\"") != NULL)
    {
        te_string desc = TE_STRING_INIT;

        if (!tapi_emu_qmp_str(text, "desc", &desc))
            te_string_append(&desc, "%s", text);

        ERROR("%s was refused: %s", command, te_string_value(&desc));
        te_string_free(&desc);

        rc = TE_RC(TE_TAPI, TE_EPROTO);
        goto out;
    }

    if (reply != NULL)
        te_string_append(reply, "%s", text);

out:
    te_string_free(&out);
    te_string_free(&err);

    return rc;
}

/* See description in tapi_emu_qmp.h */
te_errno
tapi_emu_hmp(tapi_emu_vm *vm, const char *command, int timeout_ms,
             te_string *text)
{
    te_string arguments = TE_STRING_INIT;
    te_string reply = TE_STRING_INIT;
    te_errno rc;

    /*
     * The command goes into JSON, so a quote or a backslash in it
     * would end the string early. Both are escaped; nothing else in a
     * monitor command needs to be.
     */
    te_string_append(&arguments, "{\"command-line\": \"");
    for (; *command != '\0'; command++)
    {
        if (*command == '"' || *command == '\\')
            te_string_append(&arguments, "\\");
        te_string_append(&arguments, "%c", *command);
    }
    te_string_append(&arguments, "\"}");

    rc = tapi_emu_qmp(vm, "human-monitor-command", arguments.ptr,
                      timeout_ms, &reply);

    if (rc == 0 && text != NULL)
    {
        te_string value = TE_STRING_INIT;

        if (tapi_emu_qmp_str(te_string_value(&reply), "return", &value))
            te_string_append(text, "%s", te_string_value(&value));

        te_string_free(&value);
    }

    te_string_free(&arguments);
    te_string_free(&reply);

    return rc;
}

/** Find @c "key" and return what follows the colon. */
static const char *
emu_qmp_value(const char *reply, const char *key)
{
    te_string quoted = TE_STRING_INIT;
    const char *found;

    te_string_append(&quoted, "\"%s\"", key);
    found = strstr(reply, quoted.ptr);
    te_string_free(&quoted);

    if (found == NULL)
        return NULL;

    found = strchr(found, ':');
    if (found == NULL)
        return NULL;

    found++;
    while (*found == ' ' || *found == '\t')
        found++;

    return found;
}

/* See description in tapi_emu_qmp.h */
bool
tapi_emu_qmp_str(const char *reply, const char *key, te_string *dest)
{
    const char *value = emu_qmp_value(reply, key);

    if (value == NULL || *value != '"')
        return false;

    value++;

    /*
     * Unescaped as it is read, because a monitor reply is full of
     * "\r\n" and a caller that got those back as four characters
     * would have to undo it again.
     */
    for (; *value != '\0' && *value != '"'; value++)
    {
        if (*value != '\\')
        {
            te_string_append(dest, "%c", *value);
            continue;
        }

        value++;
        switch (*value)
        {
            case 'n':
                te_string_append(dest, "\n");
                break;
            case 'r':
                te_string_append(dest, "\r");
                break;
            case 't':
                te_string_append(dest, "\t");
                break;
            case '\0':
                return true;
            default:
                te_string_append(dest, "%c", *value);
                break;
        }
    }

    return true;
}

/* See description in tapi_emu_qmp.h */
bool
tapi_emu_qmp_bool(const char *reply, const char *key, bool *value)
{
    const char *found = emu_qmp_value(reply, key);

    if (found == NULL)
        return false;

    if (strncmp(found, "true", 4) == 0)
        *value = true;
    else if (strncmp(found, "false", 5) == 0)
        *value = false;
    else
        return false;

    return true;
}
