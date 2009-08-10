/* Copyright (c) 2008-2009, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "machine.h"
#include "util.h"
#include "vector.h"
#include "process.h"
#include "assembler.h"
#include "compiler.h"
#include "arch.h"

using namespace vm;

extern "C" uint64_t
vmInvoke(void* thread, void* function, void* arguments,
         unsigned argumentFootprint, unsigned frameSize, unsigned returnType);

extern "C" void
vmJumpAndInvoke(void* thread, void* function, void* base, void* stack,
                unsigned argumentFootprint, uintptr_t* arguments,
                unsigned frameSize);

extern "C" void
vmCall();

namespace {

const bool DebugCompile = false;
const bool DebugNatives = false;
const bool DebugCallTable = false;
const bool DebugMethodTree = false;
const bool DebugFrameMaps = false;

const bool CheckArrayBounds = true;

#ifdef AVIAN_CONTINUATIONS
const bool Continuations = true;
#else
const bool Continuations = false;
#endif

const unsigned MaxNativeCallFootprint = 4;

const unsigned InitialZoneCapacityInBytes = 64 * 1024;

const unsigned ExecutableAreaSizeInBytes = 16 * 1024 * 1024;

class MyThread: public Thread {
 public:
  class CallTrace {
   public:
    CallTrace(MyThread* t, object method):
      t(t),
      base(t->base),
      stack(t->stack),
      continuation(t->continuation),
      nativeMethod((methodFlags(t, method) & ACC_NATIVE) ? method : 0),
      targetMethod(0),
      originalMethod(method),
      next(t->trace)
    {
      t->trace = this;
      t->base = 0;
      t->stack = 0;
      t->continuation = 0;
    }

    ~CallTrace() {
      t->stack = stack;
      t->base = base;
      t->continuation = continuation;
      t->trace = next;
    }

    MyThread* t;
    void* base;
    void* stack;
    object continuation;
    object nativeMethod;
    object targetMethod;
    object originalMethod;
    CallTrace* next;
  };

  MyThread(Machine* m, object javaThread, MyThread* parent):
    Thread(m, javaThread, parent),
    ip(0),
    base(0),
    stack(0),
    continuation(0),
    exceptionStackAdjustment(0),
    exceptionOffset(0),
    exceptionHandler(0),
    tailAddress(0),
    virtualCallTarget(0),
    virtualCallIndex(0),
    trace(0),
    reference(0),
    arch(parent ? parent->arch : makeArchitecture(m->system))
  {
    arch->acquire();
  }

  void* ip;
  void* base;
  void* stack;
  object continuation;
  uintptr_t exceptionStackAdjustment;
  uintptr_t exceptionOffset;
  void* exceptionHandler;
  void* tailAddress;
  void* virtualCallTarget;
  uintptr_t virtualCallIndex;
  CallTrace* trace;
  Reference* reference;
  Assembler::Architecture* arch;
};

unsigned
parameterOffset(MyThread* t, object method)
{
  return methodParameterFootprint(t, method)
    + t->arch->frameFooterSize()
    + t->arch->frameReturnAddressSize() - 1;
}

object
resolveThisPointer(MyThread* t, void* stack)
{
  return reinterpret_cast<object*>(stack)
    [t->arch->frameFooterSize() + t->arch->frameReturnAddressSize()];
}

object
resolveTarget(MyThread* t, void* stack, object method)
{
  object class_ = objectClass(t, resolveThisPointer(t, stack));

  if (classVmFlags(t, class_) & BootstrapFlag) {
    PROTECT(t, method);
    PROTECT(t, class_);

    resolveSystemClass(t, className(t, class_));
    if (UNLIKELY(t->exception)) return 0;
  }

  if (classFlags(t, methodClass(t, method)) & ACC_INTERFACE) {
    return findInterfaceMethod(t, method, class_);
  } else {
    return findMethod(t, method, class_);
  }
}

object
resolveTarget(MyThread* t, object class_, unsigned index)
{
  if (classVmFlags(t, class_) & BootstrapFlag) {
    PROTECT(t, class_);

    resolveSystemClass(t, className(t, class_));
    if (UNLIKELY(t->exception)) return 0;
  }

  return arrayBody(t, classVirtualTable(t, class_), index);
}

object&
methodTree(MyThread* t);

object
methodTreeSentinal(MyThread* t);

unsigned
compiledSize(intptr_t address)
{
  return reinterpret_cast<uintptr_t*>(address)[-1];
}

intptr_t
compareIpToMethodBounds(Thread* t, intptr_t ip, object method)
{
  intptr_t start = methodCompiled(t, method);

  if (DebugMethodTree) {
    fprintf(stderr, "find %p in (%p,%p)\n",
            reinterpret_cast<void*>(ip),
            reinterpret_cast<void*>(start),
            reinterpret_cast<void*>(start + compiledSize(start)));
  }

  if (ip < start) {
    return -1;
  } else if (ip < start + static_cast<intptr_t>
             (compiledSize(start) + BytesPerWord))
  {
    return 0;
  } else {
    return 1;
  }
}

object
methodForIp(MyThread* t, void* ip)
{
  if (DebugMethodTree) {
    fprintf(stderr, "query for method containing %p\n", ip);
  }

  // we must use a version of the method tree at least as recent as the
  // compiled form of the method containing the specified address (see
  // compile(MyThread*, Allocator*, BootContext*, object)):
  memoryBarrier();

  return treeQuery(t, methodTree(t), reinterpret_cast<intptr_t>(ip),
                   methodTreeSentinal(t), compareIpToMethodBounds);
}

class MyStackWalker: public Processor::StackWalker {
 public:
  enum State {
    Start,
    Next,
    Continuation,
    Method,
    NativeMethod,
    Finish
  };

  class MyProtector: public Thread::Protector {
   public:
    MyProtector(MyStackWalker* walker):
      Protector(walker->t), walker(walker)
    { }

    virtual void visit(Heap::Visitor* v) {
      v->visit(&(walker->method_));
      v->visit(&(walker->continuation));
    }

    MyStackWalker* walker;
  };

  MyStackWalker(MyThread* t):
    t(t),
    state(Start),
    ip_(t->ip),
    base(t->base),
    stack(t->stack),
    trace(t->trace),
    method_(0),
    continuation(t->continuation),
    protector(this)
  { }

  MyStackWalker(MyStackWalker* w):
    t(w->t),
    state(w->state),
    ip_(w->ip_),
    base(w->base),
    stack(w->stack),
    trace(w->trace),
    method_(w->method_),
    continuation(w->continuation),
    protector(this)
  { }

  virtual void walk(Processor::StackVisitor* v) {
    for (MyStackWalker it(this); it.valid();) {
      MyStackWalker walker(&it);
      if (not v->visit(&walker)) {
        break;
      }
      it.next();
    }
  }
    
  bool valid() {
    while (true) {
//       fprintf(stderr, "state: %d\n", state);
      switch (state) {
      case Start:
        if (ip_ == 0) {
          ip_ = t->arch->frameIp(stack);
        }

        if (trace and trace->nativeMethod) {
          method_ = trace->nativeMethod;
          state = NativeMethod;
        } else {
          state = Next;
        }
        break;

      case Next:
        if (stack) {
          method_ = methodForIp(t, ip_);
          if (method_) {
            state = Method;
          } else if (continuation) {
            method_ = continuationMethod(t, continuation);
            state = Continuation;
          } else if (trace) {
            continuation = trace->continuation;
            stack = trace->stack;
            base = trace->base;
            ip_ = t->arch->frameIp(stack);
            trace = trace->next;

            if (trace and trace->nativeMethod) {
              method_ = trace->nativeMethod;
              state = NativeMethod;
            }
          } else {
            state = Finish;
          }
        } else {
          state = Finish;
        }
        break;

      case Continuation:
      case Method:
      case NativeMethod:
        return true;

      case Finish:
        return false;
   
      default:
        abort(t);
      }
    }
  }
    
  void next() {
    switch (state) {
    case Continuation:
      continuation = continuationNext(t, continuation);
      break;

    case Method:
      t->arch->nextFrame(&stack, &base);
      ip_ = t->arch->frameIp(stack);
      break;

    case NativeMethod:
      break;
   
    default:
      abort(t);
    }

    state = Next;
  }

  virtual object method() {
//     fprintf(stderr, "method %s.%s\n", &byteArrayBody
//             (t, className(t, methodClass(t, method_)), 0),
//             &byteArrayBody(t, methodName(t, method_), 0));
    return method_;
  }

  virtual int ip() {
    switch (state) {
    case Continuation:
      return reinterpret_cast<intptr_t>(continuationAddress(t, continuation))
        - methodCompiled(t, continuationMethod(t, continuation));

    case Method:
      return reinterpret_cast<intptr_t>(ip_) - methodCompiled(t, method_);
        
    case NativeMethod:
      return 0;

    default:
      abort(t);
    }
  }

  virtual unsigned count() {
    unsigned count = 0;

    for (MyStackWalker walker(this); walker.valid();) {
      walker.next();
      ++ count;
    }
    
    return count;
  }

  MyThread* t;
  State state;
  void* ip_;
  void* base;
  void* stack;
  MyThread::CallTrace* trace;
  object method_;
  object continuation;
  MyProtector protector;
};

unsigned
localSize(MyThread* t, object method)
{
  unsigned size = codeMaxLocals(t, methodCode(t, method));
  if ((methodFlags(t, method) & (ACC_SYNCHRONIZED | ACC_STATIC))
      == ACC_SYNCHRONIZED)
  {
    ++ size;
  }
  return size;
}

unsigned
alignedFrameSize(MyThread* t, object method)
{
  return t->arch->alignFrameSize
    (localSize(t, method)
     - methodParameterFootprint(t, method)
     + codeMaxStack(t, methodCode(t, method))
     + t->arch->frameFootprint(MaxNativeCallFootprint));
}

int
localOffset(MyThread* t, int v, object method)
{
  int parameterFootprint = methodParameterFootprint(t, method);
  int frameSize = alignedFrameSize(t, method);

  int offset = ((v < parameterFootprint) ?
                (frameSize
                 + parameterFootprint
                 + t->arch->frameFooterSize()
                 + t->arch->frameHeaderSize()
                 - v - 1) :
                (frameSize
                 + parameterFootprint
                 - v - 1));

  assert(t, offset >= 0);
  return offset;
}

int
localOffsetFromStack(MyThread* t, int index, object method)
{
  return localOffset(t, index, method)
    + t->arch->frameReturnAddressSize();
}

object*
localObject(MyThread* t, void* stack, object method, unsigned index)
{
  return static_cast<object*>(stack) + localOffsetFromStack(t, index, method);
}

int
stackOffsetFromFrame(MyThread* t, object method)
{
  return alignedFrameSize(t, method) + t->arch->frameHeaderSize();
}

void*
stackForFrame(MyThread* t, void* frame, object method)
{
  return static_cast<void**>(frame) - stackOffsetFromFrame(t, method);
}

class PoolElement: public Promise {
 public:
  PoolElement(Thread* t, object target, PoolElement* next):
    t(t), target(target), address(0), next(next)
  { }

  virtual int64_t value() {
    assert(t, resolved());
    return address;
  }

  virtual bool resolved() {
    return address != 0;
  }

  Thread* t;
  object target;
  intptr_t address;
  PoolElement* next;
};

class Context;
class SubroutineCall;

class Subroutine {
 public:
  Subroutine(unsigned ip, unsigned logIndex, Subroutine* listNext,
             Subroutine* stackNext):
    listNext(listNext),
    stackNext(stackNext),
    calls(0),
    handle(0),
    ip(ip),
    logIndex(logIndex),
    stackIndex(0),
    callCount(0),
    tableIndex(0),
    visited(false)
  { }

  Subroutine* listNext;
  Subroutine* stackNext;
  SubroutineCall* calls;
  Compiler::Subroutine* handle;
  unsigned ip;
  unsigned logIndex;
  unsigned stackIndex;
  unsigned callCount;
  unsigned tableIndex;
  bool visited;
};

class SubroutinePath;

class SubroutineCall {
 public:
  SubroutineCall(Subroutine* subroutine, Promise* returnAddress):
    subroutine(subroutine),
    returnAddress(returnAddress),
    paths(0),
    next(subroutine->calls)
  {
    subroutine->calls = this;
    ++ subroutine->callCount;
  }

  Subroutine* subroutine;
  Promise* returnAddress;
  SubroutinePath* paths;
  SubroutineCall* next;
};

class SubroutinePath {
 public:
  SubroutinePath(SubroutineCall* call, SubroutinePath* stackNext,
                 uintptr_t* rootTable):
    call(call),
    stackNext(stackNext),
    listNext(call->paths),
    rootTable(rootTable)
  {
    call->paths = this;
  }

  SubroutineCall* call;
  SubroutinePath* stackNext;
  SubroutinePath* listNext;
  uintptr_t* rootTable;
};

void
print(SubroutinePath* path)
{
  if (path) {
    fprintf(stderr, " (");
    while (true) {
      fprintf(stderr, "%p", reinterpret_cast<void*>
              (path->call->returnAddress->value()));
      path = path->stackNext;
      if (path) {
        fprintf(stderr, ", ");
      } else {
        break;
      }
    }
    fprintf(stderr, ")");
  }
}

class SubroutineTrace {
 public:
  SubroutineTrace(SubroutinePath* path, SubroutineTrace* next):
    path(path),
    next(next)
  { }

  SubroutinePath* path;
  SubroutineTrace* next;
  uintptr_t map[0];
};

class TraceElement: public TraceHandler {
 public:
  static const unsigned VirtualCall = 1 << 0;
  static const unsigned TailCall    = 1 << 1;

  TraceElement(Context* context, object target, unsigned flags,
               TraceElement* next):
    context(context),
    address(0),
    next(next),
    subroutineTrace(0),
    subroutineTraceCount(0),
    target(target),
    argumentIndex(0),
    flags(flags)
  { }

  virtual void handleTrace(Promise* address, unsigned argumentIndex) {
    if (this->address == 0) {
      this->address = address;
      this->argumentIndex = argumentIndex;
    }
  }

  Context* context;
  Promise* address;
  TraceElement* next;
  SubroutineTrace* subroutineTrace;
  unsigned subroutineTraceCount;
  object target;
  unsigned argumentIndex;
  unsigned flags;
  uintptr_t map[0];
};

class TraceElementPromise: public Promise {
 public:
  TraceElementPromise(System* s, TraceElement* trace): s(s), trace(trace) { }

  virtual int64_t value() {
    assert(s, resolved());
    return trace->address->value();
  }

  virtual bool resolved() {
    return trace->address != 0 and trace->address->resolved();
  }

  System* s;
  TraceElement* trace;
};

enum Event {
  PushContextEvent,
  PopContextEvent,
  IpEvent,
  MarkEvent,
  ClearEvent,
  PushExceptionHandlerEvent,
  TraceEvent,
  PushSubroutineEvent,
  PopSubroutineEvent
};

unsigned
frameMapSizeInBits(MyThread* t, object method)
{
  return localSize(t, method) + codeMaxStack(t, methodCode(t, method));
}

unsigned
frameMapSizeInWords(MyThread* t, object method)
{
  return ceiling(frameMapSizeInBits(t, method), BitsPerWord);
}

uint16_t*
makeVisitTable(MyThread* t, Zone* zone, object method)
{
  unsigned size = codeLength(t, methodCode(t, method)) * 2;
  uint16_t* table = static_cast<uint16_t*>(zone->allocate(size));
  memset(table, 0, size);
  return table;
}

uintptr_t*
makeRootTable(MyThread* t, Zone* zone, object method)
{
  unsigned size = frameMapSizeInWords(t, method)
    * codeLength(t, methodCode(t, method))
    * BytesPerWord;
  uintptr_t* table = static_cast<uintptr_t*>(zone->allocate(size));
  memset(table, 0xFF, size);
  return table;
}

enum Thunk {
#define THUNK(s) s##Thunk,

#include "thunks.cpp"

#undef THUNK
};

const unsigned ThunkCount = gcIfNecessaryThunk + 1;

intptr_t
getThunk(MyThread* t, Thunk thunk);

class BootContext {
 public:
  class MyProtector: public Thread::Protector {
   public:
    MyProtector(Thread* t, BootContext* c): Protector(t), c(c) { }

    virtual void visit(Heap::Visitor* v) {
      v->visit(&(c->constants));
      v->visit(&(c->calls));
    }

    BootContext* c;
  };

  BootContext(Thread* t, object constants, object calls,
              DelayedPromise* addresses, Zone* zone):
    protector(t, this), constants(constants), calls(calls),
    addresses(addresses), addressSentinal(addresses), zone(zone)
  { }

  MyProtector protector;
  object constants;
  object calls;
  DelayedPromise* addresses;
  DelayedPromise* addressSentinal;
  Zone* zone;
};

class Context {
 public:
  class MyProtector: public Thread::Protector {
   public:
    MyProtector(Context* c): Protector(c->thread), c(c) { }

    virtual void visit(Heap::Visitor* v) {
      v->visit(&(c->method));

      for (PoolElement* p = c->objectPool; p; p = p->next) {
        v->visit(&(p->target));
      }

      for (TraceElement* p = c->traceLog; p; p = p->next) {
        v->visit(&(p->target));
      }
    }

    Context* c;
  };

  class MyClient: public Compiler::Client {
   public:
    MyClient(MyThread* t): t(t) { }

    virtual intptr_t getThunk(UnaryOperation, unsigned) {
      abort(t);
    }

    virtual intptr_t getThunk(TernaryOperation op, unsigned size) {
      switch (op) {
      case Divide:
        if (size == 8) {
          return ::getThunk(t, divideLongThunk);
        }
        break;

      case Remainder:
        if (size == 8) {
          return ::getThunk(t, moduloLongThunk);
        }
        break;

      default: break;
      }

      abort(t);
    }

    MyThread* t;
  };

  Context(MyThread* t, BootContext* bootContext, object method):
    thread(t),
    zone(t->m->system, t->m->heap, InitialZoneCapacityInBytes),
    assembler(makeAssembler(t->m->system, t->m->heap, &zone, t->arch)),
    client(t),
    compiler(makeCompiler(t->m->system, assembler, &zone, &client)),
    method(method),
    bootContext(bootContext),
    objectPool(0),
    subroutines(0),
    traceLog(0),
    visitTable(makeVisitTable(t, &zone, method)),
    rootTable(makeRootTable(t, &zone, method)),
    subroutineTable(0),
    objectPoolCount(0),
    traceLogCount(0),
    dirtyRoots(false),
    eventLog(t->m->system, t->m->heap, 1024),
    protector(this)
  { }

  Context(MyThread* t):
    thread(t),
    zone(t->m->system, t->m->heap, InitialZoneCapacityInBytes),
    assembler(makeAssembler(t->m->system, t->m->heap, &zone, t->arch)),
    client(t),
    compiler(0),
    method(0),
    bootContext(0),
    objectPool(0),
    subroutines(0),
    traceLog(0),
    visitTable(0),
    rootTable(0),
    subroutineTable(0),
    objectPoolCount(0),
    traceLogCount(0),
    dirtyRoots(false),
    eventLog(t->m->system, t->m->heap, 0),
    protector(this)
  { }

  ~Context() {
    if (compiler) compiler->dispose();
    assembler->dispose();
  }

  MyThread* thread;
  Zone zone;
  Assembler* assembler;
  MyClient client;
  Compiler* compiler;
  object method;
  BootContext* bootContext;
  PoolElement* objectPool;
  Subroutine* subroutines;
  TraceElement* traceLog;
  uint16_t* visitTable;
  uintptr_t* rootTable;
  Subroutine** subroutineTable;
  unsigned objectPoolCount;
  unsigned traceLogCount;
  bool dirtyRoots;
  Vector eventLog;
  MyProtector protector;
};

unsigned
translateLocalIndex(Context* context, unsigned footprint, unsigned index)
{
  unsigned parameterFootprint = methodParameterFootprint
    (context->thread, context->method);

  if (index < parameterFootprint) {
    return parameterFootprint - index - footprint;
  } else {
    return index;
  }
}

Compiler::Operand*
loadLocal(Context* context, unsigned footprint, unsigned index)
{
  return context->compiler->loadLocal
    (footprint, translateLocalIndex(context, footprint, index));
}

void
storeLocal(Context* context, unsigned footprint, Compiler::Operand* value,
           unsigned index)
{
  context->compiler->storeLocal
    (footprint, value, translateLocalIndex(context, footprint, index));
}

class Frame {
 public:
  enum StackType {
    Integer,
    Long,
    Object
  };

  Frame(Context* context, uint8_t* stackMap):
    context(context),
    t(context->thread),
    c(context->compiler),
    subroutine(0),
    stackMap(stackMap),
    ip(0),
    sp(localSize()),
    level(0)
  {
    memset(stackMap, 0, codeMaxStack(t, methodCode(t, context->method)));
  }

  Frame(Frame* f, uint8_t* stackMap):
    context(f->context),
    t(context->thread),
    c(context->compiler),
    subroutine(f->subroutine),
    stackMap(stackMap),
    ip(f->ip),
    sp(f->sp),
    level(f->level + 1)
  {
    memcpy(stackMap, f->stackMap, codeMaxStack
           (t, methodCode(t, context->method)));

    if (level > 1) {
      context->eventLog.append(PushContextEvent);
    }
  }

  ~Frame() {
    if (t->exception == 0) {
      if (level > 1) {
        context->eventLog.append(PopContextEvent);      
      }
    }
  }

  Compiler::Operand* append(object o) {
    if (context->bootContext) {
      BootContext* bc = context->bootContext;

      Promise* p = new (bc->zone->allocate(sizeof(ListenPromise)))
        ListenPromise(t->m->system, bc->zone);

      PROTECT(t, o);
      object pointer = makePointer(t, p);
      bc->constants = makeTriple(t, o, pointer, bc->constants);

      return c->promiseConstant(p);
    } else {
      context->objectPool = new
        (context->zone.allocate(sizeof(PoolElement)))
        PoolElement(t, o, context->objectPool);

      ++ context->objectPoolCount;

      return c->address(context->objectPool);
    }
  }

  unsigned localSize() {
    return ::localSize(t, context->method);
  }

  unsigned stackSize() {
    return codeMaxStack(t, methodCode(t, context->method));
  }

  unsigned frameSize() {
    return localSize() + stackSize();
  }

  void set(unsigned index, uint8_t type) {
    assert(t, index < frameSize());

    if (type == Object) {
      context->eventLog.append(MarkEvent);
      context->eventLog.append2(index);
    } else {
      context->eventLog.append(ClearEvent);
      context->eventLog.append2(index);
    }

    int si = index - localSize();
    if (si >= 0) {
      stackMap[si] = type;
    }
  }

  uint8_t get(unsigned index) {
    assert(t, index < frameSize());
    int si = index - localSize();
    assert(t, si >= 0);
    return stackMap[si];
  }

  void pushedInt() {
    assert(t, sp + 1 <= frameSize());
    set(sp++, Integer);
  }

  void pushedLong() {
    assert(t, sp + 2 <= frameSize());
    set(sp++, Long);
    set(sp++, Long);
  }

  void pushedObject() {
    assert(t, sp + 1 <= frameSize());
    set(sp++, Object);
  }

  void popped(unsigned count) {
    assert(t, sp >= count);
    assert(t, sp - count >= localSize());
    while (count) {
      set(--sp, Integer);
      -- count;
    }
  }
  
  void poppedInt() {
    assert(t, sp >= 1);
    assert(t, sp - 1 >= localSize());
    assert(t, get(sp - 1) == Integer);
    -- sp;
  }
  
  void poppedLong() {
    assert(t, sp >= 1);
    assert(t, sp - 2 >= localSize());
    assert(t, get(sp - 1) == Long);
    assert(t, get(sp - 2) == Long);
    sp -= 2;
  }
  
  void poppedObject() {
    assert(t, sp >= 1);
    assert(t, sp - 1 >= localSize());
    assert(t, get(sp - 1) == Object);
    set(--sp, Integer);
  }

  void storedInt(unsigned index) {
    assert(t, index < localSize());
    set(index, Integer);
  }

  void storedLong(unsigned index) {
    assert(t, index + 1 < localSize());
    set(index, Long);
    set(index + 1, Long);
  }

  void storedObject(unsigned index) {
    assert(t, index < localSize());
    set(index, Object);
  }

  void dupped() {
    assert(t, sp + 1 <= frameSize());
    assert(t, sp - 1 >= localSize());
    set(sp, get(sp - 1));
    ++ sp;
  }

  void duppedX1() {
    assert(t, sp + 1 <= frameSize());
    assert(t, sp - 2 >= localSize());

    uint8_t b2 = get(sp - 2);
    uint8_t b1 = get(sp - 1);

    set(sp - 1, b2);
    set(sp - 2, b1);
    set(sp    , b1);

    ++ sp;
  }

  void duppedX2() {
    assert(t, sp + 1 <= frameSize());
    assert(t, sp - 3 >= localSize());

    uint8_t b3 = get(sp - 3);
    uint8_t b2 = get(sp - 2);
    uint8_t b1 = get(sp - 1);

    set(sp - 2, b3);
    set(sp - 1, b2);
    set(sp - 3, b1);
    set(sp    , b1);

    ++ sp;
  }

  void dupped2() {
    assert(t, sp + 2 <= frameSize());
    assert(t, sp - 2 >= localSize());

    uint8_t b2 = get(sp - 2);
    uint8_t b1 = get(sp - 1);

    set(sp, b2);
    set(sp + 1, b1);

    sp += 2;
  }

  void dupped2X1() {
    assert(t, sp + 2 <= frameSize());
    assert(t, sp - 3 >= localSize());

    uint8_t b3 = get(sp - 3);
    uint8_t b2 = get(sp - 2);
    uint8_t b1 = get(sp - 1);

    set(sp - 1, b3);
    set(sp - 3, b2);
    set(sp    , b2);
    set(sp - 2, b1);
    set(sp + 1, b1);

    sp += 2;
  }

  void dupped2X2() {
    assert(t, sp + 2 <= frameSize());
    assert(t, sp - 4 >= localSize());

    uint8_t b4 = get(sp - 4);
    uint8_t b3 = get(sp - 3);
    uint8_t b2 = get(sp - 2);
    uint8_t b1 = get(sp - 1);

    set(sp - 2, b4);
    set(sp - 1, b3);
    set(sp - 4, b2);
    set(sp    , b2);
    set(sp - 3, b1);
    set(sp + 1, b1);

    sp += 2;
  }

  void swapped() {
    assert(t, sp - 2 >= localSize());

    uint8_t saved = get(sp - 1);

    set(sp - 1, get(sp - 2));
    set(sp - 2, saved);
  }

  Promise* addressPromise(Promise* p) {
    BootContext* bc = context->bootContext;
    if (bc) {
      bc->addresses = new (bc->zone->allocate(sizeof(DelayedPromise)))
        DelayedPromise(t->m->system, bc->zone, p, bc->addresses);
      return bc->addresses;
    } else {
      return p;
    }
  }

  Compiler::Operand* addressOperand(Promise* p) {
    return c->promiseConstant(addressPromise(p));
  }

  Compiler::Operand* machineIp(unsigned logicalIp) {
    return c->promiseConstant(c->machineIp(logicalIp));
  }

  void visitLogicalIp(unsigned ip) {
    c->visitLogicalIp(ip);

    context->eventLog.append(IpEvent);
    context->eventLog.append2(ip);
  }

  void startLogicalIp(unsigned ip) {
    if (subroutine) {
      context->subroutineTable[ip] = subroutine;
    }

    c->startLogicalIp(ip);

    context->eventLog.append(IpEvent);
    context->eventLog.append2(ip);

    this->ip = ip;
  }

  void pushQuiet(unsigned footprint, Compiler::Operand* o) {
    c->push(footprint, o);
  }

  void pushLongQuiet(Compiler::Operand* o) {
    pushQuiet(2, o);
  }

  Compiler::Operand* popQuiet(unsigned footprint) {
    return c->pop(footprint);
  }

  Compiler::Operand* popLongQuiet() {
    Compiler::Operand* r = popQuiet(2);

    return r;
  }

  void pushInt(Compiler::Operand* o) {
    pushQuiet(1, o);
    pushedInt();
  }

  void pushAddress(Compiler::Operand* o) {
    pushQuiet(1, o);
    pushedInt();
  }

  void pushObject(Compiler::Operand* o) {
    pushQuiet(1, o);
    pushedObject();
  }

  void pushObject() {
    c->pushed();

    pushedObject();
  }

  void pushLong(Compiler::Operand* o) {
    pushLongQuiet(o);
    pushedLong();
  }

  void pop(unsigned count) {
    popped(count);
    c->popped(count);
  }

  Compiler::Operand* popInt() {
    poppedInt();
    return popQuiet(1);
  }

  Compiler::Operand* popLong() {
    poppedLong();
    return popLongQuiet();
  }

  Compiler::Operand* popObject() {
    poppedObject();
    return popQuiet(1);
  }

  void loadInt(unsigned index) {
    assert(t, index < localSize());
    pushInt(loadLocal(context, 1, index));
  }

  void loadLong(unsigned index) {
    assert(t, index < static_cast<unsigned>(localSize() - 1));
    pushLong(loadLocal(context, 2, index));
  }

  void loadObject(unsigned index) {
    assert(t, index < localSize());
    pushObject(loadLocal(context, 1, index));
  }

  void storeInt(unsigned index) {
    storeLocal(context, 1, popInt(), index);
    storedInt(translateLocalIndex(context, 1, index));
  }

  void storeLong(unsigned index) {
    storeLocal(context, 2, popLong(), index);
    storedLong(translateLocalIndex(context, 2, index));
  }

  void storeObject(unsigned index) {
    storeLocal(context, 1, popObject(), index);
    storedObject(translateLocalIndex(context, 1, index));
  }

  void storeObjectOrAddress(unsigned index) {
    storeLocal(context, 1, popQuiet(1), index);

    assert(t, sp >= 1);
    assert(t, sp - 1 >= localSize());
    if (get(sp - 1) == Object) {
      storedObject(translateLocalIndex(context, 1, index));
    } else {
      storedInt(translateLocalIndex(context, 1, index));
    }

    popped(1);
  }

  void dup() {
    pushQuiet(1, c->peek(1, 0));

    dupped();
  }

  void dupX1() {
    Compiler::Operand* s0 = popQuiet(1);
    Compiler::Operand* s1 = popQuiet(1);

    pushQuiet(1, s0);
    pushQuiet(1, s1);
    pushQuiet(1, s0);

    duppedX1();
  }

  void dupX2() {
    Compiler::Operand* s0 = popQuiet(1);

    if (get(sp - 2) == Long) {
      Compiler::Operand* s1 = popLongQuiet();

      pushQuiet(1, s0);
      pushLongQuiet(s1);
      pushQuiet(1, s0);
    } else {
      Compiler::Operand* s1 = popQuiet(1);
      Compiler::Operand* s2 = popQuiet(1);

      pushQuiet(1, s0);
      pushQuiet(1, s2);
      pushQuiet(1, s1);
      pushQuiet(1, s0);
    }

    duppedX2();
  }

  void dup2() {
    if (get(sp - 1) == Long) {
      pushLongQuiet(c->peek(2, 0));
    } else {
      Compiler::Operand* s0 = popQuiet(1);
      Compiler::Operand* s1 = popQuiet(1);

      pushQuiet(1, s1);
      pushQuiet(1, s0);
      pushQuiet(1, s1);
      pushQuiet(1, s0);
    }

    dupped2();
  }

  void dup2X1() {
    if (get(sp - 1) == Long) {
      Compiler::Operand* s0 = popLongQuiet();
      Compiler::Operand* s1 = popQuiet(1);

      pushLongQuiet(s0);
      pushQuiet(1, s1);
      pushLongQuiet(s0);
    } else {
      Compiler::Operand* s0 = popQuiet(1);
      Compiler::Operand* s1 = popQuiet(1);
      Compiler::Operand* s2 = popQuiet(1);

      pushQuiet(1, s1);
      pushQuiet(1, s0);
      pushQuiet(1, s2);
      pushQuiet(1, s1);
      pushQuiet(1, s0);
    }

    dupped2X1();
  }

  void dup2X2() {
    if (get(sp - 1) == Long) {
      Compiler::Operand* s0 = popLongQuiet();

      if (get(sp - 3) == Long) {
        Compiler::Operand* s1 = popLongQuiet();

        pushLongQuiet(s0);
        pushLongQuiet(s1);
        pushLongQuiet(s0);
      } else {
        Compiler::Operand* s1 = popQuiet(1);
        Compiler::Operand* s2 = popQuiet(1);

        pushLongQuiet(s0);
        pushQuiet(1, s2);
        pushQuiet(1, s1);
        pushLongQuiet(s0);
      }
    } else {
      Compiler::Operand* s0 = popQuiet(1);
      Compiler::Operand* s1 = popQuiet(1);
      Compiler::Operand* s2 = popQuiet(1);
      Compiler::Operand* s3 = popQuiet(1);

      pushQuiet(1, s1);
      pushQuiet(1, s0);
      pushQuiet(1, s3);
      pushQuiet(1, s2);
      pushQuiet(1, s1);
      pushQuiet(1, s0);
    }

    dupped2X2();
  }

  void swap() {
    Compiler::Operand* s0 = popQuiet(1);
    Compiler::Operand* s1 = popQuiet(1);

    pushQuiet(1, s0);
    pushQuiet(1, s1);

    swapped();
  }

  TraceElement* trace(object target, unsigned flags) {
    unsigned mapSize = frameMapSizeInWords(t, context->method);

    TraceElement* e = context->traceLog = new
      (context->zone.allocate(sizeof(TraceElement) + (mapSize * BytesPerWord)))
      TraceElement(context, target, flags, context->traceLog);

    ++ context->traceLogCount;

    context->eventLog.append(TraceEvent);
    context->eventLog.appendAddress(e);

    return e;
  }

  unsigned startSubroutine(unsigned ip, Promise* returnAddress) {
    pushAddress(addressOperand(returnAddress));

    Subroutine* subroutine = 0;
    for (Subroutine* s = context->subroutines; s; s = s->listNext) {
      if (s->ip == ip) {
        subroutine = s;
        break;
      }
    }

    if (subroutine == 0) {
      context->subroutines = subroutine = new
        (context->zone.allocate(sizeof(Subroutine)))
        Subroutine(ip, context->eventLog.length() + 1 + BytesPerWord + 2,
                   context->subroutines, this->subroutine);

      if (context->subroutineTable == 0) {
        unsigned size = codeLength(t, methodCode(t, context->method))
          * sizeof(Subroutine*);

        context->subroutineTable = static_cast<Subroutine**>
          (context->zone.allocate(size));

        memset(context->subroutineTable, 0, size);
      }
    }

    subroutine->handle = c->startSubroutine();
    this->subroutine = subroutine;

    SubroutineCall* call = new
      (context->zone.allocate(sizeof(SubroutineCall)))
      SubroutineCall(subroutine, returnAddress);

    context->eventLog.append(PushSubroutineEvent);
    context->eventLog.appendAddress(call);

    unsigned nextIndexIndex = context->eventLog.length();
    context->eventLog.append2(0);

    c->saveLocals();

    return nextIndexIndex;
  }

  void returnFromSubroutine(unsigned returnAddressLocal) {
    c->endSubroutine(subroutine->handle);
    subroutine->stackIndex = localOffsetFromStack
      (t, translateLocalIndex(context, 1, returnAddressLocal),
       context->method);
  }

  void endSubroutine(unsigned nextIndexIndex) {
    c->linkSubroutine(subroutine->handle);

    poppedInt();

    context->eventLog.append(PopSubroutineEvent);

    context->eventLog.set2(nextIndexIndex, context->eventLog.length());

    subroutine = subroutine->stackNext;
  }
  
  Context* context;
  MyThread* t;
  Compiler* c;
  Subroutine* subroutine;
  uint8_t* stackMap;
  unsigned ip;
  unsigned sp;
  unsigned level;
};

unsigned
savedTargetIndex(MyThread* t, object method)
{
  return codeMaxLocals(t, methodCode(t, method));
}

object
findCallNode(MyThread* t, void* address);

void
insertCallNode(MyThread* t, object node);

void*
findExceptionHandler(Thread* t, object method, void* ip)
{
  if (t->exception) {
    object table = codeExceptionHandlerTable(t, methodCode(t, method));
    if (table) {
      object index = arrayBody(t, table, 0);
      
      uint8_t* compiled = reinterpret_cast<uint8_t*>
        (methodCompiled(t, method));

      for (unsigned i = 0; i < arrayLength(t, table) - 1; ++i) {
        unsigned start = intArrayBody(t, index, i * 3);
        unsigned end = intArrayBody(t, index, (i * 3) + 1);
        unsigned key = difference(ip, compiled) - 1;

        if (key >= start and key < end) {
          object catchType = arrayBody(t, table, i + 1);

          if (catchType == 0 or instanceOf(t, catchType, t->exception)) {
            return compiled + intArrayBody(t, index, (i * 3) + 2);
          }
        }
      }
    }
  }

  return 0;
}

void
releaseLock(MyThread* t, object method, void* stack)
{
  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    object lock;
    if (methodFlags(t, method) & ACC_STATIC) {
      lock = methodClass(t, method);
    } else {
      lock = *localObject
        (t, stackForFrame(t, stack, method), method,
         savedTargetIndex(t, method));
    }
    
    release(t, lock);
  }
}

void
findUnwindTarget(MyThread* t, void** targetIp, void** targetBase,
                 void** targetStack)
{
  void* ip = t->ip;
  void* base = t->base;
  void* stack = t->stack;
  if (ip == 0) {
    ip = t->arch->frameIp(stack);
  }

  object target = t->trace->targetMethod;

  *targetIp = 0;
  while (*targetIp == 0) {
    object method = methodForIp(t, ip);
    if (method) {
      void* handler = findExceptionHandler(t, method, ip);

      if (handler) {
        *targetIp = handler;
        *targetBase = base;

        t->arch->nextFrame(&stack, &base);

        void** sp = static_cast<void**>(stackForFrame(t, stack, method))
          + t->arch->frameReturnAddressSize();

        *targetStack = sp;

        sp[localOffset(t, localSize(t, method), method)] = t->exception;

        t->exception = 0;
      } else {
        t->arch->nextFrame(&stack, &base);
        ip = t->arch->frameIp(stack);

        if (t->exception) {
          releaseLock(t, method, stack);
        }

        target = method;
      }
    } else {
      *targetIp = ip;
      *targetBase = base;
      *targetStack = static_cast<void**>(stack)
        + t->arch->frameReturnAddressSize();

      while (Continuations and t->continuation) {
        object c = t->continuation;

        object method = continuationMethod(t, c);

        void* handler = findExceptionHandler
          (t, method, continuationAddress(t, c));

        if (handler) {
          t->exceptionHandler = handler;

          t->exceptionStackAdjustment 
            = (stackOffsetFromFrame(t, method)
               - ((continuationFramePointerOffset(t, c) / BytesPerWord)
                  - t->arch->framePointerOffset()
                  + t->arch->frameReturnAddressSize())) * BytesPerWord;

          t->exceptionOffset
            = localOffset(t, localSize(t, method), method) * BytesPerWord;
          break;
        } else if (t->exception) {
          releaseLock(t, method,
                      reinterpret_cast<uint8_t*>(c)
                      + ContinuationBody
                      + continuationReturnAddressOffset(t, c)
                      - t->arch->returnAddressOffset());
        }

        t->continuation = continuationNext(t, c);
      }
    }
  }
}

object
makeCurrentContinuation(MyThread* t, void** targetIp, void** targetBase,
                        void** targetStack)
{
  void* ip = t->ip;
  void* base = t->base;
  void* stack = t->stack;
  if (ip == 0) {
    ip = t->arch->frameIp(stack);
  }

  object context = t->continuation
    ? continuationContext(t, t->continuation)
    : makeContinuationContext(t, 0, 0, 0, 0, t->trace->originalMethod);
  PROTECT(t, context);

  object target = t->trace->targetMethod;
  PROTECT(t, target);

  object first = 0;
  PROTECT(t, first);

  object last = 0;
  PROTECT(t, last);

  *targetIp = 0;
  while (*targetIp == 0) {
    object method = methodForIp(t, ip);
    if (method) {
      PROTECT(t, method);

      void** top = static_cast<void**>(stack)
        + t->arch->frameReturnAddressSize()
        + t->arch->frameFooterSize();
      unsigned argumentFootprint
        = t->arch->argumentFootprint(methodParameterFootprint(t, target));
      unsigned alignment = t->arch->stackAlignmentInWords();
      if (TailCalls and argumentFootprint > alignment) {
        top += argumentFootprint - alignment;
      }

      t->arch->nextFrame(&stack, &base);

      void** bottom = static_cast<void**>(stack)
          + t->arch->frameReturnAddressSize();
      unsigned frameSize = bottom - top;
      unsigned totalSize = frameSize
        + t->arch->frameFooterSize()
        + t->arch->argumentFootprint(methodParameterFootprint(t, method));

      object c = makeContinuation
        (t, 0, context, method, ip,
         ((frameSize
           + t->arch->frameFooterSize()
           + t->arch->returnAddressOffset()
           - t->arch->frameReturnAddressSize()) * BytesPerWord),
         ((frameSize
           + t->arch->frameFooterSize()
           + t->arch->framePointerOffset() 
           - t->arch->frameReturnAddressSize()) * BytesPerWord),
         totalSize);

      memcpy(&continuationBody(t, c, 0), top, totalSize * BytesPerWord);

      if (last) {
        set(t, last, ContinuationNext, c);
      } else {
        first = c;
      }
      last = c;

      ip = t->arch->frameIp(stack);

      target = method;
    } else {
      *targetIp = ip;
      *targetBase = base;
      *targetStack = static_cast<void**>(stack)
        + t->arch->frameReturnAddressSize();;
    }
  }

  expect(t, last);
  set(t, last, ContinuationNext, t->continuation);

  return first;
}

void NO_RETURN
unwind(MyThread* t)
{
  void* ip;
  void* base;
  void* stack;
  findUnwindTarget(t, &ip, &base, &stack);
  vmJump(ip, base, stack, t, 0, 0);
}

object&
objectPools(MyThread* t);

object&
windMethod(MyThread* t);

object&
rewindMethod(MyThread* t);

object&
receiveMethod(MyThread* t);

uintptr_t
defaultThunk(MyThread* t);

uintptr_t
nativeThunk(MyThread* t);

uintptr_t
aioobThunk(MyThread* t);

uintptr_t
virtualThunk(MyThread* t, unsigned index);

uintptr_t
methodAddress(Thread* t, object method)
{
  if (methodFlags(t, method) & ACC_NATIVE) {
    return nativeThunk(static_cast<MyThread*>(t));
  } else {
    return methodCompiled(t, method);
  }
}

void
tryInitClass(MyThread* t, object class_)
{
  initClass(t, class_);
  if (UNLIKELY(t->exception)) unwind(t);
}

FixedAllocator*
codeAllocator(MyThread* t);

void
compile(MyThread* t, Allocator* allocator, BootContext* bootContext,
        object method);

int64_t
findInterfaceMethodFromInstance(MyThread* t, object method, object instance)
{
  if (instance) {
    object target = findInterfaceMethod(t, method, objectClass(t, instance));

    if (methodAddress(t, target) == defaultThunk(t)) {
      PROTECT(t, target);

      compile(t, codeAllocator(t), 0, target);
    }

    if (UNLIKELY(t->exception)) {
      unwind(t);
    } else {
      if (methodFlags(t, target) & ACC_NATIVE) {
        t->trace->nativeMethod = target;
      }
      return methodAddress(t, target);
    }
  } else {
    t->exception = makeNullPointerException(t);
    unwind(t);
  }
}

int64_t
compareDoublesG(uint64_t bi, uint64_t ai)
{
  double a = bitsToDouble(ai);
  double b = bitsToDouble(bi);
  
  if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  } else if (a == b) {
    return 0;
  } else {
    return 1;
  }
}

int64_t
compareDoublesL(uint64_t bi, uint64_t ai)
{
  double a = bitsToDouble(ai);
  double b = bitsToDouble(bi);
  
  if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  } else if (a == b) {
    return 0;
  } else {
    return -1;
  }
}

int64_t
compareFloatsG(uint32_t bi, uint32_t ai)
{
  float a = bitsToFloat(ai);
  float b = bitsToFloat(bi);
  
  if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  } else if (a == b) {
    return 0;
  } else {
    return 1;
  }
}

int64_t
compareFloatsL(uint32_t bi, uint32_t ai)
{
  float a = bitsToFloat(ai);
  float b = bitsToFloat(bi);
  
  if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  } else if (a == b) {
    return 0;
  } else {
    return -1;
  }
}

uint64_t
addDouble(uint64_t b, uint64_t a)
{
  return doubleToBits(bitsToDouble(a) + bitsToDouble(b));
}

uint64_t
subtractDouble(uint64_t b, uint64_t a)
{
  return doubleToBits(bitsToDouble(a) - bitsToDouble(b));
}

uint64_t
multiplyDouble(uint64_t b, uint64_t a)
{
  return doubleToBits(bitsToDouble(a) * bitsToDouble(b));
}

uint64_t
divideDouble(uint64_t b, uint64_t a)
{
  return doubleToBits(bitsToDouble(a) / bitsToDouble(b));
}

uint64_t
moduloDouble(uint64_t b, uint64_t a)
{
  return doubleToBits(fmod(bitsToDouble(a), bitsToDouble(b)));
}

uint64_t
negateDouble(uint64_t a)
{
  return doubleToBits(- bitsToDouble(a));
}

uint64_t
doubleToFloat(int64_t a)
{
  return floatToBits(static_cast<float>(bitsToDouble(a)));
}

int64_t
doubleToInt(int64_t a)
{
  return static_cast<int32_t>(bitsToDouble(a));
}

int64_t
doubleToLong(int64_t a)
{
  return static_cast<int64_t>(bitsToDouble(a));
}

uint64_t
addFloat(uint32_t b, uint32_t a)
{
  return floatToBits(bitsToFloat(a) + bitsToFloat(b));
}

uint64_t
subtractFloat(uint32_t b, uint32_t a)
{
  return floatToBits(bitsToFloat(a) - bitsToFloat(b));
}

uint64_t
multiplyFloat(uint32_t b, uint32_t a)
{
  return floatToBits(bitsToFloat(a) * bitsToFloat(b));
}

uint64_t
divideFloat(uint32_t b, uint32_t a)
{
  return floatToBits(bitsToFloat(a) / bitsToFloat(b));
}

uint64_t
moduloFloat(uint32_t b, uint32_t a)
{
  return floatToBits(fmod(bitsToFloat(a), bitsToFloat(b)));
}

uint64_t
negateFloat(uint32_t a)
{
  return floatToBits(- bitsToFloat(a));
}

int64_t
divideLong(int64_t b, int64_t a)
{
  return a / b;
}

int64_t
moduloLong(int64_t b, int64_t a)
{
  return a % b;
}

uint64_t
floatToDouble(int32_t a)
{
  return doubleToBits(static_cast<double>(bitsToFloat(a)));
}

int64_t
floatToInt(int32_t a)
{
  return static_cast<int32_t>(bitsToFloat(a));
}

int64_t
floatToLong(int32_t a)
{
  return static_cast<int64_t>(bitsToFloat(a));
}

uint64_t
intToDouble(int32_t a)
{
  return doubleToBits(static_cast<double>(a));
}

uint64_t
intToFloat(int32_t a)
{
  return floatToBits(static_cast<float>(a));
}

uint64_t
longToDouble(int64_t a)
{
  return doubleToBits(static_cast<double>(a));
}

uint64_t
longToFloat(int64_t a)
{
  return floatToBits(static_cast<float>(a));
}

uint64_t
makeBlankObjectArray(MyThread* t, object loader, object class_, int32_t length)
{
  if (length >= 0) {
    return reinterpret_cast<uint64_t>
      (makeObjectArray(t, loader, class_, length));
  } else {
    object message = makeString(t, "%d", length);
    t->exception = makeNegativeArraySizeException(t, message);
    unwind(t);
  }
}

uint64_t
makeBlankArray(MyThread* t, unsigned type, int32_t length)
{
  if (length >= 0) {
    object (*constructor)(Thread*, uintptr_t);
    switch (type) {
    case T_BOOLEAN:
      constructor = makeBooleanArray;
      break;

    case T_CHAR:
      constructor = makeCharArray;
      break;

    case T_FLOAT:
      constructor = makeFloatArray;
      break;

    case T_DOUBLE:
      constructor = makeDoubleArray;
      break;

    case T_BYTE:
      constructor = makeByteArray;
      break;

    case T_SHORT:
      constructor = makeShortArray;
      break;

    case T_INT:
      constructor = makeIntArray;
      break;

    case T_LONG:
      constructor = makeLongArray;
      break;

    default: abort(t);
    }

    return reinterpret_cast<uintptr_t>(constructor(t, length));
  } else {
    object message = makeString(t, "%d", length);
    t->exception = makeNegativeArraySizeException(t, message);
    unwind(t);
  }
}

uint64_t
lookUpAddress(int32_t key, uintptr_t* start, int32_t count,
              uintptr_t default_)
{
  int32_t bottom = 0;
  int32_t top = count;
  for (int32_t span = top - bottom; span; span = top - bottom) {
    int32_t middle = bottom + (span / 2);
    uintptr_t* p = start + (middle * 2);
    int32_t k = *p;

    if (key < k) {
      top = middle;
    } else if (key > k) {
      bottom = middle + 1;
    } else {
      return p[1];
    }
  }

  return default_;
}

void
setMaybeNull(MyThread* t, object o, unsigned offset, object value)
{
  if (LIKELY(o)) {
    set(t, o, offset, value);
  } else {
    t->exception = makeNullPointerException(t);
    unwind(t);
  }
}

void
acquireMonitorForObject(MyThread* t, object o)
{
  if (LIKELY(o)) {
    acquire(t, o);
  } else {
    t->exception = makeNullPointerException(t);
    unwind(t);
  }
}

void
releaseMonitorForObject(MyThread* t, object o)
{
  if (LIKELY(o)) {
    release(t, o);
  } else {
    t->exception = makeNullPointerException(t);
    unwind(t);
  }
}

object
makeMultidimensionalArray2(MyThread* t, object class_, uintptr_t* countStack,
                           int32_t dimensions)
{
  PROTECT(t, class_);

  int32_t counts[dimensions];
  for (int i = dimensions - 1; i >= 0; --i) {
    counts[i] = countStack[dimensions - i - 1];
    if (UNLIKELY(counts[i] < 0)) {
      object message = makeString(t, "%d", counts[i]);
      t->exception = makeNegativeArraySizeException(t, message);
      return 0;
    }
  }

  object array = makeArray(t, counts[0]);
  setObjectClass(t, array, class_);
  PROTECT(t, array);

  populateMultiArray(t, array, counts, 0, dimensions);

  return array;
}

uint64_t
makeMultidimensionalArray(MyThread* t, object class_, int32_t dimensions,
                          int32_t offset)
{
  object r = makeMultidimensionalArray2
    (t, class_, static_cast<uintptr_t*>(t->stack) + offset, dimensions);

  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else {
    return reinterpret_cast<uintptr_t>(r);
  }
}

unsigned
traceSize(Thread* t)
{
  class Counter: public Processor::StackVisitor {
   public:
    Counter(): count(0) { }

    virtual bool visit(Processor::StackWalker*) {
      ++ count;
      return true;
    }

    unsigned count;
  } counter;

  t->m->processor->walkStack(t, &counter);

  return FixedSizeOfArray + (counter.count * ArrayElementSizeOfArray)
    + (counter.count * FixedSizeOfTraceElement);
}

void NO_RETURN
throwArrayIndexOutOfBounds(MyThread* t)
{
  ensure(t, FixedSizeOfArrayIndexOutOfBoundsException + traceSize(t));
  
  t->tracing = true;
  t->exception = makeArrayIndexOutOfBoundsException(t, 0);
  t->tracing = false;

  unwind(t);
}

void NO_RETURN
throw_(MyThread* t, object o)
{
  if (LIKELY(o)) {
    t->exception = o;
  } else {
    t->exception = makeNullPointerException(t);
  }

  printTrace(t, t->exception);

  unwind(t);
}

void
checkCast(MyThread* t, object class_, object o)
{
  if (UNLIKELY(o and not isAssignableFrom(t, class_, objectClass(t, o)))) {
    object message = makeString
      (t, "%s as %s",
       &byteArrayBody(t, className(t, objectClass(t, o)), 0),
       &byteArrayBody(t, className(t, class_), 0));
    t->exception = makeClassCastException(t, message);
    unwind(t);
  }
}

uint64_t
instanceOf64(Thread* t, object class_, object o)
{
  return instanceOf(t, class_, o);
}

uint64_t
makeNewGeneral64(Thread* t, object class_)
{
  return reinterpret_cast<uintptr_t>(makeNewGeneral(t, class_));
}

uint64_t
makeNew64(Thread* t, object class_)
{
  return reinterpret_cast<uintptr_t>(makeNew(t, class_));
}

void
gcIfNecessary(MyThread* t)
{
  if (UNLIKELY(t->backupHeap)) {
    collect(t, Heap::MinorCollection);
  }
}

unsigned
resultSize(MyThread* t, unsigned code)
{
  switch (code) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    return 4;

  case ObjectField:
    return BytesPerWord;

  case LongField:
  case DoubleField:
    return 8;

  case VoidField:
    return 0;

  default:
    abort(t);
  }
}

void
pushReturnValue(MyThread* t, Frame* frame, unsigned code,
                Compiler::Operand* result)
{
  switch (code) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    return frame->pushInt(result);

  case ObjectField:
    return frame->pushObject(result);

  case LongField:
  case DoubleField:
    return frame->pushLong(result);

  default:
    abort(t);
  }
}

Compiler::Operand*
compileDirectInvoke(MyThread* t, Frame* frame, object target, bool tailCall,
                    bool useThunk, unsigned rSize, Promise* addressPromise)
{
  Compiler* c = frame->c;

  unsigned flags = (tailCall ? Compiler::TailJump : 0);

  if (useThunk or (tailCall and (methodFlags(t, target) & ACC_NATIVE))) {
    if (TailCalls and tailCall) {
      TraceElement* trace = frame->trace(target, TraceElement::TailCall);
      Compiler::Operand* returnAddress = c->promiseConstant
        (new (frame->context->zone.allocate(sizeof(TraceElementPromise)))
         TraceElementPromise(t->m->system, trace));;

      Compiler::Operand* result = c->stackCall
        (returnAddress,
         flags | Compiler::Aligned,
         trace,
         rSize,
         methodParameterFootprint(t, target));

      c->store(BytesPerWord, returnAddress, BytesPerWord,
               c->memory(c->register_(t->arch->thread()),
                         difference(&(t->tailAddress), t)));

      if (methodFlags(t, target) & ACC_NATIVE) {
        c->exit(c->constant(nativeThunk(t)));
      } else {
        c->exit(c->constant(defaultThunk(t)));
      }

      return result;
    } else {
      return c->stackCall
        (c->constant(defaultThunk(t)),
         flags | Compiler::Aligned,
         frame->trace(target, 0),
         rSize,
         methodParameterFootprint(t, target));
    }
  } else {
    Compiler::Operand* address =
      (addressPromise
       ? c->promiseConstant(addressPromise)
       : c->constant(methodAddress(t, target)));

    return c->stackCall
      (address,
       flags,
       tailCall ? 0 : frame->trace
       ((methodFlags(t, target) & ACC_NATIVE) ? target : 0, 0),
       rSize,
       methodParameterFootprint(t, target));
  }
}

bool
compileDirectInvoke(MyThread* t, Frame* frame, object target, bool tailCall)
{
  unsigned rSize = resultSize(t, methodReturnCode(t, target));

  Compiler::Operand* result = 0;

  if (emptyMethod(t, target)) {
    tailCall = false;
  } else {
    BootContext* bc = frame->context->bootContext;
    if (bc) {
      if (methodClass(t, target) == methodClass(t, frame->context->method)
          or (not classNeedsInit(t, methodClass(t, target))))
      {
        Promise* p = new (bc->zone->allocate(sizeof(ListenPromise)))
          ListenPromise(t->m->system, bc->zone);

        PROTECT(t, target);
        object pointer = makePointer(t, p);
        bc->calls = makeTriple(t, target, pointer, bc->calls);

        result = compileDirectInvoke
          (t, frame, target, tailCall, false, rSize, p);
      } else {
        result = compileDirectInvoke
          (t, frame, target, tailCall, true, rSize, 0);
      }
    } else if (methodAddress(t, target) == defaultThunk(t)
               or classNeedsInit(t, methodClass(t, target)))
    {
      result = compileDirectInvoke
        (t, frame, target, tailCall, true, rSize, 0);
    } else {
      result = compileDirectInvoke
        (t, frame, target, tailCall, false, rSize, 0);
    }
  }

  frame->pop(methodParameterFootprint(t, target));

  if (rSize) {
    pushReturnValue(t, frame, methodReturnCode(t, target), result);
  }

  return tailCall;
}

void
handleMonitorEvent(MyThread* t, Frame* frame, intptr_t function)
{
  Compiler* c = frame->c;
  object method = frame->context->method;

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    Compiler::Operand* lock;
    if (methodFlags(t, method) & ACC_STATIC) {
      lock = frame->append(methodClass(t, method));
    } else {
      lock = loadLocal(frame->context, 1, savedTargetIndex(t, method));
    }
    
    c->call(c->constant(function),
            0,
            frame->trace(0, 0),
            0,
            2, c->register_(t->arch->thread()), lock);
  }
}

void
handleEntrance(MyThread* t, Frame* frame)
{
  object method = frame->context->method;

  if ((methodFlags(t, method) & (ACC_SYNCHRONIZED | ACC_STATIC))
      == ACC_SYNCHRONIZED)
  {
    // save 'this' pointer in case it is overwritten.
    unsigned index = savedTargetIndex(t, method);
    storeLocal(frame->context, 1, loadLocal(frame->context, 1, 0), index);
    frame->set(index, Frame::Object);
  }

  handleMonitorEvent
    (t, frame, getThunk(t, acquireMonitorForObjectThunk));
}

void
handleExit(MyThread* t, Frame* frame)
{
  handleMonitorEvent
    (t, frame, getThunk(t, releaseMonitorForObjectThunk));
}

bool
inTryBlock(MyThread* t, object code, unsigned ip)
{
  object table = codeExceptionHandlerTable(t, code);
  if (table) {
    unsigned length = exceptionHandlerTableLength(t, table);
    for (unsigned i = 0; i < length; ++i) {
      ExceptionHandler* eh = exceptionHandlerTableBody(t, table, i);
      if (ip >= exceptionHandlerStart(eh)
          and ip < exceptionHandlerEnd(eh))
      {
        return true;
      }
    }
  }
  return false;
}

bool
needsReturnBarrier(MyThread* t, object method)
{
  return (methodFlags(t, method) & ConstructorFlag)
    and (classFlags(t, methodClass(t, method)) & HasFinalMemberFlag);
}

bool
returnsNext(MyThread* t, object code, unsigned ip)
{
  switch (codeBody(t, code, ip)) {
  case return_:
  case areturn:
  case ireturn:
  case freturn:
  case lreturn:
  case dreturn:
    return true;

  case goto_: {
    uint32_t offset = codeReadInt16(t, code, ++ip);
    uint32_t newIp = (ip - 3) + offset;
    assert(t, newIp < codeLength(t, code));

    return returnsNext(t, code, newIp);
  }

  case goto_w: {
    uint32_t offset = codeReadInt32(t, code, ++ip);
    uint32_t newIp = (ip - 5) + offset;
    assert(t, newIp < codeLength(t, code));
    
    return returnsNext(t, code, newIp);
  }

  default:
    return false;
  }
}

bool
isTailCall(MyThread* t, object code, unsigned ip, object caller, object callee)
{
  return TailCalls
    and ((methodFlags(t, caller) & ACC_SYNCHRONIZED) == 0)
    and (not inTryBlock(t, code, ip - 1))
    and (not needsReturnBarrier(t, caller))
    and (methodReturnCode(t, caller) == VoidField
         or methodReturnCode(t, caller) == methodReturnCode(t, callee))
    and returnsNext(t, code, ip);
}

void
compile(MyThread* t, Frame* initialFrame, unsigned ip,
        int exceptionHandlerStart = -1);

void
saveStateAndCompile(MyThread* t, Frame* initialFrame, unsigned ip)
{
  Compiler::State* state = initialFrame->c->saveState();
  compile(t, initialFrame, ip);
  initialFrame->c->restoreState(state);
}

void
compile(MyThread* t, Frame* initialFrame, unsigned ip,
        int exceptionHandlerStart)
{
  uint8_t stackMap
    [codeMaxStack(t, methodCode(t, initialFrame->context->method))];
  Frame myFrame(initialFrame, stackMap);
  Frame* frame = &myFrame;
  Compiler* c = frame->c;
  Context* context = frame->context;

  object code = methodCode(t, context->method);
  PROTECT(t, code);
    
  while (ip < codeLength(t, code)) {
    if (context->visitTable[ip] ++) {
      // we've already visited this part of the code
      frame->visitLogicalIp(ip);
      return;
    }

    frame->startLogicalIp(ip);

    if (exceptionHandlerStart >= 0) {
      c->initLocalsFromLogicalIp(exceptionHandlerStart);

      exceptionHandlerStart = -1;

      frame->pushObject();
      
      c->call
        (c->constant(getThunk(t, gcIfNecessaryThunk)),
         0,
         frame->trace(0, 0),
         0,
         1, c->register_(t->arch->thread()));
    }

//     fprintf(stderr, "ip: %d map: %ld\n", ip, *(frame->map));

    unsigned instruction = codeBody(t, code, ip++);

    switch (instruction) {
    case aaload:
    case baload:
    case caload:
    case daload:
    case faload:
    case iaload:
    case laload:
    case saload: {
      Compiler::Operand* index = frame->popInt();
      Compiler::Operand* array = frame->popObject();

      if (inTryBlock(t, code, ip - 1)) {
        c->saveLocals();
      }

      if (CheckArrayBounds) {
        c->checkBounds(array, ArrayLength, index, aioobThunk(t));
      }

      switch (instruction) {
      case aaload:
        frame->pushObject
          (c->load
           (BytesPerWord, BytesPerWord,
            c->memory(array, ArrayBody, index, BytesPerWord), BytesPerWord));
        break;

      case faload:
      case iaload:
        frame->pushInt
          (c->load(4, 4, c->memory(array, ArrayBody, index, 4), BytesPerWord));
        break;

      case baload:
        frame->pushInt
          (c->load(1, 1, c->memory(array, ArrayBody, index, 1), BytesPerWord));
        break;

      case caload:
        frame->pushInt
          (c->loadz(2, 2, c->memory(array, ArrayBody, index, 2),
                    BytesPerWord));
        break;

      case daload:
      case laload:
        frame->pushLong
          (c->load(8, 8, c->memory(array, ArrayBody, index, 8), 8));
        break;

      case saload:
        frame->pushInt
          (c->load(2, 2, c->memory(array, ArrayBody, index, 2), BytesPerWord));
        break;
      }
    } break;

    case aastore:
    case bastore:
    case castore:
    case dastore:
    case fastore:
    case iastore:
    case lastore:
    case sastore: {
      Compiler::Operand* value;
      if (instruction == dastore or instruction == lastore) {
        value = frame->popLong();
      } else if (instruction == aastore) {
        value = frame->popObject();
      } else {
        value = frame->popInt();
      }

      Compiler::Operand* index = frame->popInt();
      Compiler::Operand* array = frame->popObject();

      if (inTryBlock(t, code, ip - 1)) {
        c->saveLocals();
      }

      if (CheckArrayBounds) {
        c->checkBounds(array, ArrayLength, index, aioobThunk(t));
      }

      switch (instruction) {
      case aastore: {
        c->call
          (c->constant(getThunk(t, setMaybeNullThunk)),
           0,
           frame->trace(0, 0),
           0,
           4, c->register_(t->arch->thread()), array,
           c->add(4, c->constant(ArrayBody),
                  c->shl(4, c->constant(log(BytesPerWord)), index)),
           value);
      } break;

      case fastore:
      case iastore:
        c->store
          (BytesPerWord, value, 4, c->memory(array, ArrayBody, index, 4));
        break;

      case bastore:
        c->store
          (BytesPerWord, value, 1, c->memory(array, ArrayBody, index, 1));
        break;

      case castore:
      case sastore:
        c->store
          (BytesPerWord, value, 2, c->memory(array, ArrayBody, index, 2));
        break;

      case dastore:
      case lastore:
        c->store(8, value, 8, c->memory(array, ArrayBody, index, 8));
        break;
      }
    } break;

    case aconst_null:
      frame->pushObject(c->constant(0));
      break;

    case aload:
      frame->loadObject(codeBody(t, code, ip++));
      break;

    case aload_0:
      frame->loadObject(0);
      break;

    case aload_1:
      frame->loadObject(1);
      break;

    case aload_2:
      frame->loadObject(2);
      break;

    case aload_3:
      frame->loadObject(3);
      break;

    case anewarray: {
      uint16_t index = codeReadInt16(t, code, ip);
      
      object class_ = resolveClassInPool(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      Compiler::Operand* length = frame->popInt();

      frame->pushObject
        (c->call
         (c->constant(getThunk(t, makeBlankObjectArrayThunk)),
          0,
          frame->trace(0, 0),
          BytesPerWord,
          4, c->register_(t->arch->thread()),
          frame->append(classLoader(t, methodClass(t, context->method))),
          frame->append(class_), length));
    } break;

    case areturn: {
      handleExit(t, frame);
      c->return_(BytesPerWord, frame->popObject());
    } return;

    case arraylength: {
      frame->pushInt
        (c->load
         (BytesPerWord, BytesPerWord,
          c->memory(frame->popObject(), ArrayLength, 0, 1), BytesPerWord));
    } break;

    case astore:
      frame->storeObjectOrAddress(codeBody(t, code, ip++));
      break;

    case astore_0:
      frame->storeObjectOrAddress(0);
      break;

    case astore_1:
      frame->storeObjectOrAddress(1);
      break;

    case astore_2:
      frame->storeObjectOrAddress(2);
      break;

    case astore_3:
      frame->storeObjectOrAddress(3);
      break;

    case athrow: {
      Compiler::Operand* target = frame->popObject();
      c->call
        (c->constant(getThunk(t, throw_Thunk)),
         Compiler::NoReturn,
         frame->trace(0, 0),
         0,
         2, c->register_(t->arch->thread()), target);
    } return;

    case bipush:
      frame->pushInt
        (c->constant(static_cast<int8_t>(codeBody(t, code, ip++))));
      break;

    case checkcast: {
      uint16_t index = codeReadInt16(t, code, ip);

      object class_ = resolveClassInPool(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      Compiler::Operand* instance = c->peek(1, 0);

      c->call
        (c->constant(getThunk(t, checkCastThunk)),
         0,
         frame->trace(0, 0),
         0,
         3, c->register_(t->arch->thread()), frame->append(class_), instance);
    } break;

    case d2f: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, doubleToFloatThunk)),
          0, 0, 4, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case d2i: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, doubleToIntThunk)),
          0, 0, 4, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case d2l: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, doubleToLongThunk)),
          0, 0, 8, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case dadd: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushLong
        (c->call
         (c->constant(getThunk(t, addDoubleThunk)),
          0, 0, 8, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dcmpg: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, compareDoublesGThunk)),
          0, 0, 4, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dcmpl: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, compareDoublesLThunk)),
          0, 0, 4, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dconst_0:
      frame->pushLong(c->constant(doubleToBits(0.0)));
      break;
      
    case dconst_1:
      frame->pushLong(c->constant(doubleToBits(1.0)));
      break;

    case ddiv: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushLong
        (c->call
         (c->constant(getThunk(t, divideDoubleThunk)),
          0, 0, 8, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dmul: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushLong
        (c->call
         (c->constant(getThunk(t, multiplyDoubleThunk)),
          0, 0, 8, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dneg: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, negateDoubleThunk)),
          0, 0, 8, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case vm::drem: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushLong
        (c->call
         (c->constant(getThunk(t, moduloDoubleThunk)),
          0, 0, 8, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dsub: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushLong
        (c->call
         (c->constant(getThunk(t, subtractDoubleThunk)),
          0, 0, 8, 4,
          static_cast<Compiler::Operand*>(0), a,
          static_cast<Compiler::Operand*>(0), b));
    } break;

    case dup:
      frame->dup();
      break;

    case dup_x1:
      frame->dupX1();
      break;

    case dup_x2:
      frame->dupX2();
      break;

    case dup2:
      frame->dup2();
      break;

    case dup2_x1:
      frame->dup2X1();
      break;

    case dup2_x2:
      frame->dup2X2();
      break;

    case f2d: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, floatToDoubleThunk)),
          0, 0, 8, 1, frame->popInt()));
    } break;

    case f2i: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, floatToIntThunk)),
          0, 0, 4, 1, frame->popInt()));
    } break;

    case f2l: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, floatToLongThunk)),
          0, 0, 8, 1, frame->popInt()));
    } break;

    case fadd: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, addFloatThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fcmpg: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, compareFloatsGThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fcmpl: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, compareFloatsLThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fconst_0:
      frame->pushInt(c->constant(floatToBits(0.0)));
      break;
      
    case fconst_1:
      frame->pushInt(c->constant(floatToBits(1.0)));
      break;
      
    case fconst_2:
      frame->pushInt(c->constant(floatToBits(2.0)));
      break;

    case fdiv: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, divideFloatThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fmul: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, multiplyFloatThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fneg: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, negateFloatThunk)),
          0, 0, 4, 1, frame->popInt()));
    } break;

    case vm::frem: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, moduloFloatThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case fsub: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, subtractFloatThunk)),
          0, 0, 4, 2, a, b));
    } break;

    case getfield:
    case getstatic: {
      uint16_t index = codeReadInt16(t, code, ip);
        
      object field = resolveField(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      Compiler::Operand* table;

      if (instruction == getstatic) {
        assert(t, fieldFlags(t, field) & ACC_STATIC);

        if (fieldClass(t, field) != methodClass(t, context->method)
            and classNeedsInit(t, fieldClass(t, field)))
        {
          c->call
            (c->constant(getThunk(t, tryInitClassThunk)),
             0,
             frame->trace(0, 0),
             0,
             2, c->register_(t->arch->thread()),
             frame->append(fieldClass(t, field)));
        }

        table = frame->append(classStaticTable(t, fieldClass(t, field)));
      } else {
        assert(t, (fieldFlags(t, field) & ACC_STATIC) == 0);

        table = frame->popObject();

        if (inTryBlock(t, code, ip - 3)) {
          c->saveLocals();
        }
      }

      Compiler::Operand* fieldOperand = 0;

      if ((fieldFlags(t, field) & ACC_VOLATILE)
          and BytesPerWord == 4
          and (fieldCode(t, field) == DoubleField
               or fieldCode(t, field) == LongField))
      {
        fieldOperand = frame->append(field);

        c->call
          (c->constant(getThunk(t, acquireMonitorForObjectThunk)),
           0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()),
           fieldOperand);
      }

      switch (fieldCode(t, field)) {
      case ByteField:
      case BooleanField:
        frame->pushInt
          (c->load(1, 1, c->memory(table, fieldOffset(t, field), 0, 1),
                   BytesPerWord));
        break;

      case CharField:
        frame->pushInt
          (c->loadz(2, 2, c->memory(table, fieldOffset(t, field), 0, 1),
                    BytesPerWord));
        break;

      case ShortField:
        frame->pushInt
          (c->load(2, 2, c->memory(table, fieldOffset(t, field), 0, 1),
                   BytesPerWord));
        break;

      case FloatField:
      case IntField:
        frame->pushInt
          (c->load(4, 4, c->memory(table, fieldOffset(t, field), 0, 1),
                   BytesPerWord));
        break;

      case DoubleField:
      case LongField:
        frame->pushLong
          (c->load(8, 8, c->memory(table, fieldOffset(t, field), 0, 1), 8));
        break;

      case ObjectField:
        frame->pushObject
          (c->load
           (BytesPerWord, BytesPerWord,
            c->memory(table, fieldOffset(t, field), 0, 1), BytesPerWord));
        break;

      default:
        abort(t);
      }

      if (fieldFlags(t, field) & ACC_VOLATILE) {
        if (BytesPerWord == 4
            and (fieldCode(t, field) == DoubleField
                 or fieldCode(t, field) == LongField))
        {
          c->call
            (c->constant(getThunk(t, releaseMonitorForObjectThunk)),
             0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()),
             fieldOperand);
        } else {
          c->loadBarrier();
        }
      }
    } break;

    case goto_: {
      uint32_t offset = codeReadInt16(t, code, ip);
      uint32_t newIp = (ip - 3) + offset;
      assert(t, newIp < codeLength(t, code));

      c->jmp(frame->machineIp(newIp));
      ip = newIp;
    } break;

    case goto_w: {
      uint32_t offset = codeReadInt32(t, code, ip);
      uint32_t newIp = (ip - 5) + offset;
      assert(t, newIp < codeLength(t, code));

      c->jmp(frame->machineIp(newIp));
      ip = newIp;
    } break;

    case i2b: {
      frame->pushInt(c->load(BytesPerWord, 1, frame->popInt(), BytesPerWord));
    } break;

    case i2c: {
      frame->pushInt(c->loadz(BytesPerWord, 2, frame->popInt(), BytesPerWord));
    } break;

    case i2d: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, intToDoubleThunk)),
          0, 0, 8, 1, frame->popInt()));
    } break;

    case i2f: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, intToFloatThunk)),
          0, 0, 4, 1, frame->popInt()));
    } break;

    case i2l:
      frame->pushLong(c->load(BytesPerWord, 4, frame->popInt(), 8));
      break;

    case i2s: {
      frame->pushInt(c->load(BytesPerWord, 2, frame->popInt(), BytesPerWord));
    } break;
      
    case iadd: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->add(4, a, b));
    } break;
      
    case iand: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->and_(4, a, b));
    } break;

    case iconst_m1:
      frame->pushInt(c->constant(-1));
      break;

    case iconst_0:
      frame->pushInt(c->constant(0));
      break;

    case iconst_1:
      frame->pushInt(c->constant(1));
      break;

    case iconst_2:
      frame->pushInt(c->constant(2));
      break;

    case iconst_3:
      frame->pushInt(c->constant(3));
      break;

    case iconst_4:
      frame->pushInt(c->constant(4));
      break;

    case iconst_5:
      frame->pushInt(c->constant(5));
      break;

    case idiv: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->div(4, a, b));
    } break;

    case if_acmpeq:
    case if_acmpne: {
      uint32_t offset = codeReadInt16(t, code, ip);
      uint32_t newIp = (ip - 3) + offset;
      assert(t, newIp < codeLength(t, code));
        
      Compiler::Operand* a = frame->popObject();
      Compiler::Operand* b = frame->popObject();
      Compiler::Operand* target = frame->machineIp(newIp);

      c->cmp(BytesPerWord, a, b);
      if (instruction == if_acmpeq) {
        c->je(target);
      } else {
        c->jne(target);
      }

      saveStateAndCompile(t, frame, newIp);
      if (UNLIKELY(t->exception)) return;
    } break;

    case if_icmpeq:
    case if_icmpne:
    case if_icmpgt:
    case if_icmpge:
    case if_icmplt:
    case if_icmple: {
      uint32_t offset = codeReadInt16(t, code, ip);
      uint32_t newIp = (ip - 3) + offset;
      assert(t, newIp < codeLength(t, code));
        
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      Compiler::Operand* target = frame->machineIp(newIp);

      c->cmp(4, a, b);
      switch (instruction) {
      case if_icmpeq:
        c->je(target);
        break;
      case if_icmpne:
        c->jne(target);
        break;
      case if_icmpgt:
        c->jg(target);
        break;
      case if_icmpge:
        c->jge(target);
        break;
      case if_icmplt:
        c->jl(target);
        break;
      case if_icmple:
        c->jle(target);
        break;
      }
      
      saveStateAndCompile(t, frame, newIp);
      if (UNLIKELY(t->exception)) return;
    } break;

    case ifeq:
    case ifne:
    case ifgt:
    case ifge:
    case iflt:
    case ifle: {
      uint32_t offset = codeReadInt16(t, code, ip);
      uint32_t newIp = (ip - 3) + offset;
      assert(t, newIp < codeLength(t, code));

      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* target = frame->machineIp(newIp);

      c->cmp(4, c->constant(0), a);
      switch (instruction) {
      case ifeq:
        c->je(target);
        break;
      case ifne:
        c->jne(target);
        break;
      case ifgt:
        c->jg(target);
        break;
      case ifge:
        c->jge(target);
        break;
      case iflt:
        c->jl(target);
        break;
      case ifle:
        c->jle(target);
        break;
      }

      saveStateAndCompile(t, frame, newIp);
      if (UNLIKELY(t->exception)) return;
    } break;

    case ifnull:
    case ifnonnull: {
      uint32_t offset = codeReadInt16(t, code, ip);
      uint32_t newIp = (ip - 3) + offset;
      assert(t, newIp < codeLength(t, code));

      Compiler::Operand* a = frame->popObject();
      Compiler::Operand* target = frame->machineIp(newIp);

      c->cmp(BytesPerWord, c->constant(0), a);
      if (instruction == ifnull) {
        c->je(target);
      } else {
        c->jne(target);
      }

      saveStateAndCompile(t, frame, newIp);
      if (UNLIKELY(t->exception)) return;
    } break;

    case iinc: {
      uint8_t index = codeBody(t, code, ip++);
      int8_t count = codeBody(t, code, ip++);

      storeLocal
        (context, 1,
         c->add(4, c->constant(count), loadLocal(context, 1, index)),
         index);
    } break;

    case iload:
    case fload:
      frame->loadInt(codeBody(t, code, ip++));
      break;

    case iload_0:
    case fload_0:
      frame->loadInt(0);
      break;

    case iload_1:
    case fload_1:
      frame->loadInt(1);
      break;

    case iload_2:
    case fload_2:
      frame->loadInt(2);
      break;

    case iload_3:
    case fload_3:
      frame->loadInt(3);
      break;

    case imul: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->mul(4, a, b));
    } break;

    case ineg: {
      frame->pushInt(c->neg(4, frame->popInt()));
    } break;

    case instanceof: {
      uint16_t index = codeReadInt16(t, code, ip);

      object class_ = resolveClassInPool(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      frame->pushInt
        (c->call
         (c->constant(getThunk(t, instanceOf64Thunk)),
          0, 0, 4,
          3, c->register_(t->arch->thread()), frame->append(class_),
          frame->popObject()));
    } break;

    case invokeinterface: {
      uint16_t index = codeReadInt16(t, code, ip);
      ip += 2;

      object target = resolveMethod(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      assert(t, (methodFlags(t, target) & ACC_STATIC) == 0);

      unsigned parameterFootprint = methodParameterFootprint(t, target);

      unsigned instance = parameterFootprint - 1;

      unsigned rSize = resultSize(t, methodReturnCode(t, target));

      Compiler::Operand* result = c->stackCall
        (c->call
         (c->constant
          (getThunk(t, findInterfaceMethodFromInstanceThunk)),
          0,
          frame->trace(0, 0),
          BytesPerWord,
          3, c->register_(t->arch->thread()), frame->append(target),
          c->peek(1, instance)),
         0,
         frame->trace(0, 0),
         rSize,
         parameterFootprint);

      frame->pop(parameterFootprint);

      if (rSize) {
        pushReturnValue(t, frame, methodReturnCode(t, target), result);
      }
    } break;

    case invokespecial: {
      uint16_t index = codeReadInt16(t, code, ip);

      object target = resolveMethod(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      object class_ = methodClass(t, context->method);
      if (isSpecialMethod(t, target, class_)) {
        target = findMethod(t, target, classSuper(t, class_));
      }

      assert(t, (methodFlags(t, target) & ACC_STATIC) == 0);

      bool tailCall = isTailCall(t, code, ip, context->method, target);

      compileDirectInvoke(t, frame, target, tailCall);
    } break;

    case invokestatic: {
      uint16_t index = codeReadInt16(t, code, ip);

      object target = resolveMethod(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      assert(t, methodFlags(t, target) & ACC_STATIC);

      bool tailCall = isTailCall(t, code, ip, context->method, target);

      compileDirectInvoke(t, frame, target, tailCall);
    } break;

    case invokevirtual: {
      uint16_t index = codeReadInt16(t, code, ip);

      object target = resolveMethod(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      assert(t, (methodFlags(t, target) & ACC_STATIC) == 0);

      unsigned parameterFootprint = methodParameterFootprint(t, target);

      unsigned offset = ClassVtable + (methodOffset(t, target) * BytesPerWord);

      Compiler::Operand* instance = c->peek(1, parameterFootprint - 1);

      unsigned rSize = resultSize(t, methodReturnCode(t, target));

      bool tailCall = isTailCall(t, code, ip, context->method, target);

      Compiler::Operand* result = c->stackCall
        (c->memory
         (c->and_
          (BytesPerWord, c->constant(PointerMask),
           c->memory(instance, 0, 0, 1)), offset, 0, 1),
         tailCall ? Compiler::TailJump : 0,
         frame->trace(0, 0),
         rSize,
         parameterFootprint);

      frame->pop(parameterFootprint);

      if (rSize) {
        pushReturnValue(t, frame, methodReturnCode(t, target), result);
      }
    } break;

    case ior: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->or_(4, a, b));
    } break;

    case irem: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->rem(4, a, b));
    } break;

    case ireturn:
    case freturn: {
      handleExit(t, frame);
      c->return_(4, frame->popInt());
    } return;

    case ishl: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->shl(4, a, b));
    } break;

    case ishr: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->shr(4, a, b));
    } break;

    case istore:
    case fstore:
      frame->storeInt(codeBody(t, code, ip++));
      break;

    case istore_0:
    case fstore_0:
      frame->storeInt(0);
      break;

    case istore_1:
    case fstore_1:
      frame->storeInt(1);
      break;

    case istore_2:
    case fstore_2:
      frame->storeInt(2);
      break;

    case istore_3:
    case fstore_3:
      frame->storeInt(3);
      break;

    case isub: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->sub(4, a, b));
    } break;

    case iushr: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->ushr(4, a, b));
    } break;

    case ixor: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popInt();
      frame->pushInt(c->xor_(4, a, b));
    } break;

    case jsr:
    case jsr_w: {
      uint32_t thisIp;
      uint32_t newIp;

      if (instruction == jsr) {
        uint32_t offset = codeReadInt16(t, code, ip);
        thisIp = ip - 3;
        newIp = thisIp + offset;
      } else {
        uint32_t offset = codeReadInt32(t, code, ip);
        thisIp = ip - 5;
        newIp = thisIp + offset;
      }

      assert(t, newIp < codeLength(t, code));

      unsigned start = frame->startSubroutine(newIp, c->machineIp(ip));

      c->jmp(frame->machineIp(newIp));

      saveStateAndCompile(t, frame, newIp);
      if (UNLIKELY(t->exception)) return;

      frame->endSubroutine(start);
    } break;

    case l2d: {
      frame->pushLong
        (c->call
         (c->constant(getThunk(t, longToDoubleThunk)),
          0, 0, 8, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case l2f: {
      frame->pushInt
        (c->call
         (c->constant(getThunk(t, longToFloatThunk)),
          0, 0, 4, 2,
          static_cast<Compiler::Operand*>(0), frame->popLong()));
    } break;

    case l2i:
      frame->pushInt(c->load(8, 8, frame->popLong(), BytesPerWord));
      break;

    case ladd: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->add(8, a, b));
    } break;

    case land: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->and_(8, a, b));
    } break;

    case lcmp: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();

      frame->pushInt(c->lcmp(a, b));
    } break;

    case lconst_0:
      frame->pushLong(c->constant(0));
      break;

    case lconst_1:
      frame->pushLong(c->constant(1));
      break;

    case ldc:
    case ldc_w: {
      uint16_t index;

      if (instruction == ldc) {
        index = codeBody(t, code, ip++);
      } else {
        index = codeReadInt16(t, code, ip);
      }

      object pool = codePool(t, code);

      if (singletonIsObject(t, pool, index - 1)) {
        object v = singletonObject(t, pool, index - 1);
        if (objectClass(t, v)
            == arrayBody(t, t->m->types, Machine::ByteArrayType))
        {
          object class_ = resolveClassInPool(t, context->method, index - 1); 
          if (UNLIKELY(t->exception)) return;

          frame->pushObject(frame->append(class_));
        } else {
          frame->pushObject(frame->append(v));
        }
      } else {
        frame->pushInt(c->constant(singletonValue(t, pool, index - 1)));
      }
    } break;

    case ldc2_w: {
      uint16_t index = codeReadInt16(t, code, ip);

      object pool = codePool(t, code);

      uint64_t v;
      memcpy(&v, &singletonValue(t, pool, index - 1), 8);
      frame->pushLong(c->constant(v));
    } break;

    case ldiv_: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->div(8, a, b));
    } break;

    case lload:
    case dload:
      frame->loadLong(codeBody(t, code, ip++));
      break;

    case lload_0:
    case dload_0:
      frame->loadLong(0);
      break;

    case lload_1:
    case dload_1:
      frame->loadLong(1);
      break;

    case lload_2:
    case dload_2:
      frame->loadLong(2);
      break;

    case lload_3:
    case dload_3:
      frame->loadLong(3);
      break;

    case lmul: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->mul(8, a, b));
    } break;

    case lneg:
      frame->pushLong(c->neg(8, frame->popLong()));
      break;

    case lookupswitch: {
      int32_t base = ip - 1;

      ip = (ip + 3) & ~3; // pad to four byte boundary

      Compiler::Operand* key = frame->popInt();
    
      uint32_t defaultIp = base + codeReadInt32(t, code, ip);
      assert(t, defaultIp < codeLength(t, code));

      Compiler::Operand* default_ = frame->addressOperand
        (c->machineIp(defaultIp));

      int32_t pairCount = codeReadInt32(t, code, ip);

      Compiler::Operand* start = 0;
      uint32_t ipTable[pairCount];
      for (int32_t i = 0; i < pairCount; ++i) {
        unsigned index = ip + (i * 8);
        int32_t key = codeReadInt32(t, code, index);
        uint32_t newIp = base + codeReadInt32(t, code, index);
        assert(t, newIp < codeLength(t, code));

        ipTable[i] = newIp;

        Promise* p = c->poolAppend(key);
        if (i == 0) {
          start = frame->addressOperand(p);
        }
        c->poolAppendPromise(frame->addressPromise(c->machineIp(newIp)));
      }
      assert(t, start);

      c->jmp
        (c->call
         (c->constant(getThunk(t, lookUpAddressThunk)),
          0, 0, BytesPerWord,
          4, key, start, c->constant(pairCount), default_));

      Compiler::State* state = c->saveState();

      for (int32_t i = 0; i < pairCount; ++i) {
        compile(t, frame, ipTable[i]);
        if (UNLIKELY(t->exception)) return;

        c->restoreState(state);
      }

      ip = defaultIp;
    } break;

    case lor: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->or_(8, a, b));
    } break;

    case lrem: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->rem(8, a, b));
    } break;

    case lreturn:
    case dreturn: {
      handleExit(t, frame);
      c->return_(8, frame->popLong());
    } return;

    case lshl: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->shl(8, a, b));
    } break;

    case lshr: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->shr(8, a, b));
    } break;

    case lstore:
    case dstore:
      frame->storeLong(codeBody(t, code, ip++));
      break;

    case lstore_0:
    case dstore_0:
      frame->storeLong(0);
      break;

    case lstore_1:
    case dstore_1:
      frame->storeLong(1);
      break;

    case lstore_2:
    case dstore_2:
      frame->storeLong(2);
      break;

    case lstore_3:
    case dstore_3:
      frame->storeLong(3);
      break;

    case lsub: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->sub(8, a, b));
    } break;

    case lushr: {
      Compiler::Operand* a = frame->popInt();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->ushr(8, a, b));
    } break;

    case lxor: {
      Compiler::Operand* a = frame->popLong();
      Compiler::Operand* b = frame->popLong();
      frame->pushLong(c->xor_(8, a, b));
    } break;

    case monitorenter: {
      Compiler::Operand* target = frame->popObject();
      c->call
        (c->constant(getThunk(t, acquireMonitorForObjectThunk)),
         0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()), target);
    } break;

    case monitorexit: {
      Compiler::Operand* target = frame->popObject();
      c->call
        (c->constant(getThunk(t, releaseMonitorForObjectThunk)),
         0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()), target);
    } break;

    case multianewarray: {
      uint16_t index = codeReadInt16(t, code, ip);
      uint8_t dimensions = codeBody(t, code, ip++);

      object class_ = resolveClassInPool(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;
      PROTECT(t, class_);

      unsigned offset
        = localOffset
        (t, localSize(t, context->method) + c->topOfStack(), context->method)
        + t->arch->frameReturnAddressSize();

      Compiler::Operand* result = c->call
        (c->constant(getThunk(t, makeMultidimensionalArrayThunk)),
         0,
         frame->trace(0, 0),
         BytesPerWord,
         4, c->register_(t->arch->thread()), frame->append(class_),
         c->constant(dimensions), c->constant(offset));

      frame->pop(dimensions);
      frame->pushObject(result);
    } break;

    case new_: {
      uint16_t index = codeReadInt16(t, code, ip);
        
      object class_ = resolveClassInPool(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      if (classVmFlags(t, class_) & (WeakReferenceFlag | HasFinalizerFlag)) {
        frame->pushObject
          (c->call
           (c->constant(getThunk(t, makeNewGeneral64Thunk)),
            0,
            frame->trace(0, 0),
            BytesPerWord,
            2, c->register_(t->arch->thread()), frame->append(class_)));
      } else {
        frame->pushObject
          (c->call
           (c->constant(getThunk(t, makeNew64Thunk)),
            0,
            frame->trace(0, 0),
            BytesPerWord,
            2, c->register_(t->arch->thread()), frame->append(class_)));
      }
    } break;

    case newarray: {
      uint8_t type = codeBody(t, code, ip++);

      Compiler::Operand* length = frame->popInt();

      frame->pushObject
        (c->call
         (c->constant(getThunk(t, makeBlankArrayThunk)),
          0,
          frame->trace(0, 0),
          BytesPerWord,
          3, c->register_(t->arch->thread()), c->constant(type), length));
    } break;

    case nop: break;

    case pop_:
      frame->pop(1);
      break;

    case pop2:
      frame->pop(2);
      break;

    case putfield:
    case putstatic: {
      uint16_t index = codeReadInt16(t, code, ip);
    
      object field = resolveField(t, context->method, index - 1);
      if (UNLIKELY(t->exception)) return;

      object staticTable = 0;

      if (instruction == putstatic) {
        assert(t, fieldFlags(t, field) & ACC_STATIC);

        if (fieldClass(t, field) != methodClass(t, context->method)
            and classNeedsInit(t, fieldClass(t, field)))
        {
          c->call
            (c->constant(getThunk(t, tryInitClassThunk)),
             0,
             frame->trace(0, 0),
             0,
             2, c->register_(t->arch->thread()),
             frame->append(fieldClass(t, field)));
        }

        staticTable = classStaticTable(t, fieldClass(t, field));      
      } else {
        assert(t, (fieldFlags(t, field) & ACC_STATIC) == 0);

        if (inTryBlock(t, code, ip - 3)) {
          c->saveLocals();
        }
      }

      Compiler::Operand* value;
      switch (fieldCode(t, field)) {
      case ByteField:
      case BooleanField:
      case CharField:
      case ShortField:
      case FloatField:
      case IntField: {
        value = frame->popInt();
      } break;

      case DoubleField:
      case LongField: {
        value = frame->popLong();
      } break;

      case ObjectField: {
        value = frame->popObject();
      } break;

      default: abort(t);
      }

      Compiler::Operand* table;

      if (instruction == putstatic) {
        table = frame->append(staticTable);
      } else {
        table = frame->popObject();
      }

      Compiler::Operand* fieldOperand = 0;

      if (fieldFlags(t, field) & ACC_VOLATILE) {
        if (BytesPerWord == 4
            and (fieldCode(t, field) == DoubleField
                 or fieldCode(t, field) == LongField))
        {
          fieldOperand = frame->append(field);

          c->call
            (c->constant(getThunk(t, acquireMonitorForObjectThunk)),
             0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()),
             fieldOperand);
        } else {
          c->storeStoreBarrier();
        }
      }

      switch (fieldCode(t, field)) {
      case ByteField:
      case BooleanField:
        c->store(BytesPerWord, value, 1,
                 c->memory(table, fieldOffset(t, field), 0, 1));
        break;

      case CharField:
      case ShortField:
        c->store(BytesPerWord, value, 2,
                 c->memory(table, fieldOffset(t, field), 0, 1));
        break;
            
      case FloatField:
      case IntField:
        c->store(BytesPerWord, value, 4,
                 c->memory(table, fieldOffset(t, field), 0, 1));
        break;

      case DoubleField:
      case LongField:
        c->store(8, value, 8, c->memory(table, fieldOffset(t, field), 0, 1));
        break;

      case ObjectField:
        if (instruction == putfield) {
          c->call
            (c->constant(getThunk(t, setMaybeNullThunk)),
             0,
             frame->trace(0, 0),
             0,
             4, c->register_(t->arch->thread()), table,
             c->constant(fieldOffset(t, field)), value);
        } else {
          c->call
            (c->constant(getThunk(t, setThunk)),
             0, 0, 0,
             4, c->register_(t->arch->thread()), table,
             c->constant(fieldOffset(t, field)), value);
        }
        break;

      default: abort(t);
      }

      if (fieldFlags(t, field) & ACC_VOLATILE) {
        if (BytesPerWord == 4
            and (fieldCode(t, field) == DoubleField
                 or fieldCode(t, field) == LongField))
        {
          c->call
            (c->constant(getThunk(t, releaseMonitorForObjectThunk)),
             0, frame->trace(0, 0), 0, 2, c->register_(t->arch->thread()),
             fieldOperand);
        } else {
          c->storeLoadBarrier();
        }
      }
    } break;

    case ret: {
      unsigned index = codeBody(t, code, ip);
      c->saveLocals();
      c->jmp(loadLocal(context, 1, index));
      frame->returnFromSubroutine(index);
    } return;

    case return_:
      if (needsReturnBarrier(t, context->method)) {
        c->storeStoreBarrier();
      }

      handleExit(t, frame);
      c->return_(0, 0);
      return;

    case sipush:
      frame->pushInt
        (c->constant(static_cast<int16_t>(codeReadInt16(t, code, ip))));
      break;

    case swap:
      frame->swap();
      break;

    case tableswitch: {
      int32_t base = ip - 1;

      ip = (ip + 3) & ~3; // pad to four byte boundary

      uint32_t defaultIp = base + codeReadInt32(t, code, ip);
      assert(t, defaultIp < codeLength(t, code));
      
      int32_t bottom = codeReadInt32(t, code, ip);
      int32_t top = codeReadInt32(t, code, ip);
        
      Compiler::Operand* start = 0;
      uint32_t ipTable[top - bottom + 1];
      for (int32_t i = 0; i < top - bottom + 1; ++i) {
        unsigned index = ip + (i * 4);
        uint32_t newIp = base + codeReadInt32(t, code, index);
        assert(t, newIp < codeLength(t, code));

        ipTable[i] = newIp;

        Promise* p = c->poolAppendPromise
          (frame->addressPromise(c->machineIp(newIp)));
        if (i == 0) {
          start = frame->addressOperand(p);
        }
      }
      assert(t, start);

      Compiler::Operand* key = frame->popInt();
      
      c->cmp(4, c->constant(bottom), key);
      c->jl(frame->machineIp(defaultIp));

      c->save(1, key);

      saveStateAndCompile(t, frame, defaultIp);

      c->cmp(4, c->constant(top), key);
      c->jg(frame->machineIp(defaultIp));

      c->save(1, key);

      saveStateAndCompile(t, frame, defaultIp);

      Compiler::Operand* normalizedKey
        = (bottom ? c->sub(4, c->constant(bottom), key) : key);

      c->jmp(c->load(BytesPerWord, BytesPerWord,
                     c->memory(start, 0, normalizedKey, BytesPerWord),
                     BytesPerWord));

      Compiler::State* state = c->saveState();

      for (int32_t i = 0; i < top - bottom + 1; ++i) {
        compile(t, frame, ipTable[i]);
        if (UNLIKELY(t->exception)) return;

        c->restoreState(state);
      }

      ip = defaultIp;
    } break;

    case wide: {
      switch (codeBody(t, code, ip++)) {
      case aload: {
        frame->loadObject(codeReadInt16(t, code, ip));
      } break;

      case astore: {
        frame->storeObject(codeReadInt16(t, code, ip));
      } break;

      case iinc: {
        uint16_t index = codeReadInt16(t, code, ip);
        uint16_t count = codeReadInt16(t, code, ip);

        storeLocal
          (context, 1,
           c->add(4, c->constant(count), loadLocal(context, 1, index)),
           index);
      } break;

      case iload: {
        frame->loadInt(codeReadInt16(t, code, ip));
      } break;

      case istore: {
        frame->storeInt(codeReadInt16(t, code, ip));
      } break;

      case lload: {
        frame->loadLong(codeReadInt16(t, code, ip));
      } break;

      case lstore: {
        frame->storeLong(codeReadInt16(t, code, ip));
      } break;

      case ret: {
        unsigned index = codeReadInt16(t, code, ip);
        c->jmp(loadLocal(context, 1, index));
        frame->returnFromSubroutine(index);
      } return;

      default: abort(t);
      }
    } break;

    default: abort(t);
    }
  }
}

FILE* compileLog = 0;

void
logCompile(MyThread* t, const void* code, unsigned size, const char* class_,
           const char* name, const char* spec)
{
  static bool open = false;
  if (not open) {
    open = true;
    const char* path = findProperty(t, "avian.jit.log");
    if (path) {
      compileLog = fopen(path, "wb");
    } else if (DebugCompile) {
      compileLog = stderr;
    }
  }

  if (compileLog) {
    fprintf(compileLog, "%p %p %s.%s%s\n",
            code, static_cast<const uint8_t*>(code) + size,
            class_, name, spec);
  }
}

void
translateExceptionHandlerTable(MyThread* t, Compiler* c, object method,
                               intptr_t start)
{
  object oldTable = codeExceptionHandlerTable(t, methodCode(t, method));

  if (oldTable) {
    PROTECT(t, method);
    PROTECT(t, oldTable);

    unsigned length = exceptionHandlerTableLength(t, oldTable);

    object newIndex = makeIntArray(t, length * 3);
    PROTECT(t, newIndex);

    object newTable = makeArray(t, length + 1);
    PROTECT(t, newTable);

    set(t, newTable, ArrayBody, newIndex);

    for (unsigned i = 0; i < length; ++i) {
      ExceptionHandler* oldHandler = exceptionHandlerTableBody
        (t, oldTable, i);

      intArrayBody(t, newIndex, i * 3)
        = c->machineIp(exceptionHandlerStart(oldHandler))->value() - start;

      intArrayBody(t, newIndex, (i * 3) + 1)
        = c->machineIp(exceptionHandlerEnd(oldHandler))->value() - start;

      intArrayBody(t, newIndex, (i * 3) + 2)
        = c->machineIp(exceptionHandlerIp(oldHandler))->value() - start;

      object type;
      if (exceptionHandlerCatchType(oldHandler)) {
        type = resolveClassInPool
          (t, method, exceptionHandlerCatchType(oldHandler) - 1);
        if (UNLIKELY(t->exception)) return;
      } else {
        type = 0;
      }

      set(t, newTable, ArrayBody + ((i + 1) * BytesPerWord), type);
    }

    set(t, methodCode(t, method), CodeExceptionHandlerTable, newTable);
  }
}

void
translateLineNumberTable(MyThread* t, Compiler* c, object code, intptr_t start)
{
  object oldTable = codeLineNumberTable(t, code);
  if (oldTable) {
    PROTECT(t, code);
    PROTECT(t, oldTable);

    unsigned length = lineNumberTableLength(t, oldTable);
    object newTable = makeLineNumberTable(t, length);
    for (unsigned i = 0; i < length; ++i) {
      LineNumber* oldLine = lineNumberTableBody(t, oldTable, i);
      LineNumber* newLine = lineNumberTableBody(t, newTable, i);

      lineNumberIp(newLine)
        = c->machineIp(lineNumberIp(oldLine))->value() - start;

      lineNumberLine(newLine) = lineNumberLine(oldLine);
    }

    set(t, code, CodeLineNumberTable, newTable);
  }
}

void
printSet(uintptr_t m, unsigned limit)
{
  if (limit) {
    for (unsigned i = 0; i < 16; ++i) {
      if ((m >> i) & 1) {
        fprintf(stderr, "1");
      } else {
        fprintf(stderr, "_");
      }
    }
  }
}

unsigned
calculateFrameMaps(MyThread* t, Context* context, uintptr_t* originalRoots,
                   unsigned eventIndex, SubroutinePath* subroutinePath = 0)
{
  // for each instruction with more than one predecessor, and for each
  // stack position, determine if there exists a path to that
  // instruction such that there is not an object pointer left at that
  // stack position (i.e. it is uninitialized or contains primitive
  // data).

  unsigned mapSize = frameMapSizeInWords(t, context->method);

  uintptr_t roots[mapSize];
  if (originalRoots) {
    memcpy(roots, originalRoots, mapSize * BytesPerWord);
  } else {
    memset(roots, 0, mapSize * BytesPerWord);
  }

  int32_t ip = -1;

  // invariant: for each stack position, roots contains a zero at that
  // position if there exists some path to the current instruction
  // such that there is definitely not an object pointer at that
  // position.  Otherwise, roots contains a one at that position,
  // meaning either all known paths result in an object pointer at
  // that position, or the contents of that position are as yet
  // unknown.

  unsigned length = context->eventLog.length();
  while (eventIndex < length) {
    Event e = static_cast<Event>(context->eventLog.get(eventIndex++));
    switch (e) {
    case PushContextEvent: {
      eventIndex = calculateFrameMaps
        (t, context, roots, eventIndex, subroutinePath);
    } break;

    case PopContextEvent:
      return eventIndex;

    case IpEvent: {
      ip = context->eventLog.get2(eventIndex);
      eventIndex += 2;

      if (DebugFrameMaps) {
        fprintf(stderr, "      roots at ip %3d: ", ip);
        printSet(*roots, mapSize);
        fprintf(stderr, "\n");
      }

      uintptr_t* tableRoots
        = (subroutinePath ? subroutinePath->rootTable : context->rootTable)
        + (ip * mapSize);

      if (context->visitTable[ip] > 1) {
        for (unsigned wi = 0; wi < mapSize; ++wi) {
          uintptr_t newRoots = tableRoots[wi] & roots[wi];

          if ((eventIndex == length
               or context->eventLog.get(eventIndex) == PopContextEvent)
              and newRoots != tableRoots[wi])
          {
            if (DebugFrameMaps) {
              fprintf(stderr, "dirty roots!\n");
            }

            context->dirtyRoots = true;
          }

          tableRoots[wi] = newRoots;
          roots[wi] &= tableRoots[wi];
        }

        if (DebugFrameMaps) {
          fprintf(stderr, "table roots at ip %3d: ", ip);
          printSet(*tableRoots, mapSize);
          fprintf(stderr, "\n");
        }
      } else {
        memcpy(tableRoots, roots, mapSize * BytesPerWord);
      }
    } break;

    case MarkEvent: {
      unsigned i = context->eventLog.get2(eventIndex);
      eventIndex += 2;

      markBit(roots, i);
    } break;

    case ClearEvent: {
      unsigned i = context->eventLog.get2(eventIndex);
      eventIndex += 2;

      clearBit(roots, i);
    } break;

    case PushExceptionHandlerEvent: {
      unsigned reference = context->eventLog.get2(eventIndex);
      eventIndex += 2;

      if (context->subroutineTable and context->subroutineTable[reference]) {
        Subroutine* s = context->subroutineTable[reference];
        unsigned originalEventIndex = eventIndex;

        for (SubroutineCall* c = s->calls; c; c = c->next) {
          for (SubroutinePath* p = c->paths; p; p = p->listNext) {
            memcpy(roots, p->rootTable + (reference * mapSize),
                   mapSize * BytesPerWord);

            eventIndex = calculateFrameMaps
              (t, context, roots, originalEventIndex, p);
          }
        }
      } else {
        memcpy(roots, context->rootTable + (reference * mapSize),
               mapSize * BytesPerWord);

        eventIndex = calculateFrameMaps(t, context, roots, eventIndex, 0);
      }
    } break;

    case TraceEvent: {
      TraceElement* te; context->eventLog.get(eventIndex, &te, BytesPerWord);
      if (DebugFrameMaps) {
        fprintf(stderr, "trace roots at ip %3d: ", ip);
        printSet(*roots, mapSize);
        if (subroutinePath) {
          fprintf(stderr, " ");
          print(subroutinePath);
        }
        fprintf(stderr, "\n");
      }

      if (subroutinePath == 0) {
        memcpy(te->map, roots, mapSize * BytesPerWord);
      } else {
        SubroutineTrace* trace = 0;
        for (SubroutineTrace* t = te->subroutineTrace; t; t = t->next) {
          if (t->path == subroutinePath) {
            trace = t;
          }
        }

        if (trace == 0) {
          te->subroutineTrace = trace = new
            (context->zone.allocate
             (sizeof(SubroutineTrace) + (mapSize * BytesPerWord)))
            SubroutineTrace(subroutinePath, te->subroutineTrace);

          ++ te->subroutineTraceCount;
        }

        memcpy(trace->map, roots, mapSize * BytesPerWord);
      }

      eventIndex += BytesPerWord;
    } break;

    case PushSubroutineEvent: {
      SubroutineCall* call;
      context->eventLog.get(eventIndex, &call, BytesPerWord);
      eventIndex += BytesPerWord;

      unsigned nextIndex = context->eventLog.get2(eventIndex);

      eventIndex = nextIndex;

      SubroutinePath* path = 0;
      for (SubroutinePath* p = call->paths; p; p = p->listNext) {
        if (p->stackNext == subroutinePath) {
          path = p;
          break;
        }
      }

      if (path == 0) {
        path = new (context->zone.allocate(sizeof(SubroutinePath)))
          SubroutinePath(call, subroutinePath,
                         makeRootTable(t, &(context->zone), context->method));
      }

      calculateFrameMaps(t, context, roots, call->subroutine->logIndex, path);
    } break;

    case PopSubroutineEvent:
      return static_cast<unsigned>(-1);

    default: abort(t);
    }
  }

  return eventIndex;
}

int
compareTraceElementPointers(const void* va, const void* vb)
{
  TraceElement* a = *static_cast<TraceElement* const*>(va);
  TraceElement* b = *static_cast<TraceElement* const*>(vb);
  if (a->address->value() > b->address->value()) {
    return 1;
  } else if (a->address->value() < b->address->value()) {
    return -1;
  } else {
    return 0;
  }
}

unsigned
simpleFrameMapTableSize(MyThread* t, object method, object map)
{
  int size = frameMapSizeInBits(t, method);
  return ceiling(intArrayLength(t, map) * size, 32 + size);
}

unsigned
codeSingletonSizeInBytes(MyThread*, unsigned codeSizeInBytes)
{
  unsigned count = ceiling(codeSizeInBytes, BytesPerWord);
  unsigned size = count + singletonMaskSize(count);
  return pad(SingletonBody + (size * BytesPerWord));
}

uint8_t*
finish(MyThread* t, Allocator* allocator, Assembler* a, const char* name)
{
  uint8_t* start = static_cast<uint8_t*>
    (allocator->allocate(pad(a->length())));

  a->writeTo(start);

  logCompile(t, start, a->length(), 0, name, 0);

  return start;
}

void
setBit(int32_t* dst, unsigned index)
{
  dst[index / 32] |= static_cast<int32_t>(1) << (index % 32);
}

void
clearBit(int32_t* dst, unsigned index)
{
  dst[index / 32] &= ~(static_cast<int32_t>(1) << (index % 32));
}

void
copyFrameMap(int32_t* dst, uintptr_t* src, unsigned mapSizeInBits,
             unsigned offset, TraceElement* p,
             SubroutinePath* subroutinePath)
{
  if (DebugFrameMaps) {
    fprintf(stderr, " orig roots at ip %p: ", reinterpret_cast<void*>
            (p->address->value()));
    printSet(src[0], ceiling(mapSizeInBits, BitsPerWord));
    print(subroutinePath);
    fprintf(stderr, "\n");

    fprintf(stderr, "final roots at ip %p: ", reinterpret_cast<void*>
            (p->address->value()));
  }

  for (unsigned j = 0; j < p->argumentIndex; ++j) {
    if (getBit(src, j)) {
      if (DebugFrameMaps) {
        fprintf(stderr, "1");
      }
      setBit(dst, offset + j);
    } else {
      if (DebugFrameMaps) {
        fprintf(stderr, "_");
      }
      clearBit(dst, offset + j);
    }
  }

  if (DebugFrameMaps) {
    print(subroutinePath);
    fprintf(stderr, "\n");
  }
}

class FrameMapTableHeader {
 public:
  FrameMapTableHeader(unsigned indexCount):
    indexCount(indexCount)
  { }

  unsigned indexCount;
};

class FrameMapTableIndexElement {
 public:
  FrameMapTableIndexElement(int offset, unsigned base, unsigned path):
    offset(offset),
    base(base),
    path(path)
  { }

  int offset;
  unsigned base;
  unsigned path;
};

class FrameMapTablePath {
 public:
  FrameMapTablePath(unsigned stackIndex, unsigned elementCount, unsigned next):
    stackIndex(stackIndex),
    elementCount(elementCount),
    next(next)
  { }

  unsigned stackIndex;
  unsigned elementCount;
  unsigned next;
  int32_t elements[0];
};

int
compareInt32s(const void* va, const void* vb)
{
  return *static_cast<int32_t const*>(va) - *static_cast<int32_t const*>(vb);
}

int
compare(SubroutinePath* a, SubroutinePath* b)
{
  if (a->stackNext) {
    int d = compare(a->stackNext, b->stackNext);
    if (d) return d;
  }
  int64_t av = a->call->returnAddress->value();
  int64_t bv = b->call->returnAddress->value();
  if (av > bv) {
    return 1;
  } else if (av < bv) {
    return -1;
  } else {
    return 0;
  }
}

int
compareSubroutineTracePointers(const void* va, const void* vb)
{
  return compare((*static_cast<SubroutineTrace* const*>(va))->path,
                 (*static_cast<SubroutineTrace* const*>(vb))->path);
}

object
makeGeneralFrameMapTable(MyThread* t, Context* context, uint8_t* start,
                         TraceElement** elements, unsigned pathFootprint,
                         unsigned mapCount)
{
  unsigned mapSize = frameMapSizeInBits(t, context->method);
  unsigned indexOffset = sizeof(FrameMapTableHeader);
  unsigned mapsOffset = indexOffset
    + (context->traceLogCount * sizeof(FrameMapTableIndexElement));
  unsigned pathsOffset = mapsOffset + (ceiling(mapCount * mapSize, 32) * 4);

  object table = makeByteArray(t, pathsOffset + pathFootprint);
  
  int8_t* body = &byteArrayBody(t, table, 0);
  new (body) FrameMapTableHeader(context->traceLogCount);
 
  unsigned nextTableIndex = pathsOffset;
  unsigned nextMapIndex = 0;
  for (unsigned i = 0; i < context->traceLogCount; ++i) {
    TraceElement* p = elements[i];
    unsigned mapBase = nextMapIndex;

    unsigned pathIndex;
    if (p->subroutineTrace) {
      FrameMapTablePath* previous = 0;
      Subroutine* subroutine = p->subroutineTrace->path->call->subroutine;
      for (Subroutine* s = subroutine; s; s = s->stackNext) {
        if (s->tableIndex == 0) {
          unsigned pathObjectSize = sizeof(FrameMapTablePath)
            + (sizeof(int32_t) * s->callCount);

          assert(t, nextTableIndex + pathObjectSize
                 <= byteArrayLength(t, table));

          s->tableIndex = nextTableIndex;
          
          nextTableIndex += pathObjectSize;

          FrameMapTablePath* current = new (body + s->tableIndex)
            FrameMapTablePath
            (s->stackIndex, s->callCount,
             s->stackNext ? s->stackNext->tableIndex : 0);

          unsigned i = 0;
          for (SubroutineCall* c = subroutine->calls; c; c = c->next) {
            assert(t, i < s->callCount);

            current->elements[i++]
              = static_cast<intptr_t>(c->returnAddress->value())
              - reinterpret_cast<intptr_t>(start);
          }
          assert(t, i == s->callCount);

          qsort(current->elements, s->callCount, sizeof(int32_t),
                compareInt32s);

          if (previous) {
            previous->next = s->tableIndex;
          }

          previous = current;
        } else {
          break;
        }
      }

      pathIndex = subroutine->tableIndex;

      SubroutineTrace* traces[p->subroutineTraceCount];
      unsigned i = 0;
      for (SubroutineTrace* trace = p->subroutineTrace;
           trace; trace = trace->next)
      {
        assert(t, i < p->subroutineTraceCount);
        traces[i++] = trace;
      }
      assert(t, i == p->subroutineTraceCount);

      qsort(traces, p->subroutineTraceCount, sizeof(SubroutineTrace*),
            compareSubroutineTracePointers);

      for (unsigned i = 0; i < p->subroutineTraceCount; ++i) {
        assert(t, mapsOffset + ceiling(nextMapIndex + mapSize, 32) * 4
               <= pathsOffset);

        copyFrameMap(reinterpret_cast<int32_t*>(body + mapsOffset),
                     traces[i]->map, mapSize, nextMapIndex, p,
                     traces[i]->path);

        nextMapIndex += mapSize;
      }
    } else {
      pathIndex = 0;

      assert(t, mapsOffset + ceiling(nextMapIndex + mapSize, 32) * 4
             <= pathsOffset);

      copyFrameMap(reinterpret_cast<int32_t*>(body + mapsOffset), p->map,
                   mapSize, nextMapIndex, p, 0);
      
      nextMapIndex += mapSize;
    }

    unsigned elementIndex = indexOffset
      + (i * sizeof(FrameMapTableIndexElement));

    assert(t, elementIndex + sizeof(FrameMapTableIndexElement) <= mapsOffset);

    new (body + elementIndex) FrameMapTableIndexElement
      (static_cast<intptr_t>(p->address->value())
       - reinterpret_cast<intptr_t>(start), mapBase, pathIndex);
  }

  assert(t, nextMapIndex == mapCount * mapSize);

  return table;
}

object
makeSimpleFrameMapTable(MyThread* t, Context* context, uint8_t* start, 
                        TraceElement** elements)
{
  unsigned mapSize = frameMapSizeInBits(t, context->method);
  object table = makeIntArray
    (t, context->traceLogCount
     + ceiling(context->traceLogCount * mapSize, 32));

  assert(t, intArrayLength(t, table) == context->traceLogCount
         + simpleFrameMapTableSize(t, context->method, table));

  for (unsigned i = 0; i < context->traceLogCount; ++i) {
    TraceElement* p = elements[i];

    intArrayBody(t, table, i) = static_cast<intptr_t>(p->address->value())
      - reinterpret_cast<intptr_t>(start);

    assert(t, context->traceLogCount + ceiling((i + 1) * mapSize, 32)
           <= intArrayLength(t, table));

    if (mapSize) {
      copyFrameMap(&intArrayBody(t, table, context->traceLogCount), p->map,
                   mapSize, i * mapSize, p, 0);
    }
  }

  return table;
}

uint8_t*
finish(MyThread* t, Allocator* allocator, Context* context)
{
  Compiler* c = context->compiler;

  unsigned codeSize = c->compile();
  uintptr_t* code = static_cast<uintptr_t*>
    (allocator->allocate(pad(codeSize) + pad(c->poolSize()) + BytesPerWord));
  code[0] = codeSize;
  uint8_t* start = reinterpret_cast<uint8_t*>(code + 1);

  if (context->objectPool) {
    object pool = allocate3
      (t, allocator, Machine::ImmortalAllocation,
       FixedSizeOfArray + ((context->objectPoolCount + 1) * BytesPerWord),
       true);

    initArray(t, pool, context->objectPoolCount + 1);
    mark(t, pool, 0);

    set(t, pool, ArrayBody, objectPools(t));
    objectPools(t) = pool;

    unsigned i = 1;
    for (PoolElement* p = context->objectPool; p; p = p->next) {
      unsigned offset = ArrayBody + ((i++) * BytesPerWord);

      p->address = reinterpret_cast<uintptr_t>(pool) + offset;

      set(t, pool, offset, p->target);
    }
  }

  c->writeTo(start);

  BootContext* bc = context->bootContext;
  if (bc) {
    for (DelayedPromise* p = bc->addresses;
         p != bc->addressSentinal;
         p = p->next)
    {
      p->basis = new (bc->zone->allocate(sizeof(ResolvedPromise)))
        ResolvedPromise(p->basis->value());
    }
  }

  translateExceptionHandlerTable
    (t, c, context->method, reinterpret_cast<intptr_t>(start));
  if (UNLIKELY(t->exception)) return 0;

  translateLineNumberTable(t, c, methodCode(t, context->method),
                           reinterpret_cast<intptr_t>(start));

  { object code = methodCode(t, context->method);

    code = makeCode(t, 0,
                    codeExceptionHandlerTable(t, code),
                    codeLineNumberTable(t, code),
                    codeMaxStack(t, code),
                    codeMaxLocals(t, code),
                    0);

    set(t, context->method, MethodCode, code);
  }

  if (context->traceLogCount) {
    TraceElement* elements[context->traceLogCount];
    unsigned index = 0;
    unsigned pathFootprint = 0;
    unsigned mapCount = 0;
    for (TraceElement* p = context->traceLog; p; p = p->next) {
      assert(t, index < context->traceLogCount);

      SubroutineTrace* trace = p->subroutineTrace;
      unsigned myMapCount = 1;
      if (trace) {
        for (Subroutine* s = trace->path->call->subroutine;
             s; s = s->stackNext)
        {
          unsigned callCount = s->callCount;
          myMapCount *= callCount;
          if (not s->visited) {
            s->visited = true;
            pathFootprint += sizeof(FrameMapTablePath)
              + (sizeof(int32_t) * callCount);
          }
        }
      }
      
      mapCount += myMapCount;

      elements[index++] = p;

      if (p->target) {
        insertCallNode
          (t, makeCallNode
           (t, p->address->value(), p->target, p->flags, 0));
      }
    }

    qsort(elements, context->traceLogCount, sizeof(TraceElement*),
          compareTraceElementPointers);

    object map;
    if (pathFootprint) {
      map = makeGeneralFrameMapTable
        (t, context, start, elements, pathFootprint, mapCount);
    } else {
      map = makeSimpleFrameMapTable(t, context, start, elements);
    }

    set(t, methodCode(t, context->method), CodePool, map);
  }

  logCompile
    (t, start, codeSize,
     reinterpret_cast<const char*>
     (&byteArrayBody(t, className(t, methodClass(t, context->method)), 0)),
     reinterpret_cast<const char*>
     (&byteArrayBody(t, methodName(t, context->method), 0)),
     reinterpret_cast<const char*>
     (&byteArrayBody(t, methodSpec(t, context->method), 0)));

  // for debugging:
  if (//false and
      strcmp
      (reinterpret_cast<const char*>
       (&byteArrayBody(t, className(t, methodClass(t, context->method)), 0)),
       "org/eclipse/osgi/framework/internal/core/Constants") == 0 and
      strcmp
      (reinterpret_cast<const char*>
       (&byteArrayBody(t, methodName(t, context->method), 0)),
       "getInternalSymbolicName") == 0)
  {
    trap();
  }

  syncInstructionCache(start, codeSize);

  return start;
}

uint8_t*
compile(MyThread* t, Allocator* allocator, Context* context)
{
  Compiler* c = context->compiler;

//   fprintf(stderr, "compiling %s.%s%s\n",
//           &byteArrayBody(t, className(t, methodClass(t, context->method)), 0),
//           &byteArrayBody(t, methodName(t, context->method), 0),
//           &byteArrayBody(t, methodSpec(t, context->method), 0));

  unsigned footprint = methodParameterFootprint(t, context->method);
  unsigned locals = localSize(t, context->method);
  c->init(codeLength(t, methodCode(t, context->method)), footprint, locals,
          alignedFrameSize(t, context->method));

  uint8_t stackMap[codeMaxStack(t, methodCode(t, context->method))];
  Frame frame(context, stackMap);

  unsigned index = methodParameterFootprint(t, context->method);
  if ((methodFlags(t, context->method) & ACC_STATIC) == 0) {
    frame.set(--index, Frame::Object);
    c->initLocal(1, index);
  }

  for (MethodSpecIterator it
         (t, reinterpret_cast<const char*>
          (&byteArrayBody(t, methodSpec(t, context->method), 0)));
       it.hasNext();)
  {
    switch (*it.next()) {
    case 'L':
    case '[':
      frame.set(--index, Frame::Object);
      c->initLocal(1, index);
      break;
      
    case 'J':
    case 'D':
      frame.set(--index, Frame::Long);
      frame.set(--index, Frame::Long);
      c->initLocal(2, index);
      break;

    default:
      frame.set(--index, Frame::Integer);
      c->initLocal(1, index);
      break;
    }
  }

  handleEntrance(t, &frame);

  Compiler::State* state = c->saveState();

  compile(t, &frame, 0);
  if (UNLIKELY(t->exception)) return 0;

  context->dirtyRoots = false;
  unsigned eventIndex = calculateFrameMaps(t, context, 0, 0);

  object eht = codeExceptionHandlerTable(t, methodCode(t, context->method));
  if (eht) {
    PROTECT(t, eht);

    unsigned visitCount = exceptionHandlerTableLength(t, eht);

    bool visited[visitCount];
    memset(visited, 0, visitCount * sizeof(bool));

    while (visitCount) {
      bool progress = false;

      for (unsigned i = 0; i < exceptionHandlerTableLength(t, eht); ++i) {
        c->restoreState(state);

        ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);
        unsigned start = exceptionHandlerStart(eh);

        if ((not visited[i]) and context->visitTable[start]) {
          -- visitCount;
          visited[i] = true;
          progress = true;

          uint8_t stackMap[codeMaxStack(t, methodCode(t, context->method))];
          Frame frame2(&frame, stackMap);

          context->eventLog.append(PushExceptionHandlerEvent);
          context->eventLog.append2(start);

          for (unsigned i = 1;
               i < codeMaxStack(t, methodCode(t, context->method));
               ++i)
          {
            frame2.set(localSize(t, context->method) + i, Frame::Integer);
          }

          compile(t, &frame2, exceptionHandlerIp(eh), start);
          if (UNLIKELY(t->exception)) return 0;

          context->eventLog.append(PopContextEvent);

          eventIndex = calculateFrameMaps(t, context, 0, eventIndex);
        }
      }

      assert(t, progress);
    }
  }

  while (context->dirtyRoots) {
    context->dirtyRoots = false;
    calculateFrameMaps(t, context, 0, 0);
  }

  return finish(t, allocator, context);
}

void
updateCall(MyThread* t, UnaryOperation op, bool assertAlignment,
           void* returnAddress, void* target)
{
  t->arch->updateCall(op, assertAlignment, returnAddress, target);
}

void*
compileMethod2(MyThread* t, void* ip)
{
  object node = findCallNode(t, ip);
  object target = callNodeTarget(t, node);

  if (LIKELY(t->exception == 0)) {
    PROTECT(t, node);
    PROTECT(t, target);

    t->trace->targetMethod = target;

    compile(t, codeAllocator(t), 0, target);

    t->trace->targetMethod = 0;
  }

  if (UNLIKELY(t->exception)) {
    return 0;
  } else {
    void* address = reinterpret_cast<void*>(methodAddress(t, target));
    uint8_t* updateIp = static_cast<uint8_t*>(ip);
    
    updateCall(t, (callNodeFlags(t, node) & TraceElement::TailCall)
               ? Jump : Call, true, updateIp, address);

    return address;
  }
}

uint64_t
compileMethod(MyThread* t)
{
  void* ip;
  if (t->tailAddress) {
    ip = t->tailAddress;
    t->tailAddress = 0;
  } else {
    ip = t->arch->frameIp(t->stack);
  }

  void* r = compileMethod2(t, ip);

  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else {
    return reinterpret_cast<uintptr_t>(r);
  }
}

void*
compileVirtualMethod2(MyThread* t, object class_, unsigned index)
{
  // If class_ has BootstrapFlag set, that means its vtable is not yet
  // available.  However, we must set t->trace->targetMethod to an
  // appropriate method to ensure we can accurately scan the stack for
  // GC roots.  We find such a method by looking for a superclass with
  // a vtable and using it instead:

  object c = class_;
  while (classVmFlags(t, c) & BootstrapFlag) {
    c = classSuper(t, c);
  }
  t->trace->targetMethod = arrayBody(t, classVirtualTable(t, c), index);

  PROTECT(t, class_);

  object target = resolveTarget(t, class_, index);
  PROTECT(t, target);

  if (LIKELY(t->exception == 0)) {
    compile(t, codeAllocator(t), 0, target);
  }

  t->trace->targetMethod = 0;

  if (UNLIKELY(t->exception)) {
    return 0;
  } else {
    void* address = reinterpret_cast<void*>(methodAddress(t, target));
    if (methodFlags(t, target) & ACC_NATIVE) {
      t->trace->nativeMethod = target;
    } else {
      classVtable(t, class_, methodOffset(t, target)) = address;
    }
    return address;
  }
}

uint64_t
compileVirtualMethod(MyThread* t)
{
  object class_ = objectClass(t, static_cast<object>(t->virtualCallTarget));
  t->virtualCallTarget = 0;

  unsigned index = t->virtualCallIndex;
  t->virtualCallIndex = 0;

  void* r = compileVirtualMethod2(t, class_, index);

  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else {
    return reinterpret_cast<uintptr_t>(r);
  }
}

void
resolveNative(MyThread* t, object method)
{
  PROTECT(t, method);

  assert(t, methodFlags(t, method) & ACC_NATIVE);

  initClass(t, methodClass(t, method));

  if (LIKELY(t->exception == 0)
      and methodCompiled(t, method) == defaultThunk(t))
  {
    void* function = resolveNativeMethod(t, method);
    if (UNLIKELY(function == 0)) {
      object message = makeString
        (t, "%s.%s%s",
         &byteArrayBody(t, className(t, methodClass(t, method)), 0),
         &byteArrayBody(t, methodName(t, method), 0),
         &byteArrayBody(t, methodSpec(t, method), 0));
      t->exception = makeUnsatisfiedLinkError(t, message);
      return;
    }

    // ensure other threads see updated methodVmFlags before
    // methodCompiled, since we don't want them using the slow calling
    // convention on a function that expects the fast calling
    // convention:
    memoryBarrier();

    methodCompiled(t, method) = reinterpret_cast<uintptr_t>(function);
  }
}

uint64_t
invokeNativeFast(MyThread* t, object method)
{
  return reinterpret_cast<FastNativeFunction>(methodCompiled(t, method))
    (t, method,
     static_cast<uintptr_t*>(t->stack)
     + t->arch->frameFooterSize()
     + t->arch->frameReturnAddressSize());
}

uint64_t
invokeNativeSlow(MyThread* t, object method)
{
  PROTECT(t, method);

  object class_ = methodClass(t, method);
  PROTECT(t, class_);

  unsigned footprint = methodParameterFootprint(t, method) + 1;
  if (methodFlags(t, method) & ACC_STATIC) {
    ++ footprint;
  }
  unsigned count = methodParameterCount(t, method) + 2;

  uintptr_t args[footprint];
  unsigned argOffset = 0;
  uint8_t types[count];
  unsigned typeOffset = 0;

  args[argOffset++] = reinterpret_cast<uintptr_t>(t);
  types[typeOffset++] = POINTER_TYPE;

  uintptr_t* sp = static_cast<uintptr_t*>(t->stack)
    + t->arch->frameFooterSize()
    + t->arch->frameReturnAddressSize();

  if (methodFlags(t, method) & ACC_STATIC) {
    args[argOffset++] = reinterpret_cast<uintptr_t>(&class_);
  } else {
    args[argOffset++] = reinterpret_cast<uintptr_t>(sp++);
  }
  types[typeOffset++] = POINTER_TYPE;

  MethodSpecIterator it
    (t, reinterpret_cast<const char*>
     (&byteArrayBody(t, methodSpec(t, method), 0)));
  
  while (it.hasNext()) {
    unsigned type = types[typeOffset++]
      = fieldType(t, fieldCode(t, *it.next()));

    switch (type) {
    case INT8_TYPE:
    case INT16_TYPE:
    case INT32_TYPE:
    case FLOAT_TYPE:
      args[argOffset++] = *(sp++);
      break;

    case INT64_TYPE:
    case DOUBLE_TYPE: {
      memcpy(args + argOffset, sp, 8);
      argOffset += (8 / BytesPerWord);
      sp += 2;
    } break;

    case POINTER_TYPE: {
      if (*sp) {
        args[argOffset++] = reinterpret_cast<uintptr_t>(sp);
      } else {
        args[argOffset++] = 0;
      }
      ++ sp;
    } break;

    default: abort(t);
    }
  }

  void* function = reinterpret_cast<void*>(methodCompiled(t, method));
  unsigned returnCode = methodReturnCode(t, method);
  unsigned returnType = fieldType(t, returnCode);
  uint64_t result;

  if (DebugNatives) {
    fprintf(stderr, "invoke native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      acquire(t, methodClass(t, method));
    } else {
      acquire(t, *reinterpret_cast<object*>(args[0]));
    }
  }

  Reference* reference = t->reference;

  { ENTER(t, Thread::IdleState);

    result = t->m->system->call
      (function,
       args,
       types,
       count,
       footprint * BytesPerWord,
       returnType);
  }

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      release(t, methodClass(t, method));
    } else {
      release(t, *reinterpret_cast<object*>(args[0]));
    }
  }

  if (DebugNatives) {
    fprintf(stderr, "return from native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }

  if (LIKELY(t->exception == 0)) {
    switch (returnCode) {
    case ByteField:
    case BooleanField:
      result = static_cast<int8_t>(result);
      break;

    case CharField:
      result = static_cast<uint16_t>(result);
      break;

    case ShortField:
      result = static_cast<int16_t>(result);
      break;

    case FloatField:
    case IntField:
      result = static_cast<int32_t>(result);
      break;

    case LongField:
    case DoubleField:
      break;

    case ObjectField:
      result = static_cast<uintptr_t>(result) ? *reinterpret_cast<uintptr_t*>
        (static_cast<uintptr_t>(result)) : 0;
      break;

    case VoidField:
      result = 0;
      break;

    default: abort(t);
    }
  } else {
    result = 0;
  }

  while (t->reference != reference) {
    dispose(t, t->reference);
  }

  return result;
}
  
uint64_t
invokeNative2(MyThread* t, object method)
{
  if (methodVmFlags(t, method) & FastNative) {
    return invokeNativeFast(t, method);
  } else {
    return invokeNativeSlow(t, method);
  }
}

uint64_t
invokeNative(MyThread* t)
{
  if (t->trace->nativeMethod == 0) {
    void* ip;
    if (t->tailAddress) {
      ip = t->tailAddress;
      t->tailAddress = 0;
    } else {
      ip = t->arch->frameIp(t->stack);
    }

    object node = findCallNode(t, ip);
    object target = callNodeTarget(t, node);
    if (callNodeFlags(t, node) & TraceElement::VirtualCall) {
      target = resolveTarget(t, t->stack, target);
    }
    t->trace->nativeMethod = target;
  }

  assert(t, t->tailAddress == 0);

  uint64_t result = 0;

  t->trace->targetMethod = t->trace->nativeMethod;

  if (LIKELY(t->exception == 0)) {
    resolveNative(t, t->trace->nativeMethod);

    if (LIKELY(t->exception == 0)) {
      result = invokeNative2(t, t->trace->nativeMethod);
    }
  }

  unsigned parameterFootprint = methodParameterFootprint
    (t, t->trace->targetMethod);

  t->trace->targetMethod = 0;
  t->trace->nativeMethod = 0;

  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else {
    if (TailCalls
        and t->arch->argumentFootprint(parameterFootprint)
        > t->arch->stackAlignmentInWords())
    {
      t->stack = static_cast<uintptr_t*>(t->stack)
        + (t->arch->argumentFootprint(parameterFootprint)
           - t->arch->stackAlignmentInWords());
    }

    t->stack = static_cast<uintptr_t*>(t->stack)
      + t->arch->frameReturnAddressSize();

    return result;
  }
}

void
findFrameMapInSimpleTable(MyThread* t, object method, object table,
                          int32_t offset, int32_t** map, unsigned* start)
{
  unsigned tableSize = simpleFrameMapTableSize(t, method, table);
  unsigned indexSize = intArrayLength(t, table) - tableSize;

  *map = &intArrayBody(t, table, indexSize);
    
  unsigned bottom = 0;
  unsigned top = indexSize;
  for (unsigned span = top - bottom; span; span = top - bottom) {
    unsigned middle = bottom + (span / 2);
    int32_t v = intArrayBody(t, table, middle);
      
    if (offset == v) {
      *start = frameMapSizeInBits(t, method) * middle;
      return;
    } else if (offset < v) {
      top = middle;
    } else {
      bottom = middle + 1;
    }
  }

  abort(t);
}

unsigned
findFrameMap(MyThread* t, void* stack, object method, object table,
             unsigned pathIndex)
{
  if (pathIndex) {
    FrameMapTablePath* path = reinterpret_cast<FrameMapTablePath*>
      (&byteArrayBody(t, table, pathIndex));
    
    void* address = static_cast<void**>(stack)[path->stackIndex];
    uint8_t* base = reinterpret_cast<uint8_t*>(methodAddress(t, method));
    for (unsigned i = 0; i < path->elementCount; ++i) {
      if (address == base + path->elements[i]) {
        return i + (path->elementCount * findFrameMap
                    (t, stack, method, table, path->next));
      }
    }

    abort(t);
  } else {
    return 0;
  }
}

void
findFrameMapInGeneralTable(MyThread* t, void* stack, object method,
                           object table, int32_t offset, int32_t** map,
                           unsigned* start)
{
  FrameMapTableHeader* header = reinterpret_cast<FrameMapTableHeader*>
    (&byteArrayBody(t, table, 0));

  FrameMapTableIndexElement* index
    = reinterpret_cast<FrameMapTableIndexElement*>
    (&byteArrayBody(t, table, sizeof(FrameMapTableHeader)));

  *map = reinterpret_cast<int32_t*>(index + header->indexCount);

  unsigned bottom = 0;
  unsigned top = header->indexCount;
  for (unsigned span = top - bottom; span; span = top - bottom) {
    unsigned middle = bottom + (span / 2);
    FrameMapTableIndexElement* v = index + middle;
                                     
    if (offset == v->offset) {
      *start = v->base + (findFrameMap(t, stack, method, table, v->path)
                          * frameMapSizeInBits(t, method));
      return;
    } else if (offset < v->offset) {
      top = middle;
    } else {
      bottom = middle + 1;
    }
  }

  abort(t);
}

void
findFrameMap(MyThread* t, void* stack, object method, int32_t offset,
             int32_t** map, unsigned* start)
{
  object table = codePool(t, methodCode(t, method));
  if (objectClass(t, table)
      == arrayBody(t, t->m->types, Machine::IntArrayType))
  {
    findFrameMapInSimpleTable(t, method, table, offset, map, start);
  } else {
    findFrameMapInGeneralTable(t, stack, method, table, offset, map, start);
  }
}

void
visitStackAndLocals(MyThread* t, Heap::Visitor* v, void* frame, object method,
                    void* ip)
{
  unsigned count = frameMapSizeInBits(t, method);

  if (count) {
    void* stack = stackForFrame(t, frame, method);

    int32_t* map;
    unsigned offset;
    findFrameMap
      (t, stack, method, difference
       (ip, reinterpret_cast<void*>(methodAddress(t, method))), &map, &offset);

    for (unsigned i = 0; i < count; ++i) {
      int j = offset + i;
      if (map[j / 32] & (static_cast<int32_t>(1) << (j % 32))) {
        v->visit(localObject(t, stack, method, i));        
      }
    }
  }
}

void
visitArgument(MyThread* t, Heap::Visitor* v, void* stack, unsigned index)
{
  v->visit(static_cast<object*>(stack)
           + index
           + t->arch->frameReturnAddressSize()
           + t->arch->frameFooterSize());
}

void
visitArguments(MyThread* t, Heap::Visitor* v, void* stack, object method)
{
  unsigned index = 0;

  if ((methodFlags(t, method) & ACC_STATIC) == 0) {
    visitArgument(t, v, stack, index++);
  }

  for (MethodSpecIterator it
         (t, reinterpret_cast<const char*>
          (&byteArrayBody(t, methodSpec(t, method), 0)));
       it.hasNext();)
  {
    switch (*it.next()) {
    case 'L':
    case '[':
      visitArgument(t, v, stack, index++);
      break;
      
    case 'J':
    case 'D':
      index += 2;
      break;

    default:
      ++ index;
      break;
    }
  }
}

void
visitStack(MyThread* t, Heap::Visitor* v)
{
  void* ip = t->ip;
  void* base = t->base;
  void* stack = t->stack;
  if (ip == 0) {
    ip = t->arch->frameIp(stack);
  }

  MyThread::CallTrace* trace = t->trace;
  object targetMethod = (trace ? trace->targetMethod : 0);

  while (stack) {
    if (targetMethod) {
      visitArguments(t, v, stack, targetMethod);
      targetMethod = 0;
    }

    object method = methodForIp(t, ip);
    if (method) {
      PROTECT(t, method);

      t->arch->nextFrame(&stack, &base);

      visitStackAndLocals(t, v, stack, method, ip);

      ip = t->arch->frameIp(stack);
    } else if (trace) {
      stack = trace->stack;
      base = trace->base;
      ip = t->arch->frameIp(stack);
      trace = trace->next;

      if (trace) {
        targetMethod = trace->targetMethod;
      }
    } else {
      break;
    }
  }
}

void
walkContinuationBody(MyThread* t, Heap::Walker* w, object c, int start)
{
  const int BodyOffset = ContinuationBody / BytesPerWord;

  object method = static_cast<object>
    (t->m->heap->follow(continuationMethod(t, c)));
  int count = frameMapSizeInBits(t, method);

  if (count) {
    int stack = BodyOffset
      + (continuationFramePointerOffset(t, c) / BytesPerWord)
      - t->arch->framePointerOffset()
      - stackOffsetFromFrame(t, method);

    int first = stack + localOffsetFromStack(t, count - 1, method);
    if (start > first) {
      count -= start - first;
    }

    int32_t* map;
    unsigned offset;
    findFrameMap
      (t, reinterpret_cast<uintptr_t*>(c) + stack, method, difference
       (continuationAddress(t, c),
        reinterpret_cast<void*>(methodAddress(t, method))), &map, &offset);

    for (int i = count - 1; i >= 0; --i) {
      int j = offset + i;
      if (map[j / 32] & (static_cast<int32_t>(1) << (j % 32))) {
        if (not w->visit(stack + localOffsetFromStack(t, i, method))) {
          return;
        }
      }
    }
  }
}

void
callContinuation(MyThread* t, object continuation, object result,
                 object exception, void* ip, void* base, void* stack)
{
  assert(t, t->exception == 0);

  t->continuation = continuation;

  if (exception) {
    t->exception = exception;

    t->ip = ip;
    t->base = base;
    t->stack = stack;

    findUnwindTarget(t, &ip, &base, &stack);

    t->ip = 0;
  }

  t->trace->nativeMethod = 0;
  t->trace->targetMethod = 0;

  vmJump(ip, base, stack, t, reinterpret_cast<uintptr_t>(result), 0);
}

int8_t*
returnSpec(MyThread* t, object method)
{
  int8_t* s = &byteArrayBody(t, methodSpec(t, method), 0);
  while (*s and *s != ')') ++ s;
  expect(t, *s == ')');
  return s + 1;
}

object
returnClass(MyThread* t, object method)
{
  PROTECT(t, method);

  int8_t* spec = returnSpec(t, method);
  unsigned length = strlen(reinterpret_cast<char*>(spec));
  object name;
  if (*spec == '[') {
    name = makeByteArray(t, length + 1);
    memcpy(&byteArrayBody(t, name, 0), spec, length);
  } else {
    assert(t, *spec == 'L');
    assert(t, spec[length - 1] == ';');
    name = makeByteArray(t, length - 1);
    memcpy(&byteArrayBody(t, name, 0), spec + 1, length - 2);
  }

  return resolveClass(t, classLoader(t, methodClass(t, method)), name);
}

bool
compatibleReturnType(MyThread* t, object oldMethod, object newMethod)
{
  if (oldMethod == newMethod) {
    return true;
  } else if (methodReturnCode(t, oldMethod) == methodReturnCode(t, newMethod))
  {
    if (methodReturnCode(t, oldMethod) == ObjectField) {
      PROTECT(t, newMethod);

      object oldClass = returnClass(t, oldMethod);
      PROTECT(t, oldClass);

      object newClass = returnClass(t, newMethod);

      return isAssignableFrom(t, oldClass, newClass);
    } else {
      return true;
    }
  } else {
    return methodReturnCode(t, oldMethod) == VoidField;
  }
}

void
jumpAndInvoke(MyThread* t, object method, void* base, void* stack, ...)
{
  t->trace->targetMethod = 0;

  if (methodFlags(t, method) & ACC_NATIVE) {
    t->trace->nativeMethod = method;
  } else {
    t->trace->nativeMethod = 0;
  }

  unsigned argumentCount = methodParameterFootprint(t, method);
  uintptr_t arguments[argumentCount];
  va_list a; va_start(a, stack);
  for (unsigned i = 0; i < argumentCount; ++i) {
    arguments[i] = va_arg(a, uintptr_t);
  }
  va_end(a);
  
  vmJumpAndInvoke
    (t, reinterpret_cast<void*>(methodAddress(t, method)),
     base,
     stack,
     argumentCount * BytesPerWord,
     arguments,
     (t->arch->alignFrameSize(t->arch->argumentFootprint(argumentCount))
      + t->arch->frameReturnAddressSize())
     * BytesPerWord);
}

void
callContinuation(MyThread* t, object continuation, object result,
                 object exception)
{
  enum {
    Call,
    Unwind,
    Rewind,
    Throw
  } action;

  object nextContinuation = 0;

  if (t->continuation == 0
      or continuationContext(t, t->continuation)
      != continuationContext(t, continuation))
  {
    PROTECT(t, continuation);
    PROTECT(t, result);
    PROTECT(t, exception);

    if (compatibleReturnType
        (t, t->trace->originalMethod, continuationContextMethod
         (t, continuationContext(t, continuation))))
    {
      object oldContext;
      object unwindContext;

      if (t->continuation) {
        oldContext = continuationContext(t, t->continuation);
        unwindContext = oldContext;
      } else {
        oldContext = 0;
        unwindContext = 0;
      }

      object rewindContext = 0;

      for (object newContext = continuationContext(t, continuation);
           newContext; newContext = continuationContextNext(t, newContext))
      {
        if (newContext == oldContext) {
          unwindContext = 0;
          break;
        } else {
          rewindContext = newContext;
        }
      }

      if (unwindContext
          and continuationContextContinuation(t, unwindContext))
      {
        nextContinuation = continuationContextContinuation(t, unwindContext);
        result = makeUnwindResult(t, continuation, result, exception);
        action = Unwind;
      } else if (rewindContext
                 and continuationContextContinuation(t, rewindContext))
      {
        nextContinuation = continuationContextContinuation(t, rewindContext);
        action = Rewind;

        if (rewindMethod(t) == 0) {
          PROTECT(t, nextContinuation);
            
          object method = resolveMethod
            (t, t->m->loader, "avian/Continuations", "rewind",
             "(Ljava/lang/Runnable;Lavian/Callback;Ljava/lang/Object;"
             "Ljava/lang/Throwable;)V");
            
          if (method) {
            rewindMethod(t) = method;
            
            compile(t, ::codeAllocator(t), 0, method);

            if (UNLIKELY(t->exception)) {
              action = Throw;
            }
          } else {
            action = Throw;
          }
        }
      } else {
        action = Call;
      }
    } else {
      t->exception = makeIncompatibleContinuationException(t);
      action = Throw;
    }
  } else {
    action = Call;
  }

  void* ip;
  void* base;
  void* stack;
  findUnwindTarget(t, &ip, &base, &stack);

  switch (action) {
  case Call: {
    callContinuation(t, continuation, result, exception, ip, base, stack);
  } break;

  case Unwind: {
    callContinuation(t, nextContinuation, result, 0, ip, base, stack);
  } break;

  case Rewind: {
    t->continuation = nextContinuation;

    jumpAndInvoke
      (t, rewindMethod(t), base, stack,
       continuationContextBefore(t, continuationContext(t, nextContinuation)),
       continuation, result, exception);
  } break;

  case Throw: {
    vmJump(ip, base, stack, t, 0, 0);    
  } break;

  default:
    abort(t);
  }
}

void
callWithCurrentContinuation(MyThread* t, object receiver)
{
  object method = 0;
  void* ip = 0;
  void* base = 0;
  void* stack = 0;

  { PROTECT(t, receiver);

    if (receiveMethod(t) == 0) {
      object m = resolveMethod
        (t, t->m->loader, "avian/CallbackReceiver", "receive",
         "(Lavian/Callback;)Ljava/lang/Object;");

      if (m) {
        receiveMethod(t) = m;

        object continuationClass = arrayBody
          (t, t->m->types, Machine::ContinuationType);
        
        if (classVmFlags(t, continuationClass) & BootstrapFlag) {
          resolveSystemClass(t, vm::className(t, continuationClass));
        }
      }
    }

    if (LIKELY(t->exception == 0)) {
      method = findInterfaceMethod
        (t, receiveMethod(t), objectClass(t, receiver));
      PROTECT(t, method);
        
      compile(t, ::codeAllocator(t), 0, method);

      if (LIKELY(t->exception == 0)) {
        t->continuation = makeCurrentContinuation(t, &ip, &base, &stack);
      }
    }
  }

  if (LIKELY(t->exception == 0)) {
    jumpAndInvoke(t, method, base, stack, receiver, t->continuation);
  } else {
    unwind(t);
  }
}

void
dynamicWind(MyThread* t, object before, object thunk, object after)
{
  void* ip = 0;
  void* base = 0;
  void* stack = 0;

  { PROTECT(t, before);
    PROTECT(t, thunk);
    PROTECT(t, after);

    if (windMethod(t) == 0) {
      object method = resolveMethod
        (t, t->m->loader, "avian/Continuations", "wind",
         "(Ljava/lang/Runnable;Ljava/util/concurrent/Callable;"
        "Ljava/lang/Runnable;)Lavian/Continuations$UnwindResult;");

      if (method) {
        windMethod(t) = method;
        compile(t, ::codeAllocator(t), 0, method);
      }
    }

    if (LIKELY(t->exception == 0)) {
      t->continuation = makeCurrentContinuation(t, &ip, &base, &stack);

      object newContext = makeContinuationContext
        (t, continuationContext(t, t->continuation), before, after,
         t->continuation, t->trace->originalMethod);

      set(t, t->continuation, ContinuationContext, newContext);
    }
  }

  if (LIKELY(t->exception == 0)) {
    jumpAndInvoke(t, windMethod(t), base, stack, before, thunk, after);
  } else {
    unwind(t);
  }    
}

class ArgumentList {
 public:
  ArgumentList(Thread* t, uintptr_t* array, unsigned size, bool* objectMask,
               object this_, const char* spec, bool indirectObjects,
               va_list arguments):
    t(static_cast<MyThread*>(t)),
    array(array),
    objectMask(objectMask),
    size(size),
    position(0),
    protector(this)
  {
    if (this_) {
      addObject(this_);
    }

    for (MethodSpecIterator it(t, spec); it.hasNext();) {
      switch (*it.next()) {
      case 'L':
      case '[':
        if (indirectObjects) {
          object* v = va_arg(arguments, object*);
          addObject(v ? *v : 0);
        } else {
          addObject(va_arg(arguments, object));
        }
        break;
      
      case 'J':
      case 'D':
        addLong(va_arg(arguments, uint64_t));
        break;

      default:
        addInt(va_arg(arguments, uint32_t));
        break;        
      }
    }
  }

  ArgumentList(Thread* t, uintptr_t* array, unsigned size, bool* objectMask,
               object this_, const char* spec, object arguments):
    t(static_cast<MyThread*>(t)),
    array(array),
    objectMask(objectMask),
    size(size),
    position(0),
    protector(this)
  {
    if (this_) {
      addObject(this_);
    }

    unsigned index = 0;
    for (MethodSpecIterator it(t, spec); it.hasNext();) {
      switch (*it.next()) {
      case 'L':
      case '[':
        addObject(objectArrayBody(t, arguments, index++));
        break;
      
      case 'J':
      case 'D':
        addLong(cast<int64_t>(objectArrayBody(t, arguments, index++),
                              BytesPerWord));
        break;

      default:
        addInt(cast<int32_t>(objectArrayBody(t, arguments, index++),
                             BytesPerWord));
        break;        
      }
    }
  }

  void addObject(object v) {
    assert(t, position < size);

    array[position] = reinterpret_cast<uintptr_t>(v);
    objectMask[position] = true;
    ++ position;
  }

  void addInt(uintptr_t v) {
    assert(t, position < size);

    array[position] = v;
    objectMask[position] = false;
    ++ position;
  }

  void addLong(uint64_t v) {
    assert(t, position < size - 1);

    memcpy(array + position, &v, 8);

    objectMask[position] = false;
    objectMask[position + 1] = false;

    position += 2;
  }

  MyThread* t;
  uintptr_t* array;
  bool* objectMask;
  unsigned size;
  unsigned position;

  class MyProtector: public Thread::Protector {
   public:
    MyProtector(ArgumentList* list): Protector(list->t), list(list) { }

    virtual void visit(Heap::Visitor* v) {
      for (unsigned i = 0; i < list->position; ++i) {
        if (list->objectMask[i]) {
          v->visit(reinterpret_cast<object*>(list->array + i));
        }
      }
    }

    ArgumentList* list;
  } protector;
};

object
invoke(Thread* thread, object method, ArgumentList* arguments)
{
  MyThread* t = static_cast<MyThread*>(thread);
  
  unsigned returnCode = methodReturnCode(t, method);
  unsigned returnType = fieldType(t, returnCode);

  uint64_t result;

  { MyThread::CallTrace trace(t, method);

    assert(t, arguments->position == arguments->size);

    result = vmInvoke
      (t, reinterpret_cast<void*>(methodAddress(t, method)),
       arguments->array,
       arguments->position * BytesPerWord,
       t->arch->alignFrameSize
       (t->arch->argumentFootprint(arguments->position))
       * BytesPerWord,
       returnType);
  }

  if (t->exception) { 
    if (t->backupHeap) {
      collect(t, Heap::MinorCollection);
    }
    return 0;
  }

  object r;
  switch (returnCode) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    r = makeInt(t, result);
    break;

  case LongField:
  case DoubleField:
    r = makeLong(t, result);
    break;

  case ObjectField:
    r = reinterpret_cast<object>(result);
    break;

  case VoidField:
    r = 0;
    break;

  default:
    abort(t);
  };

  return r;
}

class SegFaultHandler: public System::SignalHandler {
 public:
  SegFaultHandler(): m(0) { }

  virtual bool handleSignal(void** ip, void** base, void** stack,
                            void** thread)
  {
    MyThread* t = static_cast<MyThread*>(m->localThread->get());
    if (t->state == Thread::ActiveState) {
      object node = methodForIp(t, *ip);
      if (node) {
        void* oldIp = t->ip;
        void* oldBase = t->base;
        void* oldStack = t->stack;

        t->ip = *ip;
        t->base = *base;
        t->stack = static_cast<void**>(*stack)
          - t->arch->frameReturnAddressSize();

        ensure(t, FixedSizeOfNullPointerException + traceSize(t));

        t->tracing = true;
        t->exception = makeNullPointerException(t);
        t->tracing = false;

        findUnwindTarget(t, ip, base, stack);

        t->ip = oldIp;
        t->base = oldBase;
        t->stack = oldStack;

        *thread = t;
        return true;
      }
    }

    if (compileLog) {
      fflush(compileLog);
    }

    return false;
  }

  Machine* m;
};

void
boot(MyThread* t, BootImage* image);

class MyProcessor;

void
compileThunks(MyThread* t, Allocator* allocator, MyProcessor* p);

MyProcessor*
processor(MyThread* t);

class MyProcessor: public Processor {
 public:
  MyProcessor(System* s, Allocator* allocator):
    s(s),
    allocator(allocator),
    defaultThunk(0),
    defaultVirtualThunk(0),
    nativeThunk(0),
    aioobThunk(0),
    callTable(0),
    callTableSize(0),
    methodTree(0),
    methodTreeSentinal(0),
    objectPools(0),
    staticTableArray(0),
    virtualThunks(0),
    receiveMethod(0),
    windMethod(0),
    rewindMethod(0),
    codeAllocator(s, 0, 0),
    bootImage(0)
  { }

  virtual Thread*
  makeThread(Machine* m, object javaThread, Thread* parent)
  {
    MyThread* t = new (m->heap->allocate(sizeof(MyThread)))
      MyThread(m, javaThread, static_cast<MyThread*>(parent));
    t->init();

    if (false) {
      fprintf(stderr, "%d\n", difference(&(t->continuation), t));
      fprintf(stderr, "%d\n", difference(&(t->exception), t));
      fprintf(stderr, "%d\n", difference(&(t->exceptionStackAdjustment), t));
      fprintf(stderr, "%d\n", difference(&(t->exceptionOffset), t));
      fprintf(stderr, "%d\n", difference(&(t->exceptionHandler), t));
      exit(0);
    }

    return t;
  }

  virtual object
  makeMethod(vm::Thread* t,
             uint8_t vmFlags,
             uint8_t returnCode,
             uint8_t parameterCount,
             uint8_t parameterFootprint,
             uint16_t flags,
             uint16_t offset,
             object name,
             object spec,
             object class_,
             object code)
  {
    return vm::makeMethod
      (t, vmFlags, returnCode, parameterCount, parameterFootprint, flags,
       offset, 0, name, spec, class_, code,
       ::defaultThunk(static_cast<MyThread*>(t)));
  }

  virtual object
  makeClass(vm::Thread* t,
            uint16_t flags,
            uint8_t vmFlags,
            uint8_t arrayDimensions,
            uint16_t fixedSize,
            uint16_t arrayElementSize,
            object objectMask,
            object name,
            object super,
            object interfaceTable,
            object virtualTable,
            object fieldTable,
            object methodTable,
            object staticTable,
            object loader,
            unsigned vtableLength)
  {
    return vm::makeClass
      (t, flags, vmFlags, arrayDimensions, fixedSize, arrayElementSize,
       objectMask, name, super, interfaceTable, virtualTable, fieldTable,
       methodTable, staticTable, loader, vtableLength);
  }

  virtual void
  initVtable(Thread* t, object c)
  {
    PROTECT(t, c);
    for (int i = classLength(t, c) - 1; i >= 0; --i) {
      void* thunk = reinterpret_cast<void*>
        (virtualThunk(static_cast<MyThread*>(t), i));
      classVtable(t, c, i) = thunk;
    }
  }

  virtual void
  visitObjects(Thread* vmt, Heap::Visitor* v)
  {
    MyThread* t = static_cast<MyThread*>(vmt);

    if (t == t->m->rootThread) {
      v->visit(&callTable);
      v->visit(&methodTree);
      v->visit(&methodTreeSentinal);
      v->visit(&objectPools);
      v->visit(&staticTableArray);
      v->visit(&virtualThunks);
      v->visit(&receiveMethod);
      v->visit(&windMethod);
      v->visit(&rewindMethod);
    }

    for (MyThread::CallTrace* trace = t->trace; trace; trace = trace->next) {
      v->visit(&(trace->continuation));
      v->visit(&(trace->nativeMethod));
      v->visit(&(trace->targetMethod));
      v->visit(&(trace->originalMethod));
    }

    v->visit(&(t->continuation));

    for (Reference* r = t->reference; r; r = r->next) {
      v->visit(&(r->target));
    }

    visitStack(t, v);
  }

  virtual void
  walkStack(Thread* vmt, StackVisitor* v)
  {
    MyThread* t = static_cast<MyThread*>(vmt);

    MyStackWalker walker(t);
    walker.walk(v);
  }

  virtual int
  lineNumber(Thread* vmt, object method, int ip)
  {
    return findLineNumber(static_cast<MyThread*>(vmt), method, ip);
  }

  virtual object*
  makeLocalReference(Thread* vmt, object o)
  {
    if (o) {
      MyThread* t = static_cast<MyThread*>(vmt);
      PROTECT(t, o);

      Reference* r = new (t->m->heap->allocate(sizeof(Reference)))
        Reference(o, &(t->reference));

      return &(r->target);
    } else {
      return 0;
    }
  }

  virtual void
  disposeLocalReference(Thread* t, object* r)
  {
    if (r) {
      vm::dispose(t, reinterpret_cast<Reference*>(r));
    }
  }

  virtual object
  invokeArray(Thread* t, object method, object this_, object arguments)
  {
    if (UNLIKELY(t->exception)) return 0;

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));

    unsigned size = methodParameterFootprint(t, method);
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list(t, array, size, objectMask, this_, spec, arguments);
    
    PROTECT(t, method);

    compile(static_cast<MyThread*>(t),
            ::codeAllocator(static_cast<MyThread*>(t)), 0, method);

    if (LIKELY(t->exception == 0)) {
      return ::invoke(t, method, &list);
    }

    return 0;
  }

  virtual object
  invokeList(Thread* t, object method, object this_, bool indirectObjects,
             va_list arguments)
  {
    if (UNLIKELY(t->exception)) return 0;

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));
    
    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));

    unsigned size = methodParameterFootprint(t, method);
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list
      (t, array, size, objectMask, this_, spec, indirectObjects, arguments);

    PROTECT(t, method);

    if (false) {
      compile(static_cast<MyThread*>(t),
              ::codeAllocator(static_cast<MyThread*>(t)), 0,
              resolveMethod(t, t->m->loader,
                            "java/beans/PropertyChangeSupport",
                            "firePropertyChange",
                            "(Ljava/beans/PropertyChangeEvent;)V"));
      trap();
    }

    compile(static_cast<MyThread*>(t),
            ::codeAllocator(static_cast<MyThread*>(t)), 0, method);

    if (LIKELY(t->exception == 0)) {
      return ::invoke(t, method, &list);
    }

    return 0;
  }

  virtual object
  invokeList(Thread* t, object loader, const char* className,
             const char* methodName, const char* methodSpec,
             object this_, va_list arguments)
  {
    if (UNLIKELY(t->exception)) return 0;

    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    unsigned size = parameterFootprint(t, methodSpec, false);
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list
      (t, array, size, objectMask, this_, methodSpec, false, arguments);

    object method = resolveMethod
      (t, loader, className, methodName, methodSpec);
    if (LIKELY(t->exception == 0)) {
      assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

      PROTECT(t, method);
      
      compile(static_cast<MyThread*>(t), 
              ::codeAllocator(static_cast<MyThread*>(t)), 0, method);

      if (LIKELY(t->exception == 0)) {
        return ::invoke(t, method, &list);
      }
    }

    return 0;
  }

  virtual void dispose(Thread* vmt) {
    MyThread* t = static_cast<MyThread*>(vmt);

    while (t->reference) {
      vm::dispose(t, t->reference);
    }

    t->arch->release();

    t->m->heap->free(t, sizeof(*t));
  }

  virtual void dispose() {
    if (codeAllocator.base) {
      s->freeExecutable(codeAllocator.base, codeAllocator.capacity);
    }

    s->handleSegFault(0);

    allocator->free(this, sizeof(*this));
  }

  virtual object getStackTrace(Thread* vmt, Thread* vmTarget) {
    MyThread* t = static_cast<MyThread*>(vmt);
    MyThread* target = static_cast<MyThread*>(vmTarget);
    MyProcessor* p = this;

    class Visitor: public System::ThreadVisitor {
     public:
      Visitor(MyThread* t, MyProcessor* p, MyThread* target):
        t(t), p(p), target(target)
      { }

      virtual void visit(void* ip, void* base, void* stack) {
        void* oldIp = target->ip;
        void* oldBase = target->base;
        void* oldStack = target->stack;

        if (methodForIp(t, ip)) {
          target->ip = ip;
          target->base = base;
          target->stack = stack;
        } else {
          uint8_t* thunkStart = p->thunkTable;
          uint8_t* thunkEnd = thunkStart + (p->thunkSize * ThunkCount);

          if (static_cast<uint8_t*>(ip) >= thunkStart
              and static_cast<uint8_t*>(ip) < thunkEnd)
          {
            target->ip = t->arch->frameIp(stack);
            target->base = base;
            target->stack = stack;            
          }
        }

        ensure(t, traceSize(target));

        t->tracing = true;
        trace = makeTrace(t, target);
        t->tracing = false;

        target->ip = oldIp;
        target->base = oldBase;
        target->stack = oldStack;
      }

      MyThread* t;
      MyProcessor* p;
      MyThread* target;
      object trace;
    } visitor(t, p, target);

    t->m->system->visit(t->systemThread, target->systemThread, &visitor);

    if (t->backupHeap) {
      PROTECT(t, visitor.trace);

      collect(t, Heap::MinorCollection);
    }

    return visitor.trace ? visitor.trace : makeArray(t, 0);
  }

  virtual void initialize(BootImage* image, uint8_t* code, unsigned capacity) {
    bootImage = image;
    codeAllocator.base = code;
    codeAllocator.capacity = capacity;
  }

  virtual void compileMethod(Thread* vmt, Zone* zone, object* constants,
                             object* calls, DelayedPromise** addresses,
                             object method)
  {
    MyThread* t = static_cast<MyThread*>(vmt);
    BootContext bootContext(t, *constants, *calls, *addresses, zone);

    compile(t, &codeAllocator, &bootContext, method);

    *constants = bootContext.constants;
    *calls = bootContext.calls;
    *addresses = bootContext.addresses;
  }

  virtual void visitRoots(HeapWalker* w) {
    bootImage->methodTree = w->visitRoot(methodTree);
    bootImage->methodTreeSentinal = w->visitRoot(methodTreeSentinal);
    bootImage->virtualThunks = w->visitRoot(virtualThunks);
  }

  virtual unsigned* makeCallTable(Thread* t, HeapWalker* w) {
    bootImage->codeSize = codeAllocator.offset;
    bootImage->callCount = callTableSize;

    unsigned* table = static_cast<unsigned*>
      (t->m->heap->allocate(callTableSize * sizeof(unsigned) * 2));

    unsigned index = 0;
    for (unsigned i = 0; i < arrayLength(t, callTable); ++i) {
      for (object p = arrayBody(t, callTable, i); p; p = callNodeNext(t, p)) {
        table[index++] = callNodeAddress(t, p)
          - reinterpret_cast<uintptr_t>(codeAllocator.base);
        table[index++] = w->map()->find(callNodeTarget(t, p))
          | (static_cast<unsigned>(callNodeFlags(t, p)) << BootShift);
      }
    }

    return table;
  }

  virtual void boot(Thread* t, BootImage* image) {
    if (codeAllocator.base == 0) {
      codeAllocator.base = static_cast<uint8_t*>
        (s->tryAllocateExecutable(ExecutableAreaSizeInBytes));
      codeAllocator.capacity = ExecutableAreaSizeInBytes;
    }

    if (image) {
      ::boot(static_cast<MyThread*>(t), image);
    } else {
      callTable = makeArray(t, 128);

      methodTree = methodTreeSentinal = makeTreeNode(t, 0, 0, 0);
      set(t, methodTree, TreeNodeLeft, methodTreeSentinal);
      set(t, methodTree, TreeNodeRight, methodTreeSentinal);

      ::compileThunks(static_cast<MyThread*>(t), &codeAllocator, this);
    }

    segFaultHandler.m = t->m;
    expect(t, t->m->system->success
           (t->m->system->handleSegFault(&segFaultHandler)));
  }

  virtual void callWithCurrentContinuation(Thread* t, object receiver) {
    if (Continuations) {
      ::callWithCurrentContinuation(static_cast<MyThread*>(t), receiver);
    } else {
      abort(t);
    }
  }

  virtual void dynamicWind(Thread* t, object before, object thunk,
                           object after)
  {
    if (Continuations) {
      ::dynamicWind(static_cast<MyThread*>(t), before, thunk, after);
    } else {
      abort(t);
    }
  }

  virtual void feedResultToContinuation(Thread* t, object continuation,
                                        object result)
  {
    if (Continuations) {
      callContinuation(static_cast<MyThread*>(t), continuation, result, 0);
    } else {
      abort(t);
    }
  }

  virtual void feedExceptionToContinuation(Thread* t, object continuation,
                                           object exception)
  {
    if (Continuations) {
      callContinuation(static_cast<MyThread*>(t), continuation, 0, exception);
    } else {
      abort(t);
    }
  }

  virtual void walkContinuationBody(Thread* t, Heap::Walker* w, object o,
                                    unsigned start)
  {
    if (Continuations) {
      ::walkContinuationBody(static_cast<MyThread*>(t), w, o, start);
    } else {
      abort(t);
    }
  }
  
  System* s;
  Allocator* allocator;
  uint8_t* defaultThunk;
  uint8_t* defaultVirtualThunk;
  uint8_t* nativeThunk;
  uint8_t* aioobThunk;
  uint8_t* thunkTable;
  unsigned thunkSize;
  object callTable;
  unsigned callTableSize;
  object methodTree;
  object methodTreeSentinal;
  object objectPools;
  object staticTableArray;
  object virtualThunks;
  object receiveMethod;
  object windMethod;
  object rewindMethod;
  SegFaultHandler segFaultHandler;
  FixedAllocator codeAllocator;
  BootImage* bootImage;
};

object
findCallNode(MyThread* t, void* address)
{
  if (DebugCallTable) {
    fprintf(stderr, "find call node %p\n", address);
  }

  // we must use a version of the call table at least as recent as the
  // compiled form of the method containing the specified address (see
  // compile(MyThread*, Allocator*, BootContext*, object)):
  memoryBarrier();

  MyProcessor* p = processor(t);
  object table = p->callTable;

  intptr_t key = reinterpret_cast<intptr_t>(address);
  unsigned index = static_cast<uintptr_t>(key) 
    & (arrayLength(t, table) - 1);

  for (object n = arrayBody(t, table, index);
       n; n = callNodeNext(t, n))
  {
    intptr_t k = callNodeAddress(t, n);

    if (k == key) {
      return n;
    }
  }

  return 0;
}

object
resizeTable(MyThread* t, object oldTable, unsigned newLength)
{
  PROTECT(t, oldTable);

  object oldNode = 0;
  PROTECT(t, oldNode);

  object newTable = makeArray(t, newLength);
  PROTECT(t, newTable);

  for (unsigned i = 0; i < arrayLength(t, oldTable); ++i) {
    for (oldNode = arrayBody(t, oldTable, i);
         oldNode;
         oldNode = callNodeNext(t, oldNode))
    {
      intptr_t k = callNodeAddress(t, oldNode);

      unsigned index = k & (newLength - 1);

      object newNode = makeCallNode
        (t, callNodeAddress(t, oldNode),
         callNodeTarget(t, oldNode),
         callNodeFlags(t, oldNode),
         arrayBody(t, newTable, index));

      set(t, newTable, ArrayBody + (index * BytesPerWord), newNode);
    }
  }

  return newTable;
}

object
insertCallNode(MyThread* t, object table, unsigned* size, object node)
{
  if (DebugCallTable) {
    fprintf(stderr, "insert call node %p\n",
            reinterpret_cast<void*>(callNodeAddress(t, node)));
  }

  PROTECT(t, table);
  PROTECT(t, node);

  ++ (*size);

  if (*size >= arrayLength(t, table) * 2) { 
    table = resizeTable(t, table, arrayLength(t, table) * 2);
  }

  intptr_t key = callNodeAddress(t, node);
  unsigned index = static_cast<uintptr_t>(key) & (arrayLength(t, table) - 1);

  set(t, node, CallNodeNext, arrayBody(t, table, index));
  set(t, table, ArrayBody + (index * BytesPerWord), node);

  return table;
}

void
insertCallNode(MyThread* t, object node)
{
  MyProcessor* p = processor(t);
  p->callTable = insertCallNode(t, p->callTable, &(p->callTableSize), node);
}

object
makeClassMap(Thread* t, unsigned* table, unsigned count, uintptr_t* heap)
{
  object array = makeArray(t, nextPowerOfTwo(count));
  object map = makeHashMap(t, 0, array);
  PROTECT(t, map);
  
  for (unsigned i = 0; i < count; ++i) {
    object c = bootObject(heap, table[i]);
    hashMapInsert(t, map, className(t, c), c, byteArrayHash);
  }

  return map;
}

object
makeStaticTableArray(Thread* t, unsigned* table, unsigned count,
                     uintptr_t* heap)
{
  object array = makeArray(t, count);
  
  for (unsigned i = 0; i < count; ++i) {
    set(t, array, ArrayBody + (i * BytesPerWord),
        classStaticTable(t, bootObject(heap, table[i])));
  }

  return array;
}

object
makeStringMap(Thread* t, unsigned* table, unsigned count, uintptr_t* heap)
{
  object array = makeArray(t, nextPowerOfTwo(count));
  object map = makeWeakHashMap(t, 0, array);
  PROTECT(t, map);
  
  for (unsigned i = 0; i < count; ++i) {
    object s = bootObject(heap, table[i]);
    hashMapInsert(t, map, s, 0, stringHash);
  }

  return map;
}

object
makeCallTable(MyThread* t, uintptr_t* heap, unsigned* calls, unsigned count,
              uintptr_t base)
{
  object table = makeArray(t, nextPowerOfTwo(count));
  PROTECT(t, table);

  unsigned size = 0;
  for (unsigned i = 0; i < count; ++i) {
    unsigned address = calls[i * 2];
    unsigned target = calls[(i * 2) + 1];

    object node = makeCallNode
       (t, base + address, bootObject(heap, target & BootMask),
        target >> BootShift, 0);

    table = insertCallNode(t, table, &size, node);
  }

  return table;
}

void
fixupHeap(MyThread* t UNUSED, uintptr_t* map, unsigned size, uintptr_t* heap)
{
  for (unsigned word = 0; word < size; ++word) {
    uintptr_t w = map[word];
    if (w) {
      for (unsigned bit = 0; bit < BitsPerWord; ++bit) {
        if (w & (static_cast<uintptr_t>(1) << bit)) {
          unsigned index = indexOf(word, bit);
          uintptr_t* p = heap + index;
          assert(t, *p);
          
          uintptr_t number = *p & BootMask;
          uintptr_t mark = *p >> BootShift;

          if (number) {
            *p = reinterpret_cast<uintptr_t>(heap + (number - 1)) | mark;
          } else {
            *p = mark;
          }
        }
      }
    }
  }
}

void
fixupCode(Thread* t, uintptr_t* map, unsigned size, uint8_t* code,
          uintptr_t* heap)
{
  Assembler::Architecture* arch = makeArchitecture(t->m->system);
  arch->acquire();

  for (unsigned word = 0; word < size; ++word) {
    uintptr_t w = map[word];
    if (w) {
      for (unsigned bit = 0; bit < BitsPerWord; ++bit) {
        if (w & (static_cast<uintptr_t>(1) << bit)) {
          unsigned index = indexOf(word, bit);
          uintptr_t oldValue; memcpy(&oldValue, code + index, BytesPerWord);
          uintptr_t newValue;
          if (oldValue & BootHeapOffset) {
            newValue = reinterpret_cast<uintptr_t>
              (heap + (oldValue & BootMask) - 1);
          } else {
            newValue = reinterpret_cast<uintptr_t>
              (code + (oldValue & BootMask));
          }
          if (oldValue & BootFlatConstant) {
            memcpy(code + index, &newValue, BytesPerWord);
          } else {
            arch->setConstant(code + index, newValue);
          }
        }
      }
    }
  }

  arch->release();
}

void
fixupMethods(Thread* t, BootImage* image, uint8_t* code)
{
  for (HashMapIterator it(t, classLoaderMap(t, t->m->loader)); it.hasMore();) {
    object c = tripleSecond(t, it.next());

    if (classMethodTable(t, c)) {
      for (unsigned i = 0; i < arrayLength(t, classMethodTable(t, c)); ++i) {
        object method = arrayBody(t, classMethodTable(t, c), i);
        if (methodCode(t, method) or (methodFlags(t, method) & ACC_NATIVE)) {
          assert(t, (methodCompiled(t, method) - image->codeBase)
                 <= image->codeSize);

          methodCompiled(t, method)
            = (methodCompiled(t, method) - image->codeBase)
            + reinterpret_cast<uintptr_t>(code);

          if (DebugCompile and (methodFlags(t, method) & ACC_NATIVE) == 0) {
            logCompile
              (static_cast<MyThread*>(t),
               reinterpret_cast<uint8_t*>(methodCompiled(t, method)),
               reinterpret_cast<uintptr_t*>
               (methodCompiled(t, method))[-1],
               reinterpret_cast<char*>
               (&byteArrayBody(t, className(t, methodClass(t, method)), 0)),
               reinterpret_cast<char*>
               (&byteArrayBody(t, methodName(t, method), 0)),
               reinterpret_cast<char*>
               (&byteArrayBody(t, methodSpec(t, method), 0)));
          }
        }
      }
    }

    t->m->processor->initVtable(t, c);
  }
}

void
fixupThunks(MyThread* t, BootImage* image, uint8_t* code)
{
  MyProcessor* p = processor(t);
  
  p->defaultThunk = code + image->defaultThunk;

  updateCall(t, LongCall, false, code + image->compileMethodCall,
             voidPointer(::compileMethod));

  p->defaultVirtualThunk = code + image->defaultVirtualThunk;

  updateCall(t, LongCall, false, code + image->compileVirtualMethodCall,
             voidPointer(::compileVirtualMethod));

  p->nativeThunk = code + image->nativeThunk;

  updateCall(t, LongCall, false, code + image->invokeNativeCall,
             voidPointer(invokeNative));

  p->aioobThunk = code + image->aioobThunk;

  updateCall(t, LongCall, false,
             code + image->throwArrayIndexOutOfBoundsCall,
             voidPointer(throwArrayIndexOutOfBounds));

  p->thunkTable = code + image->thunkTable;
  p->thunkSize = image->thunkSize;

#define THUNK(s)                                                        \
  updateCall(t, LongJump, false, code + image->s##Call, voidPointer(s));

#include "thunks.cpp"

#undef THUNK
}

void
fixupVirtualThunks(MyThread* t, BootImage* image, uint8_t* code)
{
  MyProcessor* p = processor(t);
  
  for (unsigned i = 0; i < wordArrayLength(t, p->virtualThunks); ++i) {
    if (wordArrayBody(t, p->virtualThunks, i)) {
      wordArrayBody(t, p->virtualThunks, i)
        = (wordArrayBody(t, p->virtualThunks, i) - image->codeBase)
        + reinterpret_cast<uintptr_t>(code);
    }
  }
}

void
boot(MyThread* t, BootImage* image)
{
  assert(t, image->magic == BootImage::Magic);

  unsigned* classTable = reinterpret_cast<unsigned*>(image + 1);
  unsigned* stringTable = classTable + image->classCount;
  unsigned* callTable = stringTable + image->stringCount;

  uintptr_t* heapMap = reinterpret_cast<uintptr_t*>
    (pad(reinterpret_cast<uintptr_t>(callTable + (image->callCount * 2))));
  unsigned heapMapSizeInWords = ceiling
    (heapMapSize(image->heapSize), BytesPerWord);
  uintptr_t* heap = heapMap + heapMapSizeInWords;

//   fprintf(stderr, "heap from %p to %p\n",
//           heap, heap + ceiling(image->heapSize, BytesPerWord));

  uintptr_t* codeMap = heap + ceiling(image->heapSize, BytesPerWord);
  unsigned codeMapSizeInWords = ceiling
    (codeMapSize(image->codeSize), BytesPerWord);
  uint8_t* code = reinterpret_cast<uint8_t*>(codeMap + codeMapSizeInWords);

//   fprintf(stderr, "code from %p to %p\n",
//           code, code + image->codeSize);
 
  fixupHeap(t, heapMap, heapMapSizeInWords, heap);
  
  t->m->heap->setImmortalHeap(heap, image->heapSize / BytesPerWord);

  t->m->loader = bootObject(heap, image->loader);
  t->m->types = bootObject(heap, image->types);

  MyProcessor* p = static_cast<MyProcessor*>(t->m->processor);
  
  p->methodTree = bootObject(heap, image->methodTree);
  p->methodTreeSentinal = bootObject(heap, image->methodTreeSentinal);

  p->virtualThunks = bootObject(heap, image->virtualThunks);

  fixupCode(t, codeMap, codeMapSizeInWords, code, heap);

  syncInstructionCache(code, image->codeSize);

  object classMap = makeClassMap(t, classTable, image->classCount, heap);
  set(t, t->m->loader, ClassLoaderMap, classMap);

  t->m->stringMap = makeStringMap(t, stringTable, image->stringCount, heap);

  p->callTableSize = image->callCount;
  p->callTable = makeCallTable
    (t, heap, callTable, image->callCount,
     reinterpret_cast<uintptr_t>(code));

  p->staticTableArray = makeStaticTableArray
    (t, classTable, image->classCount, heap);

  fixupThunks(t, image, code);

  fixupVirtualThunks(t, image, code);

  fixupMethods(t, image, code);

  t->m->bootstrapClassMap = makeHashMap(t, 0, 0);
}

intptr_t
getThunk(MyThread* t, Thunk thunk)
{
  MyProcessor* p = processor(t);
  
  return reinterpret_cast<intptr_t>
    (p->thunkTable + (thunk * p->thunkSize));
}

void
compileThunks(MyThread* t, Allocator* allocator, MyProcessor* p)
{
  class ThunkContext {
   public:
    ThunkContext(MyThread* t, Zone* zone):
      context(t), promise(t->m->system, zone)
    { }

    Context context;
    ListenPromise promise;
  };

  Zone zone(t->m->system, t->m->heap, 1024);

  ThunkContext defaultContext(t, &zone);

  { Assembler* a = defaultContext.context.assembler;
    
    a->saveFrame(difference(&(t->stack), t), difference(&(t->base), t));

    Assembler::Register thread(t->arch->thread());
    a->pushFrame(1, BytesPerWord, RegisterOperand, &thread);
  
    Assembler::Constant proc(&(defaultContext.promise));
    a->apply(LongCall, BytesPerWord, ConstantOperand, &proc);

    a->popFrame();

    Assembler::Register result(t->arch->returnLow());
    a->apply(Jump, BytesPerWord, RegisterOperand, &result);

    a->endBlock(false)->resolve(0, 0);
  }

  ThunkContext defaultVirtualContext(t, &zone);

  { Assembler* a = defaultVirtualContext.context.assembler;

    Assembler::Register class_(t->arch->virtualCallTarget());
    Assembler::Memory virtualCallTargetSrc
      (t->arch->stack(),
       (t->arch->frameFooterSize() + t->arch->frameReturnAddressSize())
       * BytesPerWord);

    a->apply(Move, BytesPerWord, MemoryOperand, &virtualCallTargetSrc,
             BytesPerWord, RegisterOperand, &class_);

    Assembler::Memory virtualCallTargetDst
      (t->arch->thread(), difference(&(t->virtualCallTarget), t));

    a->apply(Move, BytesPerWord, RegisterOperand, &class_,
             BytesPerWord, MemoryOperand, &virtualCallTargetDst);

    Assembler::Register index(t->arch->virtualCallIndex());
    Assembler::Memory virtualCallIndex
      (t->arch->thread(), difference(&(t->virtualCallIndex), t));

    a->apply(Move, BytesPerWord, RegisterOperand, &index,
             BytesPerWord, MemoryOperand, &virtualCallIndex);
    
    a->saveFrame(difference(&(t->stack), t), difference(&(t->base), t));

    Assembler::Register thread(t->arch->thread());
    a->pushFrame(1, BytesPerWord, RegisterOperand, &thread);
  
    Assembler::Constant proc(&(defaultVirtualContext.promise));
    a->apply(LongCall, BytesPerWord, ConstantOperand, &proc);

    a->popFrame();

    Assembler::Register result(t->arch->returnLow());
    a->apply(Jump, BytesPerWord, RegisterOperand, &result);

    a->endBlock(false)->resolve(0, 0);
  }

  ThunkContext nativeContext(t, &zone);

  { Assembler* a = nativeContext.context.assembler;

    a->saveFrame(difference(&(t->stack), t), difference(&(t->base), t));

    Assembler::Register thread(t->arch->thread());
    a->pushFrame(1, BytesPerWord, RegisterOperand, &thread);

    Assembler::Constant proc(&(nativeContext.promise));
    a->apply(LongCall, BytesPerWord, ConstantOperand, &proc);
  
    a->popFrameAndUpdateStackAndReturn(difference(&(t->stack), t));

    a->endBlock(false)->resolve(0, 0);
  }

  ThunkContext aioobContext(t, &zone);

  { Assembler* a = aioobContext.context.assembler;
      
    a->saveFrame(difference(&(t->stack), t), difference(&(t->base), t));

    Assembler::Register thread(t->arch->thread());
    a->pushFrame(1, BytesPerWord, RegisterOperand, &thread);

    Assembler::Constant proc(&(aioobContext.promise));
    a->apply(LongCall, BytesPerWord, ConstantOperand, &proc);

    a->endBlock(false)->resolve(0, 0);
  }

  ThunkContext tableContext(t, &zone);

  { Assembler* a = tableContext.context.assembler;
  
    a->saveFrame(difference(&(t->stack), t), difference(&(t->base), t));

    Assembler::Constant proc(&(tableContext.promise));
    a->apply(LongJump, BytesPerWord, ConstantOperand, &proc);

    a->endBlock(false)->resolve(0, 0);
  }

  p->thunkSize = pad(tableContext.context.assembler->length());

  p->defaultThunk = finish
    (t, allocator, defaultContext.context.assembler, "default");

  BootImage* image = p->bootImage;
  uint8_t* imageBase = p->codeAllocator.base;

  { void* call;
    defaultContext.promise.listener->resolve
      (reinterpret_cast<intptr_t>(voidPointer(compileMethod)), &call);

    if (image) {
      image->defaultThunk = p->defaultThunk - imageBase;
      image->compileMethodCall = static_cast<uint8_t*>(call) - imageBase;
    }
  }

  p->defaultVirtualThunk = finish
    (t, allocator, defaultVirtualContext.context.assembler, "defaultVirtual");

  { void* call;
    defaultVirtualContext.promise.listener->resolve
      (reinterpret_cast<intptr_t>(voidPointer(compileVirtualMethod)), &call);

    if (image) {
      image->defaultVirtualThunk = p->defaultVirtualThunk - imageBase;
      image->compileVirtualMethodCall
        = static_cast<uint8_t*>(call) - imageBase;
    }
  }

  p->nativeThunk = finish
    (t, allocator, nativeContext.context.assembler, "native");

  { void* call;
    nativeContext.promise.listener->resolve
      (reinterpret_cast<intptr_t>(voidPointer(invokeNative)), &call);

    if (image) {
      image->nativeThunk = p->nativeThunk - imageBase;
      image->invokeNativeCall = static_cast<uint8_t*>(call) - imageBase;
    }
  }

  p->aioobThunk = finish
    (t, allocator, aioobContext.context.assembler, "aioob");

  { void* call;
    aioobContext.promise.listener->resolve
      (reinterpret_cast<intptr_t>(voidPointer(throwArrayIndexOutOfBounds)),
       &call);

    if (image) {
      image->aioobThunk = p->aioobThunk - imageBase;
      image->throwArrayIndexOutOfBoundsCall
        = static_cast<uint8_t*>(call) - imageBase;
    }
  }

  p->thunkTable = static_cast<uint8_t*>
    (allocator->allocate(p->thunkSize * ThunkCount));

  if (image) {
    image->thunkTable = p->thunkTable - imageBase;
    image->thunkSize = p->thunkSize;
  }

  logCompile(t, p->thunkTable, p->thunkSize * ThunkCount, 0, "thunkTable", 0);

  uint8_t* start = p->thunkTable;

#define THUNK(s)                                                        \
  tableContext.context.assembler->writeTo(start);                       \
  start += p->thunkSize;                                                \
  { void* call;                                                         \
    tableContext.promise.listener->resolve                              \
      (reinterpret_cast<intptr_t>(voidPointer(s)), &call);              \
    if (image) {                                                        \
      image->s##Call = static_cast<uint8_t*>(call) - imageBase;         \
    }                                                                   \
  }

#include "thunks.cpp"

#undef THUNK
}

MyProcessor*
processor(MyThread* t)
{
  return static_cast<MyProcessor*>(t->m->processor);
}

object&
objectPools(MyThread* t)
{
  return processor(t)->objectPools;
}

object&
windMethod(MyThread* t)
{
  return processor(t)->windMethod;
}

object&
rewindMethod(MyThread* t)
{
  return processor(t)->rewindMethod;
}

object&
receiveMethod(MyThread* t)
{
  return processor(t)->receiveMethod;
}

uintptr_t
defaultThunk(MyThread* t)
{
  return reinterpret_cast<uintptr_t>(processor(t)->defaultThunk);
}

uintptr_t
defaultVirtualThunk(MyThread* t)
{
  return reinterpret_cast<uintptr_t>(processor(t)->defaultVirtualThunk);
}

uintptr_t
nativeThunk(MyThread* t)
{
  return reinterpret_cast<uintptr_t>(processor(t)->nativeThunk);
}

uintptr_t
aioobThunk(MyThread* t)
{
  return reinterpret_cast<uintptr_t>(processor(t)->aioobThunk);
}

uintptr_t
compileVirtualThunk(MyThread* t, unsigned index)
{
  Context context(t);
  Assembler* a = context.assembler;

  ResolvedPromise indexPromise(index);
  Assembler::Constant indexConstant(&indexPromise);
  Assembler::Register indexRegister(t->arch->virtualCallIndex());
  a->apply(Move, BytesPerWord, ConstantOperand, &indexConstant,
           BytesPerWord, RegisterOperand, &indexRegister);
  
  ResolvedPromise defaultVirtualThunkPromise(defaultVirtualThunk(t));
  Assembler::Constant thunk(&defaultVirtualThunkPromise);
  a->apply(Jump, BytesPerWord, ConstantOperand, &thunk);

  a->endBlock(false)->resolve(0, 0);

  uint8_t* start = static_cast<uint8_t*>
    (codeAllocator(t)->allocate(a->length()));

  a->writeTo(start);

  logCompile(t, start, a->length(), 0, "virtualThunk", 0);

  return reinterpret_cast<uintptr_t>(start);
}

uintptr_t
virtualThunk(MyThread* t, unsigned index)
{
  MyProcessor* p = processor(t);

  if (p->virtualThunks == 0 or wordArrayLength(t, p->virtualThunks) <= index) {
    object newArray = makeWordArray(t, nextPowerOfTwo(index + 1));
    if (p->virtualThunks) {
      memcpy(&wordArrayBody(t, newArray, 0),
             &wordArrayBody(t, p->virtualThunks, 0),
             wordArrayLength(t, p->virtualThunks) * BytesPerWord);
    }
    p->virtualThunks = newArray;
  }

  if (wordArrayBody(t, p->virtualThunks, index) == 0) {
    ACQUIRE(t, t->m->classLock);

    if (wordArrayBody(t, p->virtualThunks, index) == 0) {
      uintptr_t thunk = compileVirtualThunk(t, index);
      wordArrayBody(t, p->virtualThunks, index) = thunk;
    }
  }

  return wordArrayBody(t, p->virtualThunks, index);
}

void
compile(MyThread* t, Allocator* allocator, BootContext* bootContext,
        object method)
{
  PROTECT(t, method);

  if (bootContext == 0) {
    initClass(t, methodClass(t, method));
    if (UNLIKELY(t->exception)) return;
  }

  if (methodAddress(t, method) == defaultThunk(t)) {
    ACQUIRE(t, t->m->classLock);
    
    if (methodAddress(t, method) == defaultThunk(t)) {
      assert(t, (methodFlags(t, method) & ACC_NATIVE) == 0);

      Context context(t, bootContext, method);
      uint8_t* compiled = compile(t, allocator, &context);
      if (UNLIKELY(t->exception)) return;

      if (DebugMethodTree) {
        fprintf(stderr, "insert method at %p\n", compiled);
      }

      // We can't set the MethodCompiled field on the original method
      // before it is placed into the method tree, since another
      // thread might call the method, from which stack unwinding
      // would fail (since there is not yet an entry in the method
      // tree).  However, we can't insert the original method into the
      // tree before setting the MethodCompiled field on it since we
      // rely on that field to determine its position in the tree.
      // Therefore, we insert a clone in its place.  Later, we'll
      // replace the clone with the original to save memory.

      object clone = makeMethod
        (t, methodVmFlags(t, method),
         methodReturnCode(t, method),
         methodParameterCount(t, method),
         methodParameterFootprint(t, method),
         methodFlags(t, method),
         methodOffset(t, method),
         methodNativeID(t, method),
         methodName(t, method),
         methodSpec(t, method),
         methodClass(t, method),
         methodCode(t, method),
         reinterpret_cast<intptr_t>(compiled));

      methodTree(t) = treeInsert
        (t, &(context.zone), methodTree(t),
         reinterpret_cast<intptr_t>(compiled), clone, methodTreeSentinal(t),
         compareIpToMethodBounds);

      memoryBarrier();

      methodCompiled(t, method) = reinterpret_cast<intptr_t>(compiled);

      if (methodVirtual(t, method)) {
        classVtable(t, methodClass(t, method), methodOffset(t, method))
          = compiled;
      }

      treeUpdate(t, methodTree(t), reinterpret_cast<intptr_t>(compiled),
                 method, methodTreeSentinal(t), compareIpToMethodBounds);
    }
  }
}

object&
methodTree(MyThread* t)
{
  return processor(t)->methodTree;
}

object
methodTreeSentinal(MyThread* t)
{
  return processor(t)->methodTreeSentinal;
}

FixedAllocator*
codeAllocator(MyThread* t)
{
  return &(processor(t)->codeAllocator);
}

} // namespace

namespace vm {

Processor*
makeProcessor(System* system, Allocator* allocator)
{
  return new (allocator->allocate(sizeof(MyProcessor)))
    MyProcessor(system, allocator);
}

} // namespace vm
