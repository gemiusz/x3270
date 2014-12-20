/*
 * Copyright (c) 1995-2009, 2013-2014 Paul Mattes.
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
 * mkfb.c
 *	Utility to create RDB string definitions from a simple #ifdef'd .ad
 *	file
 */

#include "conf.h"
#if defined(_WIN32) /*[*/
# include "wincmn.h"
#endif /*]*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#define BUFSZ	1024		/* input line buffer size */
#define ARRSZ	8192		/* output array size */
#define SSSZ	10		/* maximum nested ifdef */

unsigned aix[ARRSZ];		/* fallback array indices */
unsigned xlno[ARRSZ];		/* fallback array line numbers */
unsigned n_fallbacks = 0;	/* number of fallback entries */

/* ifdef state stack */
#define MODE_COLOR	0x00000001
#define MODE_APL	0x00000040
#define MODE_STANDALONE	0x00000100
#define MODE_DBCS	0x00000400
#define MODE__WIN32	0x00000800

#define MODEMASK	0x00000fff

struct {
	unsigned long ifdefs;
	unsigned long ifndefs;
	unsigned lno;
} ss[SSSZ];
unsigned ssp = 0;

struct {
	const char *name;
	unsigned long mask;
} parts[] = {
	{ "COLOR", MODE_COLOR },
	{ "X3270_APL", MODE_APL },
	{ "STANDALONE", MODE_STANDALONE },
	{ "X3270_DBCS", MODE_DBCS },
	{ "_WIN32", MODE__WIN32 }
};
#define NPARTS	(sizeof(parts)/sizeof(parts[0]))

unsigned long is_defined =
    MODE_COLOR |
#if defined(X3270_APL)
	MODE_APL
#else
	0
#endif
|
#if defined(X3270_DBCS)
	MODE_DBCS
#else
	0
#endif
|
#if defined(FOR_WIN32) || defined(_WIN32)
	MODE__WIN32
#else
	0
#endif
    ;
unsigned long is_undefined;

char *me;

void emit(FILE *t, int ix, char c);

void
usage(void)
{
	fprintf(stderr, "usage: %s [infile [outfile]]\n", me);
	exit(1);
}

/*
 * Wrapper around Windows' brain-dead tmpfile().
 */
static FILE *
mkfb_tmpfile(void)
{
	FILE *f;
#if defined(_WIN32) /*[*/
	char *n;
#endif /*]*/

#if !defined(_WIN32) /*[*/
	f = tmpfile();
	if (f == NULL) {
		perror("tmpfile");
		exit(1);
	}
#else /*][*/
	n = _tempnam(NULL, "mkfb");
	if (n == NULL) {
		fprintf(stderr, "_tempnam failed.\n");
		exit(1);
	}
	f = fopen(n, "w+b");
	if (f == NULL) {
		fprintf(stderr, "_tempnam open(\"%s\") failed: %s\n",
			n, strerror(errno));
		exit(1);
	}
	free(n);
#endif /*]*/

	return f;
}

int
main(int argc, char *argv[])
{
	char buf[BUFSZ];
	int lno = 0;
	int cc = 0;
	unsigned i;
	int continued = 0;
	const char *filename = "standard input";
	FILE *u, *t, *tc = NULL, *tm = NULL, *o;
	int cmode = 0;
	unsigned long ifdefs;
	unsigned long ifndefs;
	int last_continue = 0;

	/* Parse arguments. */
	if ((me = strrchr(argv[0], '/')) != NULL)
		me++;
	else
		me = argv[0];
	if (argc > 1 && !strcmp(argv[1], "-c")) {
	    cmode = 1;
	    is_defined |= MODE_STANDALONE;
	    argc--;
	    argv++;
	}
	switch (argc) {
	    case 1:
		break;
	    case 2:
	    case 3:
		if (strcmp(argv[1], "-")) {
			if (freopen(argv[1], "r", stdin) == NULL) {
				perror(argv[1]);
				exit(1);
			}
			filename = argv[1];
		}
		break;
	    default:
		usage();
	}

	is_undefined = MODE_COLOR | (~is_defined & MODEMASK);

	/* Do #ifdef, comment and whitespace processing first. */
	u = mkfb_tmpfile();

	while (fgets(buf, BUFSZ, stdin) != NULL) {
		char *s = buf;
		int sl;
		unsigned i;

		lno++;

		/* Skip leading white space. */
		while (isspace(*s))
			s++;
		if (cmode &&
		    (!strncmp(s, "x3270.", 6) || !strncmp(s, "x3270*", 6))) {
			s += 6;
		}

		/* Remove trailing white space. */
		while ((sl = strlen(s)) && isspace(s[sl-1]))
			s[sl-1] = '\0';

		/* Skip comments and empty lines. */
		if ((!last_continue && *s == '!') || !*s)
			continue;

		/* Check for simple if[n]defs. */
		if (*s == '#') {
			int ifnd = 1;

			if (!strncmp(s, "#ifdef ", 7) ||
			    !(ifnd = strncmp(s, "#ifndef ", 8))) {
				char *tk;

				if (ssp >= SSSZ) {
					fprintf(stderr,
					    "%s, line %d: Stack overflow\n",
					    filename, lno);
					exit(1);
				}
				ss[ssp].ifdefs = 0L;
				ss[ssp].ifndefs = 0L;
				ss[ssp].lno = lno;

				tk = s + 7 + !ifnd;
				for (i = 0; i < NPARTS; i++) {
					if (!strcmp(tk, parts[i].name)) {
						if (!ifnd)
							ss[ssp++].ifndefs =
							    parts[i].mask;
						else
							ss[ssp++].ifdefs =
							    parts[i].mask;
						break;
					}
				}
				if (i >= NPARTS) {
					fprintf(stderr,
					    "%s, line %d: Unknown condition\n",
					    filename, lno);
					exit(1);
				}
				continue;
			}

			else if (!strcmp(s, "#else")) {
				unsigned long tmp;

				if (!ssp) {
					fprintf(stderr,
					    "%s, line %d: Missing #if[n]def\n",
					    filename, lno);
					exit(1);
				}
				tmp = ss[ssp-1].ifdefs;
				ss[ssp-1].ifdefs = ss[ssp-1].ifndefs;
				ss[ssp-1].ifndefs = tmp;
			} else if (!strcmp(s, "#endif")) {
				if (!ssp) {
					fprintf(stderr,
					    "%s, line %d: Missing #if[n]def\n",
					    filename, lno);
					exit(1);
				}
				ssp--;
			} else {
				fprintf(stderr,
				    "%s, line %d: Unrecognized # directive\n",
				    filename, lno);
				exit(1);
			}
			continue;
		}

		/* Figure out if there's anything to emit. */

		/* First, look for contradictions. */
		ifdefs = 0;
		ifndefs = 0;
		for (i = 0; i < ssp; i++) {
			ifdefs |= ss[i].ifdefs;
			ifndefs |= ss[i].ifndefs;
		}
		if (ifdefs & ifndefs) {
#ifdef DEBUG_IFDEFS
			fprintf(stderr, "contradiction, line %d\n", lno);
#endif
			continue;
		}

		/* Then, apply the actual values. */
		if (ifdefs && (ifdefs & is_defined) != ifdefs) {
#ifdef DEBUG_IFDEFS
			fprintf(stderr, "ifdef failed, line %d\n", lno);
#endif
			continue;
		}
		if (ifndefs && (ifndefs & is_undefined) != ifndefs) {
#ifdef DEBUG_IFDEFS
			fprintf(stderr, "ifndef failed, line %d\n", lno);
#endif
			continue;
		}

		/* Emit the text. */
		fprintf(u, "%lx %lx %d\n%s\n", ifdefs, ifndefs, lno, s);
		last_continue = strlen(s) > 0 && s[strlen(s) - 1] == '\\';
	}
	if (ssp) {
		fprintf(stderr, "%d missing #endif(s) in %s\n", ssp, filename);
		fprintf(stderr, "last #ifdef was at line %u\n", ss[ssp-1].lno);
		exit(1);
	}

	/* Re-scan, emitting code this time. */
	rewind(u);
	t = mkfb_tmpfile();
	if (!cmode) {
		tc = mkfb_tmpfile();
		tm = mkfb_tmpfile();
	}

	/* Emit the initial boilerplate. */
	fprintf(t, "/* This file was created automatically from %s by mkfb. */\n\n",
	    filename);
	fprintf(t, "#include \"globals.h\"\n");
	fprintf(t, "#include \"fallbacksc.h\"\n");
	if (cmode) {
		fprintf(t, "static unsigned char fsd[] = {\n");
	} else {
		fprintf(t, "unsigned char common_fallbacks[] = {\n");
		fprintf(tc, "unsigned char color_fallbacks[] = {\n");
		fprintf(tm, "unsigned char mono_fallbacks[] = {\n");
	}

	/* Scan the file, emitting the fsd array and creating the indices. */
	while (fscanf(u, "%lx %lx %d\n", &ifdefs, &ifndefs, &lno) == 3) {
		char *s = buf;
		char c;
		int white;
		FILE *t_this = t;
		int ix = 0;

		if (fgets(buf, BUFSZ, u) == NULL)
			break;
		if (strlen(buf) > 0 && buf[strlen(buf)-1] == '\n')
			buf[strlen(buf)-1] = '\0';

#if 0
		fprintf(stderr, "%lx %lx %d %s\n", ifdefs, ifndefs, lno, buf);
#endif

		/* Add array offsets. */
		if (cmode) {
			/* Ignore color.  Accumulate offsets into an array. */
			if (n_fallbacks >= ARRSZ) {
				fprintf(stderr, "%s, line %d: Buffer overflow\n", filename, lno);
				exit(1);
			}
			aix[n_fallbacks] = cc;
			xlno[n_fallbacks++] = lno;
		} else {
			/* Use color to decide which file to write into. */
			if (!(ifdefs & MODE_COLOR) && !(ifndefs & MODE_COLOR)) {
				/* Both. */
				t_this = t;
				ix = 0;
			} else if (ifdefs & MODE_COLOR) {
				/* Just color. */
				t_this = tc;
				ix = 1;
			} else {
				/* Just mono. */
				t_this = tm;
				ix = 2;
			}
		}

		continued = 0;
		white = 0;
		while ((c = *s++) != '\0') {
			if (c == ' ' || c == '\t')
				white++;
			else if (white) {
				emit(t_this, ix, ' ');
				cc++;
				white = 0;
			}
			switch (c) {
			    case ' ':
			    case '\t':
				break;
			    case '#':
				if (!cmode) {
					emit(t_this, ix, '\\');
					emit(t_this, ix, '#');
					cc += 2;
				} else {
					emit(t_this, ix, c);
					cc++;
				}
				break;
			    case '\\':
				if (*s == '\0') {
					continued = 1;
					break;
				} else if (cmode) {
				    switch ((c = *s++)) {
				    case 't':
					c = '\t';
					break;
				    case 'n':
					c = '\n';
					break;
				    default:
					break;
				    }
				}
				/* else fall through */
			    default:
				emit(t_this, ix, c);
				cc++;
				break;
			}
		}
		if (white) {
			emit(t_this, ix, ' ');
			cc++;
			white = 0;
		}
		if (!continued) {
			if (cmode)
				emit(t_this, ix, 0);
			else
				emit(t_this, ix, '\n');
			cc++;
		}
	}
	fclose(u);
	if (cmode)
		fprintf(t, "};\n\n");
	else {
		emit(t, 0, 0);
		fprintf(t, "};\n\n");
		emit(tc, 0, 0);
		fprintf(tc, "};\n\n");
		emit(tm, 0, 0);
		fprintf(tm, "};\n\n");
	}


	/* Open the output file. */
	if (argc == 3) {
		o = fopen(argv[2], "w");
		if (o == NULL) {
			perror(argv[2]);
			exit(1);
		}
	} else
		o = stdout;

	/* Copy tmp to output. */
	rewind(t);
	if (!cmode) {
		rewind(tc);
		rewind(tm);
	}
	while (fgets(buf, sizeof(buf), t) != NULL) {
		fprintf(o, "%s", buf);
	}
	if (!cmode) {
		while (fgets(buf, sizeof(buf), tc) != NULL) {
			fprintf(o, "%s", buf);
		}
		while (fgets(buf, sizeof(buf), tm) != NULL) {
			fprintf(o, "%s", buf);
		}
	}

	if (cmode) {
		/* Emit the fallback array. */
		fprintf(o, "String fallbacks[%u] = {\n", n_fallbacks + 1);
		for (i = 0; i < n_fallbacks; i++) {
			fprintf(o, "\t(String)&fsd[%u], /* line %u */\n",
					aix[i],
			    xlno[i]);
		}
		fprintf(o, "\tNULL\n};\n\n");

		/* Emit some test code. */
		fprintf(o, "%s", "#if defined(DEBUG) /*[*/\n\
#include <stdio.h>\n\
int\n\
main(int argc, char *argv[])\n\
{\n\
	int i;\n\
\n\
	for (i = 0; fallbacks[i] != NULL; i++)\n\
		printf(\"%d: %s\\n\", i, fallbacks[i]);\n\
	return 0;\n\
}\n");
		fprintf(o, "#endif /*]*/\n\n");
	}

	if (o != stdout)
		fclose(o);
	fclose(t);
	if (!cmode) {
		fclose(tc);
		fclose(tm);
	}

	return 0;
}

static int n_out[3] = { 0, 0, 0 };

void
emit(FILE *t, int ix, char c)
{
	if (n_out[ix] >= 19) {
		fprintf(t, "\n");
		n_out[ix] = 0;
	}
	fprintf(t, "%3d,", (unsigned char)c);
	n_out[ix]++;
}
