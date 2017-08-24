/*
 * Copyright (c) 1993-2016 Paul Mattes.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the names of Paul Mattes nor the names of his contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY PAUL MATTES "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL PAUL MATTES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *      login_macro.c
 *              Automatic login macros.
 */

#include "globals.h"

#include "wincmn.h"
#include <errno.h>
#include <fcntl.h>

#include "actions.h"
#include "kybd.h"
#include "lazya.h"
#include "popups.h"
#include "source.h"
#include "task.h"
#include "trace.h"
#include "utils.h"
#include "varbuf.h"
#include "w3misc.h"
#include "xio.h"

static void login_data(task_cbh handle, const char *buf, size_t len);
static bool login_done(task_cbh handle, bool success, bool abort);

/* Callback block for login. */
static tcb_t login_cb = {
    "login",
    IA_MACRO,
    CB_NEW_TASKQ,
    login_data,
    login_done,
    NULL
};
static char *login_result;

/**
 * Callback for data returned to login.
 *
 * @param[in] handle	Callback handle
 * @param[in] buf	Buffer
 * @param[in] len	Buffer length
 */
static void
login_data(task_cbh handle _is_unused, const char *buf, size_t len)
{
    if (handle != (tcb_t *)&login_cb) {
	vtrace("login_data: no match\n");
	return;
    }

    Replace(login_result, xs_buffer("%.*s", (int)len, buf));
}

/**
 * Callback for completion of one command executed from login.
 *
 * @param[in] handle		Callback handle
 * @param[in] success		True if child succeeded
 * @param[in] abort		True if aborting
 *
 * @return True if context is complete
 */
static bool
login_done(task_cbh handle _is_unused, bool success, bool abort)
{
    if (handle != (tcb_t *)&login_cb) {
	vtrace("login_done: no match\n");
	return true;
    }

#if 0
    /* If the login macro failed, exit. */
    if (!success) {
	x3270_exit(1);
    }
#endif

    if (!success) {
	popup_an_error("Login macro failed%s%s",
		login_result? ": ": "",
		login_result? login_result: "");
    }
    Replace(login_result, NULL);
    return true;
}

/**
 * Run a login macro.
 * If the string looks like an action, e.g., starts with "Xxx(", run a login
 * macro.  Otherwise, set a simple pending login string.
 *
 * @param[in] s		String or command
 */
void
login_macro(char *s)
{
    char *t = s;
    bool is_actions = false;
    char *action;

    while (isspace((unsigned char)*t)) {
	t++;
    }
    if (isalnum((unsigned char)*t)) {
	while (isalnum((unsigned char)*t)) {
	    t++;
	}
	while (isspace((unsigned char)*t)) {
	    t++;
	}
	if (*t == '(') {
	    is_actions = true;
	}
    }

    if (is_actions) {
	action = lazyaf("Wait(InputField) %s", s);
    } else {
	action = lazyaf("Wait(InputField) String(%s)", safe_param(s));
    }
    push_cb(action, strlen(action), &login_cb, (task_cbh)&login_cb);
}