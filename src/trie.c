/*
 * Copyright (C) 2001,2002 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ident "$Id$"
#include "../config.h"
#include <sys/types.h>
#include <assert.h>
#include <ctype.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <glib.h>
#include <glib-object.h>
#include "trie.h"

#ifndef TRIE_MAYBE_STATIC
#define TRIE_MAYBE_STATIC
#endif

/* Structures and whatnot for tracking character classes. */
struct char_class_data {
	wchar_t c;			/* A character. */
	int i;				/* An integer. */
	char *s;			/* A string. */
	int inc;			/* An increment value. */
};

struct char_class {
	enum cclass {
		exact = 0,		/* Not a special class. */
		digit,			/* Multiple-digit special class. */
		multi,			/* Multiple-number special class. */
		any,			/* Any single character. */
		string,			/* Any string of characters. */
		invalid,		/* A placeholder. */
	} type;
	gboolean multiple;		/* Whether a sequence of multiple
					   characters in this class should be
					   counted together. */
	wchar_t *code;			/* A magic string that indicates this
					   class should be found here. */
	size_t code_length;
	size_t ccount;			/* The maximum number of characters
					   after the format specifier to
					   consume. */
	gboolean (*check)(const wchar_t c, struct char_class_data *data);
					/* Function to check if a character
					   is in this class. */
	void (*setup)(const wchar_t *s, struct char_class_data *data, int inc);
					/* Setup the data struct for use in the
					 * above check function. */
	gboolean (*extract)(const wchar_t *s, size_t length,
			    struct char_class_data *data,
			    GValueArray *array);
					/* Extract a parameter. */
};

/* A trie to hold control sequences. */
struct vte_trie {
	const char *result;		/* If this is a terminal node, then this
					   field contains its "value". */
	GQuark quark;			/* The quark for the value of the
					   result. */
	size_t trie_path_count;		/* Number of children of this node. */
	struct {
		struct char_class *cclass;
		struct char_class_data data;
		struct vte_trie *trie;	/* The child node corresponding to this
					   character. */
	} *trie_paths;
};

/* Functions for checking if a particular character is part of a class, and
 * for setting up a structure for use when determining matches. */
static gboolean
char_class_exact_check(wchar_t c, struct char_class_data *data)
{
	return (c == data->c) ? TRUE : FALSE;
}
static void
char_class_exact_setup(const wchar_t *s, struct char_class_data *data, int inc)
{
	data->c = s[0];
	return;
}
static void
char_class_percent_setup(const wchar_t *s, struct char_class_data *data,
			 int inc)
{
	data->c = '%';
	return;
}
static gboolean
char_class_none_extract(const wchar_t *s, size_t length,
			struct char_class_data *data, GValueArray *array)
{
	return FALSE;
}

static gboolean
char_class_digit_check(wchar_t c, struct char_class_data *data)
{
	switch (c) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			return TRUE;
		default:
			return FALSE;
	}
	return FALSE;
}
static void
char_class_digit_setup(const wchar_t *s, struct char_class_data *data, int inc)
{
	data->inc = inc;
	return;
}
static gboolean
char_class_digit_extract(const wchar_t *s, size_t length,
			 struct char_class_data *data, GValueArray *array)
{
	long ret = 0;
	size_t i;
	GValue value;
	for (i = 0; i < length; i++) {
		ret *= 10;
		ret += (s[i] - '0');
	}
	memset(&value, 0, sizeof(value));
	g_value_init(&value, G_TYPE_LONG);
	g_value_set_long(&value, ret - data->inc);
	g_value_array_append(array, &value);
	g_value_unset(&value);
	return TRUE;
}

static gboolean
char_class_multi_check(wchar_t c, struct char_class_data *data)
{
	switch (c) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case ';':
			return TRUE;
		default:
			return FALSE;
	}
	return FALSE;
}
static void
char_class_multi_setup(const wchar_t *s, struct char_class_data *data, int inc)
{
	data->inc = inc;
	return;
}
static gboolean
char_class_multi_extract(const wchar_t *s, size_t length,
			 struct char_class_data *data, GValueArray *array)
{
	long ret = 0;
	size_t i;
	GValue value;
	memset(&value, 0, sizeof(value));
	g_value_init(&value, G_TYPE_LONG);
	for (i = 0; i < length; i++) {
		if (s[i] == ';') {
			g_value_set_long(&value, ret - data->inc);
			g_value_array_append(array, &value);
			ret = 0;
		} else {
			ret *= 10;
			ret += (s[i] - '0');
		}
	}
	g_value_set_long(&value, ret - data->inc);
	g_value_array_append(array, &value);
	g_value_unset(&value);
	return TRUE;
}

static gboolean
char_class_any_check(wchar_t c, struct char_class_data *data)
{
	return (c >= data->c) ? TRUE : FALSE;
}
static void
char_class_any_setup(const wchar_t *s, struct char_class_data *data, int inc)
{
	data->c = s[0] + inc;
	return;
}
static gboolean
char_class_any_extract(const wchar_t *s, size_t length,
		       struct char_class_data *data, GValueArray *array)
{
	long ret = 0;
	GValue value;
	ret = s[0] - data->c;
	memset(&value, 0, sizeof(value));
	g_value_init(&value, G_TYPE_LONG);
	g_value_set_long(&value, ret - data->inc);
	g_value_array_append(array, &value);
	g_value_unset(&value);
	return TRUE;
}

static gboolean
char_class_string_check(wchar_t c, struct char_class_data *data)
{
	return (c != data->c) ? TRUE : FALSE;
}
static void
char_class_string_setup(const wchar_t *s, struct char_class_data *data, int inc)
{
	data->c = s[0];
	return;
}
static size_t
xwcsnlen(const wchar_t *s, size_t length)
{
	size_t i;
	for (i = 0; i < length; i++) {
		if (s[i] == '\0') {
			return i;
		}
	}
	return length;
}
static gboolean
char_class_string_extract(const wchar_t *s, size_t length,
			  struct char_class_data *data, GValueArray *array)
{
	wchar_t *ret = NULL;
	size_t len;
	GValue value;

	len = xwcsnlen(s, length);
	ret = g_malloc0((len + 1) * sizeof(wchar_t));
	wcsncpy(ret, s, len);
#ifdef VTE_DEBUG_TRIE
	fprintf(stderr, "Extracting string `%ls'.\n", ret);
#endif
	memset(&value, 0, sizeof(value));

	g_value_init(&value, G_TYPE_POINTER);
	g_value_set_pointer(&value, ret);
	g_value_array_append(array, &value);
	g_value_unset(&value);

	return TRUE;
}

static wchar_t empty_wstring[] = {'\0'};
static wchar_t digit_wstring1[] = {'%', '2', '\0'};
static wchar_t digit_wstring2[] = {'%', 'd', '\0'};
static wchar_t any_wstring[] = {'%', '+', '\0'};
static wchar_t exact_wstring[] = {'%', '%', '\0'};
static wchar_t string_wstring[] = {'%', 's', '\0'};
static wchar_t multi_wstring[] = {'%', 'm', '\0'};

static struct char_class char_classes[] = {
	{exact, FALSE, empty_wstring, 0, 1,
	 char_class_exact_check,
	 char_class_exact_setup,
	 char_class_none_extract},
	{digit, TRUE, digit_wstring1, 2, 0,
	 char_class_digit_check,
	 char_class_digit_setup,
	 char_class_digit_extract},
	{digit, TRUE, digit_wstring2, 2, 0,
	 char_class_digit_check,
	 char_class_digit_setup,
	 char_class_digit_extract},
	{multi, TRUE, multi_wstring, 2, 0,
	 char_class_multi_check,
	 char_class_multi_setup,
	 char_class_multi_extract},
	{any, FALSE, any_wstring, 2, 1,
	 char_class_any_check,
	 char_class_any_setup,
	 char_class_any_extract},
	{exact, FALSE, exact_wstring, 2, 0,
	 char_class_exact_check,
	 char_class_percent_setup,
	 char_class_none_extract},
	{string, TRUE, string_wstring, 2, 0,
	 char_class_string_check,
	 char_class_string_setup,
	 char_class_string_extract},
};

/* Create a new trie. */
TRIE_MAYBE_STATIC struct vte_trie *
vte_trie_new(void)
{
	return g_malloc0(sizeof(struct vte_trie));
}

TRIE_MAYBE_STATIC void
vte_trie_free(struct vte_trie *trie)
{
	unsigned int i;
	for (i = 0; i < trie->trie_path_count; i++) {
		vte_trie_free(trie->trie_paths[i].trie);
	}
	if (trie->trie_path_count > 0) {
		g_free(trie->trie_paths);
	}
	g_free(trie);
}

/* Add the given pattern, with its own result string, to the trie, with the
 * given initial increment value. */
static void
vte_trie_addx(struct vte_trie *trie, wchar_t *pattern, size_t length,
	      const char *result, GQuark quark, int inc)
{
	unsigned long i;
	struct char_class *cclass = NULL;
	struct char_class_data data;
	wchar_t *code;
	size_t len = 0, ccount = 0;
	wchar_t inc_wstring[] = {'%', 'i', '\0'};

	/* The trivial case -- we'll just set the result at this node. */
	if (length == 0) {
		if (trie->result == NULL) {
			trie->quark = g_quark_from_string(result);
			trie->result = g_quark_to_string(trie->quark);
#ifdef VTE_DEBUG
		} else {
			g_warning("Duplicate (%s/%s)!", result, trie->result);
#endif
		}
		return;
	}

	/* If this part of the control sequence indicates incrementing a
	 * parameter, keep track of the incrementing, skip over the increment
	 * substring, and keep going. */
	if ((length >= 2) && (wcsncmp(pattern, inc_wstring, 2) == 0)) {
		vte_trie_addx(trie, pattern + 2, length - 2,
			      result, quark, inc + 1);
		return;
	}

	/* Now check for examples of character class specifiers, and use that
	 * to put this part of the pattern in a character class. */
	for (i = G_N_ELEMENTS(char_classes) - 1; i >= 0; i--) {
		len = char_classes[i].code_length;
		code = char_classes[i].code;
		ccount = char_classes[i].ccount;
		if ((len <= length) && (wcsncmp(pattern, code, len) == 0)) {
			cclass = &char_classes[i];
			break;
		}
	}
	g_assert(i >= 0);

	/* Initialize the data item using the data we have here. */
	memset(&data, 0, sizeof(data));
	cclass->setup(pattern + len, &data, inc);

	/* Hunt for a subtrie which matches this class / data pair. */
	for (i = 0; i < trie->trie_path_count; i++) {
		struct char_class_data *tdata;
		tdata =  &trie->trie_paths[i].data;
		if ((trie->trie_paths[i].cclass == cclass) &&
		    (memcmp(&data, tdata, sizeof(data)) == 0)) {
			/* It matches, so insert the rest of the pattern into
			 * this subtrie. */
			vte_trie_addx(trie->trie_paths[i].trie,
				      pattern + (len + ccount),
				      length - (len + ccount),
				      result,
				      quark,
				      inc);
			return;
		}
	}

	/* Add a new subtrie to contain the rest of this pattern. */
	trie->trie_path_count++;
	trie->trie_paths = g_realloc(trie->trie_paths,
				     trie->trie_path_count *
				     sizeof(trie->trie_paths[0]));
	i = trie->trie_path_count - 1;
	memset(&trie->trie_paths[i], 0, sizeof(trie->trie_paths[i]));
	trie->trie_paths[i].trie = vte_trie_new();
	cclass->setup(pattern + len, &trie->trie_paths[i].data, inc);
	trie->trie_paths[i].cclass = cclass;

	/* Now insert the rest of the pattern into the node we just created. */
	vte_trie_addx(trie->trie_paths[i].trie,
		      pattern + (len + ccount),
		      length - (len + ccount),
		      result,
		      quark,
		      inc);
}

/* Add the given pattern, with its own result string, to the trie. */
TRIE_MAYBE_STATIC void
vte_trie_add(struct vte_trie *trie, const char *pattern, size_t length,
	     const char *result, GQuark quark)
{
	mbstate_t state;
	char *wpattern, *wpattern_end, *tpattern;
	iconv_t conv;
	size_t wlength;

	g_return_if_fail(trie != NULL);
	g_return_if_fail(pattern != NULL);
	g_return_if_fail(length > 0);
	g_return_if_fail(result != NULL);
	if (quark == 0) {
		quark = g_quark_from_string(result);
	}

	wlength = sizeof(wchar_t) * (length + 1);
	wpattern = wpattern_end = g_malloc0(wlength + 1);
	memset(&state, 0, sizeof(state));

	conv = iconv_open("WCHAR_T", "UTF-8");
	if (conv != NULL) {
		tpattern = (char*)pattern;
		iconv(conv, &tpattern, &length, &wpattern_end, &wlength);
		if (length == 0) {
			wlength = (wpattern_end - wpattern) / sizeof(wchar_t);
			vte_trie_addx(trie, (wchar_t*)wpattern, wlength,
				      result, quark, 0);
		}
		iconv_close(conv);
	}

	g_free(wpattern);
}

/* Check if the given pattern matches part of the given trie, returning an
 * empty string on a partial initial match, a NULL if there's no match in the
 * works, and the result string if we have an exact match. */
static const char *
vte_trie_matchx(struct vte_trie *trie, const wchar_t *pattern, size_t length,
		const char **res, const wchar_t **consumed,
		GQuark *quark, GValueArray *array)
{
	unsigned int i;
	const char *hres;
	enum cclass cc;
	const char *best = NULL;
	GValueArray *bestarray = NULL;
	GQuark bestquark = 0;
	const wchar_t *bestconsumed = pattern;

	/* Make sure that attempting to save output values doesn't kill us. */
	if (res == NULL) {
		res = &hres;
	}

	/* Trivial cases.  We've matched the entire pattern, or we're out of
	 * pattern to match. */
	if (length <= 0) {
		if (trie->result) {
			*res = trie->result;
			*quark = trie->quark;
			*consumed = pattern;
			return *res;
		} else {
			if (trie->trie_path_count > 0) {
				*res = "";
				*quark = g_quark_from_static_string("");
				*consumed = pattern;
				return *res;
			} else {
				*res = NULL;
				*quark = 0;
				*consumed = pattern;
				return *res;
			}
		}
	}

	/* Now figure out which (if any) subtrees to search.  First, see
	 * which character class this character matches. */
	for (cc = exact; cc < invalid; cc++)
	for (i = 0; i < trie->trie_path_count; i++) {
		struct vte_trie *subtrie = trie->trie_paths[i].trie;
		struct char_class *cclass = trie->trie_paths[i].cclass;
		struct char_class_data *data = &trie->trie_paths[i].data;
		if (trie->trie_paths[i].cclass->type == cc) {
			/* If it matches this character class... */
			if (cclass->check(pattern[0], data)) {
				const wchar_t *prospect = pattern + 1;
				const char *tmp;
				GQuark tmpquark = 0;
				GValueArray *tmparray;
				/* Move past characters which might match this
				 * part of the string... */
				while (cclass->multiple &&
				       ((prospect - pattern) < length) &&
				       cclass->check(prospect[0], data)) {
					prospect++;
				}
				/* ... see if there's a parameter here, ... */
				tmparray = g_value_array_new(0);
				cclass->extract(pattern,
						prospect - pattern,
						data,
						tmparray);
				/* ... and check if the subtree matches the
				 * rest of the input string.  Any parameters
				 * further on will be appended to the array. */
				vte_trie_matchx(subtrie,
						prospect,
						length - (prospect - pattern),
						&tmp,
						consumed,
						&tmpquark,
						tmparray);
				/* If it's a better match than any we've seen
				 * so far, call it the "best so far". */
				if ((best == NULL) ||
				    ((best[0] == '\0') &&
				     (tmp != NULL) &&
				     (tmp[0] != '\0'))) {
					best = tmp;
					if (bestarray != NULL) {
						g_value_array_free(bestarray);
					}
					bestarray = tmparray;
					bestquark = tmpquark;
					bestconsumed = *consumed;
				} else {
					g_value_array_free(tmparray);
					tmparray = NULL;
				}
			}
		}
	}

	/* We're done searching.  Copy out any parameters we picked up. */
	if (bestarray != NULL) {
		for (i = 0; i < bestarray->n_values; i++) {
			g_value_array_append(array,
					     g_value_array_get_nth(bestarray,
						     		   i));
		}
		g_value_array_free(bestarray);
	}
#if 0
	g_print("`%s' ", best);
	dump_array(array);
#endif
	*quark = bestquark;
	*res = best;
	*consumed = bestconsumed;
	return *res;
}

/* Check if the given pattern matches part of the given trie, returning an
 * empty string on a partial initial match, a NULL if there's no match in the
 * works, and the result string if we have an exact match. */
TRIE_MAYBE_STATIC const char *
vte_trie_match(struct vte_trie *trie, const wchar_t *pattern, size_t length,
	       const char **res, const wchar_t **consumed,
	       GQuark *quark, GValueArray **array)
{
	const char *ret = NULL;
	GQuark tmpquark;
	GValueArray *valuearray;
	GValue *value;
	const wchar_t *dummyconsumed;
	gpointer ptr;
	int i;

	valuearray = g_value_array_new(0);
	if (quark == NULL) {
		quark = &tmpquark;
	}
	*quark = 0;

	if (consumed == NULL) {
		consumed = &dummyconsumed;
	}
	*consumed = pattern;

	ret = vte_trie_matchx(trie, pattern, length, res, consumed,
			      quark, valuearray);

	if (((ret == NULL) || (ret[0] == '\0')) || (valuearray->n_values == 0)){
		if (valuearray != NULL) {
			for (i = 0; i < valuearray->n_values; i++) {
				value = g_value_array_get_nth(valuearray, i);
				if (G_VALUE_HOLDS_POINTER(value)) {
					ptr = g_value_get_pointer(value);
					if (ptr != NULL) {
						g_free(ptr);
					}
				}
			}
			g_value_array_free(valuearray);
		}
		*array = NULL;
	} else {
		*array = valuearray;
	}

	return ret;
}

/* Print the next layer of the trie, indented by length spaces. */
static void
vte_trie_printx(struct vte_trie *trie, const char *previous)
{
	unsigned int i;
	char buf[LINE_MAX];

	for (i = 0; i < trie->trie_path_count; i++) {
		memset(buf, '\0', sizeof(buf));
		snprintf(buf, sizeof(buf), "%s", previous);
		switch (trie->trie_paths[i].cclass->type) {
			case exact:
				if (trie->trie_paths[i].data.c < 32) {
					snprintf(buf + strlen(buf),
						 sizeof(buf) - strlen(buf),
						 "^%lc",
						 (wint_t)trie->trie_paths[i].data.c +
						 64);
				} else {
					snprintf(buf + strlen(buf),
						 sizeof(buf) - strlen(buf),
						 "%lc",
						 (wint_t)trie->trie_paths[i].data.c);
				}
				break;
			case digit:
				snprintf(buf + strlen(buf),
					 sizeof(buf) - strlen(buf),
					 "{num+%d}",
					 trie->trie_paths[i].data.inc);
				break;
			case multi:
				snprintf(buf + strlen(buf),
					 sizeof(buf) - strlen(buf),
					 "{multinum+%d}",
					 trie->trie_paths[i].data.inc);
				break;
			case any:
				snprintf(buf + strlen(buf),
					 sizeof(buf) - strlen(buf),
					 "{char+`%lc'}",
					 (wint_t)trie->trie_paths[i].data.c);
				break;
			case string:
				snprintf(buf + strlen(buf),
					 sizeof(buf) - strlen(buf),
					 "{string}");
				break;
			case invalid:
				break;
		}
		if (trie->trie_paths[i].trie->result != NULL) {
			g_print("%s = `%s'\n", buf,
			        trie->trie_paths[i].trie->result);
		}
		vte_trie_printx(trie->trie_paths[i].trie, buf);
	}
}

/* Print the trie. */
TRIE_MAYBE_STATIC void
vte_trie_print(struct vte_trie *trie)
{
	vte_trie_printx(trie, "");
}

#ifdef TRIE_MAIN
static void
dump_array(GValueArray *array)
{
	unsigned int i;
	if (array != NULL) {
		g_print("args = {");
		for (i = 0; i < array->n_values; i++) {
			GValue *value;
			value = g_value_array_get_nth(array, i);
			if (i > 0) {
				g_print(", ");
			}
			if (G_VALUE_HOLDS_LONG(value)) {
				g_print("%ld", g_value_get_long(value));
			}
			if (G_VALUE_HOLDS_STRING(value)) {
				g_print("`%s'", g_value_get_string(value));
			}
			if (G_VALUE_HOLDS_POINTER(value)) {
				printf("`%ls'",
				       (wchar_t*)g_value_get_pointer(value));
			}
		}
		g_print("}\n");
	}
}

static void
convert_mbstowcs(const char *i, size_t ilen,
		 wchar_t *o, size_t *olen, size_t max_olen)
{
	iconv_t conv;
	size_t outlen;
	conv = iconv_open("WCHAR_T", "UTF-8");
	if (conv != NULL) {
		memset(o, 0, max_olen);
		outlen = max_olen;
		iconv(conv, (char**)&i, &ilen, (char**)&o, &outlen);
		iconv_close(conv);
	}
	if (olen) {
		*olen = (max_olen - outlen) / sizeof(wchar_t);
	}
}

int
main(int argc, char **argv)
{
	struct vte_trie *trie;
	GValueArray *array = NULL;
	GQuark quark;
	wchar_t buf[LINE_MAX];
	const wchar_t *consumed;
	size_t buflen;

	g_type_init();
	trie = vte_trie_new();

	vte_trie_add(trie, "abcdef", 6, "abcdef",
		     g_quark_from_string("abcdef"));
	vte_trie_add(trie, "abcde", 5, "abcde",
		     g_quark_from_string("abcde"));
	vte_trie_add(trie, "abcdeg", 6, "abcdeg",
		     g_quark_from_string("abcdeg"));
	vte_trie_add(trie, "abc%+Aeg", 8, "abc%+Aeg",
		     g_quark_from_string("abc%+Aeg"));
	vte_trie_add(trie, "abc%deg", 7, "abc%deg",
		     g_quark_from_string("abc%deg"));
	vte_trie_add(trie, "abc%%eg", 7, "abc%%eg",
		     g_quark_from_string("abc%%eg"));
	vte_trie_add(trie, "abc%%%i%deg", 11, "abc%%%i%deg",
		     g_quark_from_string("abc%%%i%deg"));
	vte_trie_add(trie, "<esc>[%i%d;%dH", 14, "vtmatch",
		     g_quark_from_string("vtmatch"));
	vte_trie_add(trie, "<esc>[%i%mL", 11, "multimatch",
		     g_quark_from_string("multimatch"));
	vte_trie_add(trie, "<esc>]2;%sh", 11, "decset-title",
		     g_quark_from_string("decset-title"));
	vte_trie_print(trie);
	g_print("\n");

	quark = 0;
	convert_mbstowcs("abc", 3, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abc",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abcdef", 6, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abcdef",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abcde", 5, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abcde",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abcdeg", 6, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abcdeg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abc%deg", 7, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abc%deg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abc10eg", 7, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abc10eg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abc%eg", 6, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abc%eg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abc%10eg", 8, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abc%10eg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("abcBeg", 6, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "abcBeg",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("<esc>[25;26H", 12, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>[25;26H",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("<esc>[25;2", 10, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>[25;2",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
	}

	quark = 0;
	convert_mbstowcs("<esc>[25;26L", 12, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>[25;26L",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
	}

	quark = 0;
	convert_mbstowcs("<esc>]2;WoofWoofh", 17, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>]2;WoofWoofh",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("<esc>]2;WoofWoofh<esc>]2;WoofWoofh", 34,
			 buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>]2;WoofWoofh<esc>]2;WoofWoofh",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	quark = 0;
	convert_mbstowcs("<esc>]2;WoofWoofhfoo", 20, buf, &buflen, sizeof(buf));
	g_print("`%s' = `%s'\n", "<esc>]2;WoofWoofhfoo",
	        vte_trie_match(trie, buf, buflen,
			       NULL, &consumed, &quark, &array));
	g_print("=> `%s' (%d)\n", g_quark_to_string(quark), consumed - buf);
	if (array != NULL) {
		dump_array(array);
		g_value_array_free(array);
		array = NULL;
	}

	vte_trie_free(trie);

	return 0;
}
#endif
