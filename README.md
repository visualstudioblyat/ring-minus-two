# ring-minus-two

built on top of Oliver's [SmmInfect](https://github.com/Oliver-1-1/SmmInfect). his project is an SMM backdoor that reads arbitrary process memory from ring -2 and communicates results to usermode through a shared PE section. I took that foundation and built a real-time data pipeline on top of it - batch reads, persistent connections, multithreaded usermode with WebSocket output.

## what I added

Oliver's original is a proof of concept - it reads 15 bytes of explorer.exe in a while loop and prints hex to console. the protocol has a 30-byte buffer and recreates the TBS context on every single SMI. I rewrote essentially everything above the driver core:

**SMM side (`communication.c/h`)**
- expanded the read buffer from 30 bytes to 8KB to support structured batch responses
- added two command modes: `BATCH` (read multiple entries in a single SMI, returns a packed `DemoResponse` struct) and `RAW` (read arbitrary address + size, no module base resolution)
- process/CR3 caching so we're not walking the EPROCESS list every SMI
- tick-based fail cooldown to prevent SMI spam when target process isn't running
- new protocol structs (`EntryResult`, `DemoResponse`) for typed batch data
- float support (`_fltused`) for reading positional data

**usermode (`entry.cpp`) - full rewrite**
- Oliver's: single-threaded while loop, 15 bytes of explorer.exe, printf, recreates TBS context every call. ~150 lines
- mine: persistent TBS context, two threads (read loop + WebSocket broadcast), round-robin read strategy that spreads 64 slots across 4 ticks to reduce SMI load, full discovery cycles every 60 ticks, websocketpp server broadcasting JSON at 25hz, signal handling for clean shutdown, local IP detection. ~450 lines
- PE section renamed from `.ZEPTA` to `.rango`

## what Oliver built (SmmInfect core)

everything that runs in SMM ring -2:

- `smm_entry.c` - SMI root handler registration through UEFI's SMM dispatch protocol
- `memory.c/h` - physical memory mapping via `gSmst->SmmAllocatePages`, virtual-to-physical translation by walking x86-64 page tables (PML4 > PDPT > PD > PT), handles large pages (2MB/1GB)
- `windows.c/h` - finds ntoskrnl base by scanning backwards from RIP for MZ/PE headers, resolves kernel exports (`PsInitialSystemProcess`, `PsGetProcessImageFileName`) to walk the EPROCESS linked list, extracts DirectoryTableBase (CR3) from KPROCESS, PEB/LDR module enumeration for finding DLL bases
- `linux.c/h` + `linuxdriver.c` - linux kernel base via `LSTAR` MSR, kallsyms-style symbol lookup, kernel module for SMI trigger through EFI runtime variables
- `serial.c/h`, `string.c/h` - debug serial output and string comparison for SMM (no stdlib available)
- `cache.nasm` - `wbinvd`, CR0 manipulation for cache disable/enable

credits to Oliver and his mentor Ekknod.

## file layout

```
src/
  smm_entry.c               SMI handler registration
  memory.c/h                physical/virtual memory reads, page table walking
  windows.c/h               kernel introspection - CR3, EPROCESS, PEB, exports
  linux.c/h                 linux kernel support (partial)
  communication.c/h         SMM <-> usermode protocol, batch + raw read modes
  serial.c/h                debug output over serial
  string.c/h                string utils for SMM (no stdlib)
  entry.cpp                 usermode controller - TPM trigger + WebSocket server
  linuxdriver.c             linux kernel module for SMI trigger via EFI vars
  cache.nasm                cache management (wbinvd, CR0)
  SmmInfect.inf/.dsc        EDK2 build configs
```

## build

the SMM driver builds with EDK2. the usermode app needs websocketpp, asio, and nlohmann/json.

## disclaimer

research/educational project exploring firmware-level memory access. the offsets in the code are placeholder values. use responsibly.
