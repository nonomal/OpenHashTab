//    Copyright 2019 namazso <admin@namazso.eu>
//    This file is part of OpenHashTab.
//
//    OpenHashTab is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    OpenHashTab is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with OpenHashTab.  If not, see <https://www.gnu.org/licenses/>.
#include "stdafx.h"

#include "FileHashTask.h"
#include "OpenHashTabPropPage.h"
#include "Queues.h"
#include "utl.h"

std::atomic<intptr_t> FileHashTask::s_allocations_remaining = k_max_allocations;

uint8_t* FileHashTask::BlockTryAllocate()
{
  if (--s_allocations_remaining >= 0)
  {
    const auto p = VirtualAlloc(
      nullptr,
      k_block_size,
      MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE
    );

    if (p)
      return (uint8_t*)p;
  }

  ++s_allocations_remaining;
  return nullptr;
}

void FileHashTask::BlockReset(uint8_t* p)
{
  VirtualAlloc(
    p,
    k_block_size,
    MEM_RESET,
    PAGE_READWRITE
  );
  // We don't care about errors
}

void FileHashTask::BlockFree(uint8_t* p)
{
  const auto ret = VirtualFree(p, 0, MEM_RELEASE);
  assert(ret);
  ++s_allocations_remaining;
}

VOID NTAPI FileHashTask::HashWorkCallback(
  _Inout_     PTP_CALLBACK_INSTANCE instance,
  _Inout_opt_ PVOID                 ctx,
  _Inout_     PTP_WORK              work
)
{
  UNREFERENCED_PARAMETER(instance);
  UNREFERENCED_PARAMETER(work);
  ((FileHashTask*)ctx)->DoHashRound();
}

VOID WINAPI FileHashTask::IoCallback(
  _Inout_     PTP_CALLBACK_INSTANCE instance,
  _Inout_opt_ PVOID                 ctx,
  _Inout_opt_ PVOID                 overlapped,
  _In_        ULONG                 result,
  _In_        ULONG_PTR             bytes_transferred,
  _Inout_     PTP_IO                io
)
{
  UNREFERENCED_PARAMETER(instance);
  UNREFERENCED_PARAMETER(overlapped);
  UNREFERENCED_PARAMETER(io);
  ((FileHashTask*)ctx)->OverlappedCompletionRoutine(result, bytes_transferred);
}

void FileHashTask::ProcessReadQueue(uint8_t* reuse_block)
{
  FileHashTask* waiting_for_read = nullptr;
  do
  {
    auto ret = g_read_queue.try_dequeue(waiting_for_read);
    if (!ret)
      break;
    ret = waiting_for_read->ReadBlockAsync(reuse_block);
    reuse_block = nullptr;
    if (!ret)
      break;
  }
  while (true);
  if (reuse_block)
    BlockFree(reuse_block);
}

FileHashTask::FileHashTask(const tstring& path, OpenHashTabPropPage* prop_page, tstring display_name, std::vector<uint8_t> expected_hash)
  : _hash_contexts{}
  , _prop_page{ prop_page }
  , _display_name{ std::move(display_name) }
  , _expected_hash { std::move(expected_hash) }
{
  // Instead of exception, set _error because a failed file is still a finished
  // file task. Finish mechanism will trigger on first block read

  for (auto i = 0u; i < k_hashers_count; ++i)
    _lparam_idx[i] = i;

  for (auto& ctx : _hash_contexts)
    mbedtls_md_init(&ctx);

  _handle = utl::OpenForRead(path, true);

  if(_handle == INVALID_HANDLE_VALUE)
  {
    _error = GetLastError();
    return;
  }

  for (auto i = 0u; i < k_hashers_count; ++i)
  {
    const auto ctx = &_hash_contexts[i];
    auto ret = mbedtls_md_setup(ctx, k_hashers[i], 0);
    if (ret == MBEDTLS_ERR_MD_ALLOC_FAILED)
    {
      _error = ERROR_NOT_ENOUGH_MEMORY;
      return;
    }
    assert(ret == 0);

    ret = mbedtls_md_starts(ctx);
    assert(ret == 0);
  }

  BY_HANDLE_FILE_INFORMATION fi;
  if (!GetFileInformationByHandle(_handle, &fi))
  {
    _error = GetLastError();
    return;
  }

  _file_size = uint64_t(fi.nFileSizeHigh) << 32 | fi.nFileSizeLow;
  _file_index = uint64_t(fi.nFileIndexHigh) << 32 | fi.nFileIndexLow;
  // TODO: use this in queue so a lot of files from a slower device can't slow down another faster device
  _volume_serial = fi.dwVolumeSerialNumber;

  _threadpool_hash_work = CreateThreadpoolWork(
    HashWorkCallback,
    this,
    nullptr
  );

  if (!_threadpool_hash_work)
  {
    _error = GetLastError();
    return;
  }
  
  _threadpool_io = CreateThreadpoolIo(
    _handle,
    IoCallback,
    this,
    nullptr
  );

  if(!_threadpool_io)
  {
    _error = GetLastError();
    return;
  }
}

FileHashTask::~FileHashTask()
{
  assert(_block == nullptr);

  for (auto& ctx : _hash_contexts)
    mbedtls_md_free(&ctx);

  if(_handle != INVALID_HANDLE_VALUE)
    CloseHandle(_handle);
  if(_threadpool_hash_work)
    CloseThreadpoolWork(_threadpool_hash_work);
  if(_threadpool_io)
    CloseThreadpoolIo(_threadpool_io);
}

bool FileHashTask::ReadBlockAsync(uint8_t* reuse_block)
{
  if(_error != ERROR_SUCCESS)
  {
    if (reuse_block)
      BlockFree(reuse_block);
    Finish();
    return true;
  }

  // Set up OVERLAPPED fields for either reading or enqueueing for read
  _overlapped.Internal = 0; // reserved
  _overlapped.InternalHigh = 0; // reserved
  _overlapped.Offset = (DWORD)_current_offset;
  _overlapped.OffsetHigh = (DWORD)(_current_offset >> 32);
  //_overlapped.hEvent = this; // for caller use

  if (const auto block = reuse_block ? reuse_block : BlockTryAllocate())
  {
    const auto read_size = (DWORD)GetCurrentBlockSize();

    StartThreadpoolIo(_threadpool_io);

    _block = block;

    const auto ret = ReadFile(
      _handle,
      block,
      read_size,
      nullptr,
      &_overlapped
    );

    const auto error = GetLastError();

    if (ret || error == ERROR_IO_PENDING) // succeeded
      return true;

    _block = nullptr;

    CancelThreadpoolIo(_threadpool_io);

    // We failed to start the async operation, free block - cant give it back
    BlockFree(block);

    // If we got some unknown error don't reschedule, fail instead
    if (error != ERROR_INVALID_USER_BUFFER && error != ERROR_NOT_ENOUGH_MEMORY)
    {
      assert(_error);
      _error = error;
      Finish();
      return true;
    }
  }

  // If we just ran out of memory or outstanding async ios, requeue
  g_read_queue.enqueue(this);
  return false;
}

void FileHashTask::OverlappedCompletionRoutine(ULONG error_code, ULONG_PTR bytes_transferred)
{
  UNREFERENCED_PARAMETER(bytes_transferred);

  uint8_t* reuse_block = nullptr;

  if (_cancelled)
    error_code = ERROR_CANCELLED;

  if (error_code != ERROR_SUCCESS)
  {
    _error = error_code;
    reuse_block = _block;
    _block = nullptr;
    BlockReset(reuse_block);
    Finish();
  }
  else
  {
    AddToHashQueue();
  }

  ProcessReadQueue(reuse_block);
}

void FileHashTask::AddToHashQueue()
{
  assert(_block);

  _hash_start_counter.store(k_hashers_count, std::memory_order_relaxed);
  _hash_finish_counter.store(k_hashers_count, std::memory_order_relaxed);

  for (auto i = 0u; i < k_hashers_count; ++i)
    SubmitThreadpoolWork(_threadpool_hash_work);
}

void FileHashTask::DoHashRound()
{
  const auto ctx_index = --_hash_start_counter;
  const auto ctx = &_hash_contexts[ctx_index];
  const auto block_size = GetCurrentBlockSize();
  const auto ret = mbedtls_md_update(ctx, _block, block_size);
  assert(ret == 0);
  const auto locks_on_this = --_hash_finish_counter;
  if (locks_on_this == 0)
    FinishedBlock();
}

void FileHashTask::FinishedBlock()
{
  _current_offset += GetCurrentBlockSize();
  auto reuse_block = _block;
  _block = nullptr;
  BlockReset(reuse_block);

  if (GetCurrentBlockSize() > 0 && !_cancelled)
  {
    ReadBlockAsync(reuse_block);
    reuse_block = nullptr;
  }
  else
  {
    if (_cancelled)
      _error = ERROR_CANCELLED;
    Finish();
  }

  ProcessReadQueue(reuse_block);
}

void FileHashTask::Finish()
{
  if (!_error)
  {
    // If we expect a hash but none match, write no match to all algos
    _match_state = _expected_hash.empty() ? MatchState_None : MatchState_Mismatch;

    for (auto i = 0u; i < k_hashers_count; ++i)
    {
      auto& it_ctx = _hash_contexts[i];
      auto& it_result = _hash_results[i];
      const auto hash_size = mbedtls_md_get_size(it_ctx.md_info);
      it_result.resize(hash_size);
      const auto ret = mbedtls_md_finish(&it_ctx, it_result.data());
      assert(ret == 0);
      if (_match_state == MatchState_Mismatch && it_result == _expected_hash)
        _match_state = i;
    }
  }

  _prop_page->FileCompletionCallback(this);
}
