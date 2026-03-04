#include "pch.h"
#include <limits>
#include "Debugger/Profiler.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/Debugger.h"
#include "Debugger/IDebugger.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/DebugTypes.h"
#include "Shared/Interfaces/IConsole.h"

static constexpr int32_t ResetFunctionIndex = -1;

Profiler::Profiler(Debugger* debugger, IDebugger* cpuDebugger)
{
	_debugger = debugger;
	_cpuDebugger = cpuDebugger;
	InternalReset();
}

Profiler::~Profiler()
{
}


int32_t Profiler::CreateKey(AddressInfo &addr)
{
    return addr.Address | ((uint8_t)addr.Type << 24);
}


void Profiler::SetFilter(AddressInfo &addr)
{
    _filter = CreateKey(addr);
}


void Profiler::ClearFilter()
{
    _filter.reset();
}


void Profiler::StackFunction(AddressInfo &addr, StackFrameFlags stackFlag)
{
	if(addr.Address >= 0) {
		uint32_t key = CreateKey(addr);
		if(_functions.find(key) == _functions.end()) {
			_functions[key] = ProfiledFunction();
			_functions[key].Address = addr;
		}

		UpdateCycles();

		_stackFlags.push_back(stackFlag);
		_cycleCountStack.push_back(_currentCycleCount);
		_functionStack.push_back(_currentFunction);

		if(_functionStack.size() > 100) {
			//Keep stack to 100 functions at most (to prevent performance issues, esp. in debug builds)
			//Only happens when software doesn't use JSR/RTS normally to enter/leave functions
			_functionStack.pop_front();
			_cycleCountStack.pop_front();
			_stackFlags.pop_front();
		}

		ProfiledFunction& func = _functions[key];
		func.CallCount++;
		func.Flags = stackFlag;

		_currentFunction = key;
		_currentCycleCount = 0;
	}
}

void Profiler::UpdateCycles()
{
	uint64_t masterClock = _cpuDebugger->GetCpuCycleCount(true);

    int filterFunctionSlot = -1;
    int stackDepthFilter = 0;
    if (_filter) {
        int32_t len = (int32_t)_functionStack.size();
        for(int32_t i = len - 1; i >= 0; i--) {
            if (_functionStack[i] == *_filter) {
                filterFunctionSlot = i;
                break;
            }
        }
        if (filterFunctionSlot == -1) {
            _prevMasterClock = masterClock;
            return;
        }
        stackDepthFilter = filterFunctionSlot;
    }

	ProfiledFunction& func = _functions[_currentFunction];
	uint64_t clockGap = masterClock - _prevMasterClock;
	func.ExclusiveCycles += clockGap;
	func.InclusiveCycles += clockGap;

	int32_t len = (int32_t)_functionStack.size();
	for(int32_t i = len - 1; i >= stackDepthFilter; i--) {
		_functions[_functionStack[i]].InclusiveCycles += clockGap;
		if(_stackFlags[i] != StackFrameFlags::None) {
			//Don't apply inclusive times to stack frames before an IRQ/NMI
			break;
		}
	}

	_currentCycleCount += clockGap;
	_prevMasterClock = masterClock;
}

void Profiler::UnstackFunction()
{
	if(!_functionStack.empty()) {
		UpdateCycles();

		//Return to the previous function
		ProfiledFunction& func = _functions[_currentFunction];
		func.MinCycles = std::min(func.MinCycles, _currentCycleCount);
		func.MaxCycles = std::max(func.MaxCycles, _currentCycleCount);

		_currentFunction = _functionStack.back();
		_functionStack.pop_back();
		_stackFlags.pop_back();

		//Add the subroutine's cycle count to the current routine's cycle count
		_currentCycleCount = _cycleCountStack.back() + _currentCycleCount;
		_cycleCountStack.pop_back();
	}
}

void Profiler::Reset()
{
	DebugBreakHelper helper(_debugger);
	InternalReset();
}

void Profiler::ResetState()
{
	_prevMasterClock = _cpuDebugger->GetCpuCycleCount(true);
	_currentCycleCount = 0;
	_functionStack.clear();
	_stackFlags.clear();
	_cycleCountStack.clear();
	_currentFunction = ResetFunctionIndex;
}

void Profiler::InternalReset()
{
	ResetState();

	_functions.clear();
	_functions[ResetFunctionIndex] = ProfiledFunction();
	_functions[ResetFunctionIndex].Address = { ResetFunctionIndex, MemoryType::None };
}

void Profiler::GetProfilerData(ProfiledFunction* profilerData, uint32_t& functionCount)
{
	DebugBreakHelper helper(_debugger);

	UpdateCycles();

	functionCount = 0;
	for(auto& func : _functions) {
		profilerData[functionCount] = func.second;
		functionCount++;

		if(functionCount >= 100000) {
			break;
		}
	}
}
