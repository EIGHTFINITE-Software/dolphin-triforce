#include "DSPJitTester.h"

DSPJitTester::DSPJitTester(u16 opcode, u16 opcode_ext, bool verbose)
	: be_verbose(verbose), run_count(0), fail_count(0)
{
	instruction = opcode << 9 | opcode_ext;
	opcode_template = GetOpTemplate(instruction);
	sprintf(instruction_name, "%s", opcode_template->name);
	if (opcode_template->extended) 
		sprintf(&instruction_name[strlen(instruction_name)], "'%s", 
			extOpTable[instruction & (((instruction >> 12) == 0x3) ? 0x7F : 0xFF)]->name);
}
bool DSPJitTester::Test(SDSP dsp_settings)
{
	if (be_verbose)
		printf("Running %s: ", instruction_name);
	
	last_int_dsp = RunInterpreter(dsp_settings);
	last_jit_dsp = RunJit(dsp_settings);

	run_count++;
	bool success = AreEqual(last_int_dsp, last_jit_dsp);
	if (!success)
		fail_count++;
	return success;
}
SDSP DSPJitTester::RunInterpreter(SDSP dsp_settings)
{
	ResetInterpreter();
	memcpy(&g_dsp, &dsp_settings, sizeof(SDSP));
	ExecuteInstruction(instruction);

	return g_dsp;
}
SDSP DSPJitTester::RunJit(SDSP dsp_settings)
{
	ResetJit();
	memcpy(&g_dsp, &dsp_settings, sizeof(SDSP));
	const u8* code = jit.GetCodePtr();
	jit.WriteCallInterpreter(instruction);
	jit.RET();
	((void(*)())code)();

	return g_dsp;
}
void DSPJitTester::ResetInterpreter()
{
	for (int i=0; i < WRITEBACKLOGSIZE; i++)
		writeBackLogIdx[i] = -1;
}
void DSPJitTester::ResetJit()
{
	jit.ClearCodeSpace();
}
bool DSPJitTester::AreEqual(SDSP& int_dsp, SDSP& jit_dsp)
{
	bool equal = true;
	for (int i = 0; i < 32; i++)
	{
		if (int_dsp.r[i] != jit_dsp.r[i])
		{
			if (equal && be_verbose)
				printf("failed\n");
			equal = false;
			if (be_verbose)
				printf("\t%s: int = 0x%04x, jit = 0x%04x\n", regnames[i].name, int_dsp.r[i], jit_dsp.r[i]);
		}
	}
	if (equal && be_verbose)
		printf("passed\n");
	return equal;
}
void DSPJitTester::Report()
{
	printf("%s (0x%04x): Ran %d times, Failed %d times.\n", instruction_name, instruction, run_count, fail_count);
}
void DSPJitTester::DumpJittedCode()
{
	ResetJit();
	const u8* code = jit.GetCodePtr();
	jit.WriteCallInterpreter(instruction);
	int code_size = jit.GetCodePtr() - code;

	printf("%s emitted: ", instruction_name);
	for (int i = 0; i < code_size; i++)
		printf("%02x ", code[i]);
	printf("\n");
}
void DSPJitTester::Initialize()
{
	//init int
	InitInstructionTable();
}