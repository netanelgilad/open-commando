#pragma once
#include "rebel_portable.h"
/* TCHAR is char in this build (no _UNICODE) */
#define _tcscmp strcmp
#define _tcsicmp strcasecmp
#define _tcsncmp strncmp
#define _tcsnicmp strncasecmp
#define _tcslen strlen
#define _tcsclen strlen
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcscat strcat
#define _tcsncat strncat
#define _tcschr strchr
#define _tcsrchr strrchr
#define _tcsstr strstr
#define _tcstok strtok
#define _tcsdup strdup
#define _tcsupr strupr
#define _tcslwr strlwr
#define _tcsspn strspn
#define _tcscspn strcspn
#define _tcspbrk strpbrk
#define _tcstol strtol
#define _tcstoul strtoul
#define _tcstod strtod
#define _tstoi atoi
#define _ttoi atoi
#define _ttol atol
#define _tprintf printf
#define _stprintf sprintf
#define _sntprintf snprintf
#define _vstprintf vsprintf
#define _vsntprintf vsnprintf
#define _tfopen fopen
#define _istspace isspace
#define _istdigit isdigit
#define _istalpha isalpha
#define _istalnum isalnum
#define _istupper isupper
#define _istlower islower
#define _totupper toupper
#define _totlower tolower
#define _tcsinc(p) ((p) + 1)
#define _tcsdec(s, p) ((p) - 1)
#define _tcsnextc(p) ((unsigned)*(const unsigned char *)(p))
