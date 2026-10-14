// Wide printf with Microsoft's conventions (see compat/rebel_portable.h).
//
// In Microsoft's wide printf family "%s" and "%c" take wide strings and
// characters (narrow ones are "%S"/"%hs" and "%C"/"%hc"). POSIX wide printf
// is the other way round: "%s" takes a narrow char *. The engine's wide
// formats are written for Windows, so passed straight to vswprintf every
// "%s" read a 4-byte wchar_t string as bytes and stopped after the first
// character ("Time Remaining: 0:30" showed as "T: 0", chat as "H: L").
// The format is rewritten to POSIX spelling first.
#include "rebel_portable.h"

#include <cstdarg>
#include <cwchar>
#include <vector>

static void To_Posix_Format(const wchar_t *in, std::vector<wchar_t> &out) {
  out.clear();
  while (*in) {
    wchar_t c = *in++;
    out.push_back(c);
    if (c != L'%')
      continue;
    if (*in == L'%') {
      out.push_back(*in++);
      continue;
    }
    // flags, width, precision
    while (*in && wcschr(L"-+ #0", *in))
      out.push_back(*in++);
    while (*in == L'*' || (*in >= L'0' && *in <= L'9'))
      out.push_back(*in++);
    if (*in == L'.') {
      out.push_back(*in++);
      while (*in == L'*' || (*in >= L'0' && *in <= L'9'))
        out.push_back(*in++);
    }
    // length modifier + conversion
    bool narrow = false, wide = false;
    if (*in == L'h' && (in[1] == L's' || in[1] == L'c')) {
      narrow = true;
      in++;
    } else if ((*in == L'l' || *in == L'w') && (in[1] == L's' || in[1] == L'c')) {
      wide = true;
      in++;
    }
    wchar_t conv = *in;
    if (!conv)
      break;
    in++;
    switch (conv) {
    case L's':
    case L'c':
      if (!narrow)
        out.push_back(L'l'); // Windows: wide
      out.push_back(conv);
      break;
    case L'S':
    case L'C':
      if (wide)
        out.push_back(L'l');
      out.push_back(conv == L'S' ? L's' : L'c'); // Windows: narrow
      break;
    default:
      // The other conversions mean the same; copy any length modifier.
      out.push_back(conv);
      while (wcschr(L"hlLqjztI", conv) && *in) {
        conv = *in++;
        out.push_back(conv);
        if (!wcschr(L"hlLqjztI0123456789", conv))
          break;
      }
      break;
    }
  }
  out.push_back(0);
}

extern "C" int Rebel_vsnwprintf(wchar_t *buffer, size_t count, const wchar_t *format, va_list args) {
  std::vector<wchar_t> posix;
  To_Posix_Format(format, posix);
  int r = vswprintf(buffer, count, posix.data(), args);
  if (r < 0 && count > 0)
    buffer[count - 1] = 0; // truncated (Windows also returns -1 then)
  return r;
}

extern "C" int Rebel_snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
  va_list args;
  va_start(args, format);
  int r = Rebel_vsnwprintf(buffer, count, format, args);
  va_end(args);
  return r;
}
