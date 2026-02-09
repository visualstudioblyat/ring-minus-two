#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <winnt.h>
#include <winternl.h>
#include <tbs.h>
#include <iostream>
#include <vector>
#include <math.h>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <thread>
#include <csignal>
#include <set>

#pragma comment(lib, "tbs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "user32.lib")

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_
#include <asio.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/common/functional.hpp>
#include <nlohmann/json.hpp>

using server = websocketpp::server<websocketpp::config::asio>;
using connection_hdl = websocketpp::connection_hdl;
using message_ptr = server::message_ptr;
using json = nlohmann::json;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

const unsigned short WEBSOCKET_PORT = 8765;

server g_ws_server;
std::set<connection_hdl, std::owner_less<connection_hdl>> g_connections;
std::mutex g_connection_mutex;
std::atomic<bool> g_run_program = { true };

class DemoReader;
void broadcastmsg(const std::string& message);
void o_open(connection_hdl hdl);
void o_close(connection_hdl hdl);
void o_msg(connection_hdl hdl, message_ptr msg, DemoReader* reader_ptr);
std::vector<std::string> getlocalip();
void s_handler(int signum);

#define RAW_MODULE_COMMAND_NAME L"RAW"
#define TARGET_MODULE L"example.dll"

#define DUMMY_PROCESS_NAME ""

#define OFFSET_LOCAL_ENTRY    0xDEAD0001
#define OFFSET_ENTRY_LIST     0xDEAD0002
#define OFFSET_VALUE_A        0x100
#define OFFSET_GROUP          0x104
#define OFFSET_ORIGIN         0x200
#define OFFSET_ANGLES         0x300
#define OFFSET_ENTRY_HANDLE   0x400
#define ENTRY_STRIDE          112

#pragma pack(push, 1)
typedef struct _SmmCommunicationProtocol
{
  UINT8 magic = 'i';
  UINT8 process_name[30] = { 0 };
  UINT16 module_name[30] = { 0 };
  UINT64 offset = 0;
  UINT64 read_size = 0;

#define READ_BUFFER_SIZE 8192
  UINT8 read_buffer[READ_BUFFER_SIZE] = { 0 };

  UINT64 smi_count = 1;
} SmmCommunicationProtocol;
#pragma pack(pop)

#pragma section(".rango", read, write)
__declspec(allocate(".rango")) volatile SmmCommunicationProtocol protocol;

struct Vector3 { float x, y, z; };
struct EntryData {
  int id;
  Vector3 pos;
  int value_a;
  int group;
  UINT64 cached_ptr;
};

TBS_HCONTEXT g_hTbsContext = 0;

EntryData g_all_slots[64 + 1] = { 0 };

bool InitTbsContext();
void CloseTbsContext();
void TriggerSmiTpmAcpi();
void PerformSmmRead(const char* target_proc, const wchar_t* module_name, UINT64 offset, UINT64 size);
UINT64 GetEntryAddressFromHandle(UINT64 entry_list, UINT32 handle);

class DemoReader {
private:
  std::vector<EntryData> g_entries;
  Vector3 g_localPos = { 0, 0, 0 };
  Vector3 g_localAngles = { 0, 0, 0 };
  int g_localGroup = 0;

  std::mutex data_mutex_;

  const int READ_INTERVAL_MS = 500;

  std::thread read_loop_thread;
  std::thread ws_send_thread;

  void read_loop() {
    int ticks = 0;
    int round_robin_index = 0;

    while (g_run_program.load(std::memory_order_acquire))
    {
      bool do_full_discovery = (ticks % 60 == 0);
      ticks++;

      Vector3 localPos = { 0, 0, 0 };
      Vector3 localAngles = { 0, 0, 0 };

      UINT64 local_entry = 0;
      PerformSmmRead(DUMMY_PROCESS_NAME, TARGET_MODULE, OFFSET_LOCAL_ENTRY, sizeof(UINT64));
      memcpy(&local_entry, (void*)protocol.read_buffer, sizeof(UINT64));

      if (local_entry != 0)
      {
        if (do_full_discovery || g_localGroup == 0) {
          PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, local_entry + OFFSET_GROUP, sizeof(int));
          memcpy(&g_localGroup, (void*)protocol.read_buffer, sizeof(int));
        }

        PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, local_entry + OFFSET_ORIGIN, sizeof(Vector3));
        memcpy(&localPos, (void*)protocol.read_buffer, sizeof(Vector3));

        PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, local_entry + OFFSET_ANGLES, sizeof(Vector3));
        memcpy(&localAngles, (void*)protocol.read_buffer, sizeof(Vector3));

        if (do_full_discovery)
        {
          UINT64 entry_list = 0;
          PerformSmmRead(DUMMY_PROCESS_NAME, TARGET_MODULE, OFFSET_ENTRY_LIST, sizeof(UINT64));
          memcpy(&entry_list, (void*)protocol.read_buffer, sizeof(UINT64));

          UINT64 chunk_0_ptr = 0;
          PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, entry_list + 0x10, sizeof(UINT64));
          memcpy(&chunk_0_ptr, (void*)protocol.read_buffer, sizeof(UINT64));

          if (chunk_0_ptr != 0)
          {
            PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, chunk_0_ptr, 64 * ENTRY_STRIDE);

            UINT64 controller_pointers[64] = { 0 };
            for (int i = 0; i < 64; i++) {
              memcpy(&controller_pointers[i], (void*)(protocol.read_buffer + (i * ENTRY_STRIDE)), sizeof(UINT64));
            }

            for (int i = 0; i < 64; i++)
            {
              if (controller_pointers[i] == 0) {
                g_all_slots[i].cached_ptr = 0;
                g_all_slots[i].value_a = 0;
                continue;
              }

              PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, controller_pointers[i] + OFFSET_GROUP, 1312);

              int group = 0;
              UINT32 entry_h = 0;
              memcpy(&group, (void*)(protocol.read_buffer), sizeof(int));
              memcpy(&entry_h, (void*)(protocol.read_buffer + (OFFSET_ENTRY_HANDLE - OFFSET_GROUP)), sizeof(UINT32));

              entry_h = entry_h & 0x7FFF;

              if (entry_h == 0) {
                g_all_slots[i].cached_ptr = 0;
                g_all_slots[i].value_a = 0;
                continue;
              }

              UINT64 entry_addr = 0;
              if ((entry_h & 0x7FFF) < 512) {
                UINT64 addr = chunk_0_ptr + 112 * (entry_h & 0x1FF);
                PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, addr, sizeof(UINT64));
                memcpy(&entry_addr, (void*)protocol.read_buffer, sizeof(UINT64));
              }
              else {
                entry_addr = GetEntryAddressFromHandle(entry_list, entry_h);
              }

              g_all_slots[i].cached_ptr = entry_addr;
              g_all_slots[i].group = group;
              g_all_slots[i].id = i;
            }
          }
        }

        int start_idx = round_robin_index * 16;
        int end_idx = start_idx + 16;

        round_robin_index++;
        if (round_robin_index >= 4) round_robin_index = 0;

        for (int i = start_idx; i < end_idx; i++)
        {
          if (g_all_slots[i].cached_ptr != 0)
          {
            UINT64 entry_addr = g_all_slots[i].cached_ptr;

            int val = 0;
            PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, entry_addr + OFFSET_VALUE_A, sizeof(int));
            memcpy(&val, (void*)protocol.read_buffer, sizeof(int));

            if (val > 0 && val <= 100 && g_all_slots[i].group != 0) {
              Vector3 pos;
              PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, entry_addr + OFFSET_ORIGIN, sizeof(Vector3));
              memcpy(&pos, (void*)protocol.read_buffer, sizeof(Vector3));

              g_all_slots[i].pos = pos;
              g_all_slots[i].value_a = val;
            }
            else {
              g_all_slots[i].value_a = 0;
            }
          }
        }

        std::lock_guard<std::mutex> lock(data_mutex_);
        g_entries.clear();
        for (int idx = 0; idx < 64; ++idx) {
          if (g_all_slots[idx].value_a > 0) {
            g_entries.push_back(g_all_slots[idx]);
          }
        }

        g_localPos = localPos;
        g_localAngles = localAngles;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(READ_INTERVAL_MS));
    }
  }

  void ws_broadcast_loop() {
    const int BROADCAST_RATE_MS = 40;

    while (g_run_program.load(std::memory_order_acquire))
    {
      json data;
      {
        std::lock_guard<std::mutex> lock(data_mutex_);

        data["type"] = "smm_data";
        data["local"] = {
            {"pos", {
                {"x", g_localPos.x},
                {"y", g_localPos.y},
                {"z", g_localPos.z}
            }},
            {"angle_y", g_localAngles.y},
            {"group", g_localGroup}
        };

        json entries_array = json::array();
        for (const auto& entry : g_entries) {
          entries_array.push_back({
              {"id", entry.id},
              {"pos", {
                  {"x", entry.pos.x},
                  {"y", entry.pos.y},
                  {"z", entry.pos.z}
              }},
              {"group", entry.group},
              {"value_a", entry.value_a}
            });
        }
        data["entries"] = entries_array;
      }

      if (!data["entries"].empty() || data["local"]["group"].get<int>() != 0) {
        broadcastmsg(data.dump());
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(BROADCAST_RATE_MS));
    }
  }

public:
  DemoReader() {
    if (!InitTbsContext()) {
      throw std::runtime_error("[ERROR] Failed to initialize TBS Context (Run as Admin?).");
    }
    printf("[INFO] TBS Context initialized for memory read.\n");
  }

  ~DemoReader() {
    CloseTbsContext();
    printf("[INFO] TBS Context closed.\n");
  }

  void run() {
    read_loop_thread = std::thread(&DemoReader::read_loop, this);
    ws_send_thread = std::thread(&DemoReader::ws_broadcast_loop, this);

    while (g_run_program.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (read_loop_thread.joinable()) read_loop_thread.join();
    if (ws_send_thread.joinable()) ws_send_thread.join();
  }
};

void broadcastmsg(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  if (g_connections.empty()) return;
  for (auto const& hdl : g_connections) {
    try {
      g_ws_server.send(hdl, message, websocketpp::frame::opcode::text);
    }
    catch (...) {}
  }
}
void o_open(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  g_connections.insert(hdl);
  printf("[WS] Client connected.\n");
}
void o_close(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  g_connections.erase(hdl);
  printf("[WS] Client disconnected.\n");
}
void o_msg(connection_hdl hdl, message_ptr msg, DemoReader* reader_ptr) {
  if (msg->get_opcode() == websocketpp::frame::opcode::text) {
    try {
      json received_json = json::parse(msg->get_payload());
      if (received_json.contains("type") && received_json["type"] == "get_status") {
        g_ws_server.send(hdl, "{\"type\":\"status\",\"message\":\"Active\"}", websocketpp::frame::opcode::text);
      }
    }
    catch (...) {}
  }
}

int main() {
  static_assert(sizeof(SmmCommunicationProtocol) == 8307,
    "Protocol size mismatch! Struct MUST be 8307 bytes to match driver.");

  printf("[INFO] Protocol Addr: 0x%p\n", &protocol);
  signal(SIGINT, s_handler); signal(SIGTERM, s_handler);
  SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
  WSADATA wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData);

  DemoReader* reader_ptr = nullptr;
  std::thread asio_thread;

  try {
    reader_ptr = new DemoReader();
    g_ws_server.clear_access_channels(websocketpp::log::alevel::all);
    g_ws_server.init_asio();
    g_ws_server.set_open_handler(bind(&o_open, ::_1));
    g_ws_server.set_close_handler(bind(&o_close, ::_1));
    g_ws_server.set_message_handler(bind(&o_msg, ::_1, ::_2, reader_ptr));
    g_ws_server.listen(WEBSOCKET_PORT);
    printf("[WS] Server listening on port %hu\n", WEBSOCKET_PORT);

    std::vector<std::string> ips = getlocalip();
    if (!ips.empty()) printf("IP: %s\n", ips[0].c_str());

    g_ws_server.start_accept();
    asio_thread = std::thread([&]() { try { g_ws_server.run(); } catch (...) {} });
    reader_ptr->run();
  }
  catch (...) {}

  g_run_program.store(false);
  if (reader_ptr) delete reader_ptr;
  g_ws_server.stop();
  if (asio_thread.joinable()) asio_thread.join();
  WSACleanup();
  return 0;
}

void s_handler(int) { g_run_program.store(false, std::memory_order_release); }

UINT64 GetEntryAddressFromHandle(UINT64 entry_list, UINT32 handle) {
  UINT32 index = handle & 0x7FFF;
  UINT64 list_entry = 0;
  PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, entry_list + 0x10 + 8 * (index >> 9), sizeof(UINT64));
  memcpy(&list_entry, (void*)protocol.read_buffer, sizeof(UINT64));
  if (list_entry == 0) return 0;
  UINT64 entry_address = 0;
  PerformSmmRead(DUMMY_PROCESS_NAME, RAW_MODULE_COMMAND_NAME, list_entry + 112 * (index & 0x1FF), sizeof(UINT64));
  memcpy(&entry_address, (void*)protocol.read_buffer, sizeof(UINT64));
  return entry_address;
}
bool InitTbsContext() {
  TBS_CONTEXT_PARAMS2 params = { 0 }; params.version = TPM_VERSION_20; params.includeTpm20 = TRUE;
  return Tbsi_Context_Create((PCTBS_CONTEXT_PARAMS)&params, &g_hTbsContext) == TBS_SUCCESS;
}
void CloseTbsContext() { if (g_hTbsContext) { Tbsip_Context_Close(g_hTbsContext); g_hTbsContext = 0; } }
void PerformSmmRead(const char* target_proc, const wchar_t* module_name, UINT64 offset, UINT64 size) {
  memset((void*)protocol.read_buffer, 0, sizeof(protocol.read_buffer));
  strcpy_s((char*)protocol.process_name, sizeof(protocol.process_name), target_proc);
  wcscpy_s((wchar_t*)protocol.module_name, sizeof(protocol.module_name) / sizeof(UINT16), module_name);
  protocol.offset = offset; protocol.read_size = size;
  TriggerSmiTpmAcpi();
}
void TriggerSmiTpmAcpi() {
  if (g_hTbsContext == 0) return;
  BYTE buffer[256] = { 0u }; UINT32 size = sizeof(buffer); buffer[0] = 0x00000005u;
  Tbsi_Physical_Presence_Command(g_hTbsContext, buffer, sizeof(DWORD), buffer, &size);
}
std::vector<std::string> getlocalip() {
  std::vector<std::string> addresses; ULONG bufferSize = 15000; std::unique_ptr<BYTE[]> buffer(new BYTE[bufferSize]); PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.get());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, pAddresses, &bufferSize) == NO_ERROR) {
    for (PIP_ADAPTER_ADDRESSES p = pAddresses; p; p = p->Next) {
      if (p->OperStatus == IfOperStatusUp && p->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
        for (PIP_ADAPTER_UNICAST_ADDRESS u = p->FirstUnicastAddress; u; u = u->Next) {
          char ipStr[INET_ADDRSTRLEN];
          if (inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, ipStr, INET_ADDRSTRLEN)) addresses.push_back(ipStr);
        }
      }
    }
  }
  return addresses;
}
