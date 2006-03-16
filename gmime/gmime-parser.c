/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@ximian.com>
 *
 *  Copyright 2002-2004 Ximian, Inc. (www.ximian.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <ctype.h>

#include "gmime-parser.h"

#include "gmime-stream-mem.h"
#include "gmime-message-part.h"
#include "gmime-multipart.h"
#include "gmime-part.h"

#ifndef HAVE_ISBLANK
#define isblank(c) ((c) == ' ' || (c) == '\t')
#endif /* HAVE_ISBLANK */

#ifdef ENABLE_WARNINGS
#define w(x) x
#else
#define w(x)
#endif /* ENABLE_WARNINGS */

#define d(x)

static void g_mime_parser_class_init (GMimeParserClass *klass);
static void g_mime_parser_init (GMimeParser *parser, GMimeParserClass *klass);
static void g_mime_parser_finalize (GObject *object);

static void parser_init (GMimeParser *parser, GMimeStream *stream);
static void parser_close (GMimeParser *parser);

static GMimeObject *parser_construct_leaf_part (GMimeParser *parser, GMimeContentType *content_type,
						int *found);
static GMimeObject *parser_construct_multipart (GMimeParser *parser, GMimeContentType *content_type,
						int *found);

static GObjectClass *parent_class = NULL;

#define SCAN_BUF 4096		/* size of read buffer */
#define SCAN_HEAD 128		/* headroom guaranteed to be before each read buffer */

enum {
	GMIME_PARSER_STATE_ERROR = -1,
	GMIME_PARSER_STATE_INIT,
	GMIME_PARSER_STATE_FROM,
	GMIME_PARSER_STATE_HEADERS,
	GMIME_PARSER_STATE_HEADERS_END,
	GMIME_PARSER_STATE_CONTENT,
};

struct _GMimeParserPrivate {
	int state;
	
	GMimeStream *stream;
	
	off_t offset;
	
	/* i/o buffers */
	unsigned char realbuf[SCAN_HEAD + SCAN_BUF + 1];
	unsigned char *inbuf;
	unsigned char *inptr;
	unsigned char *inend;
	
	off_t from_offset;
	GByteArray *from_line;
	
	regex_t header_regex;
	GMimeParserHeaderRegexFunc header_cb;
	gpointer user_data;
	
	/* header buffer */
	unsigned char *headerbuf;
	unsigned char *headerptr;
	unsigned int headerleft;
	
	/* raw header buffer */
	unsigned char *rawbuf;
	unsigned char *rawptr;
	unsigned int rawleft;
	
	off_t headers_start;
	off_t header_start;
	
	unsigned int unstep:26;
	unsigned int midline:1;
	unsigned int seekable:1;
	unsigned int scan_from:1;
	unsigned int have_regex:1;
	unsigned int persist_stream:1;
	unsigned int respect_content_length:1;
	
	GMimeContentType *content_type;
	struct _header_raw *headers;
	
	struct _boundary_stack *bounds;
};

struct _boundary_stack {
	struct _boundary_stack *parent;
	unsigned char *boundary;
	size_t boundarylen;
	size_t boundarylenfinal;
	size_t boundarylenmax;
	off_t content_end;
};


static void
parser_push_boundary (GMimeParser *parser, const char *boundary)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	struct _boundary_stack *s;
	size_t max;
	
	max = priv->bounds ? priv->bounds->boundarylenmax : 0;
	
	s = g_new (struct _boundary_stack, 1);
	s->parent = priv->bounds;
	priv->bounds = s;
	
	if (!strcmp (boundary, "From ")) {
		s->boundary = g_strdup ("From ");
		s->boundarylen = 5;
		s->boundarylenfinal = 5;
	} else {
		s->boundary = g_strdup_printf ("--%s--", boundary);
		s->boundarylen = strlen (boundary) + 2;
		s->boundarylenfinal = s->boundarylen + 2;
	}
	
	s->boundarylenmax = MAX (s->boundarylenfinal, max);
	
	s->content_end = -1;
}

static void
parser_pop_boundary (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	struct _boundary_stack *s;
	
	if (!priv->bounds) {
		d(g_warning ("boundary stack underflow"));
		return;
	}
	
	s = priv->bounds;
	priv->bounds = priv->bounds->parent;
	
	g_free (s->boundary);
	g_free (s);
}

struct _header_raw {
	struct _header_raw *next;
	char *name;
	char *value;
	off_t offset;
};

static const char *
header_raw_find (struct _header_raw *headers, const char *name, off_t *offset)
{
	struct _header_raw *h;
	
	h = headers;
	while (h) {
		if (!strcasecmp (h->name, name)) {
			if (offset)
				*offset = h->offset;
			return h->value;
		}
		
		h = h->next;
	}
	
	return NULL;
}

static void
header_raw_clear (struct _header_raw **headers)
{
	struct _header_raw *h, *n;
	
	h = *headers;
	while (h) {
		n = h->next;
		g_free (h->name);
		g_free (h->value);
		g_free (h);
		h = n;
	}
	
	*headers = NULL;
}

GType
g_mime_parser_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static const GTypeInfo info = {
			sizeof (GMimeParserClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc) g_mime_parser_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (GMimeParser),
			16,   /* n_preallocs */
			(GInstanceInitFunc) g_mime_parser_init,
		};
		
		type = g_type_register_static (G_TYPE_OBJECT, "GMimeParser", &info, 0);
	}
	
	return type;
}


static void
g_mime_parser_class_init (GMimeParserClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	
	parent_class = g_type_class_ref (G_TYPE_OBJECT);
	
	object_class->finalize = g_mime_parser_finalize;
}

static void
g_mime_parser_init (GMimeParser *parser, GMimeParserClass *klass)
{
	parser->priv = g_new (struct _GMimeParserPrivate, 1);
	parser_init (parser, NULL);
	
	parser->priv->scan_from = FALSE;
	parser->priv->have_regex = FALSE;
	parser->priv->persist_stream = TRUE;
	parser->priv->respect_content_length = FALSE;
}

static void
g_mime_parser_finalize (GObject *object)
{
	GMimeParser *parser = (GMimeParser *) object;
	
	parser_close (parser);
	
	if (parser->priv->have_regex)
		regfree (&parser->priv->header_regex);
	
	g_free (parser->priv);
	
	G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
parser_init (GMimeParser *parser, GMimeStream *stream)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	off_t offset = -1;
	
	if (stream) {
		g_object_ref (stream);
		offset = g_mime_stream_tell (stream);
	}
	
	priv->state = GMIME_PARSER_STATE_INIT;
	
	priv->stream = stream;
	
	priv->offset = offset;
	
	priv->inbuf = priv->realbuf + SCAN_HEAD;
	priv->inptr = priv->inbuf;
	priv->inend = priv->inbuf;
	
	priv->from_offset = -1;
	priv->from_line = g_byte_array_new ();
	
	priv->headerbuf = g_malloc (SCAN_HEAD + 1);
	priv->headerptr = priv->headerbuf;
	priv->headerleft = SCAN_HEAD;
	
	priv->rawbuf = g_malloc (SCAN_HEAD + 1);
	priv->rawptr = priv->rawbuf;
	priv->rawleft = SCAN_HEAD;
	
	priv->headers_start = -1;
	priv->header_start = -1;
	
	priv->unstep = 0;
	priv->midline = FALSE;
	priv->seekable = offset != -1;
	
	priv->headers = NULL;
	
	priv->bounds = NULL;
}

static void
parser_close (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	
	if (priv->stream)
		g_object_unref (priv->stream);
	
	g_byte_array_free (priv->from_line, TRUE);
	
	g_free (priv->headerbuf);
	g_free (priv->rawbuf);
	
	header_raw_clear (&priv->headers);
	
	while (priv->bounds)
		parser_pop_boundary (parser);
}

static void
parser_unstep (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	
	priv->unstep++;
}


/**
 * g_mime_parser_new:
 *
 * Creates a new parser object.
 *
 * Returns a new parser object.
 **/
GMimeParser *
g_mime_parser_new (void)
{
	GMimeParser *parser;
	
	parser = g_object_new (GMIME_TYPE_PARSER, NULL);
	
	return parser;
}


/**
 * g_mime_parser_new_with_stream:
 * @stream: raw message or part stream
 *
 * Creates a new parser object preset to parse @stream.
 *
 * Returns a new parser object.
 **/
GMimeParser *
g_mime_parser_new_with_stream (GMimeStream *stream)
{
	GMimeParser *parser;
	
	parser = g_mime_parser_new ();
	g_mime_parser_init_with_stream (parser, stream);
	
	return parser;
}


/**
 * g_mime_parser_init_with_stream:
 * @parser: MIME parser object
 * @stream: raw message or part stream
 *
 * Initializes @parser to use @stream.
 *
 * WARNING: Initializing a parser with a stream is comparable to
 * selling your soul (@stream) to the devil (@parser). You are
 * basically giving the parser complete control of the stream, this
 * means that you had better not touch the stream so long as the
 * parser is still using it. This means no reading, writing, seeking,
 * or resetting of the stream. Anything that will/could change the
 * current stream's offset is PROHIBITED.
 *
 * It is also recommended that you not use g_mime_stream_tell()
 * because it will not necessarily give you the current @parser offset
 * since @parser handles its own internal read-ahead buffer. Instead,
 * it is recommended that you use g_mime_parser_tell() if you have a
 * reason to need the current offset of the @parser.
 **/
void
g_mime_parser_init_with_stream (GMimeParser *parser, GMimeStream *stream)
{
	g_return_if_fail (GMIME_IS_PARSER (parser));
	g_return_if_fail (GMIME_IS_STREAM (stream));
	
	parser_close (parser);
	parser_init (parser, stream);
}


/**
 * g_mime_parser_get_persist_stream:
 * @parser: MIME parser object
 *
 * Gets whether or not the underlying stream is persistent.
 *
 * Returns %TRUE if the @parser will leave the content on disk or
 * %FALSE if it will load the content into memory.
 **/
gboolean
g_mime_parser_get_persist_stream (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), FALSE);
	
	return (parser->priv->persist_stream && parser->priv->seekable);
}


/**
 * g_mime_parser_set_persist_stream:
 * @parser: MIME parser object
 * @persist: persist attribute
 *
 * Sets whether or not the @parser's underlying stream is persistent.
 *
 * If @persist is %TRUE, the @parser will attempt to construct
 * messages/parts whose content will remain on disk rather than being
 * loaded into memory so as to reduce memory usage. This is the default.
 *
 * If @persist is %FALSE, the @parser will always load message content
 * into memory.
 *
 * Note: This attribute only serves as a hint to the @parser. If the
 * underlying stream does not support seeking, then this attribute
 * will be ignored.
 **/
void
g_mime_parser_set_persist_stream (GMimeParser *parser, gboolean persist)
{
	g_return_if_fail (GMIME_IS_PARSER (parser));
	
	parser->priv->persist_stream = persist;
}


/**
 * g_mime_parser_get_scan_from:
 * @parser: MIME parser object
 *
 * Gets whether or not @parser is set to scan mbox-style From-lines.
 *
 * Returns whether or not @parser is set to scan mbox-style
 * From-lines.
 **/
gboolean
g_mime_parser_get_scan_from (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), FALSE);
	
	return parser->priv->scan_from;
}


/**
 * g_mime_parser_set_scan_from:
 * @parser: MIME parser object
 * @scan_from: %TRUE to scan From-lines or %FALSE otherwise
 *
 * Sets whether or not @parser should scan mbox-style From-lines.
 **/
void
g_mime_parser_set_scan_from (GMimeParser *parser, gboolean scan_from)
{
	g_return_if_fail (GMIME_IS_PARSER (parser));
	
	parser->priv->scan_from = scan_from ? 1 : 0;
}


/**
 * g_mime_parser_get_respect_content_length:
 * @parser: MIME parser object
 *
 * Gets whether or not @parser is set to use Content-Length for
 * determining the offset of the end of the message.
 *
 * Returns whether or not @parser is set to use Content-Length for
 * determining the offset of the end of the message.
 **/
gboolean
g_mime_parser_get_respect_content_length (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), FALSE);
	
	return parser->priv->respect_content_length;
}


/**
 * g_mime_parser_set_respect_content_length:
 * @parser: MIME parser object
 * @respect_content_length: %TRUE if the parser should use Content-Length headers or %FALSE otherwise.
 *
 * Sets whether or not @parser should respect Content-Length headers
 * when deciding where to look for the start of the next message. Only
 * used when the parser is also set to scan for From-lines.
 *
 * Most notably useful when parsing broken Solaris mbox files (See
 * http://www.jwz.org/doc/content-length.html for details).
 **/
void
g_mime_parser_set_respect_content_length (GMimeParser *parser, gboolean respect_content_length)
{
	g_return_if_fail (GMIME_IS_PARSER (parser));
	
	parser->priv->respect_content_length = respect_content_length ? 1 : 0;
}


/**
 * g_mime_parser_set_header_regex:
 * @parser: MIME parser object
 * @regex: regular expression
 * @header_cb: callback function
 * @user_data: user data
 *
 * Sets the regular expression pattern @regex on @parser. Whenever a
 * header matching the pattern @regex is parsed, @header_cb is called
 * with @user_data as the user_data argument.
 **/
void
g_mime_parser_set_header_regex (GMimeParser *parser, const char *regex,
				GMimeParserHeaderRegexFunc header_cb, gpointer user_data)
{
	struct _GMimeParserPrivate *priv;
	
	g_return_if_fail (GMIME_IS_PARSER (parser));
	
	priv = parser->priv;
	
	if (priv->have_regex) {
		regfree (&priv->header_regex);
		priv->have_regex = FALSE;
	}
	
	if (!regex || !header_cb)
		return;
	
	priv->header_cb = header_cb;
	priv->user_data = user_data;
	
	priv->have_regex = !regcomp (&priv->header_regex, regex, REG_EXTENDED | REG_ICASE | REG_NOSUB);
}


static ssize_t
parser_fill (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	unsigned char *inbuf, *inptr, *inend;
	ssize_t nread;
	size_t inlen;
	
	inbuf = priv->inbuf;
	inptr = priv->inptr;
	inend = priv->inend;
	inlen = inend - inptr;
	
	g_assert (inptr <= inend);
	
	/* attempt to align 'inend' with realbuf + SCAN_HEAD */
	if (inptr >= inbuf) {
		inbuf -= inlen < SCAN_HEAD ? inlen : SCAN_HEAD;
		memmove (inbuf, inptr, inlen);
		inptr = inbuf;
		inbuf += inlen;
	} else if (inptr > priv->realbuf) {
		size_t shift;
		
		shift = MIN (inptr - priv->realbuf, inend - inbuf);
		memmove (inptr - shift, inptr, inlen);
		inptr -= shift;
		inbuf = inptr + inlen;
	} else {
		/* we can't shift... */
		inbuf = inend;
	}
	
	priv->inptr = inptr;
	priv->inend = inbuf;
	inend = priv->realbuf + SCAN_HEAD + SCAN_BUF - 1;
	
	if ((nread = g_mime_stream_read (priv->stream, inbuf, inend - inbuf)) > 0)
		priv->inend += nread;
	
	priv->offset = g_mime_stream_tell (priv->stream);
	
	return priv->inend - priv->inptr;
}


static off_t
parser_offset (struct _GMimeParserPrivate *priv, unsigned char *inptr)
{
	if (priv->offset == -1)
		return -1;
	
	if (!inptr)
		inptr = priv->inptr;
	
	return (priv->offset - (priv->inend - inptr));
}


/**
 * g_mime_parser_tell:
 * @parser: MIME parser object
 *
 * Gets the current stream offset from the parser's internal stream.
 *
 * Returns the current stream offset from the parser's internal stream
 * or -1 on error.
 **/
off_t
g_mime_parser_tell (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), -1);
	g_return_val_if_fail (GMIME_IS_STREAM (parser->priv->stream), -1);
	
	return parser_offset (parser->priv, NULL);
}


/**
 * g_mime_parser_eos:
 * @parser: MIME parser
 *
 * Tests the end-of-stream indicator for @parser's internal stream.
 *
 * Returns %TRUE on EOS or %FALSE otherwise.
 **/
gboolean
g_mime_parser_eos (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv;
	
	g_return_val_if_fail (GMIME_IS_STREAM (parser->priv->stream), TRUE);
	
	priv = parser->priv;
	return g_mime_stream_eos (priv->stream) && priv->inptr == priv->inend;
}

static int
parser_step_from (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	register unsigned char *inptr;
	unsigned char *start, *inend;
	size_t len, left = 0;
	
	g_byte_array_set_size (priv->from_line, 0);
	
	inptr = priv->inptr;
	
	g_assert (inptr <= priv->inend);
	
	do {
	refill:
		if (parser_fill (parser) <= left) {
			/* failed to find a From line; EOF reached */
			priv->state = GMIME_PARSER_STATE_ERROR;
			priv->inptr = priv->inend;
			return -1;
		}
		
		inptr = priv->inptr;
		inend = priv->inend;
		*inend = '\n';
		
		while (inptr < inend) {
			start = inptr;
			while (*inptr != '\n')
				inptr++;
			
			if (inptr + 1 >= inend) {
				/* we don't have enough data; if we can't get more we have to bail */
				left = inend - start;
				priv->inptr = start;
				goto refill;
			}
			
			len = inptr - start;
			inptr++;
			
			if (len >= 5 && !strncmp (start, "From ", 5)) {
				priv->from_offset = parser_offset (priv, start);
				g_byte_array_append (priv->from_line, start, len);
				goto got_from;
			}
		}
		
		priv->inptr = inptr;
		left = 0;
	} while (1);
	
 got_from:
	
	priv->state = GMIME_PARSER_STATE_HEADERS;
	
	priv->inptr = inptr;
	
	return 0;
}

#define header_append(priv, start, len) G_STMT_START {                    \
	if (priv->headerleft <= len) {                                    \
		size_t hlen, hoff;                                        \
		                                                          \
		hlen = hoff = priv->headerptr - priv->headerbuf;          \
		hlen = hlen ? hlen : 1;                                   \
		                                                          \
		while (hlen < hoff + len)                                 \
			hlen <<= 1;                                       \
		                                                          \
		priv->headerbuf = g_realloc (priv->headerbuf, hlen + 1);  \
		priv->headerptr = priv->headerbuf + hoff;                 \
		priv->headerleft = hlen - hoff;                           \
	}                                                                 \
	                                                                  \
	memcpy (priv->headerptr, start, len);                             \
	priv->headerptr += len;                                           \
	priv->headerleft -= len;                                          \
} G_STMT_END

#define raw_header_append(priv, start, len) G_STMT_START {                \
	if (priv->rawleft <= len) {                                       \
		size_t hlen, hoff;                                        \
		                                                          \
		hlen = hoff = priv->rawptr - priv->rawbuf;                \
		hlen = hlen ? hlen : 1;                                   \
		                                                          \
		while (hlen < hoff + len)                                 \
			hlen <<= 1;                                       \
		                                                          \
		priv->rawbuf = g_realloc (priv->rawbuf, hlen + 1);        \
		priv->rawptr = priv->rawbuf + hoff;                       \
		priv->rawleft = hlen - hoff;                              \
	}                                                                 \
	                                                                  \
	memcpy (priv->rawptr, start, len);                                \
	priv->rawptr += len;                                              \
	priv->rawleft -= len;                                             \
} G_STMT_END

#define raw_header_reset(priv) G_STMT_START {                             \
	priv->rawleft += priv->rawptr - priv->rawbuf;                     \
	priv->rawptr = priv->rawbuf;                                      \
} G_STMT_END

#define header_parse(parser, priv, hend) G_STMT_START {                   \
	register unsigned char *colon;                                    \
	struct _header_raw *header;                                       \
	size_t hlen;                                                      \
	                                                                  \
	header = g_new (struct _header_raw, 1);                           \
	header->next = NULL;                                              \
	                                                                  \
	*priv->headerptr = '\0';                                          \
	colon = priv->headerbuf;                                          \
	while (*colon && *colon != ':')                                   \
		colon++;                                                  \
	                                                                  \
	hlen = colon - priv->headerbuf;                                   \
	                                                                  \
	header->name = g_strndup (priv->headerbuf, hlen);                 \
	g_strstrip (header->name);                                        \
	if (*colon != ':') {                                              \
		w(g_warning ("Invalid header: %s", header->name));        \
		header->value = header->name;                             \
		header->name = g_strdup ("X-Invalid-Header");             \
	} else {                                                          \
		header->value = g_strdup (colon + 1);                     \
		g_strstrip (header->value);                               \
	}                                                                 \
	header->offset = priv->header_start;                              \
	                                                                  \
	hend->next = header;                                              \
	hend = header;                                                    \
	                                                                  \
	priv->headerleft += priv->headerptr - priv->headerbuf;            \
	priv->headerptr = priv->headerbuf;                                \
	                                                                  \
	if (priv->have_regex &&                                           \
	    !regexec (&priv->header_regex, header->name, 0, NULL, 0))     \
		priv->header_cb (parser, header->name, header->value,     \
				 header->offset, priv->user_data);        \
} G_STMT_END

static int
parser_step_headers (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	register unsigned char *inptr;
	unsigned char *start, *inend;
	struct _header_raw *hend;
	size_t len, left = 0;
	
	priv->midline = FALSE;
	hend = (struct _header_raw *) &priv->headers;
	priv->headers_start = parser_offset (priv, NULL);
	priv->header_start = parser_offset (priv, NULL);
	
	inptr = priv->inptr;
	inend = priv->inend;
	
	do {
	refill:
		if (parser_fill (parser) <= left)
			break;
		
		inptr = priv->inptr;
		inend = priv->inend;
		/* Note: see optimization comment [1] */
		*inend = '\n';
		
		g_assert (inptr <= inend);
		
		while (inptr < inend) {
			start = inptr;
			/* Note: see optimization comment [1] */
			while (*inptr != '\n')
				inptr++;
			
			raw_header_append (priv, start, inptr - start);
			
			if (inptr == inend) {
				/* we don't have enough data to tell if we
				   got all of the header or not... */
				header_append (priv, start, inptr - start);
				priv->midline = TRUE;
				left = inend - inptr;
				priv->inptr = inptr;
				goto refill;
			}
			
			/* check to see if we've reached the end of the headers */
			if (!priv->midline && (inptr == start || (inptr - start == 1 && *start == '\r')))
				goto headers_end;
			
			len = inptr - start;
			if (inptr > start && inptr[-1] == '\r')
				len--;
			
			header_append (priv, start, len);
			
			/* inptr has to be less than inend - 1 */
			raw_header_append (priv, inptr, 1);
			inptr++;
			
			if (*inptr == ' ' || *inptr == '\t') {
				priv->midline = TRUE;
			} else {
				priv->midline = FALSE;
				header_parse (parser, priv, hend);
				priv->header_start = parser_offset (priv, inptr);
			}
		}
		
		left = inend - inptr;
		priv->inptr = inptr;
	} while (1);
	
	inptr = priv->inptr;
	inend = priv->inend;
	
	header_append (priv, inptr, inend - inptr);
	raw_header_append (priv, inptr, inend - inptr);
	
 headers_end:
	
	if (priv->headerptr > priv->headerbuf)
		header_parse (parser, priv, hend);
	
	*priv->rawptr = '\0';
	
	priv->state = GMIME_PARSER_STATE_HEADERS_END;
	
	g_assert (inptr <= priv->inend);
	
	priv->inptr = inptr;
	
	return 0;
}

static GMimeContentType *
parser_content_type (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	const char *content_type;
	
	if ((content_type = header_raw_find (priv->headers, "Content-Type", NULL)))
		return g_mime_content_type_new_from_string (content_type);
	
	return NULL;
}

static int
parser_step (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	
	if (!priv->unstep) {
	step:
		switch (priv->state) {
		case GMIME_PARSER_STATE_INIT:
			if (priv->scan_from)
				priv->state = GMIME_PARSER_STATE_FROM;
			else
				priv->state = GMIME_PARSER_STATE_HEADERS;
			goto step;
			break;
		case GMIME_PARSER_STATE_FROM:
			parser_step_from (parser);
			break;
		case GMIME_PARSER_STATE_HEADERS:
			parser_step_headers (parser);
			break;
		case GMIME_PARSER_STATE_ERROR:
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	} else {
		priv->unstep--;
	}
	
	return priv->state;
}

static void
parser_skip_line (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	register unsigned char *inptr;
	unsigned char *inend;
	
	inptr = priv->inptr;
	
	do {
		if (parser_fill (parser) <= 0) {
			inptr = priv->inptr;
			break;
		}
		
		inptr = priv->inptr;
		inend = priv->inend;
		*inend = '\n';
		
		while (*inptr != '\n')
			inptr++;
		
		if (inptr < inend)
			break;
		
		priv->inptr = inptr;
	} while (1);
	
	priv->midline = FALSE;
	
	priv->inptr = MIN (inptr + 1, priv->inend);
}

enum {
	FOUND_EOS          = 1,
	FOUND_BOUNDARY,
	FOUND_END_BOUNDARY
};

#define content_save(content, start, len) G_STMT_START {   \
	if (content)                                       \
		g_byte_array_append (content, start, len); \
} G_STMT_END

#define possible_boundary(scan_from, start, len)                                      \
                         ((scan_from && len >= 5 && !strncmp (start, "From ", 5)) ||  \
			  (len >= 2 && (start[0] == '-' && start[1] == '-')))

static int
check_boundary (struct _GMimeParserPrivate *priv, const unsigned char *start, size_t len)
{
	off_t offset = parser_offset (priv, (unsigned char *) start);
	
	if (start[len - 1] == '\r')
		len--;
	
	if (possible_boundary (priv->scan_from, start, len)) {
		struct _boundary_stack *s;
		
		d(printf ("checking boundary '%.*s'\n", len, start));
		
		s = priv->bounds;
		while (s) {
			/* we use >= here because From lines are > 5 chars */
			if (offset >= s->content_end &&
			    len >= s->boundarylenfinal &&
			    !strncmp (s->boundary, start,
				      s->boundarylenfinal)) {
				d(printf ("found %s\n", s->content_end != -1 && offset >= s->content_end ?
					  "end of content" : "end boundary"));
				return FOUND_END_BOUNDARY;
			}
			
			if (len == s->boundarylen &&
			    !strncmp (s->boundary, start,
				      s->boundarylen)) {
				d(printf ("found boundary\n"));
				return FOUND_BOUNDARY;
			}
			
			s = s->parent;
		}
		
		d(printf ("'%.*s' not a boundary\n", len, start));
	}
	
	return 0;
}

/* Optimization Notes:
 *
 * 1. By making the priv->realbuf char array 1 extra char longer, we
 * can safely set '*inend' to '\n' and not fear an ABW. Setting *inend
 * to '\n' means that we can eliminate having to check that inptr <
 * inend every trip through our inner while-loop. This cuts the number
 * of instructions down from ~7 to ~4, assuming the compiler does its
 * job correctly ;-)
 **/

static int
parser_scan_content (GMimeParser *parser, GByteArray *content, int *crlf)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	register unsigned char *inptr;
	unsigned char *start, *inend;
	size_t nleft, len;
	int found = 0;
	
	d(printf ("scan-content\n"));
	
	priv->midline = FALSE;
	
	g_assert (priv->inptr <= priv->inend);
	
	start = inptr = priv->inptr;
	
	do {
	refill:
		nleft = priv->inend - inptr;
		if (parser_fill (parser) <= 0) {
			start = priv->inptr;
			found = FOUND_EOS;
			break;
		}
		
		inptr = priv->inptr;
		inend = priv->inend;
		/* Note: see optimization comment [1] */
		*inend = '\n';
		
		if (priv->midline && inend - inptr == nleft)
			found = FOUND_EOS;
		
		priv->midline = FALSE;
		
		while (inptr < inend) {
			start = inptr;
			/* Note: see optimization comment [1] */
			while (*inptr != '\n')
				inptr++;
			
			len = inptr - start;
			
			if (inptr < inend) {
				if ((found = check_boundary (priv, start, len)))
					goto boundary;
				
				inptr++;
				len++;
			} else {
				/* didn't find an end-of-line */
				priv->midline = TRUE;
				
				if (!found) {
					/* not enough to tell if we found a boundary */
					priv->inptr = start;
					inptr = start;
					goto refill;
				}
				
				/* check for a boundary not ending in a \n */
				if ((found = check_boundary (priv, start, len)))
					goto boundary;
			}
			
			content_save (content, start, len);
		}
		
		priv->inptr = inptr;
	} while (!found);
	
 boundary:
	
	/* don't chew up the boundary */
	priv->inptr = start;
	
	if (found != FOUND_EOS) {
		if (inptr[-1] == '\r')
			*crlf = 2;
		else
			*crlf = 1;
	} else {
		*crlf = 0;
	}
	
	return found;
}

static void
parser_scan_mime_part_content (GMimeParser *parser, GMimePart *mime_part, int *found)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	GMimePartEncodingType encoding;
	GByteArray *content = NULL;
	GMimeDataWrapper *wrapper;
	GMimeStream *stream;
	off_t start, end;
	int crlf;
	
	if (priv->persist_stream && priv->seekable)
		start = parser_offset (priv, NULL);
	else
		content = g_byte_array_new ();
	
	*found = parser_scan_content (parser, content, &crlf);
	if (*found != FOUND_EOS) {
		/* last '\n' belongs to the boundary */
		if (priv->persist_stream && priv->seekable)
			end = parser_offset (priv, NULL) - crlf;
		else
			g_byte_array_set_size (content, MAX (content->len - crlf, 0));
	} else if (priv->persist_stream && priv->seekable) {
		end = parser_offset (priv, NULL);
	}
	
	encoding = g_mime_part_get_encoding (mime_part);
	
	if (priv->persist_stream && priv->seekable) {
		stream = g_mime_stream_substream (priv->stream, start, end);
	} else {
		stream = g_mime_stream_mem_new_with_byte_array (content);
	}
	
	wrapper = g_mime_data_wrapper_new_with_stream (stream, encoding);
	g_mime_part_set_content_object (mime_part, wrapper);
	g_object_unref (wrapper);
	g_object_unref (stream);
}

static void
parser_scan_message_part (GMimeParser *parser, GMimeMessagePart *mpart, int *found)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	GMimeContentType *content_type;
	struct _header_raw *header;
	GMimeMessage *message;
	GMimeObject *object;
	
	/* get the headers */
	parser->priv->state = GMIME_PARSER_STATE_HEADERS;
	while (parser_step (parser) != GMIME_PARSER_STATE_HEADERS_END)
		;
	
	message = g_mime_message_new (FALSE);
	header = priv->headers;
	while (header) {
		g_mime_object_add_header ((GMimeObject *) message, header->name, header->value);
		header = header->next;
	}
	
	if (!(content_type = parser_content_type (parser)))
		content_type = g_mime_content_type_new ("text", "plain");
	
	parser_unstep (parser);
	if (g_mime_content_type_is_type (content_type, "multipart", "*")) {
		object = parser_construct_multipart (parser, content_type, found);
	} else {
		object = parser_construct_leaf_part (parser, content_type, found);
	}
	
	message->mime_part = object;
	
	g_mime_message_part_set_message (mpart, message);
	g_object_unref (message);
}

static GMimeObject *
parser_construct_leaf_part (GMimeParser *parser, GMimeContentType *content_type, int *found)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	struct _header_raw *header;
	GMimeObject *object;
	
	/* get the headers */
	while (parser_step (parser) != GMIME_PARSER_STATE_HEADERS_END)
		;
	
	object = g_mime_object_new_type (content_type->type, content_type->subtype);
	header = priv->headers;
	while (header) {
		g_mime_object_add_header (object, header->name, header->value);
		header = header->next;
	}
	
	header_raw_clear (&priv->headers);
	
	if (object->content_type)
		g_mime_content_type_destroy (object->content_type);
	object->content_type = content_type;
	
	g_mime_header_set_raw (object->headers, priv->rawbuf);
	raw_header_reset (priv);
	
	/* skip empty line after headers */
	parser_skip_line (parser);
	
	if (GMIME_IS_MESSAGE_PART (object)) {
		parser_scan_message_part (parser, (GMimeMessagePart *) object, found);
	} else {
		parser_scan_mime_part_content (parser, (GMimePart *) object, found);
	}
	
	return object;
}

static void
crlf2lf (unsigned char *in)
{
	register unsigned char *inptr = in;
	register unsigned char *outptr;
	
	while (*inptr != '\0' && !(inptr[0] == '\r' && inptr[1] == '\n'))
		inptr++;
	
	if (*inptr == '\0')
		return;
	
	outptr = inptr++;
	
	while (*inptr != '\0') {
		while (*inptr != '\0' && !(inptr[0] == '\r' && inptr[1] == '\n'))
			*outptr++ = *inptr++;
		
		if (*inptr == '\r')
			inptr++;
	}
	
	*outptr = '\0';
}

static int
parser_scan_multipart_face (GMimeParser *parser, GMimeMultipart *multipart, gboolean preface)
{
	GByteArray *buffer;
	const char *face;
	int crlf, found;
	
	buffer = g_byte_array_new ();
	found = parser_scan_content (parser, buffer, &crlf);
	
	if (buffer->len >= crlf) {
		/* last '\n' belongs to the boundary */
		g_byte_array_set_size (buffer, buffer->len + 1);
		buffer->data[buffer->len - crlf - 1] = '\0';
		crlf2lf (buffer->data);
		face = buffer->data;
		
		if (preface)
			g_mime_multipart_set_preface (multipart, face);
		else
			g_mime_multipart_set_postface (multipart, face);
	}
	
	g_byte_array_free (buffer, TRUE);
	
	return found;
}

#define parser_scan_multipart_preface(parser, multipart) parser_scan_multipart_face (parser, multipart, TRUE)
#define parser_scan_multipart_postface(parser, multipart) parser_scan_multipart_face (parser, multipart, FALSE)

static int
parser_scan_multipart_subparts (GMimeParser *parser, GMimeMultipart *multipart)
{
	GMimeContentType *content_type;
	GMimeObject *subpart;
	int found;
	
	do {
		/* skip over the boundary marker */
		parser_skip_line (parser);
		
		/* get the headers */
		parser_step_headers (parser);
		
		if (!(content_type = parser_content_type (parser)))
			content_type = g_mime_content_type_new ("text", "plain");
		
		parser_unstep (parser);
		if (g_mime_content_type_is_type (content_type, "multipart", "*")) {
			subpart = parser_construct_multipart (parser, content_type, &found);
		} else {
			subpart = parser_construct_leaf_part (parser, content_type, &found);
		}
		
		g_mime_multipart_add_part (multipart, subpart);
		g_object_unref (subpart);
	} while (found == FOUND_BOUNDARY);
	
	return found;
}

static gboolean
found_immediate_boundary (struct _GMimeParserPrivate *priv)
{
	struct _boundary_stack *s = priv->bounds;
	
	return !strncmp (s->boundary, priv->inptr, s->boundarylenfinal);
}

static GMimeObject *
parser_construct_multipart (GMimeParser *parser, GMimeContentType *content_type, int *found)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	struct _header_raw *header;
	GMimeMultipart *multipart;
	const char *boundary;
	GMimeObject *object;
	
	/* get the headers */
	while (parser_step (parser) != GMIME_PARSER_STATE_HEADERS_END)
		;
	
	object = g_mime_object_new_type (content_type->type, content_type->subtype);
	header = priv->headers;
	while (header) {
		g_mime_object_add_header (object, header->name, header->value);
		header = header->next;
	}
	
	header_raw_clear (&priv->headers);
	
	if (object->content_type)
		g_mime_content_type_destroy (object->content_type);
	object->content_type = content_type;
	
	g_mime_header_set_raw (object->headers, priv->rawbuf);
	raw_header_reset (priv);
	
	multipart = (GMimeMultipart *) object;
	
	/* skip empty line after headers */
	parser_skip_line (parser);
	
	boundary = g_mime_content_type_get_parameter (content_type, "boundary");
	if (boundary) {
		parser_push_boundary (parser, boundary);
		
		*found = parser_scan_multipart_preface (parser, multipart);
		
		if (*found == FOUND_BOUNDARY)
			*found = parser_scan_multipart_subparts (parser, multipart);
		
		if (*found == FOUND_END_BOUNDARY && found_immediate_boundary (priv)) {
			/* eat end boundary */
			parser_skip_line (parser);
			parser_pop_boundary (parser);
			*found = parser_scan_multipart_postface (parser, multipart);
		} else {
			parser_pop_boundary (parser);
		}
	} else {
		w(g_warning ("multipart without boundary encountered"));
		/* this will scan everything into the preface */
		*found = parser_scan_multipart_preface (parser, multipart);
	}
	
	return object;
}

static GMimeObject *
parser_construct_part (GMimeParser *parser)
{
	GMimeContentType *content_type;
	GMimeObject *object;
	int found;
	
	/* get the headers */
	while (parser_step (parser) != GMIME_PARSER_STATE_HEADERS_END)
		;
	
	if (!(content_type = parser_content_type (parser)))
		content_type = g_mime_content_type_new ("text", "plain");
	
	parser_unstep (parser);
	if (g_mime_content_type_is_type (content_type, "multipart", "*")) {
		object = parser_construct_multipart (parser, content_type, &found);
	} else {
		object = parser_construct_leaf_part (parser, content_type, &found);
	}
	
	return object;
}


/**
 * g_mime_parser_construct_part:
 * @parser: MIME parser object
 *
 * Constructs a MIME part from @parser.
 *
 * Returns a MIME part based on @parser or %NULL on fail.
 **/
GMimeObject *
g_mime_parser_construct_part (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), NULL);
	
	return parser_construct_part (parser);
}


static GMimeMessage *
parser_construct_message (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv = parser->priv;
	GMimeContentType *content_type;
	struct _header_raw *header;
	int content_length = -1;
	GMimeMessage *message;
	GMimeObject *object;
	int state, found;
	
	/* get the headers (and, optionally, the from-line) */
	while ((state = parser_step (parser)) != GMIME_PARSER_STATE_ERROR && state != GMIME_PARSER_STATE_HEADERS_END)
		;
	
	if (state == GMIME_PARSER_STATE_ERROR)
		return NULL;
	
	message = g_mime_message_new (FALSE);
	header = priv->headers;
	while (header) {
		if (priv->respect_content_length && !strcasecmp (header->name, "Content-Length"))
			content_length = strtoul (header->value, NULL, 10);
		
		g_mime_object_add_header ((GMimeObject *) message, header->name, header->value);
		header = header->next;
	}
	
	if (priv->scan_from) {
		parser_push_boundary (parser, "From ");
		if (priv->respect_content_length && content_length != -1)
			priv->bounds->content_end = parser_offset (priv, NULL) + content_length;
	}
	
	if (!(content_type = parser_content_type (parser)))
		content_type = g_mime_content_type_new ("text", "plain");
	
	parser_unstep (parser);
	if (content_type && g_mime_content_type_is_type (content_type, "multipart", "*")) {
		object = parser_construct_multipart (parser, content_type, &found);
	} else {
		object = parser_construct_leaf_part (parser, content_type, &found);
	}
	
	message->mime_part = object;
	
	if (priv->scan_from) {
		priv->state = GMIME_PARSER_STATE_FROM;
		parser_pop_boundary (parser);
	}
	
	return message;
}


/**
 * g_mime_parser_construct_message:
 * @parser: MIME parser object
 *
 * Constructs a MIME message from @parser.
 *
 * Returns a MIME message or %NULL on fail.
 **/
GMimeMessage *
g_mime_parser_construct_message (GMimeParser *parser)
{
	g_return_val_if_fail (GMIME_IS_PARSER (parser), NULL);
	
	return parser_construct_message (parser);
}


/**
 * g_mime_parser_get_from:
 * @parser: MIME parser object
 *
 * Gets the mbox-style From-line of the most recently parsed message
 * (gotten from g_mime_parser_construct_message()).
 *
 * Returns the mbox-style From-line of the most recently parsed
 * message or %NULL on error.
 **/
char *
g_mime_parser_get_from (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv;
	
	g_return_val_if_fail (GMIME_IS_PARSER (parser), NULL);
	
	priv = parser->priv;
	if (!priv->scan_from)
		return NULL;
	
	if (priv->from_line->len)
		return g_strndup (priv->from_line->data, priv->from_line->len);
	
	return NULL;
}


/**
 * g_mime_parser_get_from_offset:
 * @parser: MIME parser object
 *
 * Gets the offset of the most recently parsed mbox-style From-line
 * (gotten from g_mime_parser_construct_message()).
 *
 * Returns the offset of the most recently parsed mbox-style From-line
 * or -1 on error.
 **/
off_t
g_mime_parser_get_from_offset (GMimeParser *parser)
{
	struct _GMimeParserPrivate *priv;
	
	g_return_val_if_fail (GMIME_IS_PARSER (parser), -1);
	
	priv = parser->priv;
	if (!priv->scan_from)
		return -1;
	
	return priv->from_offset;
}
