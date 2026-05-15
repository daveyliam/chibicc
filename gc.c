#include "chibicc.h"

typedef struct GCInfo GCInfo;
struct GCInfo {
  GCInfo *next;
  GCInfo *prev;
  uint64_t flags;
};

static GCInfo *gc_head = NULL;
static GCInfo *gc_tail = NULL;

void *gc_alloc(size_t size) {
  void *r = calloc(1, sizeof(GCInfo) + size);
  if (r == NULL) {
    fputs("error: out of memory\n", stderr);
    exit(1);
  }
  GCInfo *info = r;
  if (gc_tail == NULL) {
    gc_head = info;
    gc_tail = info;
  } else {
    gc_tail->next = info;
    info->prev = gc_tail;
    gc_tail = info;
  }
  return r + sizeof(GCInfo);
}

char *gc_strndup(const char *s, size_t n) {
  void *s2 = gc_alloc(n + 1);
  memcpy(s2, s, n);
  return s2;
}

char *gc_strdup(const char *s) { return gc_strndup(s, strlen(s)); }

void gc_free(void *r) {
  GCInfo *info = r - sizeof(GCInfo);
  if (info == gc_head) {
    gc_head = info->next;
  } else {
    info->prev->next = info->next;
  }
  if (info == gc_tail) {
    gc_tail = info->prev;
  } else {
    info->next->prev = info->prev;
  }
  free(info);
}

void gc_free_all(void) {
  for (GCInfo *info = gc_head; info;) {
    GCInfo *info_next = info->next;
    free(info);
    info = info_next;
  }
  gc_head = NULL;
  gc_tail = NULL;
}
