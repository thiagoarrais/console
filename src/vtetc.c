/*
 * Copyright © 2006 Ryan Lortie <desrt@desrt.ca>
 * based on code © 2000-2002 Red Hat, Inc. and others.
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

#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include "vtetc.h"

/*
 * --- a termcap file is represented by a simple tree ---
 */
typedef struct _vte_termcap
{
  const char *start;
  GTree *tree;
  const char *end;
} VteTermcap;

/* a special strcmp that treats any character in
 * its third argument (plus '\0') as end of
 * string.
 *
 *  we have to be a little bit careful, however.
 *  note that '=' < 'A' < '|' and consider the
 *  following three strings with "=|" given as
 *  the 3rd argument:
 *
 *  foo=
 *  fooA
 *  foo|
 *
 *  if we just do the normal *a - *b thing when
 *  the strings don't match then we will find
 *
 *      "foo=" < "fooA"
 *
 *  and
 *
 *      "foo|" > "fooA"
 *
 *  but of course,
 *
 *      "foo=" == "foo|"
 *
 *  which means that our ordering isn't proper
 *  which may cause odd things to happen inside of
 *  the tree.  for this reason, all of the
 *  terminating characters are treated as '\0' for
 *  purposes of deciding greater or less than.
 *
 *  note: if anything in this file should be
 *        micro-optimised then it is probably
 *        this function!
 */
static int
_vte_termcap_strcmp (const char *a,
                     const char *b,
                     const char *enders)
{
  /* note: strchr on '\0' returns the
   * end of the string (not NULL)
   */

  while (!strchr (enders, *a))
  {
    if (*a != *b)
    {
      /* we're in the loop so we know that *a is not a terminator.
       * but maybe *b is?
       */
      if (strchr (enders, *b))
        return *a - '\0';
      else
        /* b is not a terminator.  proceed normally. */
        return *a - *b;
    }
    a++;
    b++;
  }

  /* *a is a terminator for sure, but maybe *b is too. */
  if (strchr (enders, *b))
    /* b is too, so we have a match. */
    return 0;

  /* else, since *a is a terminator character and *b is not, *a is
   * less than *b. */
  return -1;
}

/*
 * --- routines for searching the tree ---
 */
static const char *
_vte_termcap_find_start (VteTermcap *termcap,
                         const char *tname,
                         const char *cap)
{
  const char *contents;
  const char *start;
  char *chain;

  /* find the terminal */
  contents = g_tree_lookup (termcap->tree, tname);

  if (contents == NULL)
    return NULL;

  start = contents;
  while (contents != termcap->end)
  {
    if (*contents == '\\' &&
        contents + 1 != termcap->end &&
        contents[1] == '\n')
    {
      /* we've hit \ at the end of a line.  skip. */
      contents++;
    }
    else if (*contents == ':' || *contents == '\n')
    {
      if (!_vte_termcap_strcmp (start, cap, "=#:\n"))
        return start;

      start = contents + 1;
      if (*contents == '\n')
        break;
    }

    contents++;
  }

  /* else, try to find it in the term listed in our 'tc' entry.
   * obviously, don't recurse when we're trying to find "tc"
   * itself else we infinite loop.
   */
  if (!strcmp (cap, "tc"))
    return NULL;

  chain = _vte_termcap_find_string (termcap, tname, "tc");
  if (chain[0])
    start = _vte_termcap_find_start (termcap, chain, cap);
  g_free (chain);

  return start;
}

static int
_vte_termcap_unescape_string(const char *string, char *result)
{
  int value = -1;
  int length = 0;

  while (TRUE)
  {
    /* Each time through the loop puts a value into 'value' if it
     * wants to have it written into the string.  We do the write
     * here because it is complicated (check for NULL result, etc)
     *
     * We finish and return the length whenb value is 0.
     */
    if (value >= 0)
    {
      if (result != NULL)
        result[length] = value;
      length++;

      if (value == 0)
        return length;

      value = -1;
    }

    /* Now, decide what value should be for the next iteration.
     * Here, "continue;" means "I've possibly set 'value' and I want
     * to continue looking at the string starting at the next
     * character pointed to by 'string'.
     */
    switch (*string++)
    {
      case '\n':
      case '\0':
      case ':':
        value = 0;
        continue;

      case '\\':
        switch (*string++)
        {
          case '\n':
            while (*string == ' ' || *string == '\t')
              string++;
            continue;
          case 'E':
          case 'e':
            value = 27;
            continue;
          case 'n':
            value = 10;
            continue;
          case 'r':
            value = 13;
            continue;
          case 't':
            value = 8;
            continue;
          case 'b':
            value = 9;
            continue;
          case 'f':
            value = 12;
            continue;
          case '0':
          case '1':
            value = strtol(string - 1, (void *) &string, 8);
            continue;
          default:
            /* invalid escape sequence.  write the \ and
             * continue as if we never saw it...
             */
            value = '\\';
            string--;
            continue;
        }

      case '^':
        if (*string >= 'A' && *string <= 'Z')
        {
          value = *string++ - '@';
          break;
        }

        /* else, invalid control sequnce.  write the ^
         * and continue as if we never saw it...
         */

      default:
        /* else, the value is this character and the pointer has
         * already been advanced to the next character. */
        value = string[-1];
    }
  }
}

char *
_vte_termcap_find_string_length (VteTermcap *termcap,
                                 const char *tname,
                                 const char *cap,
                                 gssize *length)
{
  const char *result = _vte_termcap_find_start (termcap, tname, cap);
  char *string;

  if (result == NULL || result[2] != '=')
  {
    *length = 0;
    return g_strdup ("");
  }

  result += 3;

  *length = _vte_termcap_unescape_string (result, NULL);
  string = g_malloc (*length);
  _vte_termcap_unescape_string (result, string);

  (*length)--;

  return string;
}

char *
_vte_termcap_find_string (VteTermcap *termcap,
                          const char *tname,
                          const char *cap)
{
  gssize length;

  return _vte_termcap_find_string_length (termcap, tname, cap, &length);
}

long
_vte_termcap_find_numeric (VteTermcap *termcap,
                           const char *tname,
                           const char *cap)
{
  const char *result = _vte_termcap_find_start (termcap, tname, cap);
  long value;
  char *end;

  if (result == NULL || result[2] != '#')
    return 0;

  result += 3;

  value = strtol (result, &end, 0);
  if (*end != ':' && *end != '\0' && *end != '\n')
    return 0;

  return value;
}

gboolean
_vte_termcap_find_boolean (VteTermcap *termcap,
                           const char *tname,
                           const char *cap)
{
  const char *result = _vte_termcap_find_start (termcap, tname, cap);

  if (result == NULL)
    return FALSE;

  result += 2;

  if (*result != ':' && *result != '\0' && *result != '\n')
    return FALSE;

  return TRUE;
}

static GTree *
_vte_termcap_parse_file (const char *contents, int length)
{
  const char *end = contents + length;
  GTree *termcap;

  /* this tree contains terminal alias names which in a proper
   * termcap file will always be followed by : or |.  we
   * include \n to be extra-permissive. \0 is here to allow
   * us to notice the end of strings passed to us by vte.
   */
  termcap = g_tree_new_full ((GCompareDataFunc) _vte_termcap_strcmp,
                             (gpointer)":|\n", NULL, NULL);

  g_tree_insert(termcap, (gpointer) (contents + 355), (gpointer) (contents + 412));
  g_tree_insert(termcap, (gpointer) (contents + 369), (gpointer) (contents + 412));
  g_tree_insert(termcap, (gpointer) (contents + 379), (gpointer) (contents + 412));
  g_tree_insert(termcap, (gpointer) (contents + 1282), (gpointer) (contents + 1339));
  g_tree_insert(termcap, (gpointer) (contents + 1295), (gpointer) (contents + 1339));
  g_tree_insert(termcap, (gpointer) (contents + 1555), (gpointer) (contents + 1582));
  g_tree_insert(termcap, (gpointer) (contents + 1561), (gpointer) (contents + 1582));

  return termcap;
}

static VteTermcap *
_vte_termcap_create (void)
{
  const char *contents;
  VteTermcap *termcap;
  int length;

  contents = (char*) g_slice_alloc(sizeof(char) * 1602);
  strcpy(contents, "# This is a cut-down version of the termcap file from my box with some entries\n# removed (add them back in to override the terminal's behavior):\n# kI (Insert, Delete is handled programmatically)\n# kP/kN (Page Up, Page Down)\n# ku/kd/kl/kr (Up, Down, Left, Right)\n# k1/kd2/k3/k4/k5/k6/k7/k8/k9/k; (F1-F10)\n# K1/K2/K3/K4/K5 (KP Up, Down, Left, Right, Begin)\nxterm-xfree86|xterm-new|xterm terminal emulator (XFree86):\\\n\t:am:km:mi:ms:xn:\\\n\t:co#80:it#8:li#24:\\\n\t:AL=\\E[%dL:DC=\\E[%dP:DL=\\E[%dM:DO=\\E[%dB:IC=\\E[%d@:\\\n\t:LE=\\E[%dD:\\\n\t:RI=\\E[%dC:UP=\\E[%dA:ae=^O:al=\\E[L:as=^N:bl=^G:bt=\\E[Z:\\\n\t:cd=\\E[J:ce=\\E[K:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:cr=^M:\\\n\t:cs=\\E[%i%d;%dr:ct=\\E[3g:dc=\\E[P:dl=\\E[M:do=^J:ec=\\E[%dX:\\\n\t:ei=\\E[4l:ho=\\E[H:im=\\E[4h:is=\\E[!p\\E[?3;4l\\E[4l\\E>:\\\n\t:kD=\\177:\\\n\t:kb=^H:ke=\\E[?1l\\E>:\\\n\t:ks=\\E[?1h\\E=:le=^H:mb=\\E[5m:md=\\E[1m:\\\n\t:me=\\E[m\\017:mr=\\E[7m:nd=\\E[C:rc=\\E8:sc=\\E7:se=\\E[27m:\\\n\t:sf=^J:so=\\E[7m:sr=\\EM:st=\\EH:ta=^I:te=\\E[?1047l\\E[?1048l:\\\n\t:ti=\\E[?1048h\\E[?1047h:ue=\\E[24m:up=\\E[A:us=\\E[4m:\\\n\t:vb=\\E[?5h\\E[?5l:ve=\\E[?25h:vi=\\E[?25l:vs=\\E[?25h:\\\n\t:ui=\\E[N:ai=\\E[O:\n# xterm-redhat is identical to xterm-xfree86 with the addition of a small\n# VT220-ism regarding the backspace and delete keys; this is mandated by\n# the keyboard configuration section of the Debian Policy Manual.\nxterm-redhat|Red Hat xterm (backspace and delete changed):\\\n\t:kD=\\E[3~:kb=\\177:tc=xterm-xfree86:\n# This is the only entry which you should have to customize, since \"xterm\"\n# is widely used for a variety of incompatible terminal emulations including\n# color_xterm and rxvt.\nxterm|X11 terminal emulator:\\\n\t:tc=xterm-redhat:\n");

  termcap = g_slice_new (VteTermcap);
  termcap->start = contents;
  termcap->tree = _vte_termcap_parse_file (contents, length);
  termcap->end = contents + 1602;

  return termcap;
}

static void
_vte_termcap_destroy (VteTermcap *termcap)
{
  g_tree_destroy (termcap->tree);
  g_slice_free1 (1602, (gpointer) termcap->start);
  g_slice_free (VteTermcap, termcap);
}

/*
 * --- cached interface to create/destroy termcap trees ---
 */
static GStaticMutex _vte_termcap_mutex = G_STATIC_MUTEX_INIT;
static GCache *_vte_termcap_cache = NULL;

VteTermcap *
_vte_termcap_new(void)
{
  VteTermcap *result;

  g_static_mutex_lock (&_vte_termcap_mutex);

  if (_vte_termcap_cache == NULL)
    _vte_termcap_cache = g_cache_new((GCacheNewFunc) _vte_termcap_create,
                                     (GCacheDestroyFunc) _vte_termcap_destroy,
                                     (GCacheDupFunc) g_strdup,
                                     (GCacheDestroyFunc) g_free,
                                     g_str_hash, g_direct_hash, g_str_equal);

  result = g_cache_insert (_vte_termcap_cache, (gpointer) _vte_termcap_cache);

  g_static_mutex_unlock (&_vte_termcap_mutex);

  return result;
}

void
_vte_termcap_free (VteTermcap *termcap)
{
  g_static_mutex_lock (&_vte_termcap_mutex);
  g_cache_remove (_vte_termcap_cache, termcap);
  g_static_mutex_unlock (&_vte_termcap_mutex);
}

#ifdef TERMCAP_MAIN
#include <stdio.h>

int
main (int argc, char **argv)
{
  VteTermcap *tc;
  char *str;
  gssize len;
  int i;

  if (argc < 4)
  {
    g_printerr("vtetc /path/to/termcap termname attrs...\n"
                     "  where attrs are\n"
                     "    :xx for boolean\n"
                     "    =xx for string\n"
                     "    +xx for string displayed in hex\n"
                     "    #xx for numeric\n");
    return 1;
  }

  tc = _vte_termcap_new (argv[1]);

  if (tc == NULL)
  {
    perror ("open");
    return 1;
  }

  for (i = 3; i < argc; i++)
  {
    printf ("%s -> ", argv[i]);

    switch (argv[i][0])
    {
      case ':':
        printf ("%s\n", _vte_termcap_find_boolean (tc, argv[2], argv[i] + 1)?
                        "true" : "false");
        break;

      case '=':
      case '+':
        str = _vte_termcap_find_string_length (tc, argv[2], argv[i] + 1, &len);

        if (argv[i][0] == '=')
          printf ("'%s' (%d)\n", str, (int)len);
        else
        {
          int i;

          for (i = 0; str[i]; i++)
            printf ("%02x", str[i]);
          printf (" (%d) \n", (int)len);
        }
        g_free (str);
        break;

      case '#':
        printf ("%ld\n", _vte_termcap_find_numeric (tc, argv[2], argv[i] + 1));
        break;

      default:
        g_printerr("unrecognised type '%c'\n", argv[i][0]);
    }
  }

  _vte_termcap_free(tc);

  return 0;
}
#endif
