#pragma once
#include "utils.h"
#include "address_format.h"
#include "portable_executable.h"
#include <tchar.h>

using namespace Aether;
using namespace Aether::BranchTracer;

/*  test_branch_trace.h:  Trace a function until return and log APIs called from the thread.	*/

std::vector<BranchLog::LogEntry> traced_branches;

void BranchLogFullHook()
{
	std::cout << "Branch Log is full!, "
		<< "log_buffer->info.buffer 0x" << std::hex << BranchTracer::log_buffer->info.buffer
		<< " log_buffer->info.buffer_idx " << BranchTracer::log_buffer->info.buffer_idx << "\n";

	traced_branches.insert(traced_branches.end(), log_buffer->info.buffer,
		log_buffer->info.buffer + log_buffer->info.buffer_idx);
}

void BranchTraceFinished()
{
	std::cout << "Finished tracing Foo()! dumping branch log! \n";

	for (auto entry : traced_branches)
	{
		std::cout << "branch " << AddressInfo{ (void*)entry.branch_address }.Format() 
            << " -> " << AddressInfo{ (void*)entry.branch_target }.Format() << "\n";
	}
}

#pragma optimize("", off)
void Foo(int x, int y, int z) 
{
  for (int i = 0; i < x; i++) {
    if (i % 2 == 0) {
      if (y > 0) {
        if (z % 2 == 0) {
          OutputDebugString(_T("i is even, y is positive, and z is even"));
        } else {
          OutputDebugString(_T("i is even, y is positive, and z is odd"));
        }
      } else {
        if (z % 2 == 0) {
          OutputDebugString(_T("i is even, y is not positive, and z is even"));
        } else {
          OutputDebugString(_T("i is even, y is not positive, and z is odd"));
        }
      }
    } else {
      if (y > 0) {
        if (z % 2 == 0) {
          MessageBox(NULL, _T("i is odd, y is positive, and z is even"), _T(""), MB_OK);
        } else {
          MessageBox(NULL, _T("i is odd, y is positive, and z is odd"), _T(""), MB_OK);
        }
      } else {
        if (z % 2 == 0) {
          MessageBox(NULL, _T("i is odd, y is not positive, and z is even"), _T(""), MB_OK);
        } else {
          MessageBox(NULL, _T("i is odd, y is not positive, and z is odd"), _T(""), MB_OK);
        }
      }
    }
  }
}
#pragma optimize("", on)


/*	trace the test function	*/

void BranchTraceTest()
{
	auto exe_base = (uintptr_t)GetModuleHandle(NULL);

	Aether::SetCallback(Aether::branch_log_full, BranchLogFullHook);
	Aether::SetCallback(Aether::branch_trace_finished, BranchTraceFinished);

    /*  intercept the next function call of Foo */

	BranchTracer::Trace(
		(uint8_t*)Foo, exe_base, PE_HEADER(exe_base)->OptionalHeader.SizeOfImage);

    srand(time(NULL));

    Foo((rand() * 100) % 8, (rand() * 100) % 6, (rand() * 100) % 4);
}