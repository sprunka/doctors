#ifndef __assert_h
#define __assert_h

#include <pebble.h>

// Our own poor-man's assert() function, since Pebble doesn't provide one.
#define assert(condition) { \
  if (!(condition)) { \
    assert_failure(#condition, __FILE__, __LINE__); \
  } \
  }

static void assert_failure(const char *condition, const char *filename, int line_number) {
  app_log(APP_LOG_LEVEL_ERROR, filename, line_number, "assertion failed: %s", condition);

  // Force a crash.
  char *null_ptr = 0;
  (*null_ptr) = 0;
}

#endif  // __assert_h
