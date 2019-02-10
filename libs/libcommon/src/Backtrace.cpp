#include <common/Backtrace.h>

#include <sstream>
#include <cstring>
#include <cxxabi.h>
#include <execinfo.h>

#if USE_UNWIND
std::vector<void *> backtraceLibUnwind(size_t max_frames, ucontext_t & context)
{
    std::vector<void *> out_frames;
    out_frames.reserve(max_frames);
    unw_cursor_t cursor;

    if (unw_init_local2(&cursor, &context, UNW_INIT_SIGNAL_FRAME) >= 0)
    {
        for (size_t i = 0; i < max_frames; ++i)
        {
            unw_word_t ip;
            unw_get_reg(&cursor, UNW_REG_IP, &ip);
            out_frames.push_back(reinterpret_cast<void*>(ip));

            /// NOTE This triggers "AddressSanitizer: stack-buffer-overflow". Looks like false positive.
            /// It's Ok, because we use this method if the program is crashed nevertheless.
            if (!unw_step(&cursor))
                break;
        }
    }

    return out_frames;
}
#endif

std::string signalToErrorMessage(int sig, siginfo_t & info, ucontext_t & context)
{
    std::stringstream error;
    switch (sig)
    {
        case SIGSEGV:
        {
            /// Print info about address and reason.
            if (nullptr == info.si_addr)
                error << "Address: NULL pointer.";
            else
                error << "Address: " << info.si_addr;

#if defined(__x86_64__) && !defined(__FreeBSD__) && !defined(__APPLE__)
            auto err_mask = context.uc_mcontext.gregs[REG_ERR];
            if ((err_mask & 0x02))
                error << " Access: write.";
            else
                error << " Access: read.";
#endif

            switch (info.si_code)
            {
                case SEGV_ACCERR:
                    error << " Attempted access has violated the permissions assigned to the memory area.";
                    break;
                case SEGV_MAPERR:
                    error << " Address not mapped to object.";
                    break;
                default:
                    error << " Unknown si_code.";
                    break;
            }
            break;
        }

        case SIGBUS:
        {
            switch (info.si_code)
            {
                case BUS_ADRALN:
                    error << "Invalid address alignment.";
                    break;
                case BUS_ADRERR:
                    error << "Non-existant physical address.";
                    break;
                case BUS_OBJERR:
                    error << "Object specific hardware error.";
                    break;

                    // Linux specific
#if defined(BUS_MCEERR_AR)
                case BUS_MCEERR_AR:
                    error << "Hardware memory error: action required.";
                    break;
#endif
#if defined(BUS_MCEERR_AO)
                case BUS_MCEERR_AO:
                    error << "Hardware memory error: action optional.";
                    break;
#endif

                default:
                    error << "Unknown si_code.";
                    break;
            }
            break;
        }

        case SIGILL:
        {
            switch (info.si_code)
            {
                case ILL_ILLOPC:
                    error << "Illegal opcode.";
                    break;
                case ILL_ILLOPN:
                    error << "Illegal operand.";
                    break;
                case ILL_ILLADR:
                    error << "Illegal addressing mode.";
                    break;
                case ILL_ILLTRP:
                    error << "Illegal trap.";
                    break;
                case ILL_PRVOPC:
                    error << "Privileged opcode.";
                    break;
                case ILL_PRVREG:
                    error << "Privileged register.";
                    break;
                case ILL_COPROC:
                    error << "Coprocessor error.";
                    break;
                case ILL_BADSTK:
                    error << "Internal stack error.";
                    break;
                default:
                    error << "Unknown si_code.";
                    break;
            }
            break;
        }

        case SIGFPE:
        {
            switch (info.si_code)
            {
                case FPE_INTDIV:
                    error << "Integer divide by zero.";
                    break;
                case FPE_INTOVF:
                    error << "Integer overflow.";
                    break;
                case FPE_FLTDIV:
                    error << "Floating point divide by zero.";
                    break;
                case FPE_FLTOVF:
                    error << "Floating point overflow.";
                    break;
                case FPE_FLTUND:
                    error << "Floating point underflow.";
                    break;
                case FPE_FLTRES:
                    error << "Floating point inexact result.";
                    break;
                case FPE_FLTINV:
                    error << "Floating point invalid operation.";
                    break;
                case FPE_FLTSUB:
                    error << "Subscript out of range.";
                    break;
                default:
                    error << "Unknown si_code.";
                    break;
            }
            break;
        }
    }

    return error.str();
}

void * getCallerAddress(ucontext_t & context)
{
#if defined(__x86_64__)
    /// Get the address at the time the signal was raised from the RIP (x86-64)
#if defined(__FreeBSD__)
    return reinterpret_cast<void *>(context.uc_mcontext.mc_rip);
#elif defined(__APPLE__)
    return reinterpret_cast<void *>(context.uc_mcontext->__ss.__rip);
#else
    return reinterpret_cast<void *>(context.uc_mcontext.gregs[REG_RIP]);
#endif
#elif defined(__aarch64__)
    return reinterpret_cast<void *>(context.uc_mcontext.pc);
#endif

    return nullptr;
}

std::vector<void *> getBacktraceFrames(ucontext_t & context)
{
    std::vector<void *> frames;

#if USE_UNWIND
    static size_t max_frames = 50;
    frames = backtraceLibUnwind(max_frames, context);
#else
    /// No libunwind means no backtrace, because we are in a different thread from the one where the signal happened.
    /// So at least print the function where the signal happened.
    void * caller_address = getCallerAddress(context);
    if (caller_address)
        frames.push_back(caller_address);
#endif

    return frames;
}

std::string backtraceFramesToString(const std::vector<void *> & frames, const std::string & delimiter)
{
    std::stringstream backtrace;
    char ** symbols = backtrace_symbols(frames.data(), frames.size());

    if (!symbols)
    {
        if (frames.size() > 0)
            backtrace << "No symbols could be found for backtrace starting at " << frames[0];
    }
    else
    {
        for (size_t i = 0; i < frames.size(); ++i)
        {
            /// Perform demangling of names. Name is in parentheses, before '+' character.

            char * name_start = nullptr;
            char * name_end = nullptr;
            char * demangled_name = nullptr;
            int status = 0;

            if (nullptr != (name_start = strchr(symbols[i], '('))
                && nullptr != (name_end = strchr(name_start, '+')))
            {
                ++name_start;
                *name_end = '\0';
                demangled_name = abi::__cxa_demangle(name_start, 0, 0, &status);
                *name_end = '+';
            }

            backtrace << i << ". ";

            if (nullptr != demangled_name && 0 == status)
            {
                backtrace.write(symbols[i], name_start - symbols[i]);
                backtrace << demangled_name << name_end;
            }
            else
                backtrace << symbols[i] << delimiter;
        }
    }

    return backtrace.str();
}
