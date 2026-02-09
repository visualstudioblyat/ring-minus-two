#include "communication.h"
#include "windows.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

#include <Library/UefiLib.h>
#include <Protocol/SmmCpu.h>
#include <Uefi.h>
#include <Protocol/SmmCpu.h>
#include <PiSmm.h>

int _fltused = 0;

#define OFFSET_LOCAL_ENTRY    0xDEAD0001
#define OFFSET_ENTRY_LIST     0xDEAD0002

#define OFFSET_VALUE_A        0x100
#define OFFSET_GROUP          0x104
#define OFFSET_ORIGIN         0x200
#define OFFSET_ANGLES         0x300
#define OFFSET_ENTRY_HANDLE   0x400

#define ENTRY_STRIDE          112

#define READ32(addr) ReadVirtual32(addr, tcr3)
#define READ64(addr) ReadVirtual64(addr, tcr3)

UINT64 SmiCountIndex = 0;

static UINT64 CachedTargetProcess = 0;
static UINT64 CachedTargetCr3 = 0;
static UINT64 FailCooldown = 0;

UINT64 GetCommunicationProcess()
{
    return GetWindowsEProcess((char*)"SmiUM.exe");
}

EFI_STATUS PerformCommunication()
{
    SmiCountIndex++;

    UINT64 kernel = GetWindowsKernelBase();
    UINT64 kcr3 = GetWindowsKernelCr3();
    if (!kernel || !kcr3)
    {
        return EFI_NOT_FOUND;
    }

    UINT64 cprocess = GetCommunicationProcess();
    if (cprocess)
    {
        UINT64 base = GetWindowsBaseAddressModuleX64(cprocess, L"SmiUM.exe");

        if (base)
        {
            UINT64 section = GetWindowsSectionBaseAddressX64(cprocess, base, (unsigned char*)".rango");

            if (section)
            {
                SmmCommunicationProtocol protocol = { 0 };
                ReadVirtual(section + 0b0, GetWindowsProcessCr3(cprocess), (UINT8*)&protocol, (UINT64)sizeof(SmmCommunicationProtocol));

                if (protocol.magic != SMM_PROTOCOL_MAGIC)
                {
                    return EFI_NOT_FOUND;
                }

                if (CachedTargetProcess == 0)
                {
                    if (FailCooldown > 0)
                    {
                        FailCooldown--;
                        return EFI_SUCCESS;
                    }

                    CachedTargetProcess = GetWindowsEProcess("example.exe");

                    if (CachedTargetProcess != 0)
                    {
                        CachedTargetCr3 = GetWindowsProcessCr3(CachedTargetProcess);
                        FailCooldown = 0;
                        SERIAL_PRINT("SMM: Target process found! Locking on.\r\n");
                    }
                    else
                    {
                        FailCooldown = 60;
                    }
                }

                if (CachedTargetProcess == 0)
                {
                    return EFI_SUCCESS;
                }

                UINT64 tprocess = CachedTargetProcess;
                UINT64 tcr3 = CachedTargetCr3;
                UINT64 tbase = 0;

                if (WcsCmpi(protocol.module_name, BATCH_COMMAND_NAME) == 0)
                {
                    UINT64 moduleBase = GetWindowsBaseAddressModuleX64(tprocess, L"example.dll");

                    if (moduleBase != 0)
                    {
                        DemoResponse* response = (DemoResponse*)protocol.read_buffer;
                        response->entryCount = 0;

                        UINT64 localEntry = READ64(moduleBase + OFFSET_LOCAL_ENTRY);
                        if (localEntry != 0)
                        {
                            response->localGroup = READ32(localEntry + OFFSET_GROUP);
                            ReadVirtual(localEntry + OFFSET_ORIGIN, tcr3, (UINT8*)&response->localX, 12);

                            float angles[3];
                            ReadVirtual(localEntry + OFFSET_ANGLES, tcr3, (UINT8*)angles, 12);
                            response->localAngleY = angles[1];
                        }

                        UINT64 entryList = READ64(moduleBase + OFFSET_ENTRY_LIST);
                        UINT64 listEntry = READ64(entryList + 0x10);

                        if (listEntry != 0)
                        {
                            for (int i = 0; i < 64; i++)
                            {
                                UINT64 controller = READ64(listEntry + (i * ENTRY_STRIDE));
                                if (controller == 0) continue;

                                int val = READ32(controller + OFFSET_VALUE_A);
                                if (val <= 0 || val > 100) continue;

                                int group = READ32(controller + OFFSET_GROUP);

                                UINT32 entryHandle = READ32(controller + OFFSET_ENTRY_HANDLE);
                                UINT64 entryPtr = 0;

                                if ((entryHandle & 0x7FFF) > 0 && (entryHandle & 0x7FFF) < 512) {
                                     entryPtr = READ64(listEntry + ENTRY_STRIDE * (entryHandle & 0x1FF));
                                }

                                if (entryPtr != 0)
                                {
                                    EntryResult* ent = &response->entries[response->entryCount];
                                    ent->id = i;
                                    ent->value_a = val;
                                    ent->group = group;

                                    ReadVirtual(entryPtr + OFFSET_ORIGIN, tcr3, (UINT8*)&ent->x, 12);

                                    response->entryCount++;
                                }
                            }
                        }
                        protocol.read_size = sizeof(DemoResponse);
                    }
                    else
                    {
                         CachedTargetProcess = 0;
                         CachedTargetCr3 = 0;
                         protocol.read_size = 0;
                         return EFI_NOT_FOUND;
                    }
                }

                else
                {
                    if (WcsCmpi(protocol.module_name, RAW_MODULE_COMMAND_NAME) == 0) {
                        tbase = 0;
                    } else {
                        tbase = GetWindowsBaseAddressModuleX64(tprocess, protocol.module_name);
                        if (tbase == 0) {
                            CachedTargetProcess = 0;
                            CachedTargetCr3 = 0;
                            return EFI_NOT_FOUND;
                        }
                    }
                    ReadVirtual(tbase + protocol.offset, tcr3, protocol.read_buffer, protocol.read_size);
                }

                *(UINT64*)(TranslateVirtualToPhysical(GetWindowsProcessCr3(cprocess), section + SMI_COUNT_OFFSET)) = SmiCountIndex;

                UINT64 temp = TranslateVirtualToPhysical(GetWindowsProcessCr3(cprocess), section + READ_BUFFER_OFFSET);
                for (UINT64 i = 0; i < protocol.read_size; i++) {
                    *(UINT8*)(temp + i) = protocol.read_buffer[i];
                }
            }
        }
    }
    return EFI_SUCCESS;
}
