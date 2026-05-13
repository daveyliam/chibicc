#include "chibicc.h"

void strarray_push(StringArray *arr, char *s) {
  if (!arr->data) {
    arr->data = calloc(8, sizeof(char *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->data = realloc(arr->data, sizeof(char *) * arr->capacity * 2);
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++) {
      arr->data[i] = NULL;
    }
  }

  arr->data[arr->len++] = s;
}

void strarray_free(StringArray *arr) {
  if (arr->data) {
    free(arr->data);
    arr->data = NULL;
  }
}

// Takes a printf-style format string and returns a formatted string.
char *format(char *fmt, ...) {
  char *buf;
  size_t buflen;
  FILE *out = open_memstream(&buf, &buflen);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fclose(out);
  return buf;
}

void bytearray_append(ByteArray *arr, uint8_t b) {
  if (arr->len >= arr->cap) {
    if (arr->cap < 8) {
      arr->cap = 8;
    } else {
      arr->cap *= 2;
    }
    arr->data = realloc(arr->data, arr->cap);
    memset(arr->data + arr->len, 0, arr->cap - arr->len);
  }
  arr->data[arr->len++] = b;
}

void bytearray_extend(ByteArray *arr, uint8_t *data, int len) {
  for (int i = 0; i < len; i++) {
    bytearray_append(arr, data[i]);
  }
}

void bytearray_free(ByteArray *arr) {
  if (arr->data) {
    free(arr->data);
    arr->data = NULL;
  }
  arr->len = 0;
  arr->cap = 0;
}
