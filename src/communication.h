#pragma once
#include <Uefi.h>
#include <PiSmm.h>
#include <Protocol/SmmCpu.h>

#define SMM_PROTOCOL_MAGIC 'i'

#define READ_BUFFER_SIZE 8192

#define MAGIC_OFFSET 0
#define PROCESS_OFFSET 1
#define MODULE_OFFSET 31
#define MEMORY_OFFSET 91
#define READ_SIZE_OFFSET 99
#define READ_BUFFER_OFFSET 107
#define SMI_COUNT_OFFSET 8299

#define RAW_MODULE_COMMAND_NAME L"RAW"
#define BATCH_COMMAND_NAME      L"BATCH"

#pragma pack(push, 1)

typedef struct _EntryResult {
    int id;
    int value_a;
    int group;
    float x, y, z;
} EntryResult;

typedef struct _DemoResponse {
    int localGroup;
    float localX, localY, localZ;
    float localAngleY;
    int entryCount;
    EntryResult entries[64];
} DemoResponse;

#pragma pack(pop)

#pragma pack(1)
typedef struct _SmmCommunicationProtocol
{
    UINT8 magic;
    UINT8 process_name[30];
    UINT16 module_name[30];
    UINT64 offset;
    UINT64 read_size;
    UINT8 read_buffer[READ_BUFFER_SIZE];
    UINT64 smi_count;

} SmmCommunicationProtocol, * PSmmCommunicationProtocol;
#pragma pack()

UINT64 GetCommunicationProcess();
EFI_STATUS PerformCommunication();
