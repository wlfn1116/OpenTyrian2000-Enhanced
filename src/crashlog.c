/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Crash / diagnostic logger — see crashlog.h.
 */
#include "crashlog.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// One log for every kind of hard failure (exception, hang, abort, CRT fatal), written next to
// the executable; the previous session's log is rotated aside at startup. notes.md §Crash logging
#define LOG_FILENAME "opentyrian_log.log"

// How many previous crash logs to keep alongside the live one (opentyrian_log.1.log ... .N.log).
#define LOG_ROTATE_KEEP 3

extern const char *opentyrian_version;  // opentyr.c

// Guards the terminal fault paths (exception / abort / CRT fatal) so a fault raised mid-report
// can't re-enter the logger and clobber it. The hang watchdog is non-terminal and stays
// re-armable, so it doesn't use this.
static volatile LONG s_reporting = 0;

// First report of the session truncates; later ones append (keeps a HANG + later crash in one file).
static volatile LONG s_logOpened = 0;

// Last fault the vectored handler reported; the backup filter skips exactly this (code, addr)
// pair. Not a latch, only the most recent fault is held. notes.md §Crash logging
static volatile DWORD s_reportedCode = 0;
static volatile PVOID s_reportedAddr = NULL;

// Captured once at install time on the main thread, so every report can show session uptime
// and tell whether the faulting thread is the main thread or a background one (SDL audio, etc.).
static DWORD     s_mainThreadId = 0;
static ULONGLONG s_startTick    = 0;

// Serializes the single-threaded dbghelp session (Sym*/StackWalk64) so a crash walk and a hang
// walk can't corrupt each other. Recursive, so a fault while held can still re-enter and report.
static CRITICAL_SECTION s_dbghelpLock;

// Build "<exe dir>\<filename>".
static void log_path(char *out, size_t outSize, const char *filename)
{
	char exePath[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
	{
		snprintf(out, outSize, "%s", filename);
		return;
	}
	char *slash = strrchr(exePath, '\\');
	if (slash != NULL)
		*(slash + 1) = '\0';
	snprintf(out, outSize, "%s%s", exePath, filename);
}

// Open the crash log, falling back through writable locations if the exe dir is read-only: next
// to the exe, then the working directory, then %TEMP%. NULL only if all fail. The first report of
// a session truncates, later ones append, so a non-fatal hang followed by a crash is kept as two
// stacked reports.
static FILE *open_log(void)
{
	const char *mode = (InterlockedExchange(&s_logOpened, 1) == 0) ? "w" : "a";

	char path[MAX_PATH + 32];

	log_path(path, sizeof(path), LOG_FILENAME);
	FILE *f = fopen(path, mode);
	if (f != NULL)
		return f;

	f = fopen(LOG_FILENAME, mode);  // current working directory
	if (f != NULL)
		return f;

	char tmp[MAX_PATH];
	DWORD n = GetTempPathA(sizeof(tmp), tmp);
	if (n > 0 && n < sizeof(tmp))
	{
		snprintf(path, sizeof(path), "%s%s", tmp, LOG_FILENAME);
		f = fopen(path, mode);
		if (f != NULL)
			return f;
	}
	return NULL;
}

// Build "<exe dir>\opentyrian_log.<n>.log" for n >= 1, or the base log path for n == 0.
static void rotated_log_path(char *out, size_t outSize, int n)
{
	if (n <= 0)
	{
		log_path(out, outSize, LOG_FILENAME);
		return;
	}
	char name[64];
	snprintf(name, sizeof(name), "opentyrian_log.%d.log", n);
	log_path(out, outSize, name);
}

// Rotate the crash-log generations up one and discard the oldest (opentyrian_log.log -> .1.log,
// .1 -> .2, ... up to LOG_ROTATE_KEEP), preserving the previous session's log. Called once at
// startup before any handler is armed, so it never runs on a fault path. No-op when there is no
// live log, so crash-free restarts don't shift an older real report off the end. Only the exe-dir
// log is rotated (a read-only exe dir couldn't have been rotated anyway).
static void rotate_logs(void)
{
	char live[MAX_PATH + 32];
	rotated_log_path(live, sizeof(live), 0);
	if (GetFileAttributesA(live) == INVALID_FILE_ATTRIBUTES)
		return;  // nothing new to preserve

	char src[MAX_PATH + 32], dst[MAX_PATH + 32];

	// Move oldest-first so each destination is free before its move: .(KEEP-1) -> .KEEP (discarding
	// the previous oldest), ..., .1 -> .2, then the live log -> .1. A missing generation just makes
	// one MoveFileEx fail harmlessly.
	for (int n = LOG_ROTATE_KEEP - 1; n >= 0; --n)
	{
		rotated_log_path(src, sizeof(src), n);
		rotated_log_path(dst, sizeof(dst), n + 1);
		MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING);
	}
}

// Human-readable name for a Windows structured-exception code.
static const char *exception_name(DWORD code)
{
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
	case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
	case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
	case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
	case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
	case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
	case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
	case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
	case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
	case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
	case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
	case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
	case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
	case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
	case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
	case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
	case 0xC0000374:                         return "HEAP_CORRUPTION";
	case 0xC0000409:                         return "STACK_BUFFER_OVERRUN";
	case 0xE06D7363:                         return "C++ exception";
	default:                                 return "unknown";
	}
}

// Banner shared by every report: what happened, when, and which build.
static void write_header(FILE *f, const char *event)
{
	time_t now = time(NULL);
	struct tm lt;
	char when[64] = "unknown";
	if (localtime_s(&lt, &now) == 0)
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt);

	fprintf(f, "================================================================\n");
	fprintf(f, "OpenTyrian %s\n", event);
	fprintf(f, "  Version:     %s\n", opentyrian_version ? opentyrian_version : "?");
	fprintf(f, "  Time:        %s\n", when);
	fprintf(f, "  Module base: %p\n", (void *)GetModuleHandleA(NULL));
	fprintf(f, "================================================================\n\n");
}

// Process-level context under the header: game phase, session uptime, which thread faulted (main
// vs. a background/audio thread), and memory use. `faultTid` is the current thread on the
// crash/CRT-fatal paths, or the stalled main thread for the hang watchdog.
static void write_process_info(FILE *f, DWORD faultTid)
{
	fprintf(f, "Phase:       %s\n", crashlog_get_phase() ? crashlog_get_phase() : "?");

	ULONGLONG up = GetTickCount64() - s_startTick;
	fprintf(f, "Uptime:      %llu.%03llu s\n",
	        (unsigned long long)(up / 1000), (unsigned long long)(up % 1000));

	fprintf(f, "Thread:      %lu  %s\n", (unsigned long)faultTid,
	        faultTid == s_mainThreadId ? "(main thread)"
	                                   : "(NOT main -- background/SDL audio/worker thread)");

	PROCESS_MEMORY_COUNTERS pmc;
	memset(&pmc, 0, sizeof(pmc));
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		fprintf(f, "Memory:      working set %.1f MB (peak %.1f MB)\n",
		        pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);

	fprintf(f, "\n");
}

// Decode an exception record: code + name, faulting instruction address, and (for access
// violations / in-page errors) whether it was a read/write/execute and at what address.
static void write_exception_details(FILE *f, const EXCEPTION_RECORD *er)
{
	HMODULE base = GetModuleHandleA(NULL);
	void *faultAddr = er->ExceptionAddress;

	fprintf(f, "Exception:   0x%08lX (%s)\n",
	        (unsigned long)er->ExceptionCode, exception_name(er->ExceptionCode));
	fprintf(f, "Fault at:    %p  (RVA 0x%llX)\n", faultAddr,
	        (unsigned long long)((char *)faultAddr - (char *)base));

	if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
	     er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
	    er->NumberParameters >= 2)
	{
		ULONG_PTR op = er->ExceptionInformation[0];
		const char *what = (op == 0) ? "reading"
		                 : (op == 1) ? "writing"
		                 : (op == 8) ? "executing"
		                             : "accessing";
		fprintf(f, "Bad access:  %s address %p\n",
		        what, (void *)er->ExceptionInformation[1]);
	}
	fprintf(f, "\n");
}

// Register snapshot (read-only; taken before StackWalk64 mutates the context).
static void write_registers(FILE *f, const CONTEXT *c)
{
	fprintf(f, "Registers:\n");
#if defined(_M_X64)
	fprintf(f, "  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
	        (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
	        (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
	fprintf(f, "  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
	        (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
	        (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
	fprintf(f, "  R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
	        (unsigned long long)c->R8, (unsigned long long)c->R9,
	        (unsigned long long)c->R10, (unsigned long long)c->R11);
	fprintf(f, "  R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
	        (unsigned long long)c->R12, (unsigned long long)c->R13,
	        (unsigned long long)c->R14, (unsigned long long)c->R15);
	fprintf(f, "  RIP=%016llX EFL=%08lX\n",
	        (unsigned long long)c->Rip, (unsigned long)c->EFlags);
#else
	fprintf(f, "  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
	        (unsigned long)c->Eax, (unsigned long)c->Ebx,
	        (unsigned long)c->Ecx, (unsigned long)c->Edx);
	fprintf(f, "  ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n",
	        (unsigned long)c->Esi, (unsigned long)c->Edi,
	        (unsigned long)c->Ebp, (unsigned long)c->Esp);
	fprintf(f, "  EIP=%08lX EFL=%08lX\n",
	        (unsigned long)c->Eip, (unsigned long)c->EFlags);
#endif
	fprintf(f, "\n");
}

// Walk `thr`'s call stack (using `ctx` as the starting register state) and write a
// symbolised trace to `f`. The caller owns SymInitialize/SymCleanup so this can run inside
// either the crash handler or the hang watchdog. Mutates `ctx` (StackWalk64 advances it).
static void write_stack_trace(FILE *f, HANDLE proc, HANDLE thr, CONTEXT *ctx)
{
	STACKFRAME64 frame;
	memset(&frame, 0, sizeof(frame));
	DWORD machine;
#if defined(_M_X64)
	machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset    = ctx->Rip;
	frame.AddrFrame.Offset = ctx->Rbp;
	frame.AddrStack.Offset = ctx->Rsp;
#else
	machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset    = ctx->Eip;
	frame.AddrFrame.Offset = ctx->Ebp;
	frame.AddrStack.Offset = ctx->Esp;
#endif
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

	fprintf(f, "Stack trace:\n");
	for (int i = 0; i < 48; ++i)
	{
		if (!StackWalk64(machine, proc, thr, &frame, ctx, NULL,
		                 SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;

		DWORD64 addr = frame.AddrPC.Offset;
		if (addr == 0)
			break;

		DWORD64 modBase = SymGetModuleBase64(proc, addr);
		fprintf(f, "  %2d: [rva 0x%llX] ", i,
		        (unsigned long long)(modBase ? addr - modBase : addr));

		char symBuf[sizeof(SYMBOL_INFO) + 256];
		memset(symBuf, 0, sizeof(symBuf));
		SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		if (SymFromAddr(proc, addr, &disp, sym))
			fprintf(f, "%s + 0x%llX", sym->Name, (unsigned long long)disp);
		else
			fprintf(f, "0x%llX", (unsigned long long)addr);

		IMAGEHLP_LINE64 line;
		memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		DWORD lineDisp = 0;
		if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
			fprintf(f, "  (%s:%lu)", line.FileName, (unsigned long)line.LineNumber);

		fprintf(f, "\n");
	}
}

// List loaded modules with their address range, so an RVA (or a fault inside a DLL such as
// SDL2) can be attributed to the right binary even without a .pdb.
static void write_modules(FILE *f)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;

	MODULEENTRY32W me;
	memset(&me, 0, sizeof(me));
	me.dwSize = sizeof(me);

	fprintf(f, "\nLoaded modules:\n");
	if (Module32FirstW(snap, &me))
	{
		int n = 0;
		do
		{
			fprintf(f, "  %p - %p  %ls\n",
			        (void *)me.modBaseAddr,
			        (void *)(me.modBaseAddr + me.modBaseSize),
			        me.szModule);
		} while (Module32NextW(snap, &me) && ++n < 128);
	}
	CloseHandle(snap);
}

// Registers + symbolised stack + game-state snapshot + module list for thread `thr`, shared by
// every reporting path. Owns the dbghelp symbol session. The game state is written after the
// stack walk (the most fault-prone step) so a corrupt process still yields the trace; each stage
// is flushed to disk first.
static void write_context_report(FILE *f, HANDLE thr, CONTEXT *ctx)
{
	HANDLE proc = GetCurrentProcess();
	write_registers(f, ctx);

	// Flush everything so far to disk before the symbol session below. The stack walk can itself
	// fault on a corrupt process; the re-entry guard would then abort the nested report and fclose
	// never runs, losing whatever is still buffered (including the decoded fault address).
	fflush(f);

	// dbghelp is single-threaded: serialize the symbol session so a background-thread crash and the
	// hang watchdog's walk can't run it concurrently. The lock covers only the Sym* section; the
	// game-state dump and write_modules are thread-safe and stay outside it.
	EnterCriticalSection(&s_dbghelpLock);
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(proc, NULL, TRUE);
	write_stack_trace(f, proc, thr, ctx);
	SymCleanup(proc);
	LeaveCriticalSection(&s_dbghelpLock);

	fflush(f);
	crashlog_write_game_state(f);
	write_modules(f);
}

// --- Unhandled structured exception (access violation, div-by-zero, ...) ---------------------

// Codes that mean a genuine crash. The vectored handler reports on these; the game and SDL never
// handle them first-chance, so it doesn't fire spuriously, and the writer doesn't latch, so a rare
// handled first-chance fault can't suppress a later real crash.
static bool is_fatal_exception(DWORD code)
{
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
	case EXCEPTION_NONCONTINUABLE_EXCEPTION:
	case 0xC0000374:  // heap corruption (bypasses the top-level filter)
	case 0xC0000409:  // stack buffer overrun / __fastfail (bypasses it too)
		return true;
	default:
		return false;
	}
}

// Write the full crash report for `ep`. s_reporting guards against re-entry if the report itself
// faults; it is reset on the way out (not latched), so a handled first-chance fatal can't suppress
// a later real crash. On unhandled paths the process dies right after, so the reset is moot.
static void write_crash_report(EXCEPTION_POINTERS *ep, const char *event)
{
	if (InterlockedExchange(&s_reporting, 1) != 0)
		return;

	FILE *f = open_log();
	if (f != NULL)
	{
		write_header(f, event);
		write_process_info(f, GetCurrentThreadId());
		write_exception_details(f, ep->ExceptionRecord);
		write_context_report(f, GetCurrentThread(), ep->ContextRecord);
		fclose(f);

		// Record this fault so the top-level backup filter (crash_handler), firing next on the same
		// exception, recognises it and won't overwrite this report.
		s_reportedCode = ep->ExceptionRecord->ExceptionCode;
		s_reportedAddr = ep->ExceptionRecord->ExceptionAddress;
	}

	InterlockedExchange(&s_reporting, 0);
}

// Vectored handler: runs for every exception, ahead of frame handlers and the top-level filter, so
// it catches faults even when that filter is bypassed (reset by another lib, an upstream __try,
// some debugger setups). Reports on fatal codes and returns CONTINUE_SEARCH, so the process still
// dies exactly as it would have.
static LONG WINAPI crash_veh(EXCEPTION_POINTERS *ep)
{
	if (is_fatal_exception(ep->ExceptionRecord->ExceptionCode))
		write_crash_report(ep, "CRASH (fatal exception)");

	return EXCEPTION_CONTINUE_SEARCH;  // don't swallow; let normal termination proceed
}

// Top-level filter: backup for anything the vectored handler didn't report. On the same exception
// its second-chance context often points at thread start, so re-writing would replace the good
// trace with a useless 1-frame one; skip that exact (code, addr) pair, but still log a genuinely
// different fault the vectored handler missed. notes.md §Crash logging
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep)
{
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	if (er->ExceptionCode != s_reportedCode || er->ExceptionAddress != s_reportedAddr)
		write_crash_report(ep, "CRASH (unhandled exception)");
	return EXCEPTION_EXECUTE_HANDLER;  // let the process terminate normally
}

// --- CRT-level fatal errors (abort()/assert, invalid parameter, pure call) -------------------
// These terminate the process without raising an SEH exception, so crash_handler never sees
// them. Each hook captures the current context and writes the same rich report, then exits.

static void report_crt_fatal(const char *event, const char *detail)
{
	if (InterlockedExchange(&s_reporting, 1) != 0)
		_exit(3);

	FILE *f = open_log();
	if (f != NULL)
	{
		CONTEXT ctx;
		memset(&ctx, 0, sizeof(ctx));
		RtlCaptureContext(&ctx);  // capture this thread's state at the point of failure

		write_header(f, event);
		write_process_info(f, GetCurrentThreadId());
		if (detail != NULL)
			fprintf(f, "%s\n\n", detail);
		write_context_report(f, GetCurrentThread(), &ctx);
		fclose(f);
	}

	_exit(3);
}

static void on_abort(int sig)
{
	(void)sig;
	report_crt_fatal("ABORT (abort() / failed assert)", NULL);
}

static void __cdecl on_invalid_parameter(const wchar_t *expr, const wchar_t *func,
                                         const wchar_t *file, unsigned int line, uintptr_t unused)
{
	(void)unused;
	char detail[512];
	_snprintf_s(detail, sizeof(detail), _TRUNCATE,
	            "Invalid CRT parameter: %ls  (%ls, %ls:%u)",
	            expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
	report_crt_fatal("CRT INVALID PARAMETER", detail);
}

static void __cdecl on_purecall(void)
{
	report_crt_fatal("PURE VIRTUAL CALL", NULL);
}

void install_crash_handler(void)
{
	// Must come first: every reporting path locks this around its dbghelp session, and this runs
	// before watchdog_init and before any handler is armed.
	InitializeCriticalSection(&s_dbghelpLock);

	// Remember who "main" is and when the session began (this runs on the main thread at startup).
	s_mainThreadId = GetCurrentThreadId();
	s_startTick    = GetTickCount64();

	// Preserve the previous session's crash log before we arm the handlers that could overwrite it.
	// Still single-threaded here with no handler installed, so this can't race a live report.
	rotate_logs();

	// Two catches so a real fault is hard to miss: the vectored handler (crash_veh) is primary, the
	// top-level filter a backup for whatever it doesn't take. SetUnhandledExceptionFilter alone can
	// be bypassed, which would leave a genuine fault unlogged.
	AddVectoredExceptionHandler(1, crash_veh);
	SetUnhandledExceptionFilter(crash_handler);

	// Broaden coverage to CRT-level fatals that raise no SEH exception.
	signal(SIGABRT, on_abort);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no message box / WER popup
	_set_invalid_parameter_handler(on_invalid_parameter);
	_set_purecall_handler(on_purecall);
}

// --- Hang watchdog --------------------------------------------------------------------------
// A hard hang raises no exception, so a background thread watches a heartbeat pumped from
// service_SDL_events. If it stalls past crashlog_get_hang_timeout() (default 5s) the main
// thread's stack is walked into the log and the thread resumed; a legitimate long pause costs
// only a spurious log, never a kill. Re-arms on progress. notes.md §Crash logging

static volatile LONG s_heartbeat = 0;   // bumped by the main loop; frozen while it's stuck
static HANDLE        s_mainThread = NULL;
static volatile LONG s_hangLogged = 0;  // 1 once we've logged the current stall (avoid spamming)

void watchdog_heartbeat(void)
{
	InterlockedIncrement(&s_heartbeat);
}

static void watchdog_dump_hang(int seconds)
{
	FILE *f = open_log();
	if (f == NULL)
		return;

	write_header(f, "HANG (main thread stalled)");
	write_process_info(f, s_mainThreadId);
	fprintf(f, "Main thread made no progress for ~%d seconds -- likely an infinite loop.\n", seconds);
	fprintf(f, "Heartbeat: %ld\n\n", (long)s_heartbeat);

	// Capture registers under the briefest possible suspension, then resume before the stack walk:
	// symbolisation takes loader/CRT-heap locks, so walking while the main thread is frozen holding
	// one would deadlock. A hung thread makes no progress after resume, so its stack stays coherent.
	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_FULL;

	if (SuspendThread(s_mainThread) == (DWORD)-1)
	{
		fprintf(f, "(could not suspend main thread)\n");
		fclose(f);
		return;
	}

	BOOL gotContext = GetThreadContext(s_mainThread, &ctx);
	ResumeThread(s_mainThread);  // resume immediately -- all remaining work runs unsuspended

	if (gotContext)
		write_context_report(f, s_mainThread, &ctx);
	else
		fprintf(f, "(could not read main-thread context)\n");

	fclose(f);
}

static DWORD WINAPI watchdog_proc(LPVOID unused)
{
	(void)unused;
	LONG last = s_heartbeat;
	int stalled = 0;

	for (;;)
	{
		Sleep(1000);

		const LONG hb = s_heartbeat;
		if (hb != last)
		{
			last = hb;
			stalled = 0;
			s_hangLogged = 0;  // progress resumed -> re-arm for the next stall
			continue;
		}

		// Threshold is read live each second (crashlog_get_hang_timeout), so lowering it in the
		// debug menu to catch a brief freeze takes effect within ~1s without a restart.
		if (++stalled >= crashlog_get_hang_timeout() && !s_hangLogged)
		{
			watchdog_dump_hang(stalled);
			s_hangLogged = 1;  // logged this stall; wait for progress (or manual kill)
		}
	}
}

void watchdog_init(void)
{
	// GetCurrentThread() is a pseudo-handle valid only in the calling thread; duplicate it into a
	// real handle the watchdog can suspend/inspect. Must be called on the main thread.
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
	                     GetCurrentProcess(), &s_mainThread,
	                     0, FALSE, DUPLICATE_SAME_ACCESS))
		return;

	HANDLE t = CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);
	if (t != NULL)
		CloseHandle(t);  // fire-and-forget; the thread lives for the process lifetime
}

#else  // !_WIN32

void install_crash_handler(void) { }
void watchdog_init(void) { }
void watchdog_heartbeat(void) { }

#endif
