// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PYSTON_GC_GC_H
#define PYSTON_GC_GC_H

namespace pyston {
namespace gc {

// GOAL: Eventually, move any declaration that needs to be visible outside the gc/ folder
// to this file and only expose this header.

class TraceStack;
class GCVisitor {
private:
    TraceStack* stack;

public:
    GCVisitor(TraceStack* stack) : stack(stack) {}
    virtual ~GCVisitor() {}

    // These all work on *user* pointers, ie pointers to the user_data section of GCAllocations
    void visitIf(void* p) {
        if (p)
            visit(p);
    }
    void visit(void* p);
    void visitRange(void* const* start, void* const* end);
    void visitPotential(void* p);
    void visitPotentialRange(void* const* start, void* const* end);

    // Some object have fields with pointers to Pyston heap objects that we are confident are
    // already being scanned elsewhere.
    //
    // In a mark-and-sweep collector, scanning those fields would be redundant because the mark
    // phase only needs to visit each object once, so there would be a performance hit.
    //
    // In a moving collector, every reference needs to be visited since the pointer value could
    // change. We don't have a moving collector yet, but it's good practice to call visit every
    // pointer value and no-op to avoid the performance hit of the mark-and-sweep case.
    virtual void visitRedundant(void* p) {}
    virtual void visitRedundantRange(void** start, void** end) {}
    virtual void visitPotentialRedundant(void* p) {}
    virtual void visitPotentialRangeRedundant(void* const* start, void* const* end) {}
};

enum class GCKind : uint8_t {
    // Any Python object (e.g. any Box) that can be visited precisely, using
    // a GC handler function.
    PYTHON = 1,

    // An arbitrary block of memory that may contain pointers.
    CONSERVATIVE = 2,

    // An arbitrary block of memory with contiguous pointers.
    PRECISE = 3,

    // An arbitrary block of memory that does not contain pointers.
    UNTRACKED = 4,

    // C++ objects that we need to manage with our own heap and GC, either
    // because it contains pointers into our heap or our heap points to these
    // objects. These objects inherit from GCAllocatedRuntime.
    RUNTIME = 5,
};

extern "C" void* gc_alloc(size_t nbytes, GCKind kind);
extern "C" void* gc_realloc(void* ptr, size_t bytes);
extern "C" void gc_free(void* ptr);

// Use this if a C++ object needs to be allocated in our heap.
class GCAllocatedRuntime {
public:
    virtual ~GCAllocatedRuntime() {}

    void* operator new(size_t size) __attribute__((visibility("default"))) { return gc_alloc(size, GCKind::RUNTIME); }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { gc_free(ptr); }

    virtual void gc_visit(GCVisitor* visitor) = 0;

    // Analoguous to has_simple_tp_dealloc on BoxedClasses. Should only contain trivial destruction
    // logic that does not depend on the order of objects being freed, does not cause objects to be
    // resurrected, etc.
    virtual void simple_finalizer(){};
};
} // namespace gc
}

#endif
