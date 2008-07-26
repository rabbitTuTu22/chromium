// Copyright 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Wow_helper.exe is a simple Win32 64-bit executable designed to help to
// sandbox a 32 bit application running on a 64 bit OS. The basic idea is to
// perform a 64 bit interception of the target process and notify the 32-bit
// broker process whenever a DLL is being loaded. This allows the broker to
// setup the interceptions (32-bit) properly on the target.

#include <windows.h>

#include <string>

#include "base/logging.h"
#include "sandbox/wow_helper/service64_resolver.h"
#include "sandbox/wow_helper/target_code.h"

namespace {

// Grabbed from chrome/common/string_util.h
template <class char_type>
inline char_type* WriteInto(
    std::basic_string<char_type, std::char_traits<char_type>,
                      std::allocator<char_type> >* str,
    size_t length_including_null) {
  str->reserve(length_including_null);
  str->resize(length_including_null - 1);
  return &((*str)[0]);
}

// Grabbed from chrome/common/string_util.cc
std::string WideToMultiByte(const std::wstring& wide, UINT code_page) {
  if (wide.length() == 0)
    return std::string();

  // compute the length of the buffer we'll need
  int charcount = WideCharToMultiByte(code_page, 0, wide.c_str(), -1,
                                      NULL, 0, NULL, NULL);
  if (charcount == 0)
    return std::string();

  // convert
  std::string mb;
  WideCharToMultiByte(code_page, 0, wide.c_str(), -1,
                      WriteInto(&mb, charcount), charcount, NULL, NULL);

  return mb;
}

// Grabbed from chrome/common/string_util.cc
std::string WideToUTF8(const std::wstring& wide) {
  return WideToMultiByte(wide, CP_UTF8);
}

}  // namespace

namespace sandbox {

// Performs the interception of NtMapViewOfSection on the 64-bit version of
// ntdll.dll. 'thunk' is the buffer on the address space of process 'child',
// that will be used to store the information about the patch.
int PatchNtdll(HANDLE child, void* thunk, size_t thunk_bytes) {
  wchar_t* ntdll_name = L"ntdll.dll";
  HMODULE ntdll_base = ::GetModuleHandle(ntdll_name);
  if (!ntdll_base)
    return 100;

  Service64ResolverThunk resolver(child);
  size_t used = resolver.GetThunkSize();
  char* code = reinterpret_cast<char*>(thunk) + used;
  NTSTATUS ret = resolver.Setup(ntdll_base, NULL, "NtMapViewOfSection", NULL,
                                code, thunk, thunk_bytes, NULL);
  if (!NT_SUCCESS(ret))
    return 101;

  size_t size = reinterpret_cast<char*>(&TargetEnd) -
                reinterpret_cast<char*>(&TargetNtMapViewOfSection);

  if (size + used > thunk_bytes)
    return 102;

  SIZE_T written;
  if (!::WriteProcessMemory(child, code, &TargetNtMapViewOfSection, size,
                            &written))
    return 103;

  if (size != written)
    return 104;

  return 0;
}

}  // namespace sandbox

// We must receive two arguments: the process id of the target to intercept and
// the address of a page of memory on that process that will be used for the
// interception. We receive the address because the broker will cleanup the
// patch when the work is performed.
//
// It should be noted that we don't wait until the real work is done; this
// program quits as soon as the 64-bit interception is performed.
int wWinMain(HINSTANCE, HINSTANCE, wchar_t* command_line, int) {
  COMPILE_ASSERT(sizeof(void*) > sizeof(DWORD), unsupported_32_bits);
  if (!command_line)
    return 1;

  wchar_t* next;
  DWORD process_id = wcstoul(command_line, &next, 0);
  if (!process_id)
    return 2;

  DWORD access = PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
  HANDLE child = ::OpenProcess(access, FALSE, process_id);
  DCHECK(child);
  if (!child)
    return 3;

  DWORD buffer = wcstoul(next, NULL, 0);
  if (!buffer)
    return 4;

  void* thunk = reinterpret_cast<void*>(static_cast<ULONG_PTR>(buffer));

  const size_t kPageSize = 4096;
  return sandbox::PatchNtdll(child, thunk, kPageSize);
}
