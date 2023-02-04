#include "vmexit.h"
#include "npt_sandbox.h"
#include "branch_tracer.h"

void VcpuData::BreakpointHandler(GuestRegisters* guest_ctx)
{
    auto vmroot_cr3 = __readcr3();

    __writecr3(guest_vmcb.save_state_area.cr3.Flags);

    auto guest_rip = guest_vmcb.save_state_area.rip;

    if (BranchTracer::initialized && guest_rip == BranchTracer::start_address && !BranchTracer::thread_id)
    {
        NptHooks::ForEachHook(
            [](auto hook_entry, auto data) -> auto {

                if (hook_entry->address == data)
                {
                    DbgPrint("hook_entry->address == data, found stealth breakpoint! \n");

                    UnsetHook(hook_entry);
                }
                return false;
            },
            (void*)guest_rip
         );

        /*  clean TLB after removing the NPT hook   */

        guest_vmcb.control_area.vmcb_clean &= 0xFFFFFFEF;
        guest_vmcb.control_area.tlb_control = 1;

        /*  capture the ID of the target thread & start the tracer  */

        BranchTracer::Start(this);
    }
    else
    {
        InjectException(EXCEPTION_VECTOR::Breakpoint, FALSE, 0);
    }

    __writecr3(vmroot_cr3);
}