// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT
//
// PCSX2 Debug Server — JSON/TCP API wrapping full DebugInterface
// Protocol: newline-delimited JSON over TCP (port 21512)
//
// Request:  {"cmd":"read_registers","cpu":"ee","category":0}\n
// Response: {"ok":true,"data":{...}}\n
//
// Integration:
//   1. Drop DebugServer.h + DebugServer.cpp into pcsx2/DebugTools/
//   2. Add to CMakeLists.txt
//   3. Call DebugServer::Start() from VMManager::Initialize()
//   4. Call DebugServer::Stop() from VMManager::Shutdown()
//   5. Call DebugServer::OnBreakpointHit() from breakpoint handler

// ============================================================
// NOTE: This file uses a minimal inline JSON builder to avoid
// external dependencies. PCSX2 doesn't bundle nlohmann/json
// in all configurations.
// ============================================================

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#endif

#include "DebugServer.h"
#include "DebugInterface.h"
#include "Breakpoints.h"
#include "MipsStackWalk.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <algorithm>

// Forward declarations — these are PCSX2 globals
extern R5900DebugInterface r5900Debug;
extern R3000DebugInterface r3000Debug;

namespace DebugServer
{
	// ============================================================
	// Minimal JSON Builder
	// ============================================================
	class JsonBuilder
	{
	public:
		void startObject() { comma(); m_buf += '{'; m_needComma.push_back(false); }
		void endObject() { m_buf += '}'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }
		void startArray() { comma(); m_buf += '['; m_needComma.push_back(false); }
		void endArray() { m_buf += ']'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }

		void key(const char* k)
		{
			comma();
			m_buf += '"';
			escapeStr(k);
			m_buf += "\":";
			m_needComma.back() = false; // value follows
		}

		void valStr(const char* v) { comma(); m_buf += '"'; escapeStr(v); m_buf += '"'; m_needComma.back() = true; }
		void valStr(const std::string& v) { valStr(v.c_str()); }
		void valInt(int64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valUint(uint64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valBool(bool v) { comma(); m_buf += v ? "true" : "false"; m_needComma.back() = true; }
		void valNull() { comma(); m_buf += "null"; m_needComma.back() = true; }

		void valHex32(uint32_t v)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "0x%08x", v);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		void valHex128(u128 v)
		{
			char buf[40];
			snprintf(buf, sizeof(buf), "%08x%08x%08x%08x",
				v._u32[3], v._u32[2], v._u32[1], v._u32[0]);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		// Key-value shortcuts
		void kv(const char* k, const char* v) { key(k); valStr(v); }
		void kv(const char* k, const std::string& v) { key(k); valStr(v); }
		void kv(const char* k, int v) { key(k); valInt(static_cast<int64_t>(v)); }
		void kv(const char* k, unsigned int v) { key(k); valUint(static_cast<uint64_t>(v)); }
		void kv(const char* k, int64_t v) { key(k); valInt(v); }
		void kv(const char* k, uint64_t v) { key(k); valUint(v); }
		void kv(const char* k, bool v) { key(k); valBool(v); }

		std::string str() const { return m_buf; }
		void clear() { m_buf.clear(); m_needComma.clear(); }

	private:
		void comma()
		{
			if (!m_needComma.empty() && m_needComma.back())
				m_buf += ',';
		}
		void escapeStr(const char* s)
		{
			for (; *s; ++s)
			{
				switch (*s)
				{
					case '"': m_buf += "\\\""; break;
					case '\\': m_buf += "\\\\"; break;
					case '\n': m_buf += "\\n"; break;
					case '\r': m_buf += "\\r"; break;
					case '\t': m_buf += "\\t"; break;
					default: m_buf += *s; break;
				}
			}
		}

		std::string m_buf;
		std::vector<bool> m_needComma;
	};

	// ============================================================
	// Minimal JSON Parser (just enough for our commands)
	// ============================================================
	struct JsonValue
	{
		enum Type { NONE, STRING, NUMBER, BOOL, OBJECT };
		Type type = NONE;
		std::string strVal;
		int64_t numVal = 0;
		bool boolVal = false;
	};

	static std::unordered_map<std::string, JsonValue> parseJsonObject(const std::string& json)
	{
		std::unordered_map<std::string, JsonValue> result;
		size_t i = 0;
		// Skip to first '{'
		while (i < json.size() && json[i] != '{') i++;
		i++; // skip '{'

		while (i < json.size())
		{
			// Skip whitespace/commas
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ','))
				i++;
			if (i >= json.size() || json[i] == '}') break;

			// Read key
			if (json[i] != '"') break;
			i++;
			std::string key;
			while (i < json.size() && json[i] != '"')
			{
				if (json[i] == '\\' && i + 1 < json.size()) { key += json[i + 1]; i += 2; }
				else { key += json[i]; i++; }
			}
			i++; // skip closing '"'

			// Skip colon
			while (i < json.size() && json[i] != ':') i++;
			i++;

			// Skip whitespace
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;

			JsonValue val;
			if (json[i] == '"')
			{
				// String value
				i++;
				std::string sv;
				while (i < json.size() && json[i] != '"')
				{
					if (json[i] == '\\' && i + 1 < json.size()) { sv += json[i + 1]; i += 2; }
					else { sv += json[i]; i++; }
				}
				i++; // skip closing '"'
				val.type = JsonValue::STRING;
				val.strVal = sv;
			}
			else if (json[i] == 't' || json[i] == 'f')
			{
				val.type = JsonValue::BOOL;
				val.boolVal = (json[i] == 't');
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}
			else if (json[i] == '-' || (json[i] >= '0' && json[i] <= '9'))
			{
				std::string ns;
				bool isHex = false;
				if (json[i] == '0' && i + 1 < json.size() && (json[i + 1] == 'x' || json[i + 1] == 'X'))
				{
					isHex = true;
					i += 2;
				}
				while (i < json.size() && ((json[i] >= '0' && json[i] <= '9') ||
					   json[i] == '-' ||
					   (isHex && ((json[i] >= 'a' && json[i] <= 'f') || (json[i] >= 'A' && json[i] <= 'F')))))
				{
					ns += json[i]; i++;
				}
				val.type = JsonValue::NUMBER;
				if (isHex) val.numVal = (int64_t)strtoull(ns.c_str(), nullptr, 16);
				else val.numVal = strtoll(ns.c_str(), nullptr, 10);
			}
			else
			{
				// Skip unknown
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}

			result[key] = val;
		}

		return result;
	}

	static std::string getStr(const std::unordered_map<std::string, JsonValue>& m, const char* key, const char* def = "")
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::STRING) return it->second.strVal;
		return def;
	}

	static int64_t getNum(const std::unordered_map<std::string, JsonValue>& m, const char* key, int64_t def = 0)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::NUMBER) return it->second.numVal;
		// Also try parsing string as hex
		if (it != m.end() && it->second.type == JsonValue::STRING)
		{
			const auto& s = it->second.strVal;
			if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
				return (int64_t)strtoull(s.c_str() + 2, nullptr, 16);
			return strtoll(s.c_str(), nullptr, 10);
		}
		return def;
	}

	static bool getBool(const std::unordered_map<std::string, JsonValue>& m, const char* key, bool def = false)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::BOOL) return it->second.boolVal;
		return def;
	}

	// ============================================================
	// Get DebugInterface by CPU name
	// ============================================================
	static DebugInterface* getCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return &r3000Debug;
		return &r5900Debug; // default to EE
	}

	static BreakPointCpu getBpCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return BREAKPOINT_IOP;
		return BREAKPOINT_EE;
	}

	// ============================================================
	// Pad Input Injection (pad_set / pad_press)
	// ============================================================
	// The server thread only mutates this state under s_padMutex and flips
	// s_padInjectActive. The actual pad writes happen on the CPU/EE thread in
	// OnVSync(), at the same layer InputRecording uses (SetRawPressureButton /
	// SetRawAnalogs on Pad::GetPad(port)) — i.e. above SIO2, so the game reads
	// injected input from the next pad poll exactly like real input.
	//
	// Button mask uses the standard PS2 libpad bit order:
	//   0x0001 SELECT  0x0002 L3      0x0004 R3      0x0008 START
	//   0x0010 UP      0x0020 RIGHT   0x0040 DOWN    0x0080 LEFT
	//   0x0100 L2      0x0200 R2      0x0400 L1      0x0800 R1
	//   0x1000 TRIANGLE 0x2000 CIRCLE 0x4000 CROSS   0x8000 SQUARE

	struct PadInjectState
	{
		int port = 0; // unified slot (0 = port 1)

		// Base hold (pad_set): applied until cleared.
		bool holdActive = false;
		u32 holdButtons = 0;
		u8 holdLx = 0x7f, holdLy = 0x7f, holdRx = 0x7f, holdRy = 0x7f;

		// Timed press (pad_press): OR'd on top of the base hold for N frames.
		bool pressActive = false;
		u32 pressButtons = 0;
		bool pressHasAnalogs = false;
		u8 pressLx = 0x7f, pressLy = 0x7f, pressRx = 0x7f, pressRy = 0x7f;
		int pressFramesRemaining = 0;

		u64 framesApplied = 0; // diagnostic: total vsyncs this state was applied
	};

	static std::mutex s_padMutex;
	static PadInjectState s_pad;
	// True while OnVSync has work to do (any hold/press active, or a final
	// release frame still pending). Checked lock-free on the hot path.
	static std::atomic<bool> s_padInjectActive{false};

	// Map libpad mask bit index (0..15) -> PadDualshock2 input index.
	static constexpr u32 s_maskBitToInput[16] = {
		PadDualshock2::Inputs::PAD_SELECT, // 0x0001
		PadDualshock2::Inputs::PAD_L3, // 0x0002
		PadDualshock2::Inputs::PAD_R3, // 0x0004
		PadDualshock2::Inputs::PAD_START, // 0x0008
		PadDualshock2::Inputs::PAD_UP, // 0x0010
		PadDualshock2::Inputs::PAD_RIGHT, // 0x0020
		PadDualshock2::Inputs::PAD_DOWN, // 0x0040
		PadDualshock2::Inputs::PAD_LEFT, // 0x0080
		PadDualshock2::Inputs::PAD_L2, // 0x0100
		PadDualshock2::Inputs::PAD_R2, // 0x0200
		PadDualshock2::Inputs::PAD_L1, // 0x0400
		PadDualshock2::Inputs::PAD_R1, // 0x0800
		PadDualshock2::Inputs::PAD_TRIANGLE, // 0x1000
		PadDualshock2::Inputs::PAD_CIRCLE, // 0x2000
		PadDualshock2::Inputs::PAD_CROSS, // 0x4000
		PadDualshock2::Inputs::PAD_SQUARE, // 0x8000
	};

	// CPU thread only. Writes a full controller state (all 16 digital buttons +
	// both sticks) so injected frames are deterministic regardless of host input.
	static void applyPadState(PadBase* pad, u32 mask, u8 lx, u8 ly, u8 rx, u8 ry)
	{
		for (int bit = 0; bit < 16; bit++)
		{
			const bool pressed = (mask >> bit) & 1;
			pad->SetRawPressureButton(s_maskBitToInput[bit], std::make_tuple(pressed, pressed ? u8(0xFF) : u8(0)));
		}
		pad->SetRawAnalogs(std::make_tuple(lx, ly), std::make_tuple(rx, ry));
	}

	// ============================================================
	// Polled Watchpoints (watch_change) — substitute for recompiler
	// breakpoints that don't fire on the arm64 recompiler.
	// ============================================================

	struct Watch
	{
		int id = 0;
		std::string cpuName = "ee";
		u32 address = 0;
		int size = 4; // 1 | 2 | 4
		int intervalMs = 5;
		std::string description;

		bool armed = false; // first read taken
		u64 lastValue = 0;
		std::chrono::steady_clock::time_point nextPoll;

		// Last detected change
		bool triggered = false;
		u64 oldValue = 0;
		u64 newValue = 0;
		u64 cycleAtChange = 0;
		u64 changeCount = 0;
	};

	static std::mutex s_watchMutex;
	static std::vector<Watch> s_watches;
	static int s_nextWatchId = 1;
	static std::atomic<bool> s_watchThreadRunning{false};
	static constexpr size_t MAX_WATCHES = 64;

	static u64 readWatchValue(DebugInterface* cpu, u32 addr, int size, bool* valid)
	{
		switch (size)
		{
			case 1: return cpu->Read8(addr, valid);
			case 2: return cpu->Read16(addr, valid);
			default: return cpu->Read32(addr, valid);
		}
	}

	static void watchThreadLoop();

	static void ensureWatchThread()
	{
		bool expected = false;
		if (s_watchThreadRunning.compare_exchange_strong(expected, true))
			std::thread(watchThreadLoop).detach();
	}

	// ============================================================
	// Command Handlers
	// ============================================================

	static std::string handleCommand(const std::string& jsonLine)
	{
		auto params = parseJsonObject(jsonLine);
		std::string cmd = getStr(params, "cmd");
		std::string cpuName = getStr(params, "cpu", "ee");
		DebugInterface* cpu = getCpu(cpuName);

		JsonBuilder j;

		// ----- STATUS -----
		if (cmd == "status")
		{
			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();
			j.kv("alive", cpu->isAlive());
			j.kv("paused", cpu->isCpuPaused());
			j.key("pc"); j.valHex32(cpu->getPC());
			j.kv("cycles", (int64_t)cpu->getCycles());
			j.endObject();
			j.endObject();
		}
		// ----- READ REGISTERS (ALL) -----
		else if (cmd == "read_registers")
		{
			int cat = (int)getNum(params, "category", -1);
			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();

			int catStart = (cat >= 0) ? cat : 0;
			int catEnd = (cat >= 0) ? cat + 1 : cpu->getRegisterCategoryCount();

			for (int c = catStart; c < catEnd; c++)
			{
				j.key(cpu->getRegisterCategoryName(c));
				j.startObject();
				j.kv("size", cpu->getRegisterSize(c));
				j.kv("count", cpu->getRegisterCount(c));
				j.key("regs"); j.startArray();
				for (int r = 0; r < cpu->getRegisterCount(c); r++)
				{
					j.startObject();
					j.kv("name", cpu->getRegisterName(c, r));
					j.key("value"); j.valHex128(cpu->getRegister(c, r));
					j.kv("display", cpu->getRegisterString(c, r));
					j.endObject();
				}
				j.endArray();
				j.endObject();
			}

			j.key("pc"); j.valHex32(cpu->getPC());
			j.key("hi"); j.valHex128(cpu->getHI());
			j.key("lo"); j.valHex128(cpu->getLO());
			j.endObject();
			j.endObject();
		}
		// ----- WRITE REGISTER -----
		else if (cmd == "write_register")
		{
			int cat = (int)getNum(params, "category", 0);
			int num = (int)getNum(params, "index", 0);
			// Value can be a hex string or number
			std::string valStr = getStr(params, "value", "0");
			u128 newVal = {};
			// Parse hex string (up to 128-bit)
			if (valStr.size() > 2 && valStr[0] == '0' && (valStr[1] == 'x' || valStr[1] == 'X'))
				valStr = valStr.substr(2);
			// Pad to 32 hex chars (128 bits)
			while (valStr.size() < 32) valStr = "0" + valStr;
			for (int i = 0; i < 4; i++)
			{
				std::string part = valStr.substr(24 - i * 8, 8);
				newVal._u32[i] = (uint32_t)strtoul(part.c_str(), nullptr, 16);
			}
			cpu->setRegister(cat, num, newVal);

			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET PC -----
		else if (cmd == "set_pc")
		{
			u32 pc = (u32)getNum(params, "value", 0);
			cpu->setPc(pc);
			j.startObject();
			j.kv("ok", true);
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- READ MEMORY -----
		else if (cmd == "read_memory")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int len = (int)getNum(params, "length", 256);
			if (len > 65536) len = 65536;

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.kv("length", (int64_t)len);

			// Hex string output
			j.key("hex");
			std::string hexStr;
			hexStr.reserve(len * 2);
			for (int i = 0; i < len; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid) byte = 0;
				char hb[4];
				snprintf(hb, sizeof(hb), "%02x", byte & 0xFF);
				hexStr += hb;
			}
			j.valStr(hexStr);
			j.endObject();
		}
		// ----- WRITE MEMORY -----
		else if (cmd == "write_memory")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			std::string hexData = getStr(params, "data", "");
			int written = 0;
			for (size_t i = 0; i + 1 < hexData.size(); i += 2)
			{
				u8 byte = (u8)strtoul(hexData.substr(i, 2).c_str(), nullptr, 16);
				cpu->Write8(addr + written, byte);
				written++;
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("written", (int64_t)written);
			j.endObject();
		}
		// ----- DISASSEMBLE -----
		else if (cmd == "disassemble")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int count = (int)getNum(params, "count", 20);
			bool simplify = getBool(params, "simplify", true);
			if (count > 500) count = 500;

			j.startObject();
			j.kv("ok", true);
			j.key("instructions"); j.startArray();
			for (int i = 0; i < count; i++)
			{
				u32 pc = addr + i * 4;
				if (!cpu->isValidAddress(pc)) break;

				bool valid = true;
				u32 opcode = cpu->Read32(pc, &valid);

				j.startObject();
				j.key("address"); j.valHex32(pc);
				j.key("opcode"); j.valHex32(opcode);
				j.kv("disasm", cpu->disasm(pc, simplify));
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- EVALUATE EXPRESSION -----
		else if (cmd == "evaluate")
		{
			std::string expr = getStr(params, "expression", "0");
			u64 result = 0;
			std::string error;
			bool ok = cpu->evaluateExpression(expr.c_str(), result, error);

			j.startObject();
			j.kv("ok", ok);
			if (ok)
			{
				j.key("result"); j.valUint(result);
				char hexBuf[20];
				snprintf(hexBuf, sizeof(hexBuf), "0x%llx", (unsigned long long)result);
				j.kv("hex", hexBuf);
			}
			else
			{
				j.kv("error", error);
			}
			j.endObject();
		}
		// ----- SET BREAKPOINT -----
		else if (cmd == "set_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			bool temp = getBool(params, "temporary", false);
			bool enabled = getBool(params, "enabled", true);
			std::string condExpr = getStr(params, "condition", "");
			std::string desc = getStr(params, "description", "");
			auto bpCpu = getBpCpu(cpuName);

			CBreakPoints::AddBreakPoint(bpCpu, addr, temp, enabled);

			if (!desc.empty())
				CBreakPoints::ChangeBreakPointDescription(bpCpu, addr, desc);

			if (!condExpr.empty())
			{
				BreakPointCond cond;
				cond.debug = cpu;
				cond.expressionString = condExpr;
				std::string error;
				if (cpu->initExpression(condExpr.c_str(), cond.expression, error))
				{
					CBreakPoints::ChangeBreakPointAddCond(bpCpu, addr, cond);
				}
			}

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.endObject();
		}
		// ----- REMOVE BREAKPOINT -----
		else if (cmd == "remove_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			CBreakPoints::RemoveBreakPoint(getBpCpu(cpuName), addr);
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET MEMCHECK (WATCHPOINT) -----
		else if (cmd == "set_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			std::string typeStr = getStr(params, "type", "write");
			std::string actionStr = getStr(params, "action", "break");
			std::string desc = getStr(params, "description", "");
			std::string condExpr = getStr(params, "condition", "");

			MemCheckCondition cond = MEMCHECK_WRITE;
			if (typeStr == "read") cond = MEMCHECK_READ;
			else if (typeStr == "readwrite" || typeStr == "access") cond = MEMCHECK_READWRITE;
			else if (typeStr == "onchange") cond = (MemCheckCondition)(MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE);

			MemCheckResult result = MEMCHECK_BREAK;
			if (actionStr == "log") result = MEMCHECK_LOG;
			else if (actionStr == "both") result = MEMCHECK_BOTH;

			auto bpCpu = getBpCpu(cpuName);
			CBreakPoints::AddMemCheck(bpCpu, start, end, cond, result);

			if (!desc.empty())
				CBreakPoints::ChangeMemCheckDescription(bpCpu, start, end, desc);

			if (!condExpr.empty())
			{
				BreakPointCond bpCond;
				bpCond.debug = cpu;
				bpCond.expressionString = condExpr;
				std::string error;
				if (cpu->initExpression(condExpr.c_str(), bpCond.expression, error))
					CBreakPoints::ChangeMemCheckAddCond(bpCpu, start, end, bpCond);
			}

			j.startObject();
			j.kv("ok", true);
			j.key("start"); j.valHex32(start);
			j.key("end"); j.valHex32(end);
			j.endObject();
		}
		// ----- REMOVE MEMCHECK -----
		else if (cmd == "remove_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			CBreakPoints::RemoveMemCheck(getBpCpu(cpuName), start, end);
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- LIST BREAKPOINTS -----
		else if (cmd == "list_breakpoints")
		{
			auto bps = CBreakPoints::GetBreakpoints(getBpCpu(cpuName), true);
			j.startObject();
			j.kv("ok", true);
			j.key("breakpoints"); j.startArray();
			for (const auto& bp : bps)
			{
				j.startObject();
				j.key("address"); j.valHex32(bp.addr);
				j.kv("enabled", bp.enabled);
				j.kv("temporary", bp.temporary);
				j.kv("stepping", bp.stepping);
				j.kv("has_condition", bp.hasCond);
				if (bp.hasCond)
					j.kv("condition", bp.cond.expressionString);
				if (!bp.description.empty())
					j.kv("description", bp.description);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- LIST MEMCHECKS -----
		else if (cmd == "list_memchecks")
		{
			auto mcs = CBreakPoints::GetMemChecks(getBpCpu(cpuName));
			j.startObject();
			j.kv("ok", true);
			j.key("memchecks"); j.startArray();
			for (const auto& mc : mcs)
			{
				j.startObject();
				j.key("start"); j.valHex32(mc.start);
				j.key("end"); j.valHex32(mc.end);
				j.kv("hits", (int64_t)mc.numHits);
				j.key("last_pc"); j.valHex32(mc.lastPC);
				j.key("last_addr"); j.valHex32(mc.lastAddr);
				if (!mc.description.empty())
					j.kv("description", mc.description);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- PAUSE -----
		else if (cmd == "pause")
		{
			cpu->pauseCpu();
			j.startObject();
			j.kv("ok", true);
			j.key("pc"); j.valHex32(cpu->getPC());
			j.endObject();
		}
		// ----- RESUME -----
		else if (cmd == "resume")
		{
			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), cpu->getPC());
			cpu->resumeCpu();
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- STEP -----
		else if (cmd == "step")
		{
			u32 pc = cpu->getPC();

			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), pc);

			u32 nextPc = pc + 4;
			CBreakPoints::AddBreakPoint(getBpCpu(cpuName), nextPc, true, true, true);
			cpu->resumeCpu();

			// Wait for it to hit (with timeout)
			int timeout = 5000; // ms
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			u32 newPc = cpu->getPC();
			bool inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);

			j.startObject();
			j.kv("ok", true);
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", cpu->disasm(newPc, true));
			j.kv("in_bios", inBios);

			bool valid = true;
			u32 opcode = cpu->Read32(newPc, &valid);
			j.key("opcode"); j.valHex32(opcode);
			j.endObject();
		}
		// ----- STEP OVER -----
		else if (cmd == "step_over")
		{
			u32 pc = cpu->getPC();

			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), pc);

			bool valid = true;
			u32 opcode = cpu->Read32(pc, &valid);
			u32 op = (opcode >> 26) & 63;

			u32 bpAddr = pc + 8; // default: skip instruction + delay slot
			if (op == 3 || // JAL
				(op == 0 && (opcode & 63) == 9)) // JALR
			{
				bpAddr = pc + 8;
			}
			else
			{
				bpAddr = pc + 4;
			}

			CBreakPoints::AddBreakPoint(getBpCpu(cpuName), bpAddr, true, true, true);
			cpu->resumeCpu();

			int timeout = 10000;
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			u32 newPc = cpu->getPC();
			bool inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);

			j.startObject();
			j.kv("ok", true);
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", cpu->disasm(newPc, true));
			j.kv("in_bios", inBios);
			j.endObject();
		}
		// ----- GET THREADS -----
		else if (cmd == "get_threads")
		{
			auto threads = cpu->GetThreadList();
			j.startObject();
			j.kv("ok", true);
			j.key("threads"); j.startArray();
			for (const auto& t : threads)
			{
				j.startObject();
				j.kv("id", (int64_t)t->TID());
				j.key("pc"); j.valHex32(t->PC());
				j.kv("status", (int64_t)(int)t->Status());
				j.kv("wait_type", (int64_t)(int)t->Wait());
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- GET MODULES (IOP only) -----
		else if (cmd == "get_modules")
		{
			auto mods = cpu->GetModuleList();
			j.startObject();
			j.kv("ok", true);
			j.key("modules"); j.startArray();
			for (const auto& m : mods)
			{
				j.startObject();
				j.kv("name", m.name);
				j.kv("version", (int64_t)m.version);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- GET BACKTRACE -----
		else if (cmd == "get_backtrace")
		{
			int maxFrames = (int)getNum(params, "max_frames", 32);
			if (maxFrames > 128) maxFrames = 128;

			u32 pc = cpu->getPC();
			u32 ra = cpu->getRegister(0, 31); // $ra
			u32 sp = cpu->getRegister(0, 29); // $sp

			// Find the running thread to get entry point and stack top
			u32 threadEntry = 0;
			auto threads = cpu->GetThreadList();
			for (const auto& t : threads)
			{
				if (t->Status() == ThreadStatus::THS_RUN)
				{
					threadEntry = t->EntryPoint();
					break;
				}
			}

			auto frames = MipsStackWalk::Walk(cpu, pc, ra, sp, threadEntry);

			j.startObject();
			j.kv("ok", true);
			j.kv("frame_count", (int64_t)(std::min)((int)frames.size(), maxFrames));
			j.key("frames"); j.startArray();
			int count = 0;
			for (const auto& f : frames)
			{
				if (count >= maxFrames) break;
				j.startObject();
				j.key("entry"); j.valHex32(f.entry);
				j.key("pc"); j.valHex32(f.pc);
				j.key("sp"); j.valHex32(f.sp);
				j.kv("stack_size", (int64_t)f.stackSize);
				j.kv("disasm", cpu->disasm(f.pc, true));
				j.endObject();
				count++;
			}
			j.endArray();
			j.endObject();
		}
		// ----- IS VALID ADDRESS -----
		else if (cmd == "is_valid_address")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			j.startObject();
			j.kv("ok", true);
			j.kv("valid", cpu->isValidAddress(addr));
			j.endObject();
		}
		// ----- READ STRING -----
		else if (cmd == "read_string")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int maxLen = (int)getNum(params, "max_length", 256);
			if (maxLen > 4096) maxLen = 4096;

			std::string str;
			for (int i = 0; i < maxLen; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid || byte == 0) break;
				str += (char)byte;
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("string", str);
			j.kv("length", (int64_t)str.size());
			j.endObject();
		}
		// ----- CLEAR ALL BREAKPOINTS -----
		else if (cmd == "clear_breakpoints")
		{
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- PAD SET (hold until cleared) -----
		else if (cmd == "pad_set")
		{
			const bool clear = getBool(params, "clear", false);
			const int port = (int)getNum(params, "port", 0);

			std::lock_guard<std::mutex> lock(s_padMutex);
			if (clear)
			{
				s_pad.holdActive = false;
				s_pad.pressActive = false;
				// Leave s_padInjectActive set — OnVSync applies one final
				// release frame (all buttons up, sticks neutral) and then
				// deactivates, returning control to host input.
			}
			else
			{
				s_pad.port = port;
				s_pad.holdActive = true;
				s_pad.holdButtons = (u32)getNum(params, "buttons", 0) & 0xFFFF;
				s_pad.holdLx = (u8)getNum(params, "lx", 0x7f);
				s_pad.holdLy = (u8)getNum(params, "ly", 0x7f);
				s_pad.holdRx = (u8)getNum(params, "rx", 0x7f);
				s_pad.holdRy = (u8)getNum(params, "ry", 0x7f);
				s_pad.framesApplied = 0;
			}
			s_padInjectActive.store(true, std::memory_order_release);

			j.startObject();
			j.kv("ok", true);
			j.kv("holding", !clear);
			if (!clear)
			{
				j.key("buttons"); j.valHex32(s_pad.holdButtons);
				j.kv("lx", (int64_t)s_pad.holdLx);
				j.kv("ly", (int64_t)s_pad.holdLy);
				j.kv("rx", (int64_t)s_pad.holdRx);
				j.kv("ry", (int64_t)s_pad.holdRy);
			}
			j.kv("note", "applied on next vsync; VM must be running");
			j.endObject();
		}
		// ----- PAD PRESS (hold N frames, then release) -----
		else if (cmd == "pad_press")
		{
			const u32 buttons = (u32)getNum(params, "buttons", 0) & 0xFFFF;
			int frames = (int)getNum(params, "frames", 12);
			if (frames < 1) frames = 1;
			if (frames > 3600) frames = 3600;
			const int port = (int)getNum(params, "port", 0);

			{
				std::lock_guard<std::mutex> lock(s_padMutex);
				s_pad.port = port;
				s_pad.pressActive = true;
				s_pad.pressButtons = buttons;
				s_pad.pressFramesRemaining = frames;
				s_pad.pressHasAnalogs =
					params.count("lx") || params.count("ly") || params.count("rx") || params.count("ry");
				s_pad.pressLx = (u8)getNum(params, "lx", 0x7f);
				s_pad.pressLy = (u8)getNum(params, "ly", 0x7f);
				s_pad.pressRx = (u8)getNum(params, "rx", 0x7f);
				s_pad.pressRy = (u8)getNum(params, "ry", 0x7f);
				s_padInjectActive.store(true, std::memory_order_release);
			}

			j.startObject();
			j.kv("ok", true);
			j.key("buttons"); j.valHex32(buttons);
			j.kv("frames", (int64_t)frames);
			j.kv("note", "press is applied over the next N vsyncs, then released; VM must be running");
			j.endObject();
		}
		// ----- WATCH CHANGE (server-side polled watchpoint) -----
		else if (cmd == "watch_change")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int size = (int)getNum(params, "size", 4);
			if (size != 1 && size != 2 && size != 4) size = 4;
			int intervalMs = (int)getNum(params, "interval_ms", 5);
			if (intervalMs < 1) intervalMs = 1;
			if (intervalMs > 1000) intervalMs = 1000;
			std::string desc = getStr(params, "description", "");

			bool added = false;
			int id = 0;
			{
				std::lock_guard<std::mutex> lock(s_watchMutex);
				if (s_watches.size() < MAX_WATCHES)
				{
					Watch w;
					w.id = s_nextWatchId++;
					w.cpuName = cpuName;
					w.address = addr;
					w.size = size;
					w.intervalMs = intervalMs;
					w.description = desc;
					w.nextPoll = std::chrono::steady_clock::now();
					s_watches.push_back(std::move(w));
					id = s_watches.back().id;
					added = true;
				}
			}

			if (added)
				ensureWatchThread();

			j.startObject();
			j.kv("ok", added);
			if (added)
			{
				j.kv("id", (int64_t)id);
				j.key("address"); j.valHex32(addr);
				j.kv("size", (int64_t)size);
				j.kv("interval_ms", (int64_t)intervalMs);
				j.kv("note", "VM auto-pauses when the value changes; poll watch_list for old/new/cycle");
			}
			else
			{
				j.kv("error", "Too many watches (max 64)");
			}
			j.endObject();
		}
		// ----- WATCH LIST -----
		else if (cmd == "watch_list")
		{
			std::lock_guard<std::mutex> lock(s_watchMutex);
			j.startObject();
			j.kv("ok", true);
			j.key("watches"); j.startArray();
			for (const auto& w : s_watches)
			{
				j.startObject();
				j.kv("id", (int64_t)w.id);
				j.kv("cpu", w.cpuName);
				j.key("address"); j.valHex32(w.address);
				j.kv("size", (int64_t)w.size);
				j.kv("interval_ms", (int64_t)w.intervalMs);
				j.kv("armed", w.armed);
				if (w.armed)
				{
					j.key("value"); j.valHex32((u32)w.lastValue);
				}
				j.kv("triggered", w.triggered);
				j.kv("change_count", w.changeCount);
				if (w.triggered)
				{
					j.key("old_value"); j.valHex32((u32)w.oldValue);
					j.key("new_value"); j.valHex32((u32)w.newValue);
					j.kv("cycle", w.cycleAtChange);
				}
				if (!w.description.empty())
					j.kv("description", w.description);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- WATCH CLEAR -----
		else if (cmd == "watch_clear")
		{
			const int id = (int)getNum(params, "id", -1);
			size_t removed = 0;
			{
				std::lock_guard<std::mutex> lock(s_watchMutex);
				if (id < 0)
				{
					removed = s_watches.size();
					s_watches.clear();
				}
				else
				{
					const size_t before = s_watches.size();
					s_watches.erase(
						std::remove_if(s_watches.begin(), s_watches.end(),
							[id](const Watch& w) { return w.id == id; }),
						s_watches.end());
					removed = before - s_watches.size();
				}
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("removed", (int64_t)removed);
			j.endObject();
		}
		// ----- UNKNOWN COMMAND -----
		else
		{
			j.startObject();
			j.kv("ok", false);
			j.kv("error", "Unknown command: " + cmd);
			j.key("available_commands"); j.startArray();
			const char* cmds[] = {
				"status", "read_registers", "write_register", "set_pc",
				"read_memory", "write_memory", "read_string",
				"disassemble", "evaluate",
				"set_breakpoint", "remove_breakpoint", "list_breakpoints",
				"set_memcheck", "remove_memcheck", "list_memchecks",
				"pause", "resume", "step", "step_over",
				"get_threads", "get_modules",
				"is_valid_address", "clear_breakpoints",
				"pad_set", "pad_press",
				"watch_change", "watch_list", "watch_clear"
			};
			for (const char* c : cmds) j.valStr(c);
			j.endArray();
			j.endObject();
		}

		return j.str();
	}

	// ============================================================
	// TCP Server
	// ============================================================
	static std::atomic<bool> s_running{false};

	// ============================================================
	// Watch poller thread — reads watched addresses from the server
	// side and auto-pauses the VM when a value changes.
	// ============================================================
	static void watchThreadLoop()
	{
		while (s_running.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

			const auto now = std::chrono::steady_clock::now();
			std::lock_guard<std::mutex> lock(s_watchMutex);
			for (auto& w : s_watches)
			{
				if (now < w.nextPoll)
					continue;
				w.nextPoll = now + std::chrono::milliseconds(w.intervalMs);

				DebugInterface* cpu = getCpu(w.cpuName);
				if (!cpu->isAlive())
					continue;

				bool valid = true;
				const u64 value = readWatchValue(cpu, w.address, w.size, &valid);
				if (!valid)
					continue;

				if (!w.armed)
				{
					w.armed = true;
					w.lastValue = value;
					continue;
				}

				if (value != w.lastValue)
				{
					w.triggered = true;
					w.oldValue = w.lastValue;
					w.newValue = value;
					w.cycleAtChange = cpu->getCycles();
					w.changeCount++;
					w.lastValue = value;

					if (!cpu->isCpuPaused())
					{
						cpu->pauseCpu();
						fprintf(stderr,
							"[DebugServer] watch #%d: 0x%08x changed 0x%llx -> 0x%llx (cycle %llu), VM paused\n",
							w.id, w.address,
							(unsigned long long)w.oldValue, (unsigned long long)w.newValue,
							(unsigned long long)w.cycleAtChange);
					}
				}
			}
		}

		s_watchThreadRunning.store(false);
	}

	// ============================================================
	// Per-frame pad injection — runs on the CPU/EE thread, called
	// from VMManager::Internal::VSyncOnCPUThread().
	// ============================================================
	void OnVSync()
	{
		if (!s_padInjectActive.load(std::memory_order_acquire))
			return;

		std::lock_guard<std::mutex> lock(s_padMutex);

		PadBase* pad = Pad::GetPad((u8)(s_pad.port & 0x7));
		if (!pad || pad->GetType() != Pad::ControllerType::DualShock2)
			return; // not a DS2 (or not ready yet) — keep state pending

		if (s_pad.holdActive || s_pad.pressActive)
		{
			u32 buttons = s_pad.holdActive ? s_pad.holdButtons : 0;
			u8 lx = s_pad.holdActive ? s_pad.holdLx : u8(0x7f);
			u8 ly = s_pad.holdActive ? s_pad.holdLy : u8(0x7f);
			u8 rx = s_pad.holdActive ? s_pad.holdRx : u8(0x7f);
			u8 ry = s_pad.holdActive ? s_pad.holdRy : u8(0x7f);

			if (s_pad.pressActive)
			{
				buttons |= s_pad.pressButtons;
				if (s_pad.pressHasAnalogs)
				{
					lx = s_pad.pressLx;
					ly = s_pad.pressLy;
					rx = s_pad.pressRx;
					ry = s_pad.pressRy;
				}
			}

			applyPadState(pad, buttons, lx, ly, rx, ry);
			s_pad.framesApplied++;

			if (s_pad.pressActive && --s_pad.pressFramesRemaining <= 0)
				s_pad.pressActive = false;
			// If the press just ended and there is no base hold, we stay
			// active one more frame to emit the release below.
		}
		else
		{
			// Final release frame: everything up, sticks neutral, then give
			// control back to host input sources.
			applyPadState(pad, 0, 0x7f, 0x7f, 0x7f, 0x7f);
			s_padInjectActive.store(false, std::memory_order_release);
		}
	}

	static std::thread s_serverThread;
	static socket_t s_listenSocket = SOCKET_INVALID;

	static void clientHandler(socket_t clientSock)
	{
		std::string buffer;
		char recvBuf[4096];

		while (s_running.load())
		{
			int bytes = recv(clientSock, recvBuf, sizeof(recvBuf) - 1, 0);
			if (bytes <= 0) break;

			recvBuf[bytes] = '\0';
			buffer += recvBuf;

			// Process complete lines
			size_t newlinePos;
			while ((newlinePos = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newlinePos);
				buffer = buffer.substr(newlinePos + 1);

				// Trim
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();

				if (line.empty()) continue;

				std::string response = handleCommand(line);
				response += "\n";

				send(clientSock, response.c_str(), (int)response.size(), 0);
			}
		}

		CLOSE_SOCKET(clientSock);
	}

	static void serverLoop(int port)
	{
#ifdef _WIN32
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_listenSocket == SOCKET_INVALID)
		{
			fprintf(stderr, "[DebugServer] Failed to create socket\n");
			return;
		}

		int opt = 1;
		setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
		addr.sin_port = htons((u_short)port);

		if (bind(s_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to bind on port %d\n", port);
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		if (listen(s_listenSocket, 2) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to listen\n");
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		fprintf(stderr, "[DebugServer] Listening on 127.0.0.1:%d\n", port);

		while (s_running.load())
		{
			// Use select with timeout to allow clean shutdown
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(s_listenSocket, &readSet);

			struct timeval tv;
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			int selectResult = select((int)s_listenSocket + 1, &readSet, nullptr, nullptr, &tv);
			if (selectResult <= 0) continue;

			socket_t clientSock = accept(s_listenSocket, nullptr, nullptr);
			if (clientSock == SOCKET_INVALID) continue;

			fprintf(stderr, "[DebugServer] Client connected\n");

			// Handle client in a new thread
			std::thread(clientHandler, clientSock).detach();
		}

		CLOSE_SOCKET(s_listenSocket);
		s_listenSocket = SOCKET_INVALID;

#ifdef _WIN32
		WSACleanup();
#endif
	}

	void Start(int port)
	{
		if (s_running.load()) return;
		s_running.store(true);
		s_serverThread = std::thread(serverLoop, port);
		s_serverThread.detach();
	}

	void Stop()
	{
		s_running.store(false); // also stops the watch poller thread
		if (s_listenSocket != SOCKET_INVALID)
		{
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
		}

		// Drop any pending pad injection so we don't touch pads during shutdown.
		{
			std::lock_guard<std::mutex> lock(s_padMutex);
			s_pad = PadInjectState();
		}
		s_padInjectActive.store(false);

		{
			std::lock_guard<std::mutex> lock(s_watchMutex);
			s_watches.clear();
		}
	}

	bool IsRunning()
	{
		return s_running.load();
	}

	void OnBreakpointHit()
	{
		// Future: notify connected clients of breakpoint events
	}

} // namespace DebugServer
