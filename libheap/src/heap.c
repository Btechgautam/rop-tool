/************************************************************************/
/* rop-tool - A Return Oriented Programming and binary exploitation     */
/*            tool                                                      */
/*                                                                      */
/* Copyright 2013-2015, -TOSH-                                          */
/* File coded by -TOSH-                                                 */
/*                                                                      */
/* This file is part of rop-tool.                                       */
/*                                                                      */
/* rop-tool is free software: you can redistribute it and/or modify     */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* rop-tool is distributed in the hope that it will be useful,          */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with rop-tool.  If not, see <http://www.gnu.org/licenses/>     */
/************************************************************************/
#define _GNU_SOURCE
#include "roptool-api.h"
#include "libheap.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

typedef struct libheap_chunk {
  size_t prev_size;
  size_t size;

  struct libheap_chunk *fd;
  struct libheap_chunk *bk;

}libheap_chunk_s;


void* (*libheap_malloc_old)(size_t, const void*);
void* (*libheap_realloc_old)(void*, size_t, const void*);
void (*libheap_free_old)(void*, const void*);


static void libheap_initialize(void);

void (*__MALLOC_HOOK_VOLATILE __malloc_initialize_hook)(void) = libheap_initialize;


#define LIBHEAP_PREV_INUSE     0x1
#define LIBHEAP_IS_MMAPED      0x2
#define LIBHEAP_NON_MAIN_ARENA 0x4

#define LIBHEAP_CHUNK_FLAG(c,f) (c->size & f)
#define LIBHEAP_CHUNK_SIZE(c) (c->size & ~(LIBHEAP_PREV_INUSE|LIBHEAP_IS_MMAPED|LIBHEAP_NON_MAIN_ARENA))
#define LIBHEAP_NEXT_CHUNK(c) ((libheap_chunk_s*)(((u8*)c)+LIBHEAP_CHUNK_SIZE(c)))
#define LIBHEAP_GET_CHUNK(ptr) ((libheap_chunk_s*)(ptr - 2*sizeof(size_t)))
#define LIBHEAP_USER_ADDR(c) (((u8*)c) + 2*sizeof(size_t))
#define LIBHEAP_ADDR(c) (((u8*)c) + sizeof(size_t))
#define LIBHEAP_CHUNK_INUSE(c) (LIBHEAP_NEXT_CHUNK(c) > libheap_last_chunk ? 1 : LIBHEAP_CHUNK_FLAG(LIBHEAP_NEXT_CHUNK(c), LIBHEAP_PREV_INUSE))

#define LIBHEAP_DUMP(...) do {                                          \
    R_UTILS_FPRINT_RED_BG_BLACK(libheap_options_stream, libheap_options_color, __VA_ARGS__); \
    libheap_dump();                                                     \
  }while(0)

#define LIBHEAP_DUMP_FIELD(u,f,...) do {                                \
    if(u) {                                                             \
      R_UTILS_FPRINT_YELLOW_BG_BLACK(libheap_options_stream, libheap_options_color,f); \
      R_UTILS_FPRINT_GREEN_BG_BLACK(libheap_options_stream, libheap_options_color,__VA_ARGS__); \
    } else {                                                            \
      R_UTILS_FPRINT_WHITE_BG_BLACK(libheap_options_stream, libheap_options_color,f); \
      R_UTILS_FPRINT_GREEN_BG_BLACK(libheap_options_stream, libheap_options_color,__VA_ARGS__); \
    }                                                                   \
  }while(0)

static libheap_chunk_s *libheap_first_chunk = NULL;
static libheap_chunk_s *libheap_last_chunk  = NULL;

#define LIBHEAP_TRACE_NONE 0
#define LIBHEAP_TRACE_FREE 1
#define LIBHEAP_TRACE_MALLOC 2
#define LIBHEAP_TRACE_REALLOC 4
#define LIBHEAP_TRACE_CALLOC 8
#define LIBHEAP_TRACE_ALL (8|4|2|1)

static FILE* libheap_options_stream = NULL;
static int libheap_options_color = 1;
static int libheap_options_trace = LIBHEAP_TRACE_ALL;
static int libheap_options_dumpdata = 0;

static void libheap_dump_chunk(libheap_chunk_s *chunk) {
  int inuse = LIBHEAP_CHUNK_INUSE(chunk);

  LIBHEAP_DUMP_FIELD(inuse, "addr: ",  "%p, ", LIBHEAP_ADDR(chunk));
  LIBHEAP_DUMP_FIELD(inuse, "usr_addr: ", "%p, ", LIBHEAP_USER_ADDR(chunk));

  if(!LIBHEAP_CHUNK_FLAG(chunk, LIBHEAP_IS_MMAPED) &&
     !LIBHEAP_CHUNK_FLAG(chunk, LIBHEAP_PREV_INUSE)) {
    LIBHEAP_DUMP_FIELD(inuse, "prev_size: ", "0x%"SIZE_T_FMT_X", ", chunk->prev_size);
  }
  if(!inuse) {
    LIBHEAP_DUMP_FIELD(inuse, "fd: ", "%p, ", chunk->fd);
    LIBHEAP_DUMP_FIELD(inuse, "bk: ", "%p, ", chunk->bk);
  }
  LIBHEAP_DUMP_FIELD(inuse, "size: ", "0x%"SIZE_T_FMT_X", ", LIBHEAP_CHUNK_SIZE(chunk));
  LIBHEAP_DUMP_FIELD(inuse, "flags: ", "%c%c%c\n",
                     LIBHEAP_CHUNK_FLAG(chunk, LIBHEAP_IS_MMAPED) ? 'M' : '-',
                     LIBHEAP_CHUNK_FLAG(chunk, LIBHEAP_PREV_INUSE) ? 'P' : '-',
                     LIBHEAP_CHUNK_FLAG(chunk, LIBHEAP_NON_MAIN_ARENA) ? 'A' : '-');

  if(libheap_options_dumpdata) {
#if defined(__ARCH_32)
    libheap_hexdump(libheap_options_stream, libheap_options_color, LIBHEAP_ADDR(chunk), LIBHEAP_CHUNK_SIZE(chunk), (u32)(LIBHEAP_ADDR(chunk)));
#elif defined(__ARCH_64)
    libheap_hexdump(libheap_options_stream, libheap_options_color, LIBHEAP_ADDR(chunk), LIBHEAP_CHUNK_SIZE(chunk), (u64)(LIBHEAP_ADDR(chunk)));
#else
#error "Fix ARCH define"
#endif
  }
}

static void libheap_dump(void) {
  libheap_chunk_s *chunk;

  chunk = libheap_first_chunk;

  while(chunk != NULL) {
    libheap_dump_chunk(chunk);

    /*
     * Avoid infinite loop if chunk size is
     * corrupted
     */
    if(LIBHEAP_CHUNK_SIZE(chunk) == 0)
      return;

    chunk = LIBHEAP_NEXT_CHUNK(chunk);

    /* Chunk is out of allocated space */
    if(chunk > libheap_last_chunk)
      return;
  }
}

static void libheap_update_chunk(u8 *ptr) {
  if(libheap_first_chunk == NULL) {
    libheap_first_chunk = libheap_last_chunk = LIBHEAP_GET_CHUNK(ptr);
  } else {
    if(LIBHEAP_GET_CHUNK(ptr) > libheap_last_chunk)
      libheap_last_chunk = LIBHEAP_GET_CHUNK(ptr);
  }
}

FILE* libheap_fopen_stream(const char *filename) {
  FILE *f;

  if((f = fopen(filename, "w")) == NULL) {
    R_UTILS_ERRX("Failed to open libheap output stream");
  }

  return f;
}

static void libheap_get_options(void) {
  int trace = 0;
  char *env;

  libheap_options_stream = stderr;

  if((env = getenv("LIBHEAP_COLOR")) != NULL) {
    libheap_options_color = atoi(env);
  }

  if((env = getenv("LIBHEAP_TRACE_FREE")) != NULL) {
    trace |= (atoi(env) % 2) * LIBHEAP_TRACE_FREE;
  }
  if((env = getenv("LIBHEAP_TRACE_MALLOC")) != NULL) {
    trace |= (atoi(env) % 2) * LIBHEAP_TRACE_MALLOC;
  }
  if((env = getenv("LIBHEAP_TRACE_REALLOC")) != NULL) {
    trace |= (atoi(env) % 2) * LIBHEAP_TRACE_REALLOC;
  }

  if(trace != 0)
    libheap_options_trace = trace;

  if((env = getenv("LIBHEAP_DUMPDATA")) != NULL) {
    libheap_options_dumpdata = atoi(env);
  }

  if((env = getenv("LIBHEAP_OUTPUT")) != NULL) {
    libheap_options_stream = libheap_fopen_stream(env);
  }
}






/**************************************************************************************/
/* Modified libc functions : malloc, calloc, realloc, free                            */
/**************************************************************************************/

void* libheap_malloc_hook(size_t s, const void *user) {
  void *p;

  (void) user;

  __malloc_hook = libheap_malloc_old;
  p = malloc(s);

  if(!LIBHEAP_CHUNK_FLAG(LIBHEAP_GET_CHUNK(p), LIBHEAP_IS_MMAPED)) {
    libheap_update_chunk(p);
    if(libheap_options_trace & LIBHEAP_TRACE_MALLOC)
      LIBHEAP_DUMP("malloc(%" SIZE_T_FMT_X ") = %p\n", s, p);
  }

  __malloc_hook = libheap_malloc_hook;

  return p;
}

void* libheap_realloc_hook(void *ptr, size_t s, const void *user) {
  void *p;

  (void) user;

  __realloc_hook = libheap_realloc_old;
  p = realloc(ptr, s);

  if(!LIBHEAP_CHUNK_FLAG(LIBHEAP_GET_CHUNK(p), LIBHEAP_IS_MMAPED)) {
    libheap_update_chunk(p);
    if(libheap_options_trace & LIBHEAP_TRACE_MALLOC)
      LIBHEAP_DUMP("realloc(%" SIZE_T_FMT_X ") = %p\n", s, p);
  }

  __realloc_hook = libheap_realloc_hook;

  return p;
}


void libheap_free_hook(void *ptr, const void *user) {

  (void) user;

  __free_hook = libheap_free_old;

  if(ptr) {
    if(!LIBHEAP_CHUNK_FLAG(LIBHEAP_GET_CHUNK(ptr), LIBHEAP_IS_MMAPED)) {
      free(ptr);
      if(libheap_options_trace & LIBHEAP_TRACE_FREE)
        LIBHEAP_DUMP("free(%p)\n", ptr);
    }
  }

  __free_hook = libheap_free_hook;
}

static void libheap_initialize(void) {

  libheap_get_options();

  libheap_malloc_old  = __malloc_hook;
  libheap_free_old    = __free_hook;
  libheap_realloc_old = __realloc_hook;

  __malloc_hook  = libheap_malloc_hook;
  __free_hook    = libheap_free_hook;
  __realloc_hook = libheap_realloc_hook;
}
