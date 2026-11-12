#pragma once

#include <cstddef>

// HostContext is the deliberately small boundary between the guest scheduler
// and the host's cooperative-context facility. Windows uses native Fibers;
// x86-64 POSIX hosts use libco, and AArch64 uses the shared assembly backend
// with platform-specific reserved-register handling. Handles are only valid
// on the thread that initialized the scheduler.
namespace HostContext {

using Handle = void*;
using Entry = void (*)(void*);

bool InitializeScheduler(Handle* scheduler);
void ShutdownScheduler(Handle scheduler);

Handle Create(std::size_t stackSize, Entry entry, void* argument);
void Destroy(Handle context);
bool IsCurrent(Handle context);
// The context running now (the scheduler's own handle on the thread that initialized it).
Handle Current();
// Stack position at the call, used to bound a suspended stack snapshot.
void* StackPointer();
void Switch(Handle target);

} // namespace HostContext
