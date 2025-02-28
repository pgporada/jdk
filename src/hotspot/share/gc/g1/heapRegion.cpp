/*
 * Copyright (c) 2001, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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

#include "precompiled.hpp"
#include "code/nmethod.hpp"
#include "gc/g1/g1Allocator.inline.hpp"
#include "gc/g1/g1BlockOffsetTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1HeapRegionTraceType.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionBounds.inline.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "gc/g1/heapRegionTracer.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/powerOfTwo.hpp"

int    HeapRegion::LogOfHRGrainBytes = 0;
int    HeapRegion::LogCardsPerRegion = 0;
size_t HeapRegion::GrainBytes        = 0;
size_t HeapRegion::GrainWords        = 0;
size_t HeapRegion::CardsPerRegion    = 0;

size_t HeapRegion::max_region_size() {
  return HeapRegionBounds::max_size();
}

size_t HeapRegion::min_region_size_in_words() {
  return HeapRegionBounds::min_size() >> LogHeapWordSize;
}

void HeapRegion::setup_heap_region_size(size_t max_heap_size) {
  size_t region_size = G1HeapRegionSize;
  // G1HeapRegionSize = 0 means decide ergonomically.
  if (region_size == 0) {
    region_size = clamp(max_heap_size / HeapRegionBounds::target_number(),
                        HeapRegionBounds::min_size(),
                        HeapRegionBounds::max_ergonomics_size());
  }

  // Make sure region size is a power of 2. Rounding up since this
  // is beneficial in most cases.
  region_size = round_up_power_of_2(region_size);

  // Now make sure that we don't go over or under our limits.
  region_size = clamp(region_size, HeapRegionBounds::min_size(), HeapRegionBounds::max_size());

  // Calculate the log for the region size.
  int region_size_log = log2i_exact(region_size);

  // Now, set up the globals.
  guarantee(LogOfHRGrainBytes == 0, "we should only set it once");
  LogOfHRGrainBytes = region_size_log;

  guarantee(GrainBytes == 0, "we should only set it once");
  GrainBytes = region_size;

  guarantee(GrainWords == 0, "we should only set it once");
  GrainWords = GrainBytes >> LogHeapWordSize;

  guarantee(CardsPerRegion == 0, "we should only set it once");
  CardsPerRegion = GrainBytes >> G1CardTable::card_shift();

  LogCardsPerRegion = log2i(CardsPerRegion);

  if (G1HeapRegionSize != GrainBytes) {
    FLAG_SET_ERGO(G1HeapRegionSize, GrainBytes);
  }
}

void HeapRegion::handle_evacuation_failure() {
  uninstall_surv_rate_group();
  clear_young_index_in_cset();
  clear_index_in_opt_cset();
  move_to_old();

  _rem_set->clean_code_roots(this);
  _rem_set->clear_locked(true /* only_cardset */);
}

void HeapRegion::unlink_from_list() {
  set_next(NULL);
  set_prev(NULL);
  set_containing_set(NULL);
}

void HeapRegion::hr_clear(bool clear_space) {
  assert(_humongous_start_region == NULL,
         "we should have already filtered out humongous regions");

  clear_young_index_in_cset();
  clear_index_in_opt_cset();
  uninstall_surv_rate_group();
  set_free();
  reset_pre_dummy_top();

  rem_set()->clear_locked();

  init_top_at_mark_start();
  if (clear_space) clear(SpaceDecorator::Mangle);

  _gc_efficiency = -1.0;
}

void HeapRegion::clear_cardtable() {
  G1CardTable* ct = G1CollectedHeap::heap()->card_table();
  ct->clear(MemRegion(bottom(), end()));
}

void HeapRegion::calc_gc_efficiency() {
  // GC efficiency is the ratio of how much space would be
  // reclaimed over how long we predict it would take to reclaim it.
  G1Policy* policy = G1CollectedHeap::heap()->policy();

  // Retrieve a prediction of the elapsed time for this region for
  // a mixed gc because the region will only be evacuated during a
  // mixed gc.
  double region_elapsed_time_ms = policy->predict_region_total_time_ms(this, false /* for_young_only_phase */);
  _gc_efficiency = (double) reclaimable_bytes() / region_elapsed_time_ms;
}

void HeapRegion::set_free() {
  report_region_type_change(G1HeapRegionTraceType::Free);
  _type.set_free();
}

void HeapRegion::set_eden() {
  report_region_type_change(G1HeapRegionTraceType::Eden);
  _type.set_eden();
}

void HeapRegion::set_eden_pre_gc() {
  report_region_type_change(G1HeapRegionTraceType::Eden);
  _type.set_eden_pre_gc();
}

void HeapRegion::set_survivor() {
  report_region_type_change(G1HeapRegionTraceType::Survivor);
  _type.set_survivor();
}

void HeapRegion::move_to_old() {
  if (_type.relabel_as_old()) {
    report_region_type_change(G1HeapRegionTraceType::Old);
  }
}

void HeapRegion::set_old() {
  report_region_type_change(G1HeapRegionTraceType::Old);
  _type.set_old();
}

void HeapRegion::set_open_archive() {
  report_region_type_change(G1HeapRegionTraceType::OpenArchive);
  _type.set_open_archive();
}

void HeapRegion::set_closed_archive() {
  report_region_type_change(G1HeapRegionTraceType::ClosedArchive);
  _type.set_closed_archive();
}

void HeapRegion::set_starts_humongous(HeapWord* obj_top, size_t fill_size) {
  assert(!is_humongous(), "sanity / pre-condition");
  assert(top() == bottom(), "should be empty");

  report_region_type_change(G1HeapRegionTraceType::StartsHumongous);
  _type.set_starts_humongous();
  _humongous_start_region = this;

  _bot_part.set_for_starts_humongous(obj_top, fill_size);
}

void HeapRegion::set_continues_humongous(HeapRegion* first_hr) {
  assert(!is_humongous(), "sanity / pre-condition");
  assert(top() == bottom(), "should be empty");
  assert(first_hr->is_starts_humongous(), "pre-condition");

  report_region_type_change(G1HeapRegionTraceType::ContinuesHumongous);
  _type.set_continues_humongous();
  _humongous_start_region = first_hr;
}

void HeapRegion::clear_humongous() {
  assert(is_humongous(), "pre-condition");

  assert(capacity() == HeapRegion::GrainBytes, "pre-condition");
  _humongous_start_region = NULL;
}

void HeapRegion::prepare_remset_for_scan() {
  return _rem_set->reset_table_scanner();
}

HeapRegion::HeapRegion(uint hrm_index,
                       G1BlockOffsetTable* bot,
                       MemRegion mr,
                       G1CardSetConfiguration* config) :
  _bottom(mr.start()),
  _end(mr.end()),
  _top(NULL),
  _bot_part(bot, this),
  _pre_dummy_top(NULL),
  _rem_set(NULL),
  _hrm_index(hrm_index),
  _type(),
  _humongous_start_region(NULL),
  _index_in_opt_cset(InvalidCSetIndex),
  _next(NULL), _prev(NULL),
#ifdef ASSERT
  _containing_set(NULL),
#endif
  _top_at_mark_start(NULL),
  _parsable_bottom(NULL),
  _garbage_bytes(0),
  _young_index_in_cset(-1),
  _surv_rate_group(NULL), _age_index(G1SurvRateGroup::InvalidAgeIndex), _gc_efficiency(-1.0),
  _node_index(G1NUMA::UnknownNodeIndex)
{
  assert(Universe::on_page_boundary(mr.start()) && Universe::on_page_boundary(mr.end()),
         "invalid space boundaries");

  _rem_set = new HeapRegionRemSet(this, config);
  initialize();
}

void HeapRegion::initialize(bool clear_space, bool mangle_space) {
  assert(_rem_set->is_empty(), "Remembered set must be empty");

  if (clear_space) {
    clear(mangle_space);
  }

  set_top(bottom());

  hr_clear(false /*clear_space*/);
}

void HeapRegion::report_region_type_change(G1HeapRegionTraceType::Type to) {
  HeapRegionTracer::send_region_type_change(_hrm_index,
                                            get_trace_type(),
                                            to,
                                            (uintptr_t)bottom(),
                                            used());
}

void HeapRegion::note_evacuation_failure(bool during_concurrent_start) {
  // PB must be bottom - we only evacuate old gen regions after scrubbing, and
  // young gen regions never have their PB set to anything other than bottom.
  assert(parsable_bottom_acquire() == bottom(), "must be");

  _garbage_bytes = 0;

  if (during_concurrent_start) {
    // Self-forwarding marks all objects. Adjust TAMS so that these marks are
    // below it.
    set_top_at_mark_start(top());
  } else {
    // Outside of the mixed phase all regions that had an evacuation failure must
    // be young regions, and their TAMS is always bottom. Similarly, before the
    // start of the mixed phase, we scrubbed and reset TAMS to bottom.
    assert(top_at_mark_start() == bottom(), "must be");
  }
}

void HeapRegion::note_self_forward_chunk_done(size_t garbage_bytes) {
  Atomic::add(&_garbage_bytes, garbage_bytes, memory_order_relaxed);
}

// Code roots support
void HeapRegion::add_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_code_root(nm);
}

void HeapRegion::add_code_root_locked(nmethod* nm) {
  assert_locked_or_safepoint(CodeCache_lock);
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_code_root_locked(nm);
}

void HeapRegion::remove_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->remove_code_root(nm);
}

void HeapRegion::code_roots_do(CodeBlobClosure* blk) const {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->code_roots_do(blk);
}

class VerifyCodeRootOopClosure: public OopClosure {
  const HeapRegion* _hr;
  bool _failures;
  bool _has_oops_in_region;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);

      // Note: not all the oops embedded in the nmethod are in the
      // current region. We only look at those which are.
      if (_hr->is_in(obj)) {
        // Object is in the region. Check that its less than top
        if (_hr->top() <= cast_from_oop<HeapWord*>(obj)) {
          // Object is above top
          log_error(gc, verify)("Object " PTR_FORMAT " in region " HR_FORMAT " is above top ",
                                p2i(obj), HR_FORMAT_PARAMS(_hr));
          _failures = true;
          return;
        }
        // Nmethod has at least one oop in the current region
        _has_oops_in_region = true;
      }
    }
  }

public:
  VerifyCodeRootOopClosure(const HeapRegion* hr):
    _hr(hr), _failures(false), _has_oops_in_region(false) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }

  bool failures()           { return _failures; }
  bool has_oops_in_region() { return _has_oops_in_region; }
};

class VerifyCodeRootCodeBlobClosure: public CodeBlobClosure {
  const HeapRegion* _hr;
  bool _failures;
public:
  VerifyCodeRootCodeBlobClosure(const HeapRegion* hr) :
    _hr(hr), _failures(false) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = (cb == NULL) ? NULL : cb->as_compiled_method()->as_nmethod_or_null();
    if (nm != NULL) {
      // Verify that the nemthod is live
      VerifyCodeRootOopClosure oop_cl(_hr);
      nm->oops_do(&oop_cl);
      if (!oop_cl.has_oops_in_region()) {
        log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] has nmethod " PTR_FORMAT " in its code roots with no pointers into region",
                              p2i(_hr->bottom()), p2i(_hr->end()), p2i(nm));
        _failures = true;
      } else if (oop_cl.failures()) {
        log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] has other failures for nmethod " PTR_FORMAT,
                              p2i(_hr->bottom()), p2i(_hr->end()), p2i(nm));
        _failures = true;
      }
    }
  }

  bool failures()       { return _failures; }
};

void HeapRegion::verify_code_roots(VerifyOption vo, bool* failures) const {
  if (!G1VerifyHeapRegionCodeRoots) {
    // We're not verifying code roots.
    return;
  }
  if (vo == VerifyOption::G1UseFullMarking) {
    // Marking verification during a full GC is performed after class
    // unloading, code cache unloading, etc so the code roots
    // attached to each heap region are in an inconsistent state. They won't
    // be consistent until the code roots are rebuilt after the
    // actual GC. Skip verifying the code roots in this particular
    // time.
    assert(VerifyDuringGC, "only way to get here");
    return;
  }

  HeapRegionRemSet* hrrs = rem_set();
  size_t code_roots_length = hrrs->code_roots_list_length();

  // if this region is empty then there should be no entries
  // on its code root list
  if (is_empty()) {
    if (code_roots_length > 0) {
      log_error(gc, verify)("region " HR_FORMAT " is empty but has " SIZE_FORMAT " code root entries",
                            HR_FORMAT_PARAMS(this), code_roots_length);
      *failures = true;
    }
    return;
  }

  if (is_continues_humongous()) {
    if (code_roots_length > 0) {
      log_error(gc, verify)("region " HR_FORMAT " is a continuation of a humongous region but has " SIZE_FORMAT " code root entries",
                            HR_FORMAT_PARAMS(this), code_roots_length);
      *failures = true;
    }
    return;
  }

  VerifyCodeRootCodeBlobClosure cb_cl(this);
  code_roots_do(&cb_cl);

  if (cb_cl.failures()) {
    *failures = true;
  }
}

void HeapRegion::print() const { print_on(tty); }

void HeapRegion::print_on(outputStream* st) const {
  st->print("|%4u", this->_hrm_index);
  st->print("|" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT,
            p2i(bottom()), p2i(top()), p2i(end()));
  st->print("|%3d%%", (int) ((double) used() * 100 / capacity()));
  st->print("|%2s", get_short_type_str());
  if (in_collection_set()) {
    st->print("|CS");
  } else {
    st->print("|  ");
  }
  st->print("|TAMS " PTR_FORMAT "| PB " PTR_FORMAT "| %s ",
            p2i(top_at_mark_start()), p2i(parsable_bottom_acquire()), rem_set()->get_state_str());
  if (UseNUMA) {
    G1NUMA* numa = G1NUMA::numa();
    if (node_index() < numa->num_active_nodes()) {
      st->print("|%d", numa->numa_id(node_index()));
    } else {
      st->print("|-");
    }
  }
  st->print_cr("");
}

class G1VerificationClosure : public BasicOopIterateClosure {
protected:
  G1CollectedHeap* _g1h;
  G1CardTable *_ct;
  oop _containing_obj;
  bool _failures;
  int _n_failures;
  VerifyOption _vo;

public:

  G1VerificationClosure(G1CollectedHeap* g1h, VerifyOption vo) :
    _g1h(g1h), _ct(g1h->card_table()),
    _containing_obj(NULL), _failures(false), _n_failures(0), _vo(vo) {
  }

  void set_containing_obj(oop obj) {
    _containing_obj = obj;
  }

  bool failures() { return _failures; }
  int n_failures() { return _n_failures; }

  void print_object(outputStream* out, oop obj) {
#ifdef PRODUCT
    Klass* k = obj->klass();
    const char* class_name = k->external_name();
    out->print_cr("class name %s", class_name);
#else // PRODUCT
    obj->print_on(out);
#endif // PRODUCT
  }
};

class VerifyLiveClosure : public G1VerificationClosure {
public:
  VerifyLiveClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_liveness(p);
  }

  template <class T>
  void verify_liveness(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    Log(gc, verify) log;
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      bool failed = false;
      bool is_in_heap = _g1h->is_in(obj);
      if (!is_in_heap || _g1h->is_obj_dead_cond(obj, _vo)) {
        MutexLocker x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);

        if (!_failures) {
          log.error("----------");
        }
        ResourceMark rm;
        if (!is_in_heap) {
          HeapRegion* from = _g1h->heap_region_containing(p);
          log.error("Field " PTR_FORMAT " of live obj " PTR_FORMAT " in region " HR_FORMAT,
                    p2i(p), p2i(_containing_obj), HR_FORMAT_PARAMS(from));
          LogStream ls(log.error());
          print_object(&ls, _containing_obj);
          HeapRegion* const to = _g1h->heap_region_containing(obj);
          log.error("points to obj " PTR_FORMAT " in region " HR_FORMAT " remset %s",
                    p2i(obj), HR_FORMAT_PARAMS(to), to->rem_set()->get_state_str());
        } else {
          HeapRegion* from = _g1h->heap_region_containing(p);
          HeapRegion* to = _g1h->heap_region_containing(obj);
          log.error("Field " PTR_FORMAT " of live obj " PTR_FORMAT " in region " HR_FORMAT,
                    p2i(p), p2i(_containing_obj), HR_FORMAT_PARAMS(from));
          LogStream ls(log.error());
          print_object(&ls, _containing_obj);
          log.error("points to dead obj " PTR_FORMAT " in region " HR_FORMAT,
                    p2i(obj), HR_FORMAT_PARAMS(to));
          print_object(&ls, obj);
        }
        log.error("----------");
        _failures = true;
        failed = true;
        _n_failures++;
      }
    }
  }
};

class VerifyRemSetClosure : public G1VerificationClosure {
public:
  VerifyRemSetClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_remembered_set(p);
  }

  template <class T>
  void verify_remembered_set(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    Log(gc, verify) log;
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      HeapRegion* from = _g1h->heap_region_containing(p);
      HeapRegion* to = _g1h->heap_region_containing(obj);
      if (from != NULL && to != NULL &&
        from != to &&
        !to->is_pinned() &&
        to->rem_set()->is_complete()) {
        jbyte cv_obj = *_ct->byte_for_const(_containing_obj);
        jbyte cv_field = *_ct->byte_for_const(p);
        const jbyte dirty = G1CardTable::dirty_card_val();

        bool is_bad = !(from->is_young()
          || to->rem_set()->contains_reference(p)
          || (_containing_obj->is_objArray() ?
                cv_field == dirty :
                cv_obj == dirty || cv_field == dirty));
        if (is_bad) {
          MutexLocker x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);

          if (!_failures) {
            log.error("----------");
          }
          log.error("Missing rem set entry:");
          log.error("Field " PTR_FORMAT " of obj " PTR_FORMAT " in region " HR_FORMAT,
                    p2i(p), p2i(_containing_obj), HR_FORMAT_PARAMS(from));
          ResourceMark rm;
          LogStream ls(log.error());
          _containing_obj->print_on(&ls);
          log.error("points to obj " PTR_FORMAT " in region " HR_FORMAT " remset %s",
                    p2i(obj), HR_FORMAT_PARAMS(to), to->rem_set()->get_state_str());
          if (oopDesc::is_oop(obj)) {
            obj->print_on(&ls);
          }
          log.error("Obj head CTE = %d, field CTE = %d.", cv_obj, cv_field);
          log.error("----------");
          _failures = true;
          _n_failures++;
        }
      }
    }
  }
};

// Closure that applies the given two closures in sequence.
class G1Mux2Closure : public BasicOopIterateClosure {
  OopClosure* _c1;
  OopClosure* _c2;
public:
  G1Mux2Closure(OopClosure *c1, OopClosure *c2) { _c1 = c1; _c2 = c2; }
  template <class T> inline void do_oop_work(T* p) {
    // Apply first closure; then apply the second.
    _c1->do_oop(p);
    _c2->do_oop(p);
  }
  virtual inline void do_oop(oop* p) { do_oop_work(p); }
  virtual inline void do_oop(narrowOop* p) { do_oop_work(p); }
};

void HeapRegion::verify(VerifyOption vo,
                        bool* failures) const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyLiveClosure vl_cl(g1h, vo);
  VerifyRemSetClosure vr_cl(g1h, vo);
  bool is_region_humongous = is_humongous();
  // We cast p to an oop, so region-bottom must be an obj-start.
  assert(!is_region_humongous || is_starts_humongous(), "invariant");
  size_t object_num = 0;
  while (p < top()) {
    oop obj = cast_to_oop(p);
    size_t obj_size = block_size(p);
    object_num += 1;

    if (!g1h->is_obj_dead_cond(obj, this, vo)) {
      if (oopDesc::is_oop(obj)) {
        Klass* klass = obj->klass();
        bool is_metaspace_object = Metaspace::contains(klass);
        if (!is_metaspace_object) {
          log_error(gc, verify)("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                "not metadata", p2i(klass), p2i(obj));
          *failures = true;
          return;
        } else if (!klass->is_klass()) {
          log_error(gc, verify)("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                "not a klass", p2i(klass), p2i(obj));
          *failures = true;
          return;
        } else {
          vl_cl.set_containing_obj(obj);
          if (!g1h->collector_state()->in_full_gc() || G1VerifyRSetsDuringFullGC) {
            // verify liveness and rem_set
            vr_cl.set_containing_obj(obj);
            G1Mux2Closure mux(&vl_cl, &vr_cl);
            obj->oop_iterate(&mux);

            if (vr_cl.failures()) {
              *failures = true;
            }
            if (G1MaxVerifyFailures >= 0 &&
              vr_cl.n_failures() >= G1MaxVerifyFailures) {
              return;
            }
          } else {
            // verify only liveness
            obj->oop_iterate(&vl_cl);
          }
          if (vl_cl.failures()) {
            *failures = true;
          }
          if (G1MaxVerifyFailures >= 0 &&
              vl_cl.n_failures() >= G1MaxVerifyFailures) {
            return;
          }
        }
      } else {
        log_error(gc, verify)(PTR_FORMAT " not an oop", p2i(obj));
        *failures = true;
        return;
      }
    }
    prev_p = p;
    p += obj_size;
  }

  // Only regions in old generation contain valid BOT.
  if (!is_empty() && !is_young()) {
    _bot_part.verify();
  }

  if (is_region_humongous) {
    oop obj = cast_to_oop(this->humongous_start_region()->bottom());
    if (cast_from_oop<HeapWord*>(obj) > bottom() || cast_from_oop<HeapWord*>(obj) + obj->size() < bottom()) {
      log_error(gc, verify)("this humongous region is not part of its' humongous object " PTR_FORMAT, p2i(obj));
      *failures = true;
      return;
    }
  }

  if (!is_region_humongous && p != top()) {
    log_error(gc, verify)("end of last object " PTR_FORMAT " "
                          "does not match top " PTR_FORMAT, p2i(p), p2i(top()));
    *failures = true;
    return;
  }

  verify_code_roots(vo, failures);
}

void HeapRegion::verify_rem_set(VerifyOption vo, bool* failures) const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyRemSetClosure vr_cl(g1h, vo);
  while (p < top()) {
    oop obj = cast_to_oop(p);
    size_t obj_size = block_size(p);

    if (!g1h->is_obj_dead_cond(obj, this, vo)) {
      if (oopDesc::is_oop(obj)) {
        vr_cl.set_containing_obj(obj);
        obj->oop_iterate(&vr_cl);

        if (vr_cl.failures()) {
          *failures = true;
        }
        if (G1MaxVerifyFailures >= 0 &&
          vr_cl.n_failures() >= G1MaxVerifyFailures) {
          return;
        }
      } else {
        log_error(gc, verify)(PTR_FORMAT " not an oop", p2i(obj));
        *failures = true;
        return;
      }
    }

    prev_p = p;
    p += obj_size;
  }
}

void HeapRegion::verify_rem_set() const {
  bool failures = false;
  verify_rem_set(VerifyOption::G1UseConcMarking, &failures);
  guarantee(!failures, "HeapRegion RemSet verification failed");
}

void HeapRegion::clear(bool mangle_space) {
  set_top(bottom());

  if (ZapUnusedHeapArea && mangle_space) {
    mangle_unused_area();
  }
}

#ifndef PRODUCT
void HeapRegion::mangle_unused_area() {
  SpaceMangler::mangle_region(MemRegion(top(), end()));
}
#endif

void HeapRegion::update_bot_for_block(HeapWord* start, HeapWord* end) {
  _bot_part.update_for_block(start, end);
}

void HeapRegion::object_iterate(ObjectClosure* blk) {
  HeapWord* p = bottom();
  while (p < top()) {
    if (block_is_obj(p, parsable_bottom())) {
      blk->do_object(cast_to_oop(p));
    }
    p += block_size(p);
  }
}

void HeapRegion::fill_with_dummy_object(HeapWord* address, size_t word_size, bool zap) {
  // Keep the BOT in sync for old generation regions.
  if (is_old()) {
    update_bot_for_obj(address, word_size);
  }
  // Fill in the object.
  CollectedHeap::fill_with_object(address, word_size, zap);
}

void HeapRegion::fill_range_with_dead_objects(HeapWord* start, HeapWord* end) {
  size_t range_size = pointer_delta(end, start);

  // Fill the dead range with objects. G1 might need to create two objects if
  // the range is larger than half a region, which is the max_fill_size().
  CollectedHeap::fill_with_objects(start, range_size);
  HeapWord* current = start;
  do {
    // Update the BOT if the a threshold is crossed.
    size_t obj_size = cast_to_oop(current)->size();
    update_bot_for_block(current, current + obj_size);

    // Advance to the next object.
    current += obj_size;
    guarantee(current <= end, "Should never go past end");
  } while (current != end);
}
