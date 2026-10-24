/* RetroArch's config value parser, vendored for the round-trip test in
 * internal/platform/paths_config_test.c. Test-only: nothing that ships
 * compiles this.
 *
 * Source: https://github.com/libretro/RetroArch, tag v1.22.2 (commit
 * 69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576, the version retroarch-builds
 * ships), libretro-common/file/config_file.c. The two functions between the
 * VERBATIM markers are copied byte for byte by
 * scripts/retroarch-config-parser-extract.sh, and
 * scripts/check-retroarch-config-parser.sh fails if they drift from a
 * fetched tree. Everything outside the markers is a Jawaka shim.
 *
 * Copyright  (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (config_file.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* libretro-common/include/string/stdstring.h tests lr_char_props[c] & 0x80,
   which libretro-common/string/stdstring.c sets for exactly these four bytes
   ("C standard says \f \v are space, but this one disagrees"). */
#define ISSPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')

/* libretro-common/compat/strl.h; glibc before 2.38 has no strlcpy. */
static size_t jw_ra_vendored_strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#undef strlcpy
#define strlcpy jw_ra_vendored_strlcpy

/* BEGIN VERBATIM libretro-common/file/config_file.c v1.22.2 */
static char *config_file_strip_comment(char *str)
{
   /* Search for a comment (#) character */
   char *comment = strchr(str, '#');

   if (comment)
   {
      char *literal_start = NULL;

      /* Check whether entire line is a comment
       * > First character == '#' */
      if (str == comment)
      {
         /* Set 'str' to NUL and return comment
          * for processing at a higher level */
         *str = '\0';
         return ++comment;
      }

      /* Comment character occurs at an offset:
       * Search for the start of a string literal value */
      literal_start = strchr(str, '\"');

      /* Check whether string literal start occurs
       * *before* the comment character */
      if (literal_start && (literal_start < comment))
      {
         /* Search for the end of the string literal
          * value */
         char *literal_end = strchr(literal_start + 1, '\"');

         /* Check whether string literal end occurs
          * *after* the comment character
          * > If this is the case, ignore the comment
          * > Leave 'str' untouched and return NULL */
         if (literal_end && (literal_end > comment))
            return NULL;
      }

      /* If we reach this point, then a comment
       * exists outside of a string literal
       * > Trim the entire comment from the end
       *   of 'str' */
      *comment = '\0';
   }

   return NULL;
}

static char *config_file_extract_value(char *line)
{
   char *dst = NULL;
   while (ISSPACE((int)*line))
      line++;

   /* Note: From this point on, an empty value
    * string is valid - and in this case, strldup("", sizeof(""))
    * will be returned (see Note 2)
    * > If we instead return NULL, the the entry
    *   is ignored completely - which means we cannot
    *   track *changes* in entry value */

   /* If first character is ("), we have a full string
    * literal */
   if (*line == '"')
   {
      size_t idx  = 0;
      char *value = NULL;
      /* Skip to next character */
      line++;

      /* If this a ("), then value string is empty */
      if (*line != '"')
      {
         /* Find the next (") character */
         while (line[idx] && (line[idx] != '\"'))
            idx++;

         line[idx] = '\0';
         if ((value = line) && *value)
            return strdup(value);
      }
   }
   /* This is not a string literal - just read
    * until the next space is found
    * > Note: Skip this if line is empty */
   else if (*line != '\0')
   {
      size_t idx  = 0;
      char *value = NULL;
      /* Find next space character */
      while (line[idx] && isgraph((int)line[idx]))
         idx++;

      line[idx] = '\0';
      if ((value = line) && *value)
         return strdup(value);
   }

   /* Note 2: This is an unrolled strldup call
    * to avoid an unnecessary dependency -
    * call is strldup("", sizeof(""))
    **/
   dst = (char*)malloc(sizeof(char) * 2);
   strlcpy(dst, "", 1);
   return dst;
}

/* END VERBATIM */

#undef strlcpy
#undef ISSPACE
