#include "npt_hook.h"
#include "logging.h"
#include "paging_utils.h"
#include "disassembly.h"
#include "portable_executable.h"
#include "vmexit.h"
#include "utils.h"
#include "shellcode.h"

Hooks::JmpRipCode hk_MmCleanProcessAddressSpace;

namespace NptHooks
{
	int hook_count;
	NptHook first_npt_hook;

	__int64 __fastcall MmCleanProcessAddressSpace_hook(__int64 a1, __int64 a2) 
	{
		/*	unset all NPT hooks for this process	*/
		RemoveHook(__readcr3());

		return static_cast<decltype(&MmCleanProcessAddressSpace_hook)>(hk_MmCleanProcessAddressSpace.original_bytes)(a1, a2);
	}


	void PageSynchronizationPatch()
	{
		size_t nt_size = NULL;
		auto ntoskrnl = Utils::GetDriverBaseAddress(&nt_size, RTL_CONSTANT_STRING(L"ntoskrnl.exe"));

		auto pe_hdr = PeHeader(ntoskrnl);

		auto section = (IMAGE_SECTION_HEADER*)(pe_hdr + 1);

		for (int i = 0; i < pe_hdr->FileHeader.NumberOfSections; ++i)
		{
			if (!strcmp((char*)section[i].Name, ".text"))
			{
				uint8_t* start = section[i].VirtualAddress + (uint8_t*)ntoskrnl;
				uint8_t* end = section[i].Misc.VirtualSize + start;

				DbgPrint("start %p \n", start);
				DbgPrint("end %p \n", end);


				Disasm::ForEachInstruction(start, end, [](uint8_t* insn_addr, ZydisDecodedInstruction insn) -> void {

					ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];

					if (insn.mnemonic == ZYDIS_MNEMONIC_JNZ)
					{
						Disasm::Disassemble((uint8_t*)insn_addr, operands);

						auto jmp_target = Disasm::GetJmpTarget(insn, operands, (ZyanU64)insn_addr);

						size_t instruction_size = NULL;

						for (auto instruction = jmp_target; instruction < jmp_target + 0x40; instruction = instruction + instruction_size)
						{
							//DbgPrint("instruction path %p \n", instruction);

							ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];

							auto insn2 = Disasm::Disassemble((uint8_t*)instruction, operands);

							if (insn2.mnemonic == ZYDIS_MNEMONIC_MOV)
							{
								// mov edx, 403h MEMORY_MANAGEMENT page sync

								if ((operands[1].imm.value.u == 0x403) || (operands[1].imm.value.u == 0x411))
								{
									size_t nt_size = NULL;
									auto ntoskrnl = Utils::GetDriverBaseAddress(&nt_size, RTL_CONSTANT_STRING(L"ntoskrnl.exe"));

									DbgPrint("found MEMORY_MANAGEMENT jnz at +0x%p \n", insn_addr - (uint8_t*)ntoskrnl);

									PageUtils::LockPages(PAGE_ALIGN(insn_addr), IoReadAccess);

									Utils::ForEachCore([](void* instruction_addr) -> void {

										LARGE_INTEGER length_tag;
										length_tag.LowPart = NULL;
										length_tag.HighPart = 6;

										svm_vmmcall(VMMCALL_ID::set_npt_hook, instruction_addr, "\x90\x90\x90\x90\x90\x90", length_tag.QuadPart);

									}, insn_addr);
								}
							}

							instruction_size = insn2.length;
						}
					}
				});
			}
		}

		/*	place a callback on MmCleanProcessAddressSpace to remove npt hooks inside terminating processes, to prevent PFN check bsods	*/

		auto MmCleanProcessAddressSpace_ref = Utils::FindPattern((uintptr_t)ntoskrnl, nt_size, "\xE8\x00\x00\x00\x00\x33\xD2\x48\x8D\x4C\x24\x00\xE8\x00\x00\x00\x00\x4C\x39\xBE\x00\x00\x00\x00", 24, 0x00);
		auto MmCleanProcessAddressSpace = RELATIVE_ADDR(MmCleanProcessAddressSpace_ref, 1, 5);

		hk_MmCleanProcessAddressSpace = Hooks::JmpRipCode{ (uintptr_t)MmCleanProcessAddressSpace, (uintptr_t)MmCleanProcessAddressSpace_hook };

		Utils::ForEachCore([](void* params) -> void {

			auto jmp_rip_code = (Hooks::JmpRipCode*)params;

			LARGE_INTEGER length_tag;
			length_tag.LowPart = NULL;
			length_tag.HighPart = jmp_rip_code->hook_size;

		//	svm_vmmcall(VMMCALL_ID::set_npt_hook, jmp_rip_code->hook_addr, jmp_rip_code->hook_code, length_tag.QuadPart);

			//auto irql = Utils::DisableWP();
			//memcpy((void*)jmp_rip_code->hook_addr, jmp_rip_code->hook_code, length_tag.HighPart);
			//Utils::EnableWP(irql);

		}, &hk_MmCleanProcessAddressSpace);
	}


	void Init()
	{
		auto first_hookless_page = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'ENON');

		CR3 cr3;
		cr3.Flags = __readcr3();

		/*	reserve memory for hooks because we can't allocate memory in VM root	*/

		int max_hooks = 500;
		
		NptHook* hook_entry = &first_npt_hook;

		for (int i = 0; i < max_hooks; hook_entry = hook_entry->next_hook, ++i)
		{
			auto hooked_page = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'ENON');

			hook_entry->hooked_pte = PageUtils::GetPte(hooked_page, cr3.AddressOfPageDirectory << PAGE_SHIFT);

			auto copy_page = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'ENON');

			hook_entry->copy_pte = PageUtils::GetPte(copy_page, cr3.AddressOfPageDirectory << PAGE_SHIFT);
			
			hook_entry->next_hook = (NptHook*)ExAllocatePool(NonPagedPool, sizeof(NptHook));
		}
		hook_count = 0;
	}

	NptHook* ForEachHook(bool(HookCallback)(NptHook* hook_entry, void* data), void* callback_data)
	{
		auto hook_entry = &first_npt_hook;

		for (int i = 0; i < hook_count; hook_entry = hook_entry->next_hook, ++i)
		{
			if (HookCallback(hook_entry, callback_data))
			{
				return hook_entry;
			}
		}
		return 0;
	}

	void RemoveHook(int32_t tag)
	{
		ForEachHook(
			[](NptHook* hook_entry, void* data)-> bool {

				if ((int32_t)data == hook_entry->tag)
				{
					/*	TO DO: restore guest PFN, free and unlink hook from list	*/

					hook_entry->tag = 0;
					hook_entry->hookless_npte->ExecuteDisable = 0;
					hook_entry->original_pte->PageFrameNumber = hook_entry->original_pfn;

					return true;
				}

			}, (void*)tag
				);
	}

	void RemoveHook(uintptr_t current_cr3)
	{
		__debugbreak();
		ForEachHook(
			[](NptHook* hook_entry, void* data)-> bool {

				DbgPrint("hook entry cr3 %p \n", hook_entry->process_cr3);
				DbgPrint("current cr3 %p \n", __readcr3());

				if ((uintptr_t)data == hook_entry->process_cr3)
				{
					DbgPrint("hook_entry found %p \n", hook_entry);

					/*	TO DO: restore guest PFN, free and unlink hook from list	*/

					hook_entry->tag = 0;
					hook_entry->hookless_npte->ExecuteDisable = 0;
					hook_entry->original_pte->PageFrameNumber = hook_entry->original_pfn;

					return true;
				}

			}, (void*)current_cr3
		);
	}

	NptHook* SetNptHook(CoreData* vmcb_data, void* address, uint8_t* patch, size_t patch_len, int32_t tag)
	{
		auto hook_entry = &first_npt_hook;

		for (int i = 0; i < hook_count; hook_entry = hook_entry->next_hook, ++i)
		{
		}

		hook_entry->tag = tag;
		hook_entry->process_cr3 = vmcb_data->guest_vmcb.save_state_area.Cr3;

		auto vmroot_cr3 = __readcr3();

		__writecr3(vmcb_data->guest_vmcb.save_state_area.Cr3);

		/*	Sometimes we want to place a hook in a globally mapped DLL like user32.dll, ntdll.dll, etc. but we only want our hook to exist in one context.
			set the guest pte to point to a new copy page, to prevent the hook from being globally mapped.	*/
		if (vmroot_cr3 != vmcb_data->guest_vmcb.save_state_area.Cr3)
		{
			auto guest_pte = PageUtils::GetPte((void*)address, vmcb_data->guest_vmcb.save_state_area.Cr3);

			hook_entry->original_pte = guest_pte;
			hook_entry->original_pfn = guest_pte->PageFrameNumber;

			guest_pte->PageFrameNumber = hook_entry->copy_pte->PageFrameNumber;

			memcpy(PageUtils::VirtualAddrFromPfn(hook_entry->copy_pte->PageFrameNumber), PAGE_ALIGN(address), PAGE_SIZE);
		}

		/*	get the guest pte and physical address of the hooked page	*/

		auto physical_page = PAGE_ALIGN(MmGetPhysicalAddress(address).QuadPart);

		hook_entry->guest_phys_addr = (uint8_t*)physical_page;

		/*	get the nested pte of the guest physical address	*/

		hook_entry->hookless_npte = PageUtils::GetPte((void*)physical_page, Hypervisor::Get()->normal_ncr3);
		hook_entry->hookless_npte->ExecuteDisable = 1;


		/*	get the nested pte of the guest physical address in the 2nd NCR3, and map it to our hook page	*/

		auto hooked_npte = PageUtils::GetPte((void*)physical_page, Hypervisor::Get()->noexecute_ncr3);

		hooked_npte->PageFrameNumber = hook_entry->hooked_pte->PageFrameNumber;
		hooked_npte->ExecuteDisable = 0;
		
		auto hooked_copy = PageUtils::VirtualAddrFromPfn(hooked_npte->PageFrameNumber);

		auto page_offset = (uintptr_t)address & (PAGE_SIZE - 1);


		/*	place our hook in the copied page for the 2nd NCR3	*/

		memcpy(hooked_copy, PAGE_ALIGN(address), PAGE_SIZE);
		memcpy((uint8_t*)hooked_copy + page_offset, patch, patch_len);

		/*	SetNptHook epilogue	*/

		vmcb_data->guest_vmcb.control_area.TlbControl = 3;

		__writecr3(vmroot_cr3);

		hook_count += 1;

		return hook_entry;
	}

};
