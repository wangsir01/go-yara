/*
Copyright (c) 2017. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <errno.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yara_error.h>
#include <yara_libyara.h>
#include <yara_mem.h>
#include <yara_proc.h>

typedef struct _YR_PROC_INFO
{
  int pid;
  struct ptrace_vm_entry vm_entry;
} YR_PROC_INFO;

int _yr_process_attach(int pid, YR_PROC_ITERATOR_CTX* context)
{
  int status;

  YR_PROC_INFO* proc_info = (YR_PROC_INFO*) yr_malloc(sizeof(YR_PROC_INFO));

  if (proc_info == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  memset(proc_info, 0, sizeof(YR_PROC_INFO));

  proc_info->pid = pid;
  if (ptrace(PT_ATTACH, pid, NULL, 0) == -1)
  {
    yr_free(proc_info);

    return ERROR_COULD_NOT_ATTACH_TO_PROCESS;
  }

  status = 0;

  if (waitpid(pid, &status, 0) == -1)
  {
    ptrace(PT_DETACH, proc_info->pid, NULL, 0);
    yr_free(proc_info);

    return ERROR_COULD_NOT_ATTACH_TO_PROCESS;
  }

  context->proc_info = proc_info;

  return ERROR_SUCCESS;
}

int _yr_process_detach(YR_PROC_ITERATOR_CTX* context)
{
  YR_PROC_INFO* proc_info = (YR_PROC_INFO*) context->proc_info;

  ptrace(PT_DETACH, proc_info->pid, NULL, 0);
  proc_info->vm_entry.pve_path = NULL;

  return ERROR_SUCCESS;
}

YR_API const uint8_t* yr_process_fetch_memory_block_data(YR_MEMORY_BLOCK* block)
{
  YR_PROC_ITERATOR_CTX* context = (YR_PROC_ITERATOR_CTX*) block->context;
  YR_PROC_INFO* proc_info = (YR_PROC_INFO*) context->proc_info;
  struct ptrace_io_desc io_desc;

  if (context->buffer_size < block->size)
  {
    if (context->buffer != NULL)
      yr_free((void*) context->buffer);

    context->buffer = (const uint8_t*) yr_malloc(block->size);

    if (context->buffer != NULL)
    {
      context->buffer_size = block->size;
    }
    else
    {
      context->buffer_size = 0;
      return NULL;
    }
  }

  io_desc.piod_op = PIOD_READ_D;
  io_desc.piod_offs = (void*) block->base;
  io_desc.piod_addr = (void*) context->buffer;
  io_desc.piod_len = block->size;

  if (ptrace(PT_IO, proc_info->pid, (char*) &io_desc, 0) == -1)
  {
    return NULL;
  }

  return context->buffer;
}

YR_API YR_MEMORY_BLOCK* yr_process_get_next_memory_block(
    YR_MEMORY_BLOCK_ITERATOR* iterator)
{
  YR_PROC_ITERATOR_CTX* context = (YR_PROC_ITERATOR_CTX*) iterator->context;
  YR_PROC_INFO* proc_info = (YR_PROC_INFO*) context->proc_info;

  char buf[4096];

  proc_info->vm_entry.pve_path = buf;
  proc_info->vm_entry.pve_pathlen = sizeof(buf);

  uint64_t current_begin = context->current_block.base +
                           context->current_block.size;

  uint64_t max_process_memory_chunk;

  yr_get_configuration_uint64(
      YR_CONFIG_MAX_PROCESS_MEMORY_CHUNK, &max_process_memory_chunk);

  iterator->last_error = ERROR_SUCCESS;

  if (proc_info->vm_entry.pve_end <= current_begin)
  {
    if (ptrace(
            PT_VM_ENTRY, proc_info->pid, (char*) (&proc_info->vm_entry), 0) ==
        -1)
    {
      return NULL;
    }
    else
    {
      current_begin = proc_info->vm_entry.pve_start;
    }
  }

  context->current_block.base = current_begin;
  context->current_block.size = yr_min(
      proc_info->vm_entry.pve_end - current_begin + 1,
      max_process_memory_chunk);

  assert(context->current_block.size > 0);

  return &context->current_block;
}

YR_API YR_MEMORY_BLOCK* yr_process_get_first_memory_block(
    YR_MEMORY_BLOCK_ITERATOR* iterator)
{
  YR_PROC_ITERATOR_CTX* context = (YR_PROC_ITERATOR_CTX*) iterator->context;
  YR_PROC_INFO* proc_info = (YR_PROC_INFO*) context->proc_info;

  proc_info->vm_entry.pve_entry = 0;

  YR_MEMORY_BLOCK* result = yr_process_get_next_memory_block(iterator);

  if (result == NULL)
    iterator->last_error = ERROR_COULD_NOT_READ_PROCESS_MEMORY;

  return result;
}
