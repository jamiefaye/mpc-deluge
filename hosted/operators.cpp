// Global operator new and delete for everything linked with the firmware.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware replaces two of them, new(size) and delete(pointer), with its own allocator
// (memory/operators.cpp). Two things are wrong with that once the firmware is a library inside a
// plugin:
//
// 1. The replacement applies to every new in the plugin file, on every thread: the plugin's own
//    code, the SDK framework's strings and vectors on the MPC's threads. The firmware's allocator
//    is not thread-safe (on a Deluge everything runs on one thread). A second thread inside it
//    finds it "locked", gets a null pointer, and the firmware's own allocations fail with it.
// 2. It relies on the C++ library's other forms (sized and array delete, nothrow new and so on)
//    forwarding to those two. That holds where the C++ library is linked into the same file, as
//    for the ARM Linux builds. On macOS it is a system library whose other forms call the
//    system's own new and delete, so memory came from one allocator and went back to the other.
//
// So here every form is defined, and:
//
//   new      on a thread that is inside the engine (between deluge_hosted_enter_engine() and
//            deluge_hosted_leave_engine(), which every function in engine.cpp calls): the firmware's allocator, its external region, as the
//            firmware's own operator did; and if that is full, the system's malloc, rather than
//            the null pointer the firmware's version returned, which nothing that calls new is
//            prepared for. On any other thread: the system's malloc.
//   delete   the firmware's allocator if the address lies in its block, otherwise free()
//
// A pointer is therefore always released by whoever made it. The engine's functions hand nothing
// allocated across their boundary, so memory from the firmware's allocator is also freed inside
// the engine, by the one thread that is in it.
//
// These are compiled with hidden visibility like the rest of the library: they apply to the plugin
// file they are linked into and never to the host application.
#include "hosted.h"
#include "memory/general_memory_allocator.h"

#include <cstdlib>
#include <new>

namespace {
thread_local int engineDepth = 0;
} // namespace

void deluge_hosted_enter_engine() noexcept {
	engineDepth++;
}
void deluge_hosted_leave_engine() noexcept {
	engineDepth--;
}

namespace {
inline bool fromFirmwareAllocator(void* p) {
	const uintptr_t a = (uintptr_t)p;
	return a >= deluge_hosted_external_begin && a < deluge_hosted_internal_end;
}

inline void* allocate(std::size_t n) {
	void* p = nullptr;
	if (engineDepth > 0) {
		p = GeneralMemoryAllocator::get().allocExternal((uint32_t)n);
	}
	if (p == nullptr) {
		p = std::malloc(n ? n : 1);
	}
	return p;
}

inline void release(void* p) {
	if (p == nullptr) {
		return;
	}
	if (fromFirmwareAllocator(p)) {
		delugeDealloc(p);
	}
	else {
		std::free(p);
	}
}

// Alignment beyond what the firmware's allocator gives (8 bytes) comes from the system.
inline void* allocateAligned(std::size_t n, std::align_val_t alignment) {
	std::size_t a = (std::size_t)alignment;
	if (a <= 8) {
		return allocate(n);
	}
	void* p = nullptr;
	if (posix_memalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n ? n : 1) != 0) {
		return nullptr;
	}
	return p;
}
} // namespace

void* operator new(std::size_t n) noexcept(false) {
	return allocate(n);
}
void* operator new[](std::size_t n) noexcept(false) {
	return allocate(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
	return allocate(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
	return allocate(n);
}
void* operator new(std::size_t n, std::align_val_t alignment) noexcept(false) {
	return allocateAligned(n, alignment);
}
void* operator new[](std::size_t n, std::align_val_t alignment) noexcept(false) {
	return allocateAligned(n, alignment);
}
void* operator new(std::size_t n, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return allocateAligned(n, alignment);
}
void* operator new[](std::size_t n, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return allocateAligned(n, alignment);
}

void operator delete(void* p) noexcept {
	release(p);
}
void operator delete[](void* p) noexcept {
	release(p);
}
void operator delete(void* p, std::size_t) noexcept {
	release(p);
}
void operator delete[](void* p, std::size_t) noexcept {
	release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
	release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
	release(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
	release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	release(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
	release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
	release(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
	release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
	release(p);
}
