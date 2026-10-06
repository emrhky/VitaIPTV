#ifndef LANG_H
#define LANG_H
/* English / Turkish texts. Code uses the English text as the key: T("Back") -> "Geri". */
enum { LANG_EN = 0, LANG_TR = 1 };

void        lang_set(int lang);
int         lang_get(void);
const char *T(const char *english);          /* fixed text (or a printf format) */
const char *T_msg(const char *english);      /* also messages with numbers in them, e.g. "Server answered HTTP 407" */

#endif
