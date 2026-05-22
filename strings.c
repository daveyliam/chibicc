#include "chibicc.h"

void strarray_push(StringArray *arr, char *s) {
  if (!arr->data) {
    arr->data = calloc(8, sizeof(char *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    // TODO : replace with realloc when supported in libc.c.
    char **data2 = calloc(arr->capacity * 2, sizeof(char *));
    memcpy(data2, arr->data, sizeof(char *) * arr->len);
    free(arr->data);
    arr->data = data2;
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++) {
      arr->data[i] = NULL;
    }
  }

  arr->data[arr->len++] = s;
}

void strarray_clear(StringArray *arr, bool should_free_elems) {
  if (arr->data) {
    if (should_free_elems) {
      for (int i = 0; i < arr->len; i++) {
        free(arr->data[i]);
        arr->data[i] = NULL;
      }
    }
    free(arr->data);
    arr->data = NULL;
  }
  arr->len = 0;
  arr->capacity = 0;
}

// Takes a printf-style format string and returns a formatted string.
char *format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (n < 0) {
    fputs("error: format: vsnprintf failed\n", stderr);
    exit(1);
  }

  char *buf = calloc(n + 1, 1);

  va_start(ap, fmt);
  vsnprintf(buf, n + 1, fmt, ap);
  va_end(ap);

  return buf;
}

char *dirname2(const char *s) {
  if (s == NULL || *s == 0) {
    return strdup(".");
  }
  int i = strlen(s) - 1;
  for (; s[i] == '/'; i--) {
    if (i == 0) {
      return strdup("/");
    }
  }
  for (; s[i] != '/'; i--) {
    if (i == 0) {
      return strdup(".");
    }
  }
  for (; s[i] == '/'; i--) {
    if (i == 0) {
      return strdup("/");
    }
  }
  return strndup(s, i + 1);
}

void bytearray_append(ByteArray *arr, uint8_t b) {
  if (arr->len >= arr->cap) {
    int cap2;
    if (arr->cap < 8) {
      cap2 = 8;
    } else {
      cap2 = arr->cap * 2;
    }
    // TODO : replace with realloc when supported in libc.c.
    uint8_t *data2 = calloc(cap2, sizeof(uint8_t));
    if (arr->data != NULL) {
      memcpy(data2, arr->data, arr->len);
      free(arr->data);
    }
    arr->data = data2;
    arr->cap = cap2;
    memset(arr->data + arr->len, 0, arr->cap - arr->len);
  }
  arr->data[arr->len++] = b;
}

void bytearray_extend(ByteArray *arr, uint8_t *data, int len) {
  for (int i = 0; i < len; i++) {
    bytearray_append(arr, data[i]);
  }
}

void bytearray_clear(ByteArray *arr) {
  if (arr->data) {
    free(arr->data);
    arr->data = NULL;
  }
  arr->len = 0;
  arr->cap = 0;
}
