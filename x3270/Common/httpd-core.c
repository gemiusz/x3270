/*
 * Copyright (c) 2014-2015 Paul Mattes.
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
 *      httpd-core.c
 *              x3270 webserver, main protocol module
 */

#include "globals.h"

#include <errno.h>
#include <limits.h>

#include "appres.h"
#include "asprintf.h"
#include "lazya.h"
#include "trace.h"
#include "util.h"
#include "varbuf.h"

#include "httpd-core.h"
#include "httpd-io.h"

#define DIRLIST_NLEN	14

/* Typedefs */
typedef enum {		/* Print mode: */
    HP_SEND,		/*  Send directly */
    HP_BUFFER		/*  Buffer */
} httpd_print_t;

typedef enum {		/* Error type: */
    ERRMODE_NON_HTTP,	/*  The request makes no sense at all -- it might not
			     even be HTTP. Don't bother with an HTTP header in
			     the response. */
    ERRMODE_FATAL,	/*  The request appears to be HTTP, but processing
			     cannot continue. Wrap the response in HTTP. */
    ERRMODE_NONFATAL	/*  The request cannot be satisfied, but if this is a
			     persistent connection, keep it open. */
} errmode_t;

typedef enum {		/* Supported verbs: */
    VERB_GET,		/*  GET */
    VERB_HEAD,		/*  HEAD */
    VERB_OTHER		/*  anything else */
} verb_t;

/* fields */
typedef struct _field {	/* HTTP request fields (name: value) */
    struct _field *next; /* linkage */
    char *name;		/* name */
    char *value;	/* value */
} field_t;

/* Per-request state */
typedef struct {
    varbuf_t print_buf;	/* pending output */
#define MAX_HTTPD_REQUEST	(8192 - 1)
    char request_buf[MAX_HTTPD_REQUEST + 1]; /* request buffer */
    int nr;		/* length of input up through blank line */
    Boolean saw_first;	/* we have digested the first line of the request */
    int rll;		/* length of each request line parsed */
    verb_t verb;	/* parsed verb */
    Boolean http_1_0;	/* is the client speaking HTTP 1.0? */
    Boolean persistent; /* is the client expecting a persistent connection? */
    char *uri;		/* start of URI */
    char *query;	/* query */
    field_t *queries;	/* list of query values */
    char *fragment;	/* fragment */
    char *fields_start;	/* start of fields */
    field_t *fields;	/* field values */
    char *location;	/* real location for 301 errors */
    struct _httpd_reg *async_node; /* asynchronous event node */
    size_t it_offset;	/* input trace offset */
    size_t ot_offset;	/* output trace offset */
} request_t;

/* connection state */
typedef struct {
    /* Global state */
    void *mhandle;	/* the handle from the main procedure */
    Boolean cr;		/* last character seen was a CR */
    unsigned long seq;	/* connection sequence number, for tracing */

    /* Per-request state */
    request_t request;
} httpd_t;

/* object registry */
typedef enum {
    OR_DIR,		/* Directory */
    OR_FIXED,		/* object has fixed value */
    OR_FIXED_BINARY,	/* object has fixed binary value */
    OR_DYN_TERM,	/* object is dynamic, terminal */
    OR_DYN_NONTERM	/* object is dynamic, non-terminal */
} or_t;
typedef struct _httpd_reg {
    struct _httpd_reg *next;	/* linkage */
    const char *path;		/* full path, including leading / */
    const char *desc;		/* description, for directory display */
    const char *alias;		/* alias, for directory display */
    content_t content_type;
    const char *content_str;
    unsigned flags;	/* HF_xxx */
    or_t type;
    union {
	const char *fixed;	/* fixed html */
	struct {
	    const unsigned char *fixed;
	    unsigned length;
	} fixed_binary;		/* fixed binary */
	reg_dyn_t *dyn;		/* dynamic output */
    } u;
    httpd_t *async_session;
} httpd_reg_t;

/* Globals */

/* Statics */
static httpd_reg_t *httpd_reg;
static unsigned long httpd_seq = 0;

/* Code */

/**
 * Expand an HTTP status code to a string.
 *
 * @param[in] status_code
 *
 * @return Expanded text
 */
static const char *
status_text(int status_code)
{
    switch (status_code) {
    case 200:
	return "OK";
    case 301:
	return "Moved Permanently";
    case 400:
	return "Bad Request";
    case 404:
	return "Not Found";
    case 409:
	return "Conflict";
    case 500:
	return "Internal Server Error";
    case 501:
	return "Not implemented";
    default:
	return "Unknown";
    }
}

/**
 * Trace network data.
 *
 * @param[in] direction		Descriptive string for data direction
 * @param[in] buf		Data buffer
 * @param[in] len		Length of data
 * @param[in,out] doffset	Display offset
 */
static void
httpd_data_trace(httpd_t *h, const char *direction, const char *buf,
	size_t len, size_t *doffset)
{
    size_t i;
#define BPL 16
    unsigned char linebuf[BPL];
    size_t j;

    for (i = 0; i < len; i++) {
	if (!(i % BPL)) {
	    if (i) {
		vtrace(" ");
		for (j = 0; j < BPL; j++) {
		    vtrace("%c", iscntrl(linebuf[j])? '.': linebuf[j]);
		}
	    }
	    vtrace("%sh%s [%lu] 0x%04x",
		    i? "\n": "",
		    direction,
		    h->seq,
		    (unsigned)(*doffset + i));
	}
	vtrace(" %02x", (unsigned char)buf[i]);
	linebuf[i % BPL] = buf[i];
    }

    /* Space over the missing data bytes on the line. */
    if (i % BPL) {
	vtrace("%*s", (int)((BPL - (i % BPL)) * 3 + 1), "");
    } else {
	vtrace(" ");
    }

    /* Trace the last chunk of data as text. */
    for (j = 0; j < ((i % BPL)? (i % BPL): BPL); j++) {
	vtrace("%c", iscntrl(linebuf[j])? '.': linebuf[j]);
    }
    vtrace("\n");

    *doffset += len;
}

/**
 * Send data on a connection.
 *
 * @param[in] h		State
 * @param[in] buf	Data buffer
 * @param[in] len	Data buffer length
 */
static void
httpd_send(httpd_t *h, const char *buf, size_t len)
{
    httpd_data_trace(h, ">", buf, len, &h->request.ot_offset);
    hio_send(h->mhandle, buf, len);
}

/**
 * Transfer data to the HTTPD socket or the deferred output buffer.
 *
 * @param[in,out] h	State
 * @param[in] type	How to print (send immediate or buffer)
 * @param[in] buf	Buffer to print
 * @param[in] len	Length of buffer
 */
static void
httpd_print_buf(httpd_t *h, httpd_print_t type, const char *buf,
	size_t len)
{
    request_t *r = &h->request;

    if (type == HP_SEND) {
	httpd_send(h, buf, len);
    } else {
	vb_append(&r->print_buf, buf, len);
    }
}

/**
 * Print HTTPD response text, varargs interface.
 *
 * Responsible for expanding newlines to CR/LF pairs and directing the output
 * to the correct socket.
 *
 * @param[in,out] h	State
 * @param[in] type	How to print (send immediate or buffer)
 * @param[in] format	printf()-style format
 * @param[in] ap	varargs
 */
static void
httpd_vprint(httpd_t *h, httpd_print_t type, const char *format, va_list ap)
{
    char *buf;
    size_t sl;
    char *sp;			/* pointer through the string */

    /* Expand the text. */
    buf = xs_vbuffer(format, ap);
    sl = strlen(buf);

    /* Write it in chunks, doing CR/LF expansion. */
    sp = buf;
    while (sl > 0) {
	char *nl;		/* location of next newline */
	ssize_t wlen;		/* number of bytes before the newline */
	Boolean crlf;		/* True if newline found */

	nl = strchr(sp, '\n');
	if (nl != NULL) {
	    wlen = nl - sp;
	    crlf = True;
	} else {
	    wlen = sl;
	    crlf = False;
	}
	if (wlen) {
	    /* Send the text up to (but not including) the newline. */
	    httpd_print_buf(h, type, sp, wlen);

	    /* Account for the bytes written. */
	    sp += wlen;
	    sl -= wlen;
	}
	if (crlf) {
	    /* Expand the newline to CR/LF. */
	    httpd_print_buf(h, type, "\r\n", 2);

	    /* Account for the newline. */
	    sp++;
	    sl--;
	}
    }

    Free(buf);
}

/**
 * Print HTTPD response text
 *
 * Responsible for expanding newlines to CR/LF pairs and directing the output
 * to the correct socket.
 *
 * @param[in,out] h	State
 * @param[in] type	How to print (send immediate or buffer)
 * @param[in] format	printf()-style format
 */
static void
httpd_print(httpd_t *h, httpd_print_t type, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    httpd_vprint(h, type, format, ap);
    va_end(ap);
}

/**
 * Dump out a Content-Length string.
 *
 * This helps ensure that a typical response comes in just three chunks:
 *  HTTP header (except for Content-Length)
 *  Content-Length and the double CR+LF
 *  Body
 *
 * @param[in] h		State
 * @param[in] len	Length
 */
static void
httpd_content_len(httpd_t *h, size_t len)
{
    char *cl;

    /* Do our own CR+LF expansion and send directly. */
    cl = lazyaf("Content-Length: %u\r\n\r\n", (unsigned)len);
    httpd_send(h, cl, strlen(cl));
}

/**
 * Dump the buffered http_print() data.
 *
 * @param[in,out] h	State
 */
typedef enum { DUMP_WITH_LENGTH, DUMP_WITHOUT_LENGTH } dump_t;
static void
httpd_print_dump(httpd_t *h, dump_t type)
{
    request_t *r = &h->request;

    if (type == DUMP_WITH_LENGTH) {
	httpd_content_len(h, vb_len(&r->print_buf));
    }
    if (vb_len(&r->print_buf)) {
	httpd_send(h, vb_buf(&r->print_buf), vb_len(&r->print_buf));
    }
    vb_reset(&r->print_buf);
}

/**
 * Free a collection of fields.
 *
 * @param[in,out] fp	pointer to list of fields
 */
static void
free_fields(field_t **fp)
{
    field_t *f;

    while ((f = *fp) != NULL) {
	field_t *g = f;

	*fp = f->next;
	Free(g);
    }
}

/**
 * Reinitialize the HTTPD request state.
 *
 * We do this when we close a connection, and when we complete a request on
 * a persistent connection.
 *
 * @param[in,out] r	request state
 */
static void
httpd_reinit_request(request_t *r)
{
    r->nr = 0;
    r->saw_first = False;
    r->rll = 0;
    r->http_1_0 = False;
    r->persistent = True;
    free_fields(&r->fields);
    r->fields_start = NULL;
    free_fields(&r->queries);
    vb_reset(&r->print_buf);
    r->verb = VERB_OTHER;
    r->it_offset = 0;
    r->ot_offset = 0;
}

/**
 * Initialize the HTTPD request state.
 *
 * This is done with fresh connection, so there is no previous state to
 * clean up.
 *
 * @param[out] r	request state
 */
static void
httpd_init_request(request_t *r)
{
    memset(r, 0, sizeof(*r));
    httpd_reinit_request(r);
}

/**
 * Free HTTPD request state.
 *
 * @param[in] r		Request
 */
static void
httpd_free_request(request_t *r)
{
    /* Free the print buffer. */
    vb_free(&r->print_buf);

    /* Reinitialize everthing. */
    httpd_reinit_request(r);
}

/**
 * Initialize the entire HTTPD state for a connection.
 *
 * @param[in,out] h	state
 * @param[in] mhandle	main logic handle
 */
static void
httpd_init_state(httpd_t *h, void *mhandle)
{
    httpd_init_request(&h->request);

    h->cr = False;
    h->mhandle = mhandle;
    h->seq = httpd_seq++;
}

/**
 * Write the HTTP header.
 *
 * @param[in] h			State
 * @param[in] status_code	HTTP status code
 * @param[in] do_close		True if we should send 'Connection: close'
 * @param[in] content_type	Value for Content-Type field
 */
static void
httpd_http_header(httpd_t *h, int status_code, Boolean do_close,
	const char *content_type)
{
    request_t *r = &h->request;
    time_t t;
    char *a;

    vtrace("h> [%lu] Response: %d %s\n", h->seq, status_code,
	    status_text(status_code));

    httpd_print(h, HP_BUFFER, "HTTP/1.1 %d %s\n", status_code,
	    status_text(status_code));
    t = time(NULL);
    a = asctime(gmtime(&t));
    httpd_print(h, HP_BUFFER, "Date: %.*s UTC\n", strlen(a) - 1, a);
    httpd_print(h, HP_BUFFER, "Server: %s\n", build);
    if (do_close) {
	httpd_print(h, HP_BUFFER, "Connection: close\n");
    }
    if (status_code == 301 && r->location != NULL) {
	httpd_print(h, HP_BUFFER, "Location: %s\n", r->location);
    }
    httpd_print(h, HP_BUFFER, "Content-Type: %s\n", content_type);

    /* Now write it. */
    httpd_print_dump(h, DUMP_WITHOUT_LENGTH);
}

/**
 * Write the standard trailer.
 *
 * The trailer includes the </BODY> bracket.
 *
 * @param[in] h		State
 * @param[in] type	Print type (send or buffer)
 */
static void
httpd_html_trailer(httpd_t *h, httpd_print_t type)
{
    httpd_print(h, type, "\n");
    httpd_print(h, type, " <HR>\n");
    httpd_print(h, type,
	    " <I>%s - <A HREF=\"http://x3270.bgp.nu/\">x3270.bgp.nu</A></I>\n",
	    build);
    httpd_print(h, type, " </BODY>\n");
}

/**
 * Fail an HTTP request, varargs interface
 *
 * @param[in,out] h		State
 * @param[in] mode		Error mode (how far we got before the error)
 * @param[in] status_code	HTTP status code
 * @param[in] verb		Request verb
 * @param[in] format		printf format for extended error message
 * @param[in] ap		printf args
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_verror(httpd_t *h, errmode_t mode, int status_code, verb_t verb,
	const char *format, va_list ap)
{
    request_t *r = &h->request;

    /* If the request wasn't complete junk, wrap the error response in HTTP. */
    if (mode != ERRMODE_NON_HTTP) {
	httpd_http_header(h, status_code, mode <= ERRMODE_FATAL,
		"text/html; charset=iso8859-1");
    } else {
	vtrace("h> [%lu] Response: %d %s\n", h->seq, status_code,
		status_text(status_code));
    }

    /*
     * For (apparent) HTTP connections, buffer the body of the error message
     * so we can send a Content-Length field. Otherwise, send it straight out.
     *
     * If we ever are in danger of sending anything larger than the output
     * buffer in an error message, we can used chunked encoding.
     */
    if (mode == ERRMODE_NON_HTTP) {
	httpd_print(h, HP_BUFFER, "\n");
    }

    if (verb != VERB_HEAD) {
	/* Generate the body. */
	httpd_print(h, HP_BUFFER,
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n");
	httpd_print(h, HP_BUFFER, "<HTML>\n");
	httpd_print(h, HP_BUFFER, " <HEAD>\n");
	httpd_print(h, HP_BUFFER, "  <TITLE>%d %s</TITLE>\n", status_code,
		status_text(status_code));
	httpd_print(h, HP_BUFFER, " </HEAD>\n");
	httpd_print(h, HP_BUFFER, " <BODY>\n");
	httpd_print(h, HP_BUFFER, " <H1>%d %s</H1>\n", status_code,
		status_text(status_code));
	httpd_vprint(h, HP_BUFFER, format, ap);
	httpd_html_trailer(h, HP_BUFFER);
	httpd_print(h, HP_BUFFER, "</HTML>\n");

	/*
	 * Dump the Content-Length (if HTTP) now and terminate the response
	 * header.
	 */
	httpd_print_dump(h, (mode > ERRMODE_NON_HTTP)? DUMP_WITH_LENGTH:
						       DUMP_WITHOUT_LENGTH);
    }

    /*
     * If this is a fatal error, or if the connection is not persistent, close
     * the connection.
     */
    if (mode <= ERRMODE_FATAL || !r->persistent) {
	return HS_ERROR_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_ERROR_OPEN;
    }
}

/**
 * Fail an HTTP request.
 *
 * @param[in,out] h		State
 * @param[in] mode		Error mode (how far we got before the error)
 * @param[in] status_code	HTTP status code
 * @param[in] format		printf format for extended error message
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_error(httpd_t *h, errmode_t mode, int status_code,
	const char *format, ...)
{
    request_t *r = &h->request;
    va_list ap;
    httpd_status_t rv;

    va_start(ap, format);
    rv = httpd_verror(h, mode, status_code, r->verb, format, ap);
    va_end(ap);

    return rv;
}

/**
 * Parse a number inline in a string.
 *
 * @param[in] s		String to parse
 * @param[out] nlp	Returned length of number (number of bytes)
 * @param[out] np	Returned numeric value
 *
 * @return False for no valid number present, True for success
 */
static Boolean
httpd_parse_number(const char *s, size_t *nlp, unsigned long *np)
{
    unsigned long int u;
    char *end;

    u = strtoul(s, &end, 10);
    if ((u == ULONG_MAX && errno == ERANGE) || end == s) {
	*nlp = 0;
	*np = 0;
	return False;
    }
    *nlp = end - s;
    *np = u;
    return True;
}

/**
 * Partially validate the first line of a request.
 *
 * As a side-effect, remember the verb in r->verb, and the URI in r->uri.
 *
 * The request is in r->request_buf[], NULL terminated, with the length
 * (not including the NULL) in r->nr.
 *
 * @param[in,out] h	State
 *
 * @return httpd_status_t
 */
static int
httpd_digest_request_line(httpd_t *h)
{
    request_t *r = &h->request;
    char *verb;
    char *protocol;
    char *junk;
    size_t major_len, minor_len;
    unsigned long major, minor;
    errmode_t errmode;
    int i;
    char *rq;
    static const char *known_verbs[] = {
	"GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE",
	NULL
    };
    static const char *supported_verbs[] = {
	/* Must use same order as the http_verb enumeration. */
	"GET", "HEAD", NULL
    };
    static char http_token[] = "HTTP/";
#   define HTTP_TOKEN_SIZE (sizeof(http_token) - 1)
    static char *whitespace = " \t\f\v";

    /* Until we find HTTP/, errors are junk. */
    errmode = ERRMODE_NON_HTTP;

    rq = r->request_buf;
    vtrace("h< [%lu] Request: %s\n", h->seq, rq);

    /*
     * We need to see something that looks like:
     *  <verb> <uri> [http/n.n]
     */

    /* White space at the beginning of the input is bad. */
    if (isspace((unsigned char)rq[0])) {
	return httpd_error(h, errmode, 400, "<P>Invalid request "
		"syntax.</P>\n<P>Whitespace at the beginning of the "
		"request.</P>");
    }

    /* We expect two or three tokens. */
    verb = strtok(rq, whitespace);
    r->uri = strtok(NULL, whitespace);
    protocol = strtok(NULL, whitespace);
    if (protocol != NULL) {
	junk = strtok(NULL, whitespace);
    } else {
	junk = NULL;
    }
    if (verb == NULL || r->uri == NULL || junk != NULL) {
	return httpd_error(h, errmode, 400, "<P>Invalid request "
		"syntax.</P>\n<P>Invalid number of tokens.</P>");
    }

    /*
     * Check the syntax of the protocol version.
     */
    if (protocol != NULL) {
	if (strncasecmp(protocol, http_token, HTTP_TOKEN_SIZE) ||
		!httpd_parse_number(protocol + HTTP_TOKEN_SIZE, &major_len,
		    &major) ||
		protocol[HTTP_TOKEN_SIZE + major_len] != '.' ||
		!httpd_parse_number(protocol + HTTP_TOKEN_SIZE + major_len + 1,
		    &minor_len, &minor)) {
	    if (!strcmp(verb, "HEAD")) {
		r->verb = VERB_HEAD;
	    }
	    return httpd_error(h, errmode, 400, "Invalid protocol '%s'.",
		    protocol);
	}
	r->http_1_0 = (major == 1 && minor == 0);
	r->persistent = !r->http_1_0;
	errmode = ERRMODE_FATAL;
    } else {
	/* No third token. Assume HTTP 1.0. */
	r->http_1_0 = True;
	r->persistent = False;
    }

    /* Check the verb. */
    for (i = 0; known_verbs[i] != NULL; i++) {
	if (!strcmp(verb, known_verbs[i])) {
	    break;
	}
    }
    if (known_verbs[i] == NULL) {
	return httpd_error(h, errmode, 400, "Unknown verb '%s'.", verb);
    }
    for (i = 0; supported_verbs[i] != NULL; i++) {
	if (!strcmp(verb, supported_verbs[i])) {
	    r->verb = i;
	    break;
	}
    }
    if (supported_verbs[i] == NULL) {
	return httpd_error(h, errmode, 501, "Unsupported verb '%s'.", verb);
    }

    return HS_CONTINUE;
}

/**
 * Translate a hex digit to a number.
 *
 * @param[in] c		Digit character
 *
 * @return Value, or -1 if not a valid digit
 */
static int
hex_digit(char c)
{
    static const char *xlc = "0123456789abcdef";
    static const char *xuc = "0123456789ABCDEF";
    char *x;

    x = strchr(xlc, c);
    if (x != NULL) {
	return x - xlc;
    }
    x = strchr(xuc, c);
    if (x != NULL) {
	return x - xuc;
    }
    return -1;
}

/**
 * Do percent substitution decoding on a URI element.
 *
 * @param[in] uri	URI to parse
 * @param[in] len	Length of URI
 * @param[in] plus	Translate '+' to ' ' as well
 *
 * @return Translated, newly-allocated and NULL-terminated URI, or NULL if
 *  there is a syntax error
 */
static char *
percent_decode(const char *uri, size_t len, Boolean plus)
{
    enum {
	PS_BASE,	/* base state */
	PS_PCT,		/* saw % */
	PS_HEX1		/* saw % and one hex digit */
    } state = PS_BASE;
    int hex1 = 0, hex2;
    const char *s;
    char c;
    varbuf_t r;
    char xc;

    vb_init(&r);

    /* Walk and translate. */
    s = uri;
    while (s < uri + len) {
	c = *s++;

	switch (state) {
	case PS_BASE:
	    if (c == '%') {
		state = PS_PCT;
	    } else {
		if (plus && c == '+') {
		    vb_appends(&r, " ");
		} else {
		    vb_append(&r, &c, 1);
		}
	    }
	    break;
	case PS_PCT:
	    hex1 = hex_digit(c);
	    if (hex1 < 0) {
		vb_free(&r);
		return NULL;
	    }
	    state = PS_HEX1;
	    break;
	case PS_HEX1:
	    hex2 = hex_digit(c);
	    if (hex2 < 0) {
		vb_free(&r);
		return NULL;
	    }
	    xc = (hex1 << 4) | hex2;
	    vb_append(&r, &xc, 1);
	    state = PS_BASE;
	    break;
	}
    }

    /* If we end with a partially-digested sequence, fail. */
    if (state != PS_BASE) {
	vb_free(&r);
	return NULL;
    }

    /* Done. */
    return vb_consume(&r);
}

/**
 * Validate a registered path.
 *
 * @param[in] path
 *
 * @return !=0 for success, 0 for failure
 */
static int
httpd_valid_path(const char *path)
{
    const char *s;
    int nsl;
    char c;

    /* Check for NULL pointer. */
    if (path == NULL) {
	return 0;
    }

    s = path;
    nsl = 0;
    while ((c = *s) != '\0') {
	if (c == '/') {
	    /* We see a slash. */

	    if (nsl) {
		/* Two slashes in a row. */
		return 0;
	    } else {
		/* Remember the slash we saw. */
		nsl++;
	    }
	} else {
	    /* Not a slash. */

	    /* Path must begin with a slash. */
	    if (s == path) {
		return 0;
	    }
	    nsl = 0;
	}
	s++;
    }

    /* Path must not be empty. */
    if (s == path) {
	return 0;
    }

    /* Path must not end with a slash. */
    if (c == '/') {
	return 0;
    }

    /* Okay. */
    return 1;
}

/**
 * Register a dynamic object.
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 * @param[in] content_type Content type CT_xxx
 * @param[in] content_str Content-Type value
 * @param[in] flags	Flags
 * @param[in] dyn	Callback to produce output
 * @param[in] type	Object type (terminal or nonterminal)
 *
 * @return handle for further operations
 */
static void *
httpd_register_dyn(const char *path, const char *desc, content_t content_type,
	const char *content_str, unsigned flags, reg_dyn_t *dyn, or_t type)
{
    httpd_reg_t *reg;

    if (!httpd_valid_path(path)) {
	return NULL;
    }

    reg = Calloc(1, sizeof(*reg));

    reg->path = path;
    reg->desc = desc;
    reg->type = type;
    reg->content_type = content_type;
    reg->content_str = content_str;
    reg->flags = flags;
    reg->u.dyn = dyn;

    reg->next = httpd_reg;
    httpd_reg = reg;

    return reg;
}

/**
 * Reply to a successful URI lookup.
 *
 * @param[in,out] h	State
 * @param[in] reg	Registry entry
 * @param[in] uri	URI
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_reply(httpd_t *h, httpd_reg_t *reg, const char *uri)
{
    request_t *r = &h->request;
    const char *nonterm;

    /* Check for a busy object. */
    if (reg->async_session != NULL) {
	char *q_uri;

	q_uri = html_quote(uri);
	httpd_error(h, ERRMODE_NONFATAL, 409,
		"<P>Object is busy.</P><P>Only one client may access '%s' at "
		"a time.", q_uri);
	Free(q_uri);
	return HS_ERROR_OPEN;
    }

    switch (reg->type) {
    case OR_DYN_TERM:
    case OR_DYN_NONTERM:
	/* Save state. */
	reg->async_session = h;
	r->async_node = reg;

	/*
	 * Call the dynamic function.
	 * It's responsible for calling httpd_dyn_complete() or
	 * httpd_dyn_error().
	 */
	nonterm = uri + strlen(reg->path);
	if (*nonterm == '/') {
	    nonterm++;
	}
	return (*reg->u.dyn)(nonterm, h);
    default:
	break;
    }

    httpd_http_header(h, 200, !r->persistent, reg->content_str);
    httpd_print(h, HP_SEND, "Cache-Control: max-age=43200\n");

    switch (r->verb) {
    case VERB_GET:
    case VERB_OTHER:
	/* Generate the body. */
	if (reg->content_type == CT_HTML) {
	    httpd_print(h, HP_BUFFER,
		    "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n");
	    httpd_print(h, HP_BUFFER, "<HTML>\n");
	}

	switch (reg->type) {
	case OR_FIXED:
	    httpd_print(h, HP_BUFFER, "%s", reg->u.fixed);
	    break;
	case OR_FIXED_BINARY:
	    httpd_content_len(h, reg->u.fixed_binary.length);
	    httpd_send(h, (char *)reg->u.fixed_binary.fixed,
		    reg->u.fixed_binary.length);
	    break;
	case OR_DYN_TERM:
	case OR_DYN_NONTERM:
	case OR_DIR:
	    /* Can't happen. */
	    break;
	}

	if (reg->content_type == CT_HTML) {
	    if (reg->flags & HF_TRAILER) {
		httpd_html_trailer(h, HP_BUFFER);
	    }
	    httpd_print(h, HP_BUFFER, "</HTML>\n");
	}

	/* Dump the Content-Length now and terminate the response header. */
	if (reg->type != OR_FIXED_BINARY) {
	    httpd_print_dump(h, DUMP_WITH_LENGTH);
	}
	break;
    case VERB_HEAD:
	httpd_print(h, HP_SEND, "\n");
	break;
    }

    /* If the connection is not persistent, close the connection. */
    if (!r->persistent) {
	return HS_SUCCESS_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_SUCCESS_OPEN;
    }
}

/**
 * List a directory as the response.
 *
 * @param[in,out] h	State
 * @param[in] uri	URI matched
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_dirlist(httpd_t *h, const char *uri)
{
    request_t *r = &h->request;
    char *q_uri;
    httpd_reg_t *reg;

    httpd_http_header(h, 200, !r->persistent, "text/html; charset=iso8859-1");

    switch (r->verb) {
    case VERB_GET:
    case VERB_OTHER:
	/* Generate the body. */
	q_uri = html_quote(uri);
	httpd_print(h, HP_BUFFER,
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n");
	httpd_print(h, HP_BUFFER, "<HTML>\n");

	httpd_print(h, HP_BUFFER, " <HEAD>\n");
	httpd_print(h, HP_BUFFER, "  <TITLE>Directory of %s</TITLE>\n", q_uri);
	httpd_print(h, HP_BUFFER, " </HEAD>\n");

	httpd_print(h, HP_BUFFER, " <BODY>\n");

	httpd_print(h, HP_BUFFER, " <H1>Directory of %s</H1>\n", q_uri);
	Free(q_uri);

	for (reg = httpd_reg; reg != NULL; reg = reg->next) {
	    if (!strncmp(reg->path, uri, strlen(uri)) &&
		    strchr(reg->path + strlen(uri), '/') == NULL &&
		    !(reg->flags & HF_HIDDEN)) {
		size_t nlen;
		char *q1, *q2;

		nlen = strlen(reg->path + strlen(uri));
		if (reg->type == OR_DIR || reg->type == OR_DYN_NONTERM) {
		    nlen++;
		}
		if (nlen > DIRLIST_NLEN) {
		    nlen = 2;
		} else {
		    nlen = DIRLIST_NLEN + 2 - nlen;
		}
		httpd_print(h, HP_BUFFER,
			"<P><TT><A HREF=\"%s%s\">%s%s</A>",
			(q1 = html_quote(reg->alias? reg->alias: reg->path)),
			(reg->type == OR_DIR && !reg->alias)? "/": "",
			(q2 = html_quote(reg->path + strlen(uri))),
			(reg->type == OR_DIR || reg->type == OR_DYN_NONTERM)?
			    "/": "");
		Free(q1);
		Free(q2);
		while (nlen--) {
		    httpd_print(h, HP_BUFFER, "&nbsp;");
		}
		httpd_print(h, HP_BUFFER, "</TT>%s</P>\n", reg->desc);
	    }
	}

	httpd_html_trailer(h, HP_BUFFER);
	httpd_print(h, HP_BUFFER, "</HTML>\n");

	/* Dump the Content-Length now and terminate the response header. */
	httpd_print_dump(h, DUMP_WITH_LENGTH);
	break;
    case VERB_HEAD:
	httpd_print(h, HP_SEND, "\n");
	break;
    }

    /* If the connection is not persistent, close the connection. */
    if (!r->persistent) {
	return HS_SUCCESS_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_SUCCESS_OPEN;
    }
}

/**
 * Look up the value of a field.
 *
 * @param[in] name	Field name
 *
 * @return Field value, or NULL
 */
static const char *
lookup_field(const char *name, field_t *f)
{
    while (f != NULL) {
	if (!strcasecmp(f->name, name)) {
	    return f->value;
	}
	f = f->next;
    }
    return NULL;
}

/**
 * Redirect a directory name by appending a '/'.
 *
 * @param[in,out] h	State
 * @param[in] uri	URI matched
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_redirect(httpd_t *h, const char *uri)
{
    request_t *r = &h->request;
    const char *host = lookup_field("Host", r->fields);

    if (host == NULL) {
	return httpd_error(h, ERRMODE_NONFATAL, 404, "Document not found.");
    }

    r->location = xs_buffer(r->location, "http://%s%s/", host, uri);
    httpd_error(h, ERRMODE_NONFATAL, 301, "The document has moved "
	    "<A HREF=\"http://%s%s/\">here.</A>.", host, uri);
    Free(r->location);
    r->location = NULL;

    if (!r->persistent) {
	return HS_SUCCESS_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_SUCCESS_OPEN;
    }
}

/**
 * URI not found
 *
 * @param[in,out] h	State
 * @param[in] uri	URI not matched
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_notfound(httpd_t *h, const char *uri)
{
    request_t *r = &h->request;
    char *q_uri = html_quote(uri);

    httpd_error(h, ERRMODE_NONFATAL, 404,
	    "The requested URL %s was not found on this server.", q_uri);
    Free(q_uri);

    if (!r->persistent) {
	return HS_SUCCESS_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_SUCCESS_OPEN;
    }
}

/**
 * Look up a URI in the registry and act on it.
 * 
 * @param[in,out] h	State
 * @param[in] uri	URI to look up, NULL terminated
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_lookup_uri(httpd_t *h, const char *uri)
{
    httpd_reg_t *reg;

    if (!strcmp(uri, "/") || !strcmp(uri, "//")) {
	return httpd_dirlist(h, "/");
    }

    /* Look for an exact match. */
    for (reg = httpd_reg; reg != NULL; reg = reg->next) {
	switch (reg->type) {
	case OR_DIR:
	    if (!strcmp(uri, reg->path)) {
		/* Directory without trailing slash. */
		return httpd_redirect(h, uri);
	    }
	    if (uri[strlen(uri) - 1] == '/' &&
		    strlen(reg->path) == strlen(uri) - 1 &&
		    !strncmp(uri, reg->path, strlen(reg->path))) {
		/* Directory with a trailing slash. */
		return httpd_dirlist(h, uri);
	    }
	    break;
	case OR_FIXED:
	case OR_FIXED_BINARY:
	case OR_DYN_TERM:
	    /* Terminal object. */
	    if (!strcmp(uri, reg->path)) {
		return httpd_reply(h, reg, uri);
	    }
	    break;
	case OR_DYN_NONTERM:
	    /* Nonterminal object. */
	    if (strlen(uri) >= strlen(reg->path) &&
		    !strncmp(uri, reg->path, strlen(reg->path)) &&
		    (strlen(uri) == strlen(reg->path) ||
		     uri[strlen(reg->path)] == '/')) {
		return httpd_reply(h, reg, uri);
	    }
	    break;
	}
    }

    /* Not found. */
    return httpd_notfound(h, uri);
}

/**
 * Parse the query field for a URL.
 *
 * @param[in] h		State
 * @param[in] query	Raw query string from request
 */
static void
parse_queries(httpd_t *h, const char *query)
{
    const char *s = query;
    field_t *f;
    field_t *f_last = NULL;
    Boolean last = False;
    char *eov;

    /* Split the string at each '&'. */
    for (s = query; !last; s = eov + 1) {
	char *eq;
	char *name;
	char *value;

	eov = strchr(s, '&');
	if (!eov) {
	    eov = strchr(s, '\0');
	    last = True;
	}

	eq = strchr(s, '=');
	if (eq == NULL || eq == s || eq > eov) {
	    continue;
	}

	name = percent_decode(s, eq - s, False);
	value = percent_decode(eq + 1, eov - (eq + 1), True);
	f = Malloc(sizeof(*f) + strlen(name) + 1 + strlen(value) + 1);
	f->next = NULL;
	f->name = (char *)(f + 1);
	strcpy(f->name, name);
	f->value = f->name + strlen(name) + 1;
	strcpy(f->value, value);
	Free(name);
	Free(value);

	if (f_last) {
	    f_last->next = f;
	} else {
	    h->request.queries = f;
	}
	f_last = f;
    }
}

/**
 * Digest the entire request.
 *
 * The entire text is in r->request_buf, NULL terminated, including newline
 * characters. The length of the request, not including the NULL, is in
 * r->nr. The fields in the request are pointed to by f->fields_start.
 *
 * @param[in,out] h	State
 *
 * @return httpd_status_t
 */
static httpd_status_t
httpd_digest_request(httpd_t *h)
{
    request_t *r = &h->request;
    char *s = r->fields_start;
    char *cand_uri;
    char *uri;
    const char *connection;
    httpd_status_t rv;
    char *query;
    char *fragment;

    /*
     * Parse the fields.
     *  We ignore fields we don't understand.
     *  We require, but actually pay no attention to, the host field.
     *  We understand 'connection: close', but ignore other 'connection:'
     *   values. The close state is left in r->persistent.
     * I'm sure this is HTTP 1.1 blasphemy.
     */
    while (*s) {
	while (*s != '\n') {
	    char *field_name = s;
	    size_t field_name_len;
	    char *value;
	    size_t value_len;
	    field_t *f;

	    /* The field name needs to start with a non-space, non-colon. */
	    if (iscntrl((unsigned char)*s) ||
		    isspace((unsigned char)*s) ||
		    *s == ':') {
		return httpd_error(h, ERRMODE_FATAL, 400, "Malformed "
			"field name in request.");
	    }

	    /* Parse the rest of the name. */
	    while (*s != '\n' && *s != ':' && !isspace((unsigned char)*s)) {
		if (iscntrl((unsigned char)*s)) {
		    return httpd_error(h, ERRMODE_FATAL, 400,
			    "Malformed field name in request.");
		}
		s++;
	    }
	    field_name_len = s - field_name;

	    /* Skip spaces after the name (technically illegal). */
	    while (*s != '\n' && isspace((unsigned char)*s)) {
		s++;
	    }

	    /* Now we need a colon. */
	    if (*s != ':') {
		return httpd_error(h, ERRMODE_FATAL, 400, "Malformed "
			"field (missing colon) in request.");
	    }
	    s++;

	    /* Skip spaces after the colon. */
	    while (*s != '\n' && isspace((unsigned char)*s)) {
		s++;
	    }

	    /* What's after that whitespace is the value. */
	    value = s;
	    while (*s != '\n') {
		s++;
	    }
	    value_len = s - value;

	    /* Trim trailing spaces from the value. */
	    while (value_len && isspace((unsigned char)s[value_len - 1])) {
		value_len--;
	    }
	    if (value_len == 0) {
		return httpd_error(h, ERRMODE_FATAL, 400, "Malformed "
			"field (missing value) in request.");
	    }

	    /* Store it. */
	    f = Malloc(sizeof(*f) + field_name_len + 1 + value_len + 1);
	    f->name = (char *)(f + 1);
	    strncpy(f->name, field_name, field_name_len);
	    f->name[field_name_len] = '\0';
	    f->value = f->name + field_name_len + 1;
	    strncpy(f->value, value, value_len);
	    f->value[value_len] = '\0';

	    /* Choke on duplicates. */
	    if (lookup_field(f->name, r->fields) != NULL) {
		return httpd_error(h, ERRMODE_FATAL, 400, "Duplicate "
			"field in request.");
	    }

	    /* Link it in. */
	    f->next = r->fields;
	    r->fields = f;
	}
	s++;
    }

    /* For HTTP 1.1, require a 'Host:' field. */
    if (!r->http_1_0 && lookup_field("Host", r->fields) == NULL) {
	return httpd_error(h, ERRMODE_FATAL, 400, "Missing hostname.");
    }

    /* Check for connection close request. */
    if ((connection = lookup_field("Connection", r->fields)) != NULL &&
	    !strcasecmp(connection, "close")) {
	r->persistent = False;
    }

    /*
     * Split the URI at '?' or '#' before doing percent decodes.
     * This allows '?' and '#' to be percent-encoded in any of the elements.
     */
    query = strchr(r->uri, '?');
    fragment = strchr(r->uri, '#');
    if (query && (!fragment || query < fragment)) {
	*query = '\0';
	r->query = query + 1;
	if (fragment) {
	    *fragment = '\0';
	    r->fragment = fragment + 1;
	}
    }
    if (fragment && (!query || fragment < query)) {
	*fragment = '\0';
	r->fragment = fragment + 1;
    }

    /* Do percent substitution on the URI. */
    cand_uri = percent_decode(r->uri, strlen(r->uri), False);
    if (cand_uri == NULL) {
	return httpd_error(h, ERRMODE_FATAL, 400,
		"Invalid URI (percent substution error).");
    }

    /*
     * Parse the URI.
     *  We understand requests that start with '/', which are relative to our
     *   root. (After that, we expect '3270/'.)
     *  We understand requests that start with 'http://<whatever>/', and
     *   ignore everything through <whatever>.
     *  Anything else we barf on.
     */
    if (strlen(cand_uri) > 7 && !strncasecmp(cand_uri, "http://", 7)) {
	char *slash = strchr(cand_uri + 7, '/');

	if (slash == NULL) {
	    Free(cand_uri);
	    return httpd_error(h, ERRMODE_FATAL, 400, "Invalid URI "
		    "syntax after http://.");
	} else {
	    uri = slash;
	}
    } else {
	uri = cand_uri;
    }
    if (uri[0] != '/') {
	Free(cand_uri);
	return httpd_error(h, ERRMODE_FATAL, 400, "Invalid URI");
    }

    /* Pick apart the query fields. */
    if (r->query) {
	parse_queries(h, r->query);
    }

    /*
     * Now we have a URI in what seems like valid form.
     * Search the registry for a match.
     */
    rv = httpd_lookup_uri(h, cand_uri);
    Free(cand_uri);
    return rv;
}

/**
 * Process a byte of incoming HTTP data.
 *
 * @param[in,out] h	State
 * @param[in] c		Next input character
 *
 * @return httpd_status_t
 */
static int
httpd_input_char(httpd_t *h, char c)
{
    request_t *r = &h->request;

    /*
     * CRLF processing. We translate CRs into Newlines, and ignore LFs
     * after CRs.
     */
    if (h->cr) {
	h->cr = False;

	/* CR followed by LF. Ignore the LF. */
	if (c == '\n') {
	    return HS_CONTINUE;
	}
    }
    if (c == '\r') {
	h->cr = True;

	/* Treat CRs as Newline characters. */
	c = '\n';
    }

    /* If there's no room to store the character, we're done. */
    if (r->nr >= MAX_HTTPD_REQUEST) {
	return httpd_error(h,
		r->saw_first? ERRMODE_FATAL: ERRMODE_NON_HTTP,
		400, "The request is too big.");
    }

    /* Store the character. */
    r->request_buf[r->nr++] = c;

    /* Check for a newline. */
    if (c == '\n') {
	if (r->rll == 0) {
	    /* Empty line: digest the entire request. */
	    if (!r->saw_first) {
		return httpd_error(h, ERRMODE_FATAL, 400,
			"Missing request.");
	    }
	    r->request_buf[r->nr] = '\0';
	    return httpd_digest_request(h);
	} else {
	    /* Beginning of new line; set the length to 0. */
	    r->rll = 0;

	    /* If this is the first line, validate it. */
	    if (!r->saw_first) {
		r->request_buf[r->nr - 1] = '\0';
		r->fields_start = &r->request_buf[r->nr];
		r->saw_first = True;
		return httpd_digest_request_line(h);
	    }
	}
    } else {
	/* Non-newline character: increment the line length. */
	r->rll++;
    }

    /* Not done yet. */
    return HS_CONTINUE;
}

/*****************************************************************************
 * Functions called by the main logic.
 *****************************************************************************/

/**
 * Register a directory (give its description)
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 *
 * @return handle for further operations
 */
void *
httpd_register_dir(const char *path, const char *desc)
{
    httpd_reg_t *reg;

    if (!httpd_valid_path(path)) {
	return NULL;
    }

    reg = Calloc(1, sizeof(*reg));

    reg->path = path;
    reg->desc = desc;
    reg->type = OR_DIR;

    reg->next = httpd_reg;
    httpd_reg = reg;

    return reg;
}

/**
 * Register a fixed-content object.
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 * @param[in] content_type Content type CT_xxx
 * @param[in] content_str Content-Type value
 * @param[in] flags	Flags
 * @param[in] fixed	Fixed text to return
 *
 * @return handle for further operations
 */
void *
httpd_register_fixed(const char *path, const char *desc,
	content_t content_type, const char *content_str, unsigned flags,
	const char *fixed)
{
    httpd_reg_t *reg;

    if (!httpd_valid_path(path)) {
	return NULL;
    }

    reg = Calloc(1, sizeof(*reg));

    reg->path = path;
    reg->desc = desc;
    reg->type = OR_FIXED;
    reg->content_type = content_type;
    reg->content_str = content_str;
    reg->flags = flags;
    reg->u.fixed = fixed;

    reg->next = httpd_reg;
    httpd_reg = reg;

    return reg;
}

/**
 * Register a fixed-content binary object.
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 * @param[in] content_type Content type CT_xxx
 * @param[in] content_str Content-Type value
 * @param[in] flags	Flags
 * @param[in] fixed	Fixed text to return
 * @param[in] length	Length of text
 *
 * @return handle for further operations
 */
void *
httpd_register_fixed_binary(const char *path, const char *desc,
	content_t content_type, const char *content_str, unsigned flags,
	const unsigned char *fixed, unsigned length)
{
    httpd_reg_t *reg;

    if (!httpd_valid_path(path)) {
	return NULL;
    }

    reg = Calloc(1, sizeof(*reg));

    reg->path = path;
    reg->desc = desc;
    reg->type = OR_FIXED_BINARY;
    reg->content_type = content_type;
    reg->content_str = content_str;
    reg->flags = flags;
    reg->u.fixed_binary.fixed = fixed;
    reg->u.fixed_binary.length = length;

    reg->next = httpd_reg;
    httpd_reg = reg;

    return reg;
}

/**
 * Register a dynamic terminal object.
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 * @param[in] content_type Content type CT_xxx
 * @param[in] content_str Content-Type value
 * @param[in] flags	Flags
 * @param[in] dyn	Callback to produce output
 *
 * @return handle for further operations
 */
void *
httpd_register_dyn_term(const char *path, const char *desc,
	content_t content_type, const char *content_str, unsigned flags,
	reg_dyn_t *dyn)
{
    return httpd_register_dyn(path, desc, content_type, content_str, flags,
	    dyn, OR_DYN_TERM);
}

/**
 * Register a dynamic nonterminal object.
 *
 * @param[in] path	Path, must include leading /
 * @param[in] desc	Description
 * @param[in] content_type Content type CT_xxx
 * @param[in] content_str Content-Type value
 * @param[in] flags	Flags
 * @param[in] dyn	Callback to produce output
 *
 * @return handle for further operations
 */
void *
httpd_register_dyn_nonterm(const char *path, const char *desc,
	content_t content_type, const char *content_str, unsigned flags,
	reg_dyn_t *dyn)
{
    return httpd_register_dyn(path, desc, content_type, content_str, flags,
	    dyn, OR_DYN_NONTERM);
}

/**
 * Register an alias for a node, displayed in the directory entry.
 *
 * @param[in] nhandle	Node handle returned from httpd_register_xxx()
 * @param[in] text	Alternate text to display
 */
void
httpd_set_alias(void *nhandle, const char *text)
{
    httpd_reg_t *reg = nhandle;

    if (reg) {
	reg->alias = text;
    }
}

/**
 * Initialize a new connection.
 *
 * Called when a connection is accepted.
 *
 * @param[in] mhandle		Handle from main process
 * @param[in] client_name	Client name, for debug
 *
 * @return handle for further operations
 */
void *
httpd_new(void *mhandle, const char *client_name)
{
    httpd_t *h;

    h = Malloc(sizeof(httpd_t));
    memset(h, 0, sizeof(*h));
    httpd_init_state(h, mhandle);

    vtrace("h< [%lu] New session from %s\n", h->seq, client_name);

    return h;
}

/**
 * Process incoming HTTP data.
 *
 * Called with data read from the HTTP socket.
 *
 * @param[in] dhandle	handle returned by httpd_new
 * @param[in] data	data buffer
 * @param[in] len	length of data in buffer
 *
 * @return httpd_status_t
 */
httpd_status_t
httpd_input(void *dhandle, const char *data, size_t len)
{
    httpd_t *h = (httpd_t *)dhandle;
    request_t *r = &h->request;
    size_t i;
    httpd_status_t rv = HS_CONTINUE;

    httpd_data_trace(h, "<", data, len, &r->it_offset);

    /* Process a byte at a time, skipping CRs. */
    for (i = 0; i < len; i++) {
	switch ((rv = httpd_input_char(h, data[i]))) {
	case HS_CONTINUE:
	    /* Keep parsing. */
	    continue;
	case HS_SUCCESS_OPEN:
	    httpd_reinit_request(r);
	    return rv;
	case HS_ERROR_OPEN:
	    /* Request failed, but keep the socket open. */
	    httpd_reinit_request(r);
	    /* fall through */
	case HS_ERROR_CLOSE:
	    /* Request failed, close the socket. */
	case HS_SUCCESS_CLOSE:
	    /* Request succeeded, close the socket. */
	case HS_PENDING:
	    /* Request pending, hold off further input. */
	    return rv;
	}
    }

    /* Success, at least so far. */
    return rv;
}

/**
 * Close the HTTPD connection.
 *
 * @param[in] dhandle	Handle returned by httpd_new
 * @param[in] why	Reason (for debug)
 */
void
httpd_close(void *dhandle, const char *why)
{
    httpd_t *h = dhandle;

    vtrace("h> [%lu] Close: %s\n", h->seq, why);

    /* Wipe the existing request state. */
    httpd_free_request(&h->request);

    /* Free it. */
    memset(h, 0, sizeof(*h));
    Free(h);
}

/**
 * Map a dhandle (handle returned from httpd_new()) onto an mhandle (handle
 * passed into httpd_new()).
 *
 * This is a helper function used during async processing.
 *
 * @param[in] dhandle	Daemon handle
 *
 * @return Main handle
 */
void *
httpd_mhandle(void *dhandle)
{
    httpd_t *h = dhandle;

    return h->mhandle;
}

/*****************************************************************************
 * Functions called by methods.
 *****************************************************************************/

/**
 * Successfully complete a dynamic HTTP request.
 *
 * Called from a synchronous method or an asynchronous completion function.
 * Writes the entire response back to the socket.
 *
 * @param[in] dhandle	handle returned by httpd_new
 * @param[in] format	printf format string
 *
 * @return httpd_status_t, suitable for return from completion function
 *  (HS_SUCCESS_OPEN or HS_SUCCESS_CLOSE).
 */
httpd_status_t
httpd_dyn_complete(void *dhandle, const char *format, ...)
{
    httpd_t *h = (httpd_t *)dhandle;
    request_t *r = &h->request;
    httpd_reg_t *reg = r->async_node;
    va_list ap;

    /* Un-mark the node and session as busy. */
    reg->async_session = NULL;
    r->async_node = NULL;

    /* Generate the output. */
    httpd_http_header(h, 200, !r->persistent, reg->content_str);
    httpd_print(h, HP_SEND, "Cache-Control: no-store\n");

    switch (r->verb) {
    case VERB_GET:
    case VERB_OTHER:
	/* Generate the body. */
	if (reg->content_type == CT_HTML) {
	    httpd_print(h, HP_BUFFER,
		    "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n");
	    httpd_print(h, HP_BUFFER, "<HTML>\n");
	}
	va_start(ap, format);
	httpd_vprint(h, HP_BUFFER, format, ap);
	va_end(ap);
	if (reg->content_type == CT_HTML) {
	    if (reg->flags & HF_TRAILER) {
		httpd_html_trailer(h, HP_BUFFER);
	    }
	    httpd_print(h, HP_BUFFER, "</HTML>\n");
	}

	/* Dump the Content-Length now and terminate the response header. */
	httpd_print_dump(h, DUMP_WITH_LENGTH);
	break;
    case VERB_HEAD:
	httpd_print(h, HP_SEND, "\n");
	break;
    }

    /* Return status. */
    if (!r->persistent) {
	return HS_SUCCESS_CLOSE;
    } else {
	httpd_reinit_request(r);
	return HS_SUCCESS_OPEN;
    }
}

/**
 * Unsuccessfully complete a dynamic HTTP request.
 *
 * Called from a synchronous method or an asynchronous completion function.
 * Writes the entire response back to the socket.
 *
 * @param[in] dhandle	Connection handle
 * @param[in] status_code HTTP error code
 * @param[in] format	text to display
 *
 * @return httpd_status_t, suitable for return from completion function
 *  (HS_ERROR_OPEN or HS_ERROR_CLOSE).
 */
httpd_status_t
httpd_dyn_error(void *dhandle, int status_code, const char *format, ...)
{
    httpd_t *h = dhandle;
    request_t *r = &h->request;
    va_list ap;
    httpd_status_t rv;

    /* Un-mark the node and session as busy. */
    r->async_node->async_session = NULL;
    r->async_node = NULL;

    va_start(ap, format);
    rv = httpd_verror(h, ERRMODE_NONFATAL, status_code, r->verb, format, ap);
    va_end(ap);

    return rv;
}

/**
 * Quote text to pass transparently through to HTML.
 *
 * @param[in] text	Text to expand
 *
 * @return Expanded text, needs to be freed afterward
 */
char *
html_quote(const char *text)
{
    varbuf_t r;
    char c;

    vb_init(&r);

    while ((c = *text++)) {
	switch (c) {
	case '&':
	    vb_appends(&r, "&amp;");
	    break;
	case '<':
	    vb_appends(&r, "&lt;");
	    break;
	case '>':
	    vb_appends(&r, "&gt;");
	    break;
	case '"':
	    vb_appends(&r, "&quot;");
	    break;
	default:
	    vb_append(&r, &c, 1);
	    break;
	}
    }
    return vb_consume(&r);
}

/**
 * Quote a URI. Uses percent encoding.
 *
 * @param[in] uri	URI to quote
 *
 * @return Expanded URI, needs to be freed afterward
 */
char *
uri_quote(const char *text)
{
    varbuf_t r;
    char c;

    vb_init(&r);

    while ((c = *text++)) {
	if (c > ' ' && c < 0x7f && c != '%') {
	    vb_append(&r, &c, 1);
	} else {
	    vb_appendf(&r, "%%%02x", c & 0xff);
	}
    }
    return vb_consume(&r);
}

/**
 * Fetch a query from the current request.
 *
 * @param[in] dhandle	Connection handle
 * @param[in] name	Name of item to fetch
 *
 * @return Query value, or NULL
 */
const char *
httpd_fetch_query(void *dhandle, const char *name)
{
    httpd_t *h = dhandle;
    request_t *r = &h->request;
    field_t *f;

    for (f = r->queries; f != NULL; f = f->next) {
	if (!strcmp(f->name, name)) {
	    return f->value;
	}
    }
    return NULL;

}
