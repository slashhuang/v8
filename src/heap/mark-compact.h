// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_MARK_COMPACT_H_
#define V8_HEAP_MARK_COMPACT_H_

#include <deque>

#include "src/base/bits.h"
#include "src/base/platform/condition-variable.h"
#include "src/cancelable-task.h"
#include "src/heap/marking.h"
#include "src/heap/spaces.h"
#include "src/heap/store-buffer.h"

namespace v8 {
namespace internal {

// Callback function, returns whether an object is alive. The heap size
// of the object is returned in size. It optionally updates the offset
// to the first live object in the page (only used for old and map objects).
typedef bool (*IsAliveFunction)(HeapObject* obj, int* size, int* offset);

// Callback function to mark an object in a given heap.
typedef void (*MarkObjectFunction)(Heap* heap, HeapObject* object);

// Forward declarations.
class CodeFlusher;
class HeapObjectVisitor;
class MarkCompactCollector;
class MinorMarkCompactCollector;
class MarkingVisitor;

class MarkingState {
 public:
  static MarkingState FromPageInternal(MemoryChunk* chunk) {
    return MarkingState(chunk->markbits<MarkingMode::FULL>(),
                        chunk->live_bytes_address<MarkingMode::FULL>());
  }

  static MarkingState FromPageExternal(MemoryChunk* chunk) {
    return MarkingState(
        chunk->markbits<MarkingMode::YOUNG_GENERATION>(),
        chunk->live_bytes_address<MarkingMode::YOUNG_GENERATION>());
  }

  MarkingState(Bitmap* bitmap, int* live_bytes)
      : bitmap(bitmap), live_bytes(live_bytes) {}

  void IncrementLiveBytes(intptr_t by) const {
    *live_bytes += static_cast<int>(by);
  }
  void SetLiveBytes(intptr_t value) const {
    *live_bytes = static_cast<int>(value);
  }

  void ClearLiveness() const {
    bitmap->Clear();
    *live_bytes = 0;
  }

  Bitmap* bitmap;
  int* live_bytes;
};

// TODO(mlippautz): Remove duplicate accessors once the architecture for
// different markers is fixed.
class ObjectMarking : public AllStatic {
 public:
  V8_INLINE static MarkBit MarkBitFrom(HeapObject* obj) {
    const Address address = obj->address();
    const MemoryChunk* p = MemoryChunk::FromAddress(address);
    return p->markbits()->MarkBitFromIndex(p->AddressToMarkbitIndex(address));
  }

  V8_INLINE static MarkBit MarkBitFrom(HeapObject* obj,
                                       const MarkingState& state) {
    const Address address = obj->address();
    const MemoryChunk* p = MemoryChunk::FromAddress(address);
    return state.bitmap->MarkBitFromIndex(p->AddressToMarkbitIndex(address));
  }

  static Marking::ObjectColor Color(HeapObject* obj) {
    return Marking::Color(ObjectMarking::MarkBitFrom(obj));
  }

  static Marking::ObjectColor Color(HeapObject* obj,
                                    const MarkingState& state) {
    return Marking::Color(ObjectMarking::MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsImpossible(HeapObject* obj) {
    return Marking::IsImpossible<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsImpossible(HeapObject* obj,
                                     const MarkingState& state) {
    return Marking::IsImpossible<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsBlack(HeapObject* obj) {
    return Marking::IsBlack<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsBlack(HeapObject* obj, const MarkingState& state) {
    return Marking::IsBlack<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsWhite(HeapObject* obj) {
    return Marking::IsWhite<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsWhite(HeapObject* obj, const MarkingState& state) {
    return Marking::IsWhite<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsGrey(HeapObject* obj) {
    return Marking::IsGrey<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsGrey(HeapObject* obj, const MarkingState& state) {
    return Marking::IsGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsBlackOrGrey(HeapObject* obj) {
    return Marking::IsBlackOrGrey<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool IsBlackOrGrey(HeapObject* obj,
                                      const MarkingState& state) {
    return Marking::IsBlackOrGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool BlackToGrey(HeapObject* obj) {
    DCHECK((access_mode == MarkBit::ATOMIC || IsBlack<access_mode>(obj)));
    MarkBit markbit = MarkBitFrom(obj);
    if (!Marking::BlackToGrey<access_mode>(markbit)) return false;
    MemoryChunk::IncrementLiveBytes(obj, -obj->Size());
    return true;
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool BlackToGrey(HeapObject* obj,
                                    const MarkingState& state) {
    DCHECK(
        (access_mode == MarkBit::ATOMIC || IsBlack<access_mode>(obj, state)));
    MarkBit markbit = MarkBitFrom(obj, state);
    if (!Marking::BlackToGrey<access_mode>(markbit)) return false;
    state.IncrementLiveBytes(-obj->Size());
    return true;
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool WhiteToGrey(HeapObject* obj) {
    DCHECK((access_mode == MarkBit::ATOMIC || IsWhite<access_mode>(obj)));
    return Marking::WhiteToGrey<access_mode>(MarkBitFrom(obj));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool WhiteToGrey(HeapObject* obj,
                                    const MarkingState& state) {
    DCHECK(
        (access_mode == MarkBit::ATOMIC || IsWhite<access_mode>(obj, state)));
    return Marking::WhiteToGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool WhiteToBlack(HeapObject* obj) {
    DCHECK((access_mode == MarkBit::ATOMIC || IsWhite<access_mode>(obj)));
    if (!ObjectMarking::WhiteToGrey<access_mode>(obj)) return false;
    return ObjectMarking::GreyToBlack<access_mode>(obj);
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool WhiteToBlack(HeapObject* obj,
                                     const MarkingState& state) {
    DCHECK(
        (access_mode == MarkBit::ATOMIC || IsWhite<access_mode>(obj, state)));
    if (!ObjectMarking::WhiteToGrey<access_mode>(obj, state)) return false;
    return ObjectMarking::GreyToBlack<access_mode>(obj, state);
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool GreyToBlack(HeapObject* obj) {
    DCHECK((access_mode == MarkBit::ATOMIC || IsGrey<access_mode>(obj)));
    MarkBit markbit = MarkBitFrom(obj);
    if (!Marking::GreyToBlack<access_mode>(markbit)) return false;
    MemoryChunk::IncrementLiveBytes(obj, obj->Size());
    return true;
  }

  template <MarkBit::AccessMode access_mode = MarkBit::NON_ATOMIC>
  V8_INLINE static bool GreyToBlack(HeapObject* obj,
                                    const MarkingState& state) {
    DCHECK((access_mode == MarkBit::ATOMIC || IsGrey<access_mode>(obj, state)));
    MarkBit markbit = MarkBitFrom(obj, state);
    if (!Marking::GreyToBlack<access_mode>(markbit)) return false;
    state.IncrementLiveBytes(obj->Size());
    return true;
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(ObjectMarking);
};

// ----------------------------------------------------------------------------
// Marking deque for tracing live objects.
class MarkingDeque {
 public:
  explicit MarkingDeque(Heap* heap)
      : backing_store_(nullptr),
        backing_store_committed_size_(0),
        array_(nullptr),
        top_(0),
        bottom_(0),
        mask_(0),
        overflowed_(false),
        in_use_(false),
        uncommit_task_pending_(false),
        heap_(heap) {}

  void SetUp();
  void TearDown();

  // Ensures that the marking deque is committed and will stay committed until
  // StopUsing() is called.
  void StartUsing();
  void StopUsing();
  void Clear();

  inline bool IsFull() { return ((top_ + 1) & mask_) == bottom_; }

  inline bool IsEmpty() { return top_ == bottom_; }

  bool overflowed() const { return overflowed_; }

  void ClearOverflowed() { overflowed_ = false; }

  void SetOverflowed() { overflowed_ = true; }

  // Push the object on the marking stack if there is room, otherwise mark the
  // deque as overflowed and wait for a rescan of the heap.
  INLINE(bool Push(HeapObject* object)) {
    DCHECK(object->IsHeapObject());
    if (IsFull()) {
      SetOverflowed();
      return false;
    } else {
      array_[top_] = object;
      top_ = ((top_ + 1) & mask_);
      return true;
    }
  }

  INLINE(HeapObject* Pop()) {
    DCHECK(!IsEmpty());
    top_ = ((top_ - 1) & mask_);
    HeapObject* object = array_[top_];
    DCHECK(object->IsHeapObject());
    return object;
  }

  // Unshift the object into the marking stack if there is room, otherwise mark
  // the deque as overflowed and wait for a rescan of the heap.
  INLINE(bool Unshift(HeapObject* object)) {
    DCHECK(object->IsHeapObject());
    if (IsFull()) {
      SetOverflowed();
      return false;
    } else {
      bottom_ = ((bottom_ - 1) & mask_);
      array_[bottom_] = object;
      return true;
    }
  }

  HeapObject** array() { return array_; }
  int bottom() { return bottom_; }
  int top() { return top_; }
  int mask() { return mask_; }
  void set_top(int top) { top_ = top; }

 private:
  // This task uncommits the marking_deque backing store if
  // markin_deque->in_use_ is false.
  class UncommitTask : public CancelableTask {
   public:
    explicit UncommitTask(Isolate* isolate, MarkingDeque* marking_deque)
        : CancelableTask(isolate), marking_deque_(marking_deque) {}

   private:
    // CancelableTask override.
    void RunInternal() override {
      base::LockGuard<base::Mutex> guard(&marking_deque_->mutex_);
      if (!marking_deque_->in_use_) {
        marking_deque_->Uncommit();
      }
      marking_deque_->uncommit_task_pending_ = false;
    }

    MarkingDeque* marking_deque_;
    DISALLOW_COPY_AND_ASSIGN(UncommitTask);
  };

  static const size_t kMaxSize = 4 * MB;
  static const size_t kMinSize = 256 * KB;

  // Must be called with mutex lock.
  void EnsureCommitted();

  // Must be called with mutex lock.
  void Uncommit();

  // Must be called with mutex lock.
  void StartUncommitTask();

  base::Mutex mutex_;

  base::VirtualMemory* backing_store_;
  size_t backing_store_committed_size_;
  HeapObject** array_;
  // array_[(top - 1) & mask_] is the top element in the deque.  The Deque is
  // empty when top_ == bottom_.  It is full when top_ + 1 == bottom
  // (mod mask + 1).
  int top_;
  int bottom_;
  int mask_;
  bool overflowed_;
  // in_use_ == true after taking mutex lock implies that the marking deque is
  // committed and will stay committed at least until in_use_ == false.
  bool in_use_;
  bool uncommit_task_pending_;
  Heap* heap_;

  DISALLOW_COPY_AND_ASSIGN(MarkingDeque);
};


// CodeFlusher collects candidates for code flushing during marking and
// processes those candidates after marking has completed in order to
// reset those functions referencing code objects that would otherwise
// be unreachable. Code objects can be referenced in two ways:
//    - SharedFunctionInfo references unoptimized code.
//    - JSFunction references either unoptimized or optimized code.
// We are not allowed to flush unoptimized code for functions that got
// optimized or inlined into optimized code, because we might bailout
// into the unoptimized code again during deoptimization.
class CodeFlusher {
 public:
  explicit CodeFlusher(Isolate* isolate)
      : isolate_(isolate),
        jsfunction_candidates_head_(nullptr),
        shared_function_info_candidates_head_(nullptr) {}

  inline void AddCandidate(SharedFunctionInfo* shared_info);
  inline void AddCandidate(JSFunction* function);

  void EvictCandidate(SharedFunctionInfo* shared_info);
  void EvictCandidate(JSFunction* function);

  void ProcessCandidates() {
    ProcessSharedFunctionInfoCandidates();
    ProcessJSFunctionCandidates();
  }

  void IteratePointersToFromSpace(ObjectVisitor* v);

 private:
  void ProcessJSFunctionCandidates();
  void ProcessSharedFunctionInfoCandidates();

  static inline JSFunction** GetNextCandidateSlot(JSFunction* candidate);
  static inline JSFunction* GetNextCandidate(JSFunction* candidate);
  static inline void SetNextCandidate(JSFunction* candidate,
                                      JSFunction* next_candidate);
  static inline void ClearNextCandidate(JSFunction* candidate,
                                        Object* undefined);

  static inline SharedFunctionInfo* GetNextCandidate(
      SharedFunctionInfo* candidate);
  static inline void SetNextCandidate(SharedFunctionInfo* candidate,
                                      SharedFunctionInfo* next_candidate);
  static inline void ClearNextCandidate(SharedFunctionInfo* candidate);

  Isolate* isolate_;
  JSFunction* jsfunction_candidates_head_;
  SharedFunctionInfo* shared_function_info_candidates_head_;

  DISALLOW_COPY_AND_ASSIGN(CodeFlusher);
};


// Defined in isolate.h.
class ThreadLocalTop;

class MarkBitCellIterator BASE_EMBEDDED {
 public:
  MarkBitCellIterator(MemoryChunk* chunk, MarkingState state) : chunk_(chunk) {
    last_cell_index_ = Bitmap::IndexToCell(Bitmap::CellAlignIndex(
        chunk_->AddressToMarkbitIndex(chunk_->area_end())));
    cell_base_ = chunk_->area_start();
    cell_index_ = Bitmap::IndexToCell(
        Bitmap::CellAlignIndex(chunk_->AddressToMarkbitIndex(cell_base_)));
    cells_ = state.bitmap->cells();
  }

  inline bool Done() { return cell_index_ == last_cell_index_; }

  inline bool HasNext() { return cell_index_ < last_cell_index_ - 1; }

  inline MarkBit::CellType* CurrentCell() {
    DCHECK(cell_index_ == Bitmap::IndexToCell(Bitmap::CellAlignIndex(
                              chunk_->AddressToMarkbitIndex(cell_base_))));
    return &cells_[cell_index_];
  }

  inline Address CurrentCellBase() {
    DCHECK(cell_index_ == Bitmap::IndexToCell(Bitmap::CellAlignIndex(
                              chunk_->AddressToMarkbitIndex(cell_base_))));
    return cell_base_;
  }

  MUST_USE_RESULT inline bool Advance() {
    cell_base_ += Bitmap::kBitsPerCell * kPointerSize;
    return ++cell_index_ != last_cell_index_;
  }

  inline bool Advance(unsigned int new_cell_index) {
    if (new_cell_index != cell_index_) {
      DCHECK_GT(new_cell_index, cell_index_);
      DCHECK_LE(new_cell_index, last_cell_index_);
      unsigned int diff = new_cell_index - cell_index_;
      cell_index_ = new_cell_index;
      cell_base_ += diff * (Bitmap::kBitsPerCell * kPointerSize);
      return true;
    }
    return false;
  }

  // Return the next mark bit cell. If there is no next it returns 0;
  inline MarkBit::CellType PeekNext() {
    if (HasNext()) {
      return cells_[cell_index_ + 1];
    }
    return 0;
  }

 private:
  MemoryChunk* chunk_;
  MarkBit::CellType* cells_;
  unsigned int last_cell_index_;
  unsigned int cell_index_;
  Address cell_base_;
};

// Grey objects can happen on black pages when black objects transition to
// grey e.g. when calling RecordWrites on them.
enum LiveObjectIterationMode {
  kBlackObjects,
  kGreyObjects,
  kAllLiveObjects
};

template <LiveObjectIterationMode T>
class LiveObjectIterator BASE_EMBEDDED {
 public:
  LiveObjectIterator(MemoryChunk* chunk, MarkingState state)
      : chunk_(chunk),
        it_(chunk_, state),
        cell_base_(it_.CurrentCellBase()),
        current_cell_(*it_.CurrentCell()) {}

  HeapObject* Next();

 private:
  inline Heap* heap() { return chunk_->heap(); }

  MemoryChunk* chunk_;
  MarkBitCellIterator it_;
  Address cell_base_;
  MarkBit::CellType current_cell_;
};

class LiveObjectVisitor BASE_EMBEDDED {
 public:
  enum IterationMode {
    kKeepMarking,
    kClearMarkbits,
  };

  // Visits black objects on a MemoryChunk until the Visitor returns for an
  // object. If IterationMode::kClearMarkbits is passed the markbits and slots
  // for visited objects are cleared for each successfully visited object.
  template <class Visitor>
  bool VisitBlackObjects(MemoryChunk* chunk, const MarkingState& state,
                         Visitor* visitor, IterationMode iteration_mode);

 private:
  void RecomputeLiveBytes(MemoryChunk* chunk, const MarkingState& state);
};

enum PageEvacuationMode { NEW_TO_NEW, NEW_TO_OLD };

class MinorMarkCompactCollector {
 public:
  explicit MinorMarkCompactCollector(Heap* heap)
      : heap_(heap), marking_deque_(heap) {}

  void SetUp();
  void TearDown();

  void CollectGarbage();

 private:
  class RootMarkingVisitor;

  static MarkingState StateForObject(HeapObject* object) {
    return MarkingState::FromPageExternal(Page::FromAddress(object->address()));
  }

  inline Heap* heap() { return heap_; }
  inline Isolate* isolate() { return heap()->isolate(); }
  inline MarkingDeque* marking_deque() { return &marking_deque_; }

  V8_INLINE void MarkObject(HeapObject* obj);
  V8_INLINE void PushBlack(HeapObject* obj);

  SlotCallbackResult CheckAndMarkObject(Heap* heap, Address slot_address);
  void MarkLiveObjects();
  void ProcessMarkingDeque();
  void EmptyMarkingDeque();

  Heap* heap_;
  MarkingDeque marking_deque_;

  friend class StaticYoungGenerationMarkingVisitor;
};

// -------------------------------------------------------------------------
// Mark-Compact collector
class MarkCompactCollector {
 public:
  class RootMarkingVisitor;

  class Sweeper {
   public:
    class SweeperTask;

    enum FreeListRebuildingMode { REBUILD_FREE_LIST, IGNORE_FREE_LIST };
    enum FreeSpaceTreatmentMode { IGNORE_FREE_SPACE, ZAP_FREE_SPACE };
    enum ClearOldToNewSlotsMode {
      DO_NOT_CLEAR,
      CLEAR_REGULAR_SLOTS,
      CLEAR_TYPED_SLOTS
    };

    typedef std::deque<Page*> SweepingList;
    typedef List<Page*> SweptList;

    static int RawSweep(Page* p, FreeListRebuildingMode free_list_mode,
                        FreeSpaceTreatmentMode free_space_mode);

    explicit Sweeper(Heap* heap)
        : heap_(heap),
          pending_sweeper_tasks_semaphore_(0),
          semaphore_counter_(0),
          sweeping_in_progress_(false),
          num_sweeping_tasks_(0) {}

    bool sweeping_in_progress() { return sweeping_in_progress_; }

    void AddPage(AllocationSpace space, Page* page);

    int ParallelSweepSpace(AllocationSpace identity, int required_freed_bytes,
                           int max_pages = 0);
    int ParallelSweepPage(Page* page, AllocationSpace identity);

    // After calling this function sweeping is considered to be in progress
    // and the main thread can sweep lazily, but the background sweeper tasks
    // are not running yet.
    void StartSweeping();
    void StartSweeperTasks();
    void EnsureCompleted();
    void EnsureNewSpaceCompleted();
    bool AreSweeperTasksRunning();
    void SweepOrWaitUntilSweepingCompleted(Page* page);

    void AddSweptPageSafe(PagedSpace* space, Page* page);
    Page* GetSweptPageSafe(PagedSpace* space);

   private:
    static const int kAllocationSpaces = LAST_PAGED_SPACE + 1;

    static ClearOldToNewSlotsMode GetClearOldToNewSlotsMode(Page* p);

    template <typename Callback>
    void ForAllSweepingSpaces(Callback callback) {
      for (int i = 0; i < kAllocationSpaces; i++) {
        callback(static_cast<AllocationSpace>(i));
      }
    }

    Page* GetSweepingPageSafe(AllocationSpace space);
    void AddSweepingPageSafe(AllocationSpace space, Page* page);

    void PrepareToBeSweptPage(AllocationSpace space, Page* page);

    Heap* heap_;
    base::Semaphore pending_sweeper_tasks_semaphore_;
    // Counter is only used for waiting on the semaphore.
    intptr_t semaphore_counter_;
    base::Mutex mutex_;
    SweptList swept_list_[kAllocationSpaces];
    SweepingList sweeping_list_[kAllocationSpaces];
    bool sweeping_in_progress_;
    // Counter is actively maintained by the concurrent tasks to avoid querying
    // the semaphore for maintaining a task counter on the main thread.
    base::AtomicNumber<intptr_t> num_sweeping_tasks_;
  };

  enum IterationMode {
    kKeepMarking,
    kClearMarkbits,
  };

  static void Initialize();

  static SlotCallbackResult CheckAndMarkObject(Heap* heap,
                                               Address slot_address);

  void SetUp();

  void TearDown();

  void CollectEvacuationCandidates(PagedSpace* space);

  void AddEvacuationCandidate(Page* p);

  // Prepares for GC by resetting relocation info in old and map spaces and
  // choosing spaces to compact.
  void Prepare();

  // Performs a global garbage collection.
  void CollectGarbage();

  bool StartCompaction();

  void AbortCompaction();

  // Determine type of object and emit deletion log event.
  static void ReportDeleteIfNeeded(HeapObject* obj, Isolate* isolate);

  // Distinguishable invalid map encodings (for single word and multiple words)
  // that indicate free regions.
  static const uint32_t kSingleFreeEncoding = 0;
  static const uint32_t kMultiFreeEncoding = 1;

  inline Heap* heap() const { return heap_; }
  inline Isolate* isolate() const;

  CodeFlusher* code_flusher() { return code_flusher_; }
  inline bool is_code_flushing_enabled() const { return code_flusher_ != NULL; }

  INLINE(static bool ShouldSkipEvacuationSlotRecording(Object* host)) {
    return Page::FromAddress(reinterpret_cast<Address>(host))
        ->ShouldSkipEvacuationSlotRecording();
  }

  static inline bool IsOnEvacuationCandidate(HeapObject* obj) {
    return Page::FromAddress(reinterpret_cast<Address>(obj))
        ->IsEvacuationCandidate();
  }

  void RecordRelocSlot(Code* host, RelocInfo* rinfo, Object* target);
  void RecordCodeEntrySlot(HeapObject* host, Address slot, Code* target);
  void RecordCodeTargetPatch(Address pc, Code* target);
  INLINE(void RecordSlot(HeapObject* object, Object** slot, Object* target));
  INLINE(void ForceRecordSlot(HeapObject* object, Object** slot,
                              Object* target));
  void RecordLiveSlotsOnPage(Page* page);

  void UpdateSlots(SlotsBuffer* buffer);
  void UpdateSlotsRecordedIn(SlotsBuffer* buffer);

  void InvalidateCode(Code* code);

  void ClearMarkbits();

  bool is_compacting() const { return compacting_; }

  // Ensures that sweeping is finished.
  //
  // Note: Can only be called safely from main thread.
  void EnsureSweepingCompleted();

  // Help out in sweeping the corresponding space and refill memory that has
  // been regained.
  //
  // Note: Thread-safe.
  void SweepAndRefill(CompactionSpace* space);

  // Checks if sweeping is in progress right now on any space.
  bool sweeping_in_progress() { return sweeper().sweeping_in_progress(); }

  void set_evacuation(bool evacuation) { evacuation_ = evacuation; }

  bool evacuation() const { return evacuation_; }

  MarkingDeque* marking_deque() { return &marking_deque_; }

  Sweeper& sweeper() { return sweeper_; }

#ifdef DEBUG
  // Checks whether performing mark-compact collection.
  bool in_use() { return state_ > PREPARE_GC; }
  bool are_map_pointers_encoded() { return state_ == UPDATE_POINTERS; }
#endif

#ifdef VERIFY_HEAP
  void VerifyValidStoreAndSlotsBufferEntries();
  void VerifyMarkbitsAreClean();
  static void VerifyMarkbitsAreClean(PagedSpace* space);
  static void VerifyMarkbitsAreClean(NewSpace* space);
  void VerifyWeakEmbeddedObjectsInCode();
  void VerifyOmittedMapChecks();
#endif

 private:
  explicit MarkCompactCollector(Heap* heap);

  bool WillBeDeoptimized(Code* code);

  void ComputeEvacuationHeuristics(size_t area_size,
                                   int* target_fragmentation_percent,
                                   size_t* max_evacuated_bytes);

  void VisitAllObjects(HeapObjectVisitor* visitor);

  void RecordObjectStats();

  // Finishes GC, performs heap verification if enabled.
  void Finish();

  // -----------------------------------------------------------------------
  // Phase 1: Marking live objects.
  //
  //  Before: The heap has been prepared for garbage collection by
  //          MarkCompactCollector::Prepare() and is otherwise in its
  //          normal state.
  //
  //   After: Live objects are marked and non-live objects are unmarked.

  friend class CodeMarkingVisitor;
  friend class IncrementalMarkingMarkingVisitor;
  friend class MarkCompactMarkingVisitor;
  friend class MarkingVisitor;
  friend class RecordMigratedSlotVisitor;
  friend class SharedFunctionInfoMarkingVisitor;
  friend class StaticYoungGenerationMarkingVisitor;

  // Mark code objects that are active on the stack to prevent them
  // from being flushed.
  void PrepareThreadForCodeFlushing(Isolate* isolate, ThreadLocalTop* top);

  void PrepareForCodeFlushing();

  // Marking operations for objects reachable from roots.
  void MarkLiveObjects();

  // Pushes a black object onto the marking stack and accounts for live bytes.
  // Note that this assumes live bytes have not yet been counted.
  V8_INLINE void PushBlack(HeapObject* obj);

  // Unshifts a black object into the marking stack and accounts for live bytes.
  // Note that this assumes lives bytes have already been counted.
  V8_INLINE void UnshiftBlack(HeapObject* obj);

  // Marks the object black and pushes it on the marking stack.
  // This is for non-incremental marking only.
  V8_INLINE void MarkObject(HeapObject* obj);

  // Mark the heap roots and all objects reachable from them.
  void MarkRoots(RootMarkingVisitor* visitor);

  // Mark the string table specially.  References to internalized strings from
  // the string table are weak.
  void MarkStringTable(RootMarkingVisitor* visitor);

  // Mark objects reachable (transitively) from objects in the marking stack
  // or overflowed in the heap.
  void ProcessMarkingDeque();

  // Mark objects reachable (transitively) from objects in the marking stack
  // or overflowed in the heap.  This respects references only considered in
  // the final atomic marking pause including the following:
  //    - Processing of objects reachable through Harmony WeakMaps.
  //    - Objects reachable due to host application logic like object groups,
  //      implicit references' groups, or embedder heap tracing.
  void ProcessEphemeralMarking(ObjectVisitor* visitor,
                               bool only_process_harmony_weak_collections);

  // If the call-site of the top optimized code was not prepared for
  // deoptimization, then treat the maps in the code as strong pointers,
  // otherwise a map can die and deoptimize the code.
  void ProcessTopOptimizedFrame(ObjectVisitor* visitor);

  // Collects a list of dependent code from maps embedded in optimize code.
  DependentCode* DependentCodeListFromNonLiveMaps();

  // Mark objects reachable (transitively) from objects in the marking
  // stack.  This function empties the marking stack, but may leave
  // overflowed objects in the heap, in which case the marking stack's
  // overflow flag will be set.
  void EmptyMarkingDeque();

  // Refill the marking stack with overflowed objects from the heap.  This
  // function either leaves the marking stack full or clears the overflow
  // flag on the marking stack.
  void RefillMarkingDeque();

  // Helper methods for refilling the marking stack by discovering grey objects
  // on various pages of the heap. Used by {RefillMarkingDeque} only.
  template <class T>
  void DiscoverGreyObjectsWithIterator(T* it);
  void DiscoverGreyObjectsOnPage(MemoryChunk* p);
  void DiscoverGreyObjectsInSpace(PagedSpace* space);
  void DiscoverGreyObjectsInNewSpace();

  // Callback function for telling whether the object *p is an unmarked
  // heap object.
  static bool IsUnmarkedHeapObject(Object** p);

  // Clear non-live references in weak cells, transition and descriptor arrays,
  // and deoptimize dependent code of non-live maps.
  void ClearNonLiveReferences();
  void MarkDependentCodeForDeoptimization(DependentCode* list);
  // Find non-live targets of simple transitions in the given list. Clear
  // transitions to non-live targets and if needed trim descriptors arrays.
  void ClearSimpleMapTransitions(Object* non_live_map_list);
  void ClearSimpleMapTransition(Map* map, Map* dead_transition);
  // Compact every array in the global list of transition arrays and
  // trim the corresponding descriptor array if a transition target is non-live.
  void ClearFullMapTransitions();
  bool CompactTransitionArray(Map* map, TransitionArray* transitions,
                              DescriptorArray* descriptors);
  void TrimDescriptorArray(Map* map, DescriptorArray* descriptors);
  void TrimEnumCache(Map* map, DescriptorArray* descriptors);

  // Mark all values associated with reachable keys in weak collections
  // encountered so far.  This might push new object or even new weak maps onto
  // the marking stack.
  void ProcessWeakCollections();

  // After all reachable objects have been marked those weak map entries
  // with an unreachable key are removed from all encountered weak maps.
  // The linked list of all encountered weak maps is destroyed.
  void ClearWeakCollections();

  // We have to remove all encountered weak maps from the list of weak
  // collections when incremental marking is aborted.
  void AbortWeakCollections();

  void ClearWeakCells(Object** non_live_map_list,
                      DependentCode** dependent_code_list);
  void AbortWeakCells();

  void AbortTransitionArrays();

  // Starts sweeping of spaces by contributing on the main thread and setting
  // up other pages for sweeping. Does not start sweeper tasks.
  void StartSweepSpaces();
  void StartSweepSpace(PagedSpace* space);

  void EvacuatePrologue();
  void EvacuateEpilogue();
  void EvacuatePagesInParallel();

  // The number of parallel compaction tasks, including the main thread.
  int NumberOfParallelCompactionTasks(int pages, intptr_t live_bytes);

  void EvacuateNewSpaceAndCandidates();

  void UpdatePointersAfterEvacuation();

  void ReleaseEvacuationCandidates();


#ifdef DEBUG
  friend class MarkObjectVisitor;
  static void VisitObject(HeapObject* obj);

  friend class UnmarkObjectVisitor;
  static void UnmarkObject(HeapObject* obj);
#endif

  Heap* heap_;

  base::Semaphore page_parallel_job_semaphore_;

#ifdef DEBUG
  enum CollectorState {
    IDLE,
    PREPARE_GC,
    MARK_LIVE_OBJECTS,
    SWEEP_SPACES,
    ENCODE_FORWARDING_ADDRESSES,
    UPDATE_POINTERS,
    RELOCATE_OBJECTS
  };

  // The current stage of the collector.
  CollectorState state_;
#endif

  bool was_marked_incrementally_;

  bool evacuation_;

  // True if we are collecting slots to perform evacuation from evacuation
  // candidates.
  bool compacting_;

  bool black_allocation_;

  bool have_code_to_deoptimize_;

  MarkingDeque marking_deque_;

  CodeFlusher* code_flusher_;

  // Candidates for pages that should be evacuated.
  List<Page*> evacuation_candidates_;
  // Pages that are actually processed during evacuation.
  List<Page*> old_space_evacuation_pages_;
  List<Page*> new_space_evacuation_pages_;

  Sweeper sweeper_;

  friend class Heap;
  friend class StoreBuffer;
};


class EvacuationScope BASE_EMBEDDED {
 public:
  explicit EvacuationScope(MarkCompactCollector* collector)
      : collector_(collector) {
    collector_->set_evacuation(true);
  }

  ~EvacuationScope() { collector_->set_evacuation(false); }

 private:
  MarkCompactCollector* collector_;
};

V8_EXPORT_PRIVATE const char* AllocationSpaceName(AllocationSpace space);
}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_MARK_COMPACT_H_
