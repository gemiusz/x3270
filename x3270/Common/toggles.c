/*
 * Copyright (c) 1993-2009, 2013-2015 Paul Mattes.
 * Copyright (c) 1990, Jeff Sparkes.
 * Copyright (c) 1989, Georgia Tech Research Corporation (GTRC), Atlanta, GA
 *  30332.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the names of Paul Mattes, Jeff Sparkes, GTRC nor the names of
 *       their contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY PAUL MATTES, JEFF SPARKES AND GTRC "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL PAUL MATTES, JEFF SPARKES OR GTRC BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *	toggles.c
 *		This module handles toggles.
 */

#include "globals.h"
#include "appres.h"

#include "actionsc.h"
#include "ctlrc.h"
#include "menubarc.h"
#include "nvtc.h"
#include "popupsc.h"
#include "screenc.h"
#include "trace.h"
#include "togglesc.h"


/*
 * Generic toggle stuff
 */
static void
do_toggle_reason(int ix, enum toggle_type reason)
{
    struct toggle *t = &appres.toggle[ix];

    /*
     * Change the value, call the internal update routine, and reset the
     * menu label(s).
     */
    toggle_toggle(t);
    if (t->upcall != NULL) {
	t->upcall(t, reason);
    }
    menubar_retoggle(t, ix);
}

void
do_toggle(int ix)
{
    do_toggle_reason(ix, TT_INTERACTIVE);
}

void
do_menu_toggle(int ix)
{
    do_toggle_reason(ix, TT_XMENU);
}

static void
init_toggle_fallible(int ix)
{
    if (toggled(ix)) {
	appres.toggle[ix].upcall(&appres.toggle[ix], TT_INITIAL);
	if (!toggled(ix)) {
	    menubar_retoggle(&appres.toggle[ix], ix);
	}
    }
}

/*
 * Called from system initialization code to handle initial toggle settings.
 */
void
initialize_toggles(void)
{
    appres.toggle[TRACING].upcall =          toggle_tracing;
    appres.toggle[SCREEN_TRACE].upcall =     toggle_screenTrace;
    appres.toggle[LINE_WRAP].upcall =        toggle_lineWrap;

#if defined(X3270_INTERACTIVE) /*[*/
    appres.toggle[MONOCASE].upcall =         toggle_monocase;
#endif /*]*/

#if defined(X3270_DISPLAY) /*[*/
    appres.toggle[ALT_CURSOR].upcall =       toggle_altCursor;
    appres.toggle[CURSOR_BLINK].upcall =     toggle_cursorBlink;
    appres.toggle[SHOW_TIMING].upcall =      toggle_showTiming;
    appres.toggle[CURSOR_POS].upcall =       toggle_cursorPos;
    appres.toggle[SCROLL_BAR].upcall =       toggle_scrollBar;
    appres.toggle[CROSSHAIR].upcall =        toggle_crosshair;
    appres.toggle[VISIBLE_CONTROL].upcall =  toggle_visible_control;
#endif /*]*/

#if defined(C3270) /*[*/
    appres.toggle[UNDERSCORE].upcall =	     toggle_underscore;
#endif /*]*/

    init_toggle_fallible(TRACING);
    init_toggle_fallible(SCREEN_TRACE);
}

/*
 * Called from system exit code to handle toggles.
 */
void
shutdown_toggles(void)
{
    /* Clean up the data stream trace monitor window. */
    if (toggled(TRACING)) {
	appres.toggle[TRACING].value = False;
	toggle_tracing(&appres.toggle[TRACING], TT_FINAL);
    }

    /* Clean up the screen trace file. */
    if (toggled(SCREEN_TRACE)) {
	appres.toggle[SCREEN_TRACE].value = False;
	toggle_screenTrace(&appres.toggle[SCREEN_TRACE], TT_FINAL);
    }
}

Boolean
Toggle_action(ia_t ia, unsigned argc, const char **argv)
{
    int j;
    int ix;

    action_debug("Toggle", ia, argc, argv);
    if (check_argc("Toggle", argc, 1, 2) < 0) {
	return False;
    }
    for (j = 0; toggle_names[j].name != NULL; j++) {
	if (!TOGGLE_SUPPORTED(toggle_names[j].index)) {
	    continue;
	}
	if (!strcasecmp(argv[0], toggle_names[j].name)) {
	    ix = toggle_names[j].index;
	    break;
	}
    }
    if (toggle_names[j].name == NULL) {
	popup_an_error("Toggle: Unknown toggle name '%s'", argv[0]);
	return False;
    }

    if (argc == 1) {
	do_toggle_reason(ix, TT_ACTION);
    } else if (!strcasecmp(argv[1], "set")) {
	if (!toggled(ix)) {
	    do_toggle_reason(ix, TT_ACTION);
	}
    } else if (!strcasecmp(argv[1], "clear")) {
	if (toggled(ix)) {
	    do_toggle_reason(ix, TT_ACTION);
	}
    } else {
	popup_an_error("Toggle: Unknown keyword '%s' (must be 'set' or "
		"'clear')", argv[1]);
	return False;
    }
    return True;
}

void
toggles_init(void)
{
    static action_table_t toggle_actions[] = {
	{ "Toggle",		Toggle_action,	ACTION_KE }
    };

    register_actions(toggle_actions, array_count(toggle_actions));
}
