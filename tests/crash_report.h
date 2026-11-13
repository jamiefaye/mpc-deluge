// For test programs run in the emulated machine, where there is no debugger: on a crash, print
// the faulting address, the program counter, the link register and every word on the stack that
// points into the program's code. tools/armtest/symbolize.py turns those into function names.
// Link the program with -no-pie so the addresses are the ones in the file.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <csignal>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#if !defined(__linux__)
// Anywhere else (the Mac) there is a debugger, and a crash report from the system.
namespace crash_report {
inline void install() {
}
} // namespace crash_report
#else
#include <ucontext.h>

extern "C" char __executable_start;
extern "C" char etext;

namespace crash_report {
inline void handler(int sig, siginfo_t* info, void* context) {
	auto* uc = static_cast<ucontext_t*>(context);
#if defined(__arm__)
	uintptr_t pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr, sp = uc->uc_mcontext.arm_sp;
#else
	uintptr_t pc = 0, lr = 0, sp = 0;
	(void)uc;
#endif
	std::fprintf(stderr, "CRASH signal %d fault address %p\nCRASH-PC %p\nCRASH-LR %p\n", sig, info->si_addr, (void*)pc,
	             (void*)lr);
	const uintptr_t lo = (uintptr_t)&__executable_start, hi = (uintptr_t)&etext;
	int shown = 0;
	for (uintptr_t* p = (uintptr_t*)sp; shown < 48 && p < (uintptr_t*)sp + 4096; p++) {
		if (*p >= lo && *p < hi) {
			std::fprintf(stderr, "CRASH-STACK %p\n", (void*)*p);
			shown++;
		}
	}
	std::fflush(stderr);
	_exit(128 + sig);
}

inline void install() {
	static char altStack[65536]; // so a stack overflow can still be reported
	stack_t ss{};
	ss.ss_sp = altStack;
	ss.ss_size = sizeof altStack;
	sigaltstack(&ss, nullptr);
	struct sigaction sa {};
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
		sigaction(sig, &sa, nullptr);
	}
}
} // namespace crash_report
#endif
