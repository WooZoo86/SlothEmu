#include "emu.h"
#include "plugin.h"

#include <vector>
#include <inttypes.h>

bool g_EngineInit; 
bool isDebugging;
uc_engine* g_engine = NULL;
Capstone g_capstone;

std::vector<MEMACCESSINFO> memoryAccessList;
std::vector<DSTADDRINFO> destAddrInfoList;

// some logic for emulated code
bool isSegmentAccessed = false;

static void CodeHook(uc_engine* uc, duint address, size_t size, void* userdata)
{
	_plugin_logprintf("Executing code at 0x%X, length = 0x%X\n", address, size);
}

bool InitEmuEngine()
{
    //initialize the engine 
	if (g_EngineInit || g_engine)
	{
		// close any previous running instances
		uc_err err = uc_close(g_engine);
	}
#ifdef _WIN64
	uc_err err = uc_open(UC_ARCH_X86, UC_MODE_64, &g_engine);
#else
    uc_err err = uc_open(UC_ARCH_X86, UC_MODE_32, &g_engine);
#endif
    if(err != UC_ERR_OK)
    {
        _plugin_logputs("Failed to load emu engine");
		g_EngineInit = false;
        return false;
    }
	g_EngineInit = true;
	_plugin_logputs("Emulation Engine Started!");
	
	//prepare the environment
    return true;
}


bool PrepareDataToEmulate(const unsigned char* data, size_t dataLen, duint start_addr, bool curCip = false)
{

	if (!isDebugging)
	{
		_plugin_logputs("not debugging..stopping");
		return false;
	}

	//clear our global vars
	destAddrInfoList.clear();
	memoryAccessList.clear();
	isSegmentAccessed = false;


	_plugin_logprintf("About to start emulating address: %08x with %x bytes\n", start_addr, dataLen);

	// disassemble and determine if code accesses any segments we need to setup or syscalls
	if (!g_EngineInit || !g_engine)
	{
		_plugin_logputs("Engine not started!");
		return false;
	}

	//iterate through the stream of data and disassemble
	for (size_t index = 0; index < dataLen; )
	{
		if (!g_capstone.Disassemble(start_addr, data + index))
		{
			// try reading forward: DANGER
			_plugin_logputs("Couldn't disassemble start of data, trying next byte..");
			start_addr++;
			index++;
			continue;
		}

		if (g_capstone.Size() == 0)
		{
			_plugin_logputs("Could not disassemble code");
			return false;
		}

		_plugin_logprintf("Instruction: %08X %s\n", start_addr, g_capstone.InstructionText(false).c_str());

		// Lets determine what needs to be prepared for the env
		// Here we determine the destination for any branches outside of emulated region.
		// Data accesses will be handled by hooks later in emulation
		if (g_capstone.InGroup(CS_GRP_CALL))
		{
			DSTADDRINFO dinfo;
			_plugin_logputs("Call instruction reached..");
			for (auto i = 0; i < g_capstone.OpCount(); ++i)
			{
				duint dest = g_capstone.ResolveOpValue(i, [](x86_reg)->size_t
				{
					return 0;
				});
				_plugin_logprintf("Destination to: %08X\n", dest);

				// is it a syscall?
				char modName[256];
				auto base = DbgFunctions()->ModBaseFromAddr(dest);
				DbgFunctions()->ModNameFromAddr(base, modName, true);
				auto party = DbgFunctions()->ModGetParty(base);
				
				dinfo.from = start_addr;
				dinfo.to = dest;
				dinfo.toMainMod = (party == 1) ? 0 : 1;

				_plugin_logprintf("Calling to module: %s\tIs call to system module: %d\n", modName, dinfo.toMainMod);
				// add it to our list of destination addresses
				destAddrInfoList.push_back(dinfo);
			}

		}
		else if (g_capstone.InGroup(CS_GRP_JUMP))
		{
			DSTADDRINFO dinfo;
			_plugin_logputs("jmp instruction reached..");
			for (auto i = 0; i < g_capstone.OpCount(); ++i)
			{
				duint dest = g_capstone.ResolveOpValue(i, [](x86_reg)->size_t
				{
					return 0;
				});
				_plugin_logprintf("Destination to: %08X\n", dest);

				// is it a syscall?
				char modName[256];
				auto base = DbgFunctions()->ModBaseFromAddr(dest);
				DbgFunctions()->ModNameFromAddr(base, modName, true);
				auto party = DbgFunctions()->ModGetParty(base);

				dinfo.from = start_addr;
				dinfo.to = dest;
				dinfo.toMainMod = (party == 1) ? 0 : 1;
				_plugin_logprintf("Jump to module: %s\tIs jump to system: %d\n", modName, dinfo.toMainMod);

				destAddrInfoList.push_back(dinfo);
			}

		}
		index += g_capstone.Size();
		start_addr += g_capstone.Size();
	}
	return true;
}

bool AddHooks(uc_engine* uc)
{
	// add code hook

}

bool EmuGetCurrentStackAddr(duint addr) 
{
	if (!isDebugging)
	{
		_plugin_logputs("Not debugging");
	}
	STACKINFO sinfo;
	EmuGetStackLimitForThread(DbgGetThreadId(), &sinfo);

}

// returns stack base and limit for a specified thread ID
void EmuGetStackLimitForThread(duint threadId, STACKINFO* sinfo)
{
	if (!isDebugging)
	{
		_plugin_logputs("Not debugging");
		return;
	}
	// get stack info from teb
	auto teb = (PTEB)DbgGetTebAddress(threadId);
	if (teb)
	{
		sinfo->base = (duint)teb->Tib.StackBase;
		sinfo->limit = (duint)teb->Tib.StackLimit;
		sinfo->tid = threadId;
	}
}

bool EmuSetupRegs(uc_engine* uc, Cpu* cpu) 
{
	if (!isDebugging)
		return false;

	auto regWrite = [&uc](int regid, void* value)
	{
		uc_err err = uc_reg_write(uc, regid, value);
		if (err != UC_ERR_OK)
		{
			_plugin_logputs("Register write failed");
			return false;
		}	
		return true;
	};

#ifdef _WIN64
	regWrite(UC_X86_REG_RAX, (void*)cpu->getCAX());
	regWrite(UC_X86_REG_RCX, (void*)cpu->getCCX());
	regWrite(UC_X86_REG_RBX, (void*)cpu->getCBX());
	regWrite(UC_X86_REG_RDX, (void*)cpu->getCDX());
	regWrite(UC_X86_REG_RSI, (void*)cpu->getCSI());
	regWrite(UC_X86_REG_RDI, (void*)cpu->getCDI());
	regWrite(UC_X86_REG_RBP, (void*)cpu->getCBP());
	regWrite(UC_X86_REG_RSP, (void*)cpu->getCSP());
	
#else
	regWrite(UC_X86_REG_EAX, (void*)cpu->getCAX());
	regWrite(UC_X86_REG_ECX, (void*)cpu->getCCX());
	regWrite(UC_X86_REG_EBX, (void*)cpu->getCBX());
	regWrite(UC_X86_REG_EDX, (void*)cpu->getCDX());
	regWrite(UC_X86_REG_ESI, (void*)cpu->getCSI());
	regWrite(UC_X86_REG_EDI, (void*)cpu->getCDI());
	regWrite(UC_X86_REG_EBP, (void*)cpu->getCBP());
	regWrite(UC_X86_REG_ESP, (void*)cpu->getCSP());
#endif

	regWrite(UC_X86_REG_GS, (void*)cpu->getGS());
	regWrite(UC_X86_REG_CS, (void*)cpu->getCS());
	regWrite(UC_X86_REG_FS, (void*)cpu->getFS());
	regWrite(UC_X86_REG_SS, (void*)cpu->getSS());

	//map the memory for the segments and stack


}

bool EmulateData(const char* data, size_t size, duint start_address, bool nullInit)
{
	if (!isDebugging)
		return false;
	uc_err err;
	// set up current registers and stack mem
	Cpu cpu;

	// For segment registers (probably switch to this eventually)
	REGDUMP rDump;
	DbgGetRegDump(&rDump);

#ifdef _WIN64
	cpu.setCAX(Script::Register::GetRAX());
	cpu.setCBX(Script::Register::GetRBX());
	cpu.setCCX(Script::Register::GetRCX());
	cpu.setCDI(Script::Register::GetRDI());
	cpu.setCDX(Script::Register::GetRDX());
	cpu.setCSI(Script::Register::GetRSI());
	cpu.setCSP(Script::Register::GetRSP());
	cpu.setCBP(Script::Register::GetRBP());

	cpu.setR8(Script::Register::GetR8());
	cpu.setR9(Script::Register::GetR9());
	cpu.setR10(Script::Register::GetR10());
	cpu.setR11(Script::Register::GetR11());
	cpu.setR12(Script::Register::GetR12());
	cpu.setR13(Script::Register::GetR13());
	cpu.setR14(Script::Register::GetR14());
	cpu.setR15(Script::Register::GetR15());
#else
	cpu.setCAX(Script::Register::GetEAX());
	cpu.setCBX(Script::Register::GetEBX());
	cpu.setCCX(Script::Register::GetECX());
	cpu.setCDI(Script::Register::GetEDI());
	cpu.setCDX(Script::Register::GetEDX());
	cpu.setCSI(Script::Register::GetESI());
	cpu.setCSP(Script::Register::GetESP());
	cpu.setCBP(Script::Register::GetEBP());
#endif
	// segment
	cpu.setCS(rDump.regcontext.cs);
	cpu.setGS(rDump.regcontext.gs);
	cpu.setFS(rDump.regcontext.fs);
	cpu.setSS(rDump.regcontext.ss);

	cpu.setEFLAGS(Script::Register::GetCFLAGS());
	cpu.setCIP(start_address);

	// TODO: set up segment selectors
	uc_x86_mmr gdtr;
	duint r_cs;
	duint r_ss;
	duint r_fs;
	duint r_gs;
	duint r_es;
	duint r_ds;

	if (!EmuSetupRegs(g_engine, &cpu))
	{
		_plugin_logputs("Register setups failed");
		return false;
	}

	// map the stack
	auto stack_addr = 

	auto aligned_address = PAGE_ALIGN(start_address);
	auto filler_size = start_address - aligned_address;
#define FILLER 0x90
	


}

void CleanupEmuEngine()
{
	if (g_engine)
	{
		uc_close(g_engine);
	}
}

bool SetupEnvironment(uc_engine* eng, duint threadID)
{
	_plugin_logputs("setup environment impl");
    return false;
}

bool SetupDescriptorTable(uc_engine* eng)
{
    return false;
}

bool SetupContext(uc_engine* eng)
{
    return false;
}

bool SetupStack(uc_engine* eng)
{
	return false;
}