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

#include <deque>
#include <memory>
#include <stddef.h>

// Files outside of the gc/ folder should only import gc.h or gc_alloc.h
// which are the "public" memory management interface.

// Some code is only useful towards an effort to implement a
// moving gc, gate behind this flag for now.
#define MOVING_GC 0

#define GC_KEEP_ALIVE(t) asm volatile("" : : "X"(t))

#define TRACE_GC_MARKING 0
#if TRACE_GC_MARKING
extern FILE* trace_fp;
#define GC_TRACE_LOG(...) fprintf(pyston::gc::trace_fp, __VA_ARGS__)
#else
#define GC_TRACE_LOG(...)
#endif

struct _PyWeakReference;
typedef struct _PyWeakReference PyWeakReference;

namespace pyston {

class Box;

namespace gc {
class GCVisitable;
}

namespace threading {
void pushGCObject(gc::GCVisitable* obj);
void popGCObject(gc::GCVisitable* obj);
}

namespace gc {

class TraceStack;

// The base version of the GC visitor is used for marking, in conjuction with a TraceStack.
//
// Conceptually, GCVisitor should be abstract and the 'marking' behavior should be specific
// to a subclass of GCVisitor. However, that requires the use of virtual functions which
// introduce an overhead. Eventually if we really need multiple different kinds of visitors
// we will need some dispatching mechanism but for now, since the moving GC is still WIP,
// the virtualness property is #if'd out for the regular use case with only mark-and-sweep.
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
#if MOVING_GC
    virtual void visit(void* p);
    virtual void visitRange(void* const* start, void* const* end);
    virtual void visitPotential(void* p);
    virtual void visitPotentialRange(void* const* start, void* const* end);
#else
    void visit(void* p);
    void visitRange(void* const* start, void* const* end);
    void visitPotential(void* p);
    void visitPotentialRange(void* const* start, void* const* end);
#endif

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

// Python programs are allowed to pause the GC.  This is supposed to pause automatic GC,
// but does not seem to pause manual calls to gc.collect().  So, callers should check gcIsEnabled(),
// if appropriate, before calling runCollection().
bool gcIsEnabled();
void disableGC();
void enableGC();

void runCollection();

void dumpHeapStatistics(int level);

// These are exposed since the GC isn't necessarily responsible for calling finalizeres.
void callPendingDestructionLogic();
extern std::deque<Box*> pending_finalization_list;
extern std::deque<PyWeakReference*> weakrefs_needing_callback_list;

// These should only be used for debugging outside of the GC module. Except for functions that print
// some debugging information, it should be possible to replace calls to these functions with true
// without changing the behavior of the program.

// If p is a valid gc-allocated pointer (or a non-heap root)
bool isValidGCMemory(void* p);
// Whether p is valid gc memory and is set to have Python destructor semantics applied
bool isValidGCObject(void* p);

// Situation: Sometimes, we allocate an object on the stack (e.g. ASTInterpreter) who fields may be pointers
// to objects in the Pyston heap. These pointers need to be scanned by the GC. Since the GC scans the entire
// stack conservatively, these fields will be scanned. However, it is also possible that the stack-allocated
// object points to a non-Pyston heap object which contains pointers to Pyston heap objects. In that case, the
// conservative scanner won't get to those pointers.
//
// As such, objects who contain pointers to pointers to Pyston heap objects need a GC handler function.

// Runtime objects who need to be visited by the GC should inherit from this.
class GCVisitable {
public:
    virtual ~GCVisitable() = default;
    virtual void gc_visit(GCVisitor* visitor) = 0;
};

// Use this if a C++ object needs to be allocated in our heap.
class GCAllocatedRuntime : public GCVisitable {
public:
    virtual ~GCAllocatedRuntime() = default;

    void* operator new(size_t size) __attribute__((visibility("default"))) { return gc_alloc(size, GCKind::RUNTIME); }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { gc_free(ptr); }

    virtual void gc_visit(GCVisitor* visitor) = 0;
};

// This is a way to call gc_visit on objects whose lifetime is bound to the stack,
// but may be contained within a unique_ptr or some other container.
template <typename T> class UniqueScanningHandle {
    T* obj = NULL;

public:
    UniqueScanningHandle(T* obj) : obj(obj) {
#if MOVING_GC
        if (obj) {
            threading::pushGCObject(obj);
        }
#endif
    }

    ~UniqueScanningHandle() {
#if MOVING_GC
        if (obj) {
            threading::popGCObject(obj);
        }
#endif
        delete obj;
    }

    T* operator->() { return obj; }
    T* get() { return obj; }
    void reset(T* t = nullptr) {
#if MOVING_GC
        if (obj) {
            threading::popGCObject(obj);
        }
#endif
        delete obj;
        obj = t;
#if MOVING_GC
        if (t) {
            threading::pushGCObject(t);
        }
#endif
    }
};

} // namespace gc
}

#endif
