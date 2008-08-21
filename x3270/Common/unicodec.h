/*
 * Copyright 2008 by Paul Mattes.
 *   Permission to use, copy, modify, and distribute this software and its
 *   documentation for any purpose and without fee is hereby granted,
 *   provided that the above copyright notice appear in all copies and that
 *   both that copyright notice and this permission notice appear in
 *   supporting documentation.
 *
 * wc3270 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the file LICENSE for more details.
 */

extern unsigned long ebcdic_to_unicode(unsigned short e, unsigned char cs,
	Boolean for_display);
extern unsigned long ebcdic_base_to_unicode(unsigned short e,
	Boolean blank_undef, Boolean for_display);
extern unsigned short unicode_to_ebcdic(unsigned long u);
extern unsigned short unicode_to_ebcdic_ge(unsigned long u, Boolean *ge);
extern int set_uni(const char *csname, const char **codepage,
	const char **display_charsets);
extern int linedraw_to_unicode(unsigned short e);
extern int apl_to_unicode(unsigned short e);
#if defined(USE_ICONV) /*[*/
extern iconv_t i_u2mb;
extern iconv_t i_mb2u;
#endif /*]*/
