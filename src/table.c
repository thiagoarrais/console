/*
 * Copyright (C) 2002 Red Hat, Inc.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include "debug.h"
#include "iso2022.h"
#include "table.h"

/* Table info. */
#define VTE_TABLE_MAX_LITERAL 160
#define _vte_table_map_literal(__c) \
	(((__c) < (VTE_TABLE_MAX_LITERAL)) ? (__c) : 0)
#define _vte_table_is_numeric(__c) \
	((((__c) >= '0') && ((__c) <= '9')) || ((__c) == ';'))
enum _vte_table_specials {
	_vte_table_string = VTE_TABLE_MAX_LITERAL,
	_vte_table_number,
	_vte_table_max
};
struct _vte_table {
	GQuark resultq;
	const char *result;
	unsigned char *original;
	gssize original_length;
	int increment;
	struct _vte_table *table[_vte_table_max];
};

/* Argument info. */
enum _vte_table_argtype {
	_vte_table_arg_number,
	_vte_table_arg_string,
	_vte_table_arg_char
};
struct _vte_table_arginfo {
	enum _vte_table_argtype type;
	const gunichar *start;
	gssize length;
};

/* Create an empty, one-level table. */
struct _vte_table *
_vte_table_new(void)
{
	return g_malloc0(sizeof(struct _vte_table));
}

/* Free a table. */
void
_vte_table_free(struct _vte_table *table)
{
	unsigned int i;
	for (i = 0; i < G_N_ELEMENTS(table->table); i++) {
		if (table->table[i] != NULL) {
			_vte_table_free(table->table[i]);
			table->table[i] = NULL;
		}
	}
	if (table->original_length == 0) {
		g_assert(table->original == NULL);
	} else {
		g_assert(table->original != NULL);
	}
	if (table->original != NULL) {
		table->original_length = 0;
		g_free(table->original);
		table->original = NULL;
	}
	g_free(table);
}

/* Add a string to the tree with the given increment value. */
static void
_vte_table_addi(struct _vte_table *table,
	        const unsigned char *original, gssize original_length,
	        const char *pattern, gssize length,
	        const char *result, GQuark quark, int inc)
{
	int i;
	guint8 check;
	struct _vte_table *subtable;

	/* If this is the terminal node, set the result. */
	if (length == 0) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_PARSE)) {
			if (table->result != NULL) {
				g_warning("`%s' and `%s' are indistinguishable",
					  table->result, result);
			}
		}
#endif
		table->resultq = g_quark_from_string(result);
		table->result = g_quark_to_string(table->resultq);
		if (table->original != NULL) {
			g_free(table->original);
		}
		table->original = g_malloc(original_length);
		table->original_length = original_length;
		memcpy(table->original, original, original_length);
		table->increment = inc;
		return;
	}

	/* All of the interesting arguments begin with '%'. */
	if (pattern[0] == '%') {
		/* Handle an increment. */
		if (pattern[1] == 'i') {
			_vte_table_addi(table, original, original_length,
				        pattern + 2, length - 2,
				        result, quark, inc + 1);
			return;
		}

		/* Handle numeric parameters. */
		if ((pattern[1] == 'd') ||
		    (pattern[1] == '2') ||
		    (pattern[1] == 'm')) {
			/* Create a new subtable. */
			if (table->table[_vte_table_number] == NULL) {
				subtable = _vte_table_new();
				table->table[_vte_table_number] = subtable;
			} else {
				subtable = table->table[_vte_table_number];
			}
			/* Add the rest of the string to the subtable. */
			_vte_table_addi(subtable, original, original_length,
				        pattern + 2, length - 2,
				        result, quark, inc);
			return;
		}

		/* Handle string parameters. */
		if (pattern[1] == 's') {
			/* It must have a terminator. */
			g_assert(length >= 3);
			/* Create a new subtable. */
			if (table->table[_vte_table_string] == NULL) {
				subtable = _vte_table_new();
				table->table[_vte_table_string] = subtable;
			} else {
				subtable = table->table[_vte_table_string];
			}
			/* Add the rest of the string to the subtable. */
			_vte_table_addi(subtable, original, original_length,
				        pattern + 2, length - 2,
				        result, quark, inc);
			return;
		}

		/* Handle an escaped '%'. */
		if (pattern[1] == '%') {
			/* Create a new subtable. */
			if (table->table['%'] == NULL) {
				subtable = _vte_table_new();
				table->table['%'] = subtable;
			} else {
				subtable = table->table['%'];
			}
			/* Add the rest of the string to the subtable. */
			_vte_table_addi(subtable, original, original_length,
				        pattern + 2, length - 2,
				        result, quark, inc);
			return;
		}

		/* Handle a parameter character. */
		if (pattern[1] == '+') {
			/* It must have an addend. */
			g_assert(length >= 3);
			/* Fill in all of the table entries above the given
			 * character value. */
			for (i = pattern[2]; i < VTE_TABLE_MAX_LITERAL; i++) {
				/* Create a new subtable. */
				if (table->table[i] == NULL) {
					subtable = _vte_table_new();
					table->table[i] = subtable;
				} else {
					subtable = table->table[i];
				}
				/* Add the rest of the string to the subtable. */
				_vte_table_addi(subtable,
					        original, original_length,
					        pattern + 3, length - 3,
					        result, quark, inc);
			}
			/* Also add a subtable for higher characters. */
			if (table->table[0] == NULL) {
				subtable = _vte_table_new();
				table->table[0] = subtable;
			} else {
				subtable = table->table[0];
			}
			/* Add the rest of the string to the subtable. */
			_vte_table_addi(subtable, original, original_length,
				        pattern + 3, length - 3,
				        result, quark, inc);
			return;
		}
	}

	/* A literal (or an unescaped '%', which is also a literal). */
	check = (guint8) pattern[0];
	g_assert(check < VTE_TABLE_MAX_LITERAL);
	if (table->table[check] == NULL) {
		subtable = _vte_table_new();
		table->table[check] = subtable;
	} else {
		subtable = table->table[check];
	}
	/* Add the rest of the string to the subtable. */
	_vte_table_addi(subtable, original, original_length,
		        pattern + 1, length - 1,
		        result, quark, inc);
}

/* Add a string to the matching tree. */
void
_vte_table_add(struct _vte_table *table,
	       const char *pattern, gssize length,
	       const char *result, GQuark quark)
{
	char *pattern_copy, *p;
	pattern_copy = g_strndup(pattern, length);
	/* Collapse as many numeric parameters as possible into '%m'. */
	while ((p = strstr(pattern_copy, "%d")) != NULL) {
		memcpy(p, "%m", 2);
	}
	while ((p = strstr(pattern_copy, "%3")) != NULL) {
		memcpy(p, "%m", 2);
	}
	while ((p = strstr(pattern_copy, "%2")) != NULL) {
		memcpy(p, "%m", 2);
	}
	while ((p = strstr(pattern_copy, "%m;%m")) != NULL) {
		memmove(p, p + 3, length - 3 - (p - pattern_copy));
		length -= 3;
	}
	_vte_table_addi(table, pattern_copy, length, pattern_copy, length,
		        result, quark, 0);
	g_free(pattern_copy);
}

/* Match a string in a subtree. */
static const char *
_vte_table_matchi(struct _vte_table *table,
		  const gunichar *pattern, gssize length,
		  const char **res, const gunichar **consumed, GQuark *quark,
		  unsigned char **original, gssize *original_length,
		  GList **params)
{
	int i = 0;
	struct _vte_table *subtable = NULL;
	struct _vte_table_arginfo *arginfo;

	/* Check if this is a result node. */
	if (table->result != NULL) {
		*consumed = pattern;
		*original = table->original;
		*original_length = table->original_length;
		*res = table->result;
		*quark = table->resultq;
		return table->result;
	}

	/* If we're out of data, but we still have children, return the empty
	 * string. */
	if ((length == 0) && (table != NULL)) {
		*consumed = pattern;
		return "";
	}

	/* Check if this node has a string disposition. */
	if (table->table[_vte_table_string] != NULL) {
		/* Iterate over all non-terminator values. */
		subtable = table->table[_vte_table_string];
		for (i = 0; i < length; i++) {
			if (subtable->table[_vte_table_map_literal(pattern[i])] != NULL) {
				break;
			}
		}
		/* Save the parameter info. */
		arginfo = g_malloc(sizeof(struct _vte_table_arginfo));
		arginfo->type = _vte_table_arg_string;
		arginfo->start = pattern;
		arginfo->length = i;
		*params = g_list_append(*params, arginfo);
		/* Continue. */
		return _vte_table_matchi(subtable, pattern + i, length - i,
					 res, consumed, quark,
					 original, original_length, params);
	}

	/* Check if this could be a number. */
	if ((_vte_table_is_numeric(pattern[0])) &&
	    (table->table[_vte_table_number] != NULL)) {
		subtable = table->table[_vte_table_number];
		/* Iterate over all numeric characters. */
		for (i = 0; i < length; i++) {
			if (!_vte_table_is_numeric(pattern[i])) {
				break;
			}
		}
		/* Save the parameter info. */
		arginfo = g_malloc(sizeof(struct _vte_table_arginfo));
		arginfo->type = _vte_table_arg_number;
		arginfo->start = pattern;
		arginfo->length = i;
		*params = g_list_append(*params, arginfo);
		/* Continue. */
		return _vte_table_matchi(subtable, pattern + i, length - i,
					 res, consumed, quark,
					 original, original_length, params);
	}

	/* Check for an exact match. */
	if (table->table[_vte_table_map_literal(pattern[0])] != NULL) {
		subtable = table->table[_vte_table_map_literal(pattern[0])];
		/* Save the parameter info. */
		arginfo = g_malloc(sizeof(struct _vte_table_arginfo));
		arginfo->type = _vte_table_arg_char;
		arginfo->start = pattern;
		arginfo->length = 1;
		*params = g_list_append(*params, arginfo);
		/* Continue. */
		return _vte_table_matchi(subtable, pattern + 1, length - 1,
					 res, consumed, quark,
					 original, original_length, params);
	}

	/* If there's nothing else to do, then we can't go on.  Keep track of
	 * where we are. */
	*consumed = pattern;
	return NULL;
}

static void
_vte_table_extract_number(GValueArray **array,
			  struct _vte_table_arginfo *arginfo, long increment)
{
	GValue value = {0,};
	GString *tmp;
	char **vals;
	int i;

	tmp = g_string_new("");
	for (i = 0; i < arginfo->length; i++) {
		tmp = g_string_append_unichar(tmp, arginfo->start[i]);
	}

	vals = g_strsplit(tmp->str, ";", -1);

	if (vals != NULL) {
		g_value_init(&value, G_TYPE_LONG);

		for (i = 0; vals[i] != NULL; i++) {
			if (*array == NULL) {
				*array = g_value_array_new(1);
			}
			g_value_set_long(&value, atol(vals[i]));
			g_value_array_append(*array, &value);
		}

		g_strfreev(vals);

		g_value_unset(&value);
	}

	g_string_free(tmp, TRUE);
}

static void
_vte_table_extract_string(GValueArray **array,
			  struct _vte_table_arginfo *arginfo)
{
	GValue value = {0,};
	gunichar *ptr;
	int i;

	ptr = g_malloc(sizeof(gunichar) * (arginfo->length + 1));
	memcpy(ptr, arginfo->start, (arginfo->length * sizeof(gunichar)));
	for (i = 0; i < arginfo->length; i++) {
		ptr[i] &= ~(VTE_ISO2022_WIDTH_MASK);
	}
	ptr[arginfo->length] = '\0';
	g_value_init(&value, G_TYPE_POINTER);
	g_value_set_pointer(&value, ptr);

	if (*array == NULL) {
		*array = g_value_array_new(1);
	}
	g_value_array_append(*array, &value);
	g_value_unset(&value);
}

static void
_vte_table_extract_char(GValueArray **array,
		        struct _vte_table_arginfo *arginfo, long increment)
{
	GValue value = {0,};

	g_value_init(&value, G_TYPE_LONG);
	g_value_set_long(&value, *(arginfo->start) - increment);

	if (*array == NULL) {
		*array = g_value_array_new(1);
	}
	g_value_array_append(*array, &value);
	g_value_unset(&value);
}

/* Check if a string matches something in the tree. */
const char *
_vte_table_match(struct _vte_table *table,
		 const gunichar *pattern, gssize length,
		 const char **res, const gunichar **consumed,
		 GQuark *quark, GValueArray **array)
{
	struct _vte_table *head;
	const gunichar *dummy_consumed = NULL;
	const char *dummy_res = NULL;
	GQuark dummy_quark = 0;
	GValueArray *dummy_array = NULL;
	const char *ret = NULL;
	unsigned char *original = NULL, *p = NULL;
	gssize original_length;
	GList *params = NULL, *tmp;
	long increment = 0;
	int i;
	struct _vte_table_arginfo *arginfo;

	/* Clean up extracted parameters. */
	if (res == NULL) {
		res = &dummy_res;
	}
	*res = NULL;
	if (consumed == NULL) {
		consumed = &dummy_consumed;
	}
	*consumed = pattern;
	if (quark == NULL) {
		quark = &dummy_quark;
	}
	*quark = 0;
	if (array == NULL) {
		array = &dummy_array;
	}
	*array = NULL;

	/* Provide a fast path for the usual "not a sequence" cases. */
	if (length == 0) {
		return NULL;
	}
	if (pattern == NULL) {
		return NULL;
	}

	/* If there's no literal path, and no generic path, and the numeric
	 * path isn't available, then it's not a sequence, either. */
	if (table->table[_vte_table_map_literal(pattern[0])] == NULL) {
		if (table->table[_vte_table_string] == NULL) {
			if (!(_vte_table_is_numeric(pattern[0])) ||
			    (table->table[_vte_table_number] == NULL)) {
				/* No match. */
				return NULL;
			}
		}
	}

	/* Check for a literal match. */
	for (i = 0, head = table; (i < length) && (head != NULL); i++) {
		head = head->table[_vte_table_map_literal(pattern[i])];
	}
	if ((head != NULL) && (head->result != NULL)) {
		/* Got a literal match. */
		*consumed = pattern + i;
		*res = head->result;
		*quark = head->resultq;
		return *res;
	}

	/* Check for a pattern match. */
	ret = _vte_table_matchi(table, pattern, length,
			        res, consumed, quark,
			        &original, &original_length,
			        &params);
	*res = ret;

	/* If we got a match, extract the parameters. */
	if ((ret != NULL) && (strlen(ret) > 0) && (array != &dummy_array)) {
		tmp = params;
		g_assert(original != NULL);
		p = original;
		while (p < original + original_length) {
			/* All of the interesting arguments begin with '%'. */
			if (p[0] == '%') {
				/* Handle an increment. */
				if (p[1] == 'i') {
					increment++;
					p += 2;
					continue;
				}
				/* Handle an escaped '%'. */
				if (p[1] == '%') {
					tmp = g_list_next(tmp);
					p += 2;
					continue;
				}
				/* Handle numeric parameters. */
				if ((p[1] == 'd') ||
				    (p[1] == '2') ||
				    (p[1] == 'm')) {
					arginfo = tmp->data;
					_vte_table_extract_number(array,
								  arginfo,
								  increment);
					tmp = g_list_next(tmp);
					p += 2;
					continue;
				}
				/* Handle string parameters. */
				if (p[1] == 's') {
					arginfo = tmp->data;
					_vte_table_extract_string(array,
								  arginfo);
					tmp = g_list_next(tmp);
					p += 2;
					continue;
				}
				/* Handle a parameter character. */
				if (p[1] == '+') {
					arginfo = tmp->data;
					_vte_table_extract_char(array,
							        arginfo,
							        p[2]);
					tmp = g_list_next(tmp);
					p += 3;
					continue;
				}
				g_assert_not_reached();
			} else {
				/* Literal. */
				tmp = g_list_next(tmp);
				p++;
				continue;
			}
		}
	}

	/* Clean up extracted parameters. */
	if (params != NULL) {
		for (tmp = params; tmp != NULL; tmp = g_list_next(tmp)) {
			g_free(tmp->data);
		}
		g_list_free(params);
	}

	return ret;
}

static void
_vte_table_printi(struct _vte_table *table, const char *lead, int *count)
{
	unsigned int i;
	char *newlead = NULL;

	(*count)++;

	/* Result? */
	if (table->result != NULL) {
		fprintf(stderr, "%s = `%s'(%d)\n", lead,
		        table->result, table->increment);
	}

	/* Literal? */
	for (i = 1; i < VTE_TABLE_MAX_LITERAL; i++) {
		if (table->table[i] != NULL) {
			if (i < 32) {
				newlead = g_strdup_printf("%s^%c", lead,
							  i + 64);
			} else {
				newlead = g_strdup_printf("%s%c", lead, i);
			}
			_vte_table_printi(table->table[i], newlead, count);
			g_free(newlead);
		}
	}

	/* String? */
	if (table->table[_vte_table_string] != NULL) {
		newlead = g_strdup_printf("%s{string}", lead);
		_vte_table_printi(table->table[_vte_table_string],
				  newlead, count);
		g_free(newlead);
	}

	/* Number(+)? */
	if (table->table[_vte_table_number] != NULL) {
		newlead = g_strdup_printf("%s{number}", lead);
		_vte_table_printi(table->table[_vte_table_number],
				  newlead, count);
		g_free(newlead);
	}
}

/* Dump out the contents of a tree. */
void
_vte_table_print(struct _vte_table *table)
{
	int count = 0;
	_vte_table_printi(table, "", &count);
	fprintf(stderr, "%d nodes = %ld bytes.\n",
	        count, (long) count * sizeof(struct _vte_table));
}

#ifdef TABLE_MAIN
/* Return an escaped version of a string suitable for printing. */
static char *
escape(const char *p)
{
	char *tmp;
	GString *ret;
	int i;
	guint8 check;
	ret = g_string_new("");
	for (i = 0; p[i] != '\0'; i++) {
		tmp = NULL;
		check = p[i];
		if (check < 32) {
			tmp = g_strdup_printf("^%c", check + 64);
		} else
		if (check >= 0x80) {
			tmp = g_strdup_printf("{0x%x}", check);
		} else {
			tmp = g_strdup_printf("%c", check);
		}
		g_string_append(ret, tmp);
		g_free(tmp);
	}
	return g_string_free(ret, FALSE);
}

/* Spread out a narrow ASCII string into a wide-character string. */
static gunichar *
make_wide(const char *p)
{
	gunichar *ret;
	guint8 check;
	int i;
	ret = g_malloc((strlen(p) + 1) * sizeof(gunichar));
	for (i = 0; p[i] != 0; i++) {
		check = (guint8) p[i];
		g_assert(check < 0x80);
		ret[i] = check;
	}
	ret[i] = '\0';
	return ret;
}

/* Print the contents of a GValueArray. */
static void
print_array(GValueArray *array)
{
	int i;
	GValue *value;
	if (array != NULL) {
		printf(" (");
		for (i = 0; i < array->n_values; i++) {
			value = g_value_array_get_nth(array, i);
			if (i > 0) {
				printf(", ");
			}
			if (G_VALUE_HOLDS_LONG(value)) {
				printf("%ld", g_value_get_long(value));
			} else
			if (G_VALUE_HOLDS_STRING(value)) {
				printf("\"%s\"", g_value_get_string(value));
			} else
			if (G_VALUE_HOLDS_POINTER(value)) {
				printf("\"%ls\"",
				       (wchar_t*) g_value_get_pointer(value));
				g_free(g_value_get_pointer(value));
			}
		}
		printf(")");
		g_value_array_free(array);
	}
}

int
main(int argc, char **argv)
{
	struct _vte_table *table;
	int i;
	const char *patterns[] = {
		"ABCD",
		"ABCDEF",
		"]2;foo",
		"]3;foo",
		"]3;fook",
		"[3;foo",
		"[3;3m",
		"[3;3mk",
		"[3;3hk",
		"[3;3h",
		"]3;3h",
		"[3;3k",
		"[3;3kj",
		"s",
	};
	const char *result, *p;
	const gunichar *consumed;
	char *tmp;
	gunichar *pattern;
	GQuark quark;
	GValueArray *array;
	g_type_init();
	table = _vte_table_new();
	_vte_table_add(table, "ABCDEFG", 7, "ABCDEFG", 0);
	_vte_table_add(table, "ABCD", 4, "ABCD", 0);
	_vte_table_add(table, "ABCDEFH", 7, "ABCDEFH", 0);
	_vte_table_add(table, "ACDEFH", 6, "ACDEFH", 0);
	_vte_table_add(table, "ACDEF%sJ", 8, "ACDEF%sJ", 0);
	_vte_table_add(table, "ACDEF%i%mJ", 10, "ACDEF%dJ", 0);
	_vte_table_add(table, "[%mh", 5, "move-cursor", 0);
	_vte_table_add(table, "[%d;%d;%dm", 11, "set-graphic-rendition", 0);
	_vte_table_add(table, "]3;%s", 7, "set-icon-title", 0);
	_vte_table_add(table, "]4;%s", 7, "set-window-title", 0);
	printf("Table contents:\n");
	_vte_table_print(table);
	printf("\nTable matches:\n");
	for (i = 0; i < G_N_ELEMENTS(patterns); i++) {
		p = patterns[i];
		pattern = make_wide(p);
		array = NULL;
		_vte_table_match(table, pattern, strlen(p),
				 &result, &consumed, &quark, &array);
		tmp = escape(p);
		printf("`%s' => `%s'", tmp, (result ? result : "(NULL)"));
		g_free(tmp);
		print_array(array);
		printf(" (%d chars)\n", consumed ? consumed - pattern : 0);
		g_free(pattern);
	}
	_vte_table_free(table);
	return 0;
}
#endif
