/*
 * Copyright (c) 2015, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahAsserts.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahStringDedup.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"

template <class T>
void ShenandoahConcurrentMark::do_task(ShenandoahObjToScanQueue* q, T* cl, jushort* live_data, ShenandoahMarkTask* task) {
  oop obj = task->obj();

  shenandoah_assert_not_forwarded(NULL, obj);
  shenandoah_assert_marked_next(NULL, obj);
  shenandoah_assert_not_in_cset_except(NULL, obj, _heap->cancelled_gc());

  if (task->is_not_chunked()) {
    count_liveness(live_data, obj);
    if (obj->is_instance()) {
      // Case 1: Normal oop, process as usual.
      obj->oop_iterate(cl);
    } else if (obj->is_objArray()) {
      // Case 2: Object array instance and no chunk is set. Must be the first
      // time we visit it, start the chunked processing.
      do_chunked_array_start<T>(q, cl, obj);
    } else {
      // Case 3: Primitive array. Do nothing, no oops there. We use the same
      // performance tweak TypeArrayKlass::oop_oop_iterate_impl is using:
      // We skip iterating over the klass pointer since we know that
      // Universe::TypeArrayKlass never moves.
      assert (obj->is_typeArray(), "should be type array");
    }
  } else {
    // Case 4: Array chunk, has sensible chunk id. Process it.
    do_chunked_array<T>(q, cl, obj, task->chunk(), task->pow());
  }
}

inline void ShenandoahConcurrentMark::count_liveness(jushort* live_data, oop obj) {
  size_t region_idx = _heap->heap_region_index_containing(obj);
  ShenandoahHeapRegion* region = _heap->get_region(region_idx);
  if (!region->is_humongous_start()) {
    assert(!region->is_humongous(), "Cannot have continuations here");
    jushort cur = live_data[region_idx];
    size_t size = obj->size() + BrooksPointer::word_size();
    size_t max = (1 << (sizeof(jushort) * 8)) - 1;
    if (size >= max) {
      // too big, add to region data directly
      region->increase_live_data_gc_words(size);
    } else {
      size_t new_val = cur + size;
      if (new_val >= max) {
        // overflow, flush to region data
        region->increase_live_data_gc_words(new_val);
        live_data[region_idx] = 0;
      } else {
        // still good, remember in locals
        live_data[region_idx] = (jushort) new_val;
      }
    }
  } else {
    count_liveness_humongous(obj);
  }
}

inline void ShenandoahConcurrentMark::count_liveness_humongous(oop obj) {
  shenandoah_assert_in_correct_region(NULL, obj);
  size_t region_idx = _heap->heap_region_index_containing(obj);
  size_t size = obj->size() + BrooksPointer::word_size();
  size_t num_regions = ShenandoahHeapRegion::required_regions(size * HeapWordSize);

  for (size_t i = region_idx; i < region_idx + num_regions; i++) {
    ShenandoahHeapRegion* chain_reg = _heap->get_region(i);
    assert(chain_reg->is_humongous(), "Expecting a humongous region");
    chain_reg->increase_live_data_gc_words(chain_reg->used() >> LogHeapWordSize);
  }
}

template <class T>
inline void ShenandoahConcurrentMark::do_chunked_array_start(ShenandoahObjToScanQueue* q, T* cl, oop obj) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);
  int len = array->length();

  if (len <= (int) ObjArrayMarkingStride*2) {
    // A few slices only, process directly
    array->oop_iterate_range(cl, 0, len);
  } else {
    int bits = log2_long(len);
    // Compensate for non-power-of-two arrays, cover the array in excess:
    if (len != (1 << bits)) bits++;

    // Only allow full chunks on the queue. This frees do_chunked_array() from checking from/to
    // boundaries against array->length(), touching the array header on every chunk.
    //
    // To do this, we cut the prefix in full-sized chunks, and submit them on the queue.
    // If the array is not divided in chunk sizes, then there would be an irregular tail,
    // which we will process separately.

    int last_idx = 0;

    int chunk = 1;
    int pow = bits;

    // Handle overflow
    if (pow >= 31) {
      assert (pow == 31, "sanity");
      pow--;
      chunk = 2;
      last_idx = (1 << pow);
      bool pushed = q->push(ShenandoahMarkTask(array, 1, pow));
      assert(pushed, "overflow queue should always succeed pushing");
    }

    // Split out tasks, as suggested in ObjArrayChunkedTask docs. Record the last
    // successful right boundary to figure out the irregular tail.
    while ((1 << pow) > (int)ObjArrayMarkingStride &&
           (chunk*2 < ShenandoahMarkTask::chunk_size())) {
      pow--;
      int left_chunk = chunk*2 - 1;
      int right_chunk = chunk*2;
      int left_chunk_end = left_chunk * (1 << pow);
      if (left_chunk_end < len) {
        bool pushed = q->push(ShenandoahMarkTask(array, left_chunk, pow));
        assert(pushed, "overflow queue should always succeed pushing");
        chunk = right_chunk;
        last_idx = left_chunk_end;
      } else {
        chunk = left_chunk;
      }
    }

    // Process the irregular tail, if present
    int from = last_idx;
    if (from < len) {
      array->oop_iterate_range(cl, from, len);
    }
  }
}

template <class T>
inline void ShenandoahConcurrentMark::do_chunked_array(ShenandoahObjToScanQueue* q, T* cl, oop obj, int chunk, int pow) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);

  assert (ObjArrayMarkingStride > 0, "sanity");

  // Split out tasks, as suggested in ObjArrayChunkedTask docs. Avoid pushing tasks that
  // are known to start beyond the array.
  while ((1 << pow) > (int)ObjArrayMarkingStride && (chunk*2 < ShenandoahMarkTask::chunk_size())) {
    pow--;
    chunk *= 2;
    bool pushed = q->push(ShenandoahMarkTask(array, chunk - 1, pow));
    assert(pushed, "overflow queue should always succeed pushing");
  }

  int chunk_size = 1 << pow;

  int from = (chunk - 1) * chunk_size;
  int to = chunk * chunk_size;

#ifdef ASSERT
  int len = array->length();
  assert (0 <= from && from < len, err_msg("from is sane: %d/%d", from, len));
  assert (0 < to && to <= len, err_msg("to is sane: %d/%d", to, len));
#endif

  array->oop_iterate_range(cl, from, to);
}

inline bool ShenandoahConcurrentMark::try_queue(ShenandoahObjToScanQueue* q, ShenandoahMarkTask &task) {
  return (q->pop_buffer(task) ||
          q->pop_local(task) ||
          q->pop_overflow(task));
}

class ShenandoahSATBBufferClosure : public SATBBufferClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahSATBBufferClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->next_marking_context())
  {
  }

  void do_buffer(void **buffer, size_t size) {
    if (_heap->has_forwarded_objects()) {
      do_buffer_impl<RESOLVE>(buffer, size);
    } else {
      do_buffer_impl<NONE>(buffer, size);
    }
  }

  template<UpdateRefsMode UPDATE_REFS>
  void do_buffer_impl(void **buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      oop *p = (oop *) &buffer[i];
      ShenandoahConcurrentMark::mark_through_ref<oop, UPDATE_REFS>(p, _heap, _queue, _mark_context);
    }
  }
};

template<class T, UpdateRefsMode UPDATE_REFS>
inline void ShenandoahConcurrentMark::mark_through_ref(T *p, ShenandoahHeap* heap, ShenandoahObjToScanQueue* q, ShenandoahMarkingContext* const mark_context) {
  ShenandoahConcurrentMark::mark_through_ref<T, UPDATE_REFS, false /* string dedup */>(p, heap, q, mark_context, NULL);
}

template<class T, UpdateRefsMode UPDATE_REFS, bool STRING_DEDUP>
inline void ShenandoahConcurrentMark::mark_through_ref(T *p, ShenandoahHeap* heap, ShenandoahObjToScanQueue* q, ShenandoahMarkingContext* const mark_context, ShenandoahStrDedupQueue* dq) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    switch (UPDATE_REFS) {
    case NONE:
      break;
    case RESOLVE:
      obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      break;
    case SIMPLE:
      // We piggy-back reference updating to the marking tasks.
      obj = heap->update_with_forwarded_not_null(p, obj);
      break;
    case CONCURRENT:
      obj = heap->maybe_update_with_forwarded_not_null(p, obj);
      break;
    default:
      ShouldNotReachHere();
    }

    // Note: Only when concurrently updating references can obj become NULL here.
    // It happens when a mutator thread beats us by writing another value. In that
    // case we don't need to do anything else.
    if (UPDATE_REFS != CONCURRENT || !oopDesc::is_null(obj)) {
      shenandoah_assert_not_forwarded(p, obj);
      shenandoah_assert_not_in_cset_except(p, obj, heap->cancelled_gc());

      if (mark_context->mark(obj)) {
        bool pushed = q->push(ShenandoahMarkTask(obj));
        assert(pushed, "overflow queue should always succeed pushing");

        if (STRING_DEDUP && ShenandoahStringDedup::is_candidate(obj)) {
          assert(ShenandoahStringDedup::is_enabled(), "Must be enabled");
          assert(dq != NULL, "Dedup queue not set");
          ShenandoahStringDedup::enqueue_candidate(obj, dq);
        }
      }

      shenandoah_assert_marked_next(p, obj);
    }
  }
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
