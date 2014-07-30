// Copyright (c) 2012, Cloudera, inc.

#include <boost/thread/thread.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <string>
#include <vector>

#include "kudu/common/common.pb.h"
#include "kudu/common/generic_iterators.h"
#include "kudu/common/row.h"
#include "kudu/consensus/opid_anchor_registry.h"
#include "kudu/gutil/atomicops.h"
#include "kudu/gutil/dynamic_annotations.h"
#include "kudu/tablet/memrowset.h"
#include "kudu/tablet/compaction.h"
#include "kudu/util/mem_tracker.h"

DEFINE_int32(memrowset_throttle_mb, 0,
             "number of MB of RAM beyond which memrowset inserts will be throttled");


namespace kudu { namespace tablet {

using consensus::OpId;
using log::OpIdAnchorRegistry;
using log::OpIdLessThan;
using std::pair;
using strings::Substitute;

static const int kInitialArenaSize = 1536*1024;
static const int kMaxArenaBufferSize = 8*1024*1024;

bool MRSRow::IsGhost() const {
  bool is_ghost = false;
  for (const Mutation *mut = header_->redo_head;
       mut != NULL;
       mut = mut->next()) {
    RowChangeListDecoder decoder(schema(), mut->changelist());
    Status s = decoder.Init();
    if (!PREDICT_TRUE(s.ok())) {
      LOG(FATAL) << "Failed to decode: " << mut->changelist().ToString(*schema())
                  << " (" << s.ToString() << ")";
    }
    if (decoder.is_delete()) {
      DCHECK(!is_ghost);
      is_ghost = true;
    } else if (decoder.is_reinsert()) {
      DCHECK(is_ghost);
      is_ghost = false;
    }
  }
  return is_ghost;
}

namespace {

shared_ptr<MemTracker> CreateMemTrackerForMemRowSet(int64_t id,
                                                    MemTracker* parent_tracker) {
  string mem_tracker_id = Substitute("MemRowSet-$0", id);
  if (parent_tracker != NULL) {
    mem_tracker_id = Substitute("$0-$1", parent_tracker->id(), mem_tracker_id);
  }
  return MemTracker::CreateTracker(-1, mem_tracker_id, parent_tracker);
}

} // anonymous namespace

MemRowSet::MemRowSet(int64_t id,
                     const Schema &schema,
                     OpIdAnchorRegistry* opid_anchor_registry,
                     const shared_ptr<MemTracker>& parent_tracker)
  : id_(id),
    schema_(schema),
    parent_tracker_(parent_tracker),
    mem_tracker_(CreateMemTrackerForMemRowSet(id, parent_tracker.get())),
    allocator_(new MemoryTrackingBufferAllocator(HeapBufferAllocator::Get(), mem_tracker_)),
    arena_(new ThreadSafeMemoryTrackingArena(kInitialArenaSize, kMaxArenaBufferSize,
                                             allocator_)),
    tree_(arena_),
    debug_insert_count_(0),
    debug_update_count_(0),
    has_logged_throttling_(false),
    anchorer_(opid_anchor_registry, Substitute("MemRowSet-$0", id_)) {
  CHECK(schema.has_column_ids());
  ANNOTATE_BENIGN_RACE(&debug_insert_count_, "insert count isnt accurate");
  ANNOTATE_BENIGN_RACE(&debug_update_count_, "update count isnt accurate");
}

Status MemRowSet::AlterSchema(const Schema& schema) {
  // The MemRowSet is flushed and re-created with the new Schema.
  // See Tablet::AlterSchema()
  return Status::NotSupported("AlterSchema not supported by MemRowSet");
}

Status MemRowSet::DebugDump(vector<string> *lines) {
  gscoped_ptr<Iterator> iter(NewIterator());
  RETURN_NOT_OK(iter->Init(NULL));
  while (iter->HasNext()) {
    MRSRow row = iter->GetCurrentRow();
    LOG_STRING(INFO, lines)
      << "@" << row.insertion_timestamp() << ": row "
      << schema_.DebugRow(row)
      << " mutations=" << Mutation::StringifyMutationList(schema_, row.header_->redo_head)
      << std::endl;
    iter->Next();
  }

  return Status::OK();
}


Status MemRowSet::Insert(Timestamp timestamp,
                         const ConstContiguousRow& row,
                         const OpId& op_id) {
  CHECK(row.schema()->has_column_ids());
  DCHECK_SCHEMA_EQ(schema_, *row.schema());

  faststring enc_key_buf;
  schema_.EncodeComparableKey(row, &enc_key_buf);
  Slice enc_key(enc_key_buf);

  btree::PreparedMutation<MSBTreeTraits> mutation(enc_key);
  mutation.Prepare(&tree_);

  // TODO: for now, the key ends up stored doubly --
  // once encoded in the btree key, and again in the value
  // (unencoded).
  // That's not very memory-efficient!

  if (mutation.exists()) {
    // It's OK for it to exist if it's just a "ghost" row -- i.e the
    // row is deleted.
    MRSRow ms_row(this, mutation.current_mutable_value());
    if (!ms_row.IsGhost()) {
      return Status::AlreadyPresent("entry already present in memrowset");
    }

    // Insert a "reinsert" mutation.
    return Reinsert(timestamp, row, &ms_row);
  }

  // Copy the non-encoded key onto the stack since we need
  // to mutate it when we relocate its Slices into our arena.
  DEFINE_MRSROW_ON_STACK(this, mrsrow, mrsrow_slice);
  mrsrow.header_->insertion_timestamp = timestamp;
  mrsrow.header_->redo_head = NULL;
  RETURN_NOT_OK(mrsrow.CopyRow(row, arena_.get()));

  CHECK(mutation.Insert(mrsrow_slice))
    << "Expected to be able to insert, since the prepared mutation "
    << "succeeded!";

  anchorer_.AnchorIfMinimum(op_id);

  debug_insert_count_++;
  SlowMutators();
  return Status::OK();
}

Status MemRowSet::Reinsert(Timestamp timestamp, const ConstContiguousRow& row, MRSRow *ms_row) {
  DCHECK_SCHEMA_EQ(schema_, *row.schema());

  // TODO(perf): This path makes some unnecessary copies that could be reduced,
  // but let's assume that REINSERT is really rare and code for clarity over speed
  // here.

  // Make a copy of the row, and relocate any of its indirected data into
  // our Arena.
  DEFINE_MRSROW_ON_STACK(this, row_copy, row_copy_slice);
  RETURN_NOT_OK(row_copy.CopyRow(row, arena_.get()));

  // Encode the REINSERT mutation from the relocated row copy.
  faststring buf;
  RowChangeListEncoder encoder(&schema_, &buf);
  encoder.SetToReinsert(row_copy.row_slice());

  // Move the REINSERT mutation itself into our Arena.
  Mutation *mut = Mutation::CreateInArena(arena_.get(), timestamp, encoder.as_changelist());

  // Append the mutation into the row's mutation list.
  // This function has "release" semantics which ensures that the memory writes
  // for the mutation are fully published before any concurrent reader sees
  // the appended mutation.
  mut->AppendToListAtomic(&ms_row->header_->redo_head);
  return Status::OK();
}

Status MemRowSet::MutateRow(Timestamp timestamp,
                            const RowSetKeyProbe &probe,
                            const RowChangeList &delta,
                            const consensus::OpId& op_id,
                            ProbeStats* stats,
                            OperationResultPB *result) {
  {
    btree::PreparedMutation<MSBTreeTraits> mutation(probe.encoded_key_slice());
    mutation.Prepare(&tree_);

    if (!mutation.exists()) {
      return Status::NotFound("not in memrowset");
    }

    MRSRow row(this, mutation.current_mutable_value());

    // If the row exists, it may still be a "ghost" row -- i.e a row
    // that's been deleted. If that's the case, we should treat it as
    // NotFound.
    if (row.IsGhost()) {
      return Status::NotFound("not in memrowset (ghost)");
    }

    // Append to the linked list of mutations for this row.
    Mutation *mut = Mutation::CreateInArena(arena_.get(), timestamp, delta);

    // This function has "release" semantics which ensures that the memory writes
    // for the mutation are fully published before any concurrent reader sees
    // the appended mutation.
    mut->AppendToListAtomic(&row.header_->redo_head);

    MemStoreTargetPB* target = result->add_mutated_stores();
    target->set_mrs_id(id_);
  }

  stats->mrs_consulted++;

  anchorer_.AnchorIfMinimum(op_id);

  // Throttle the writer if we're low on memory, but do this outside of the lock
  // so we don't slow down readers.
  debug_update_count_++;
  SlowMutators();
  return Status::OK();
}

Status MemRowSet::CheckRowPresent(const RowSetKeyProbe &probe, bool *present,
                                  ProbeStats* stats) const {
  // Use a PreparedMutation here even though we don't plan to mutate. Even though
  // this takes a lock rather than an optimistic copy, it should be a very short
  // critical section, and this call is only made on updates, which are rare.

  stats->mrs_consulted++;

  btree::PreparedMutation<MSBTreeTraits> mutation(probe.encoded_key_slice());
  mutation.Prepare(const_cast<MSBTree *>(&tree_));

  if (!mutation.exists()) {
    *present = false;
    return Status::OK();
  }

  // TODO(perf): using current_mutable_value() will actually change the data's
  // version number, even though we're not going to do any mutation. This would
  // make concurrent readers retry, even though they don't have to (we aren't
  // actually mutating anything here!)
  MRSRow row(this, mutation.current_mutable_value());

  // If the row exists, it may still be a "ghost" row -- i.e a row
  // that's been deleted. If that's the case, we should treat it as
  // NotFound.
  *present = !row.IsGhost();
  return Status::OK();
}

void MemRowSet::SlowMutators() {
  if (FLAGS_memrowset_throttle_mb == 0) return;

  ssize_t over_mem = memory_footprint() - FLAGS_memrowset_throttle_mb * 1024 * 1024;
  if (over_mem > 0) {
    if (!has_logged_throttling_ &&
        base::subtle::NoBarrier_AtomicExchange(&has_logged_throttling_, 1) == 0) {
      LOG(WARNING) << "Throttling memrowset insert rate";
    }

    size_t us_to_sleep = over_mem / 1024 / 512;
    boost::this_thread::sleep(boost::posix_time::microseconds(us_to_sleep));
  }
}

MemRowSet::Iterator *MemRowSet::NewIterator(const Schema *projection,
                                            const MvccSnapshot &snap) const {
  return new MemRowSet::Iterator(shared_from_this(), tree_.NewIterator(),
                                projection, snap);
}

MemRowSet::Iterator *MemRowSet::NewIterator() const {
  // TODO: can we kill this function? should be only used by tests?
  return NewIterator(&schema(), MvccSnapshot::CreateSnapshotIncludingAllTransactions());
}

RowwiseIterator *MemRowSet::NewRowIterator(const Schema *projection,
                                           const MvccSnapshot &snap) const {
  return NewIterator(projection, snap);
}

CompactionInput *MemRowSet::NewCompactionInput(const Schema* projection,
                                               const MvccSnapshot &snap) const  {
  return CompactionInput::Create(*this, projection, snap);
}

Status MemRowSet::GetBounds(Slice *min_encoded_key,
                            Slice *max_encoded_key) const {
  return Status::NotSupported("");
}

Status MemRowSet::Iterator::Init(ScanSpec *spec) {
  DCHECK_EQ(state_, kUninitialized);

  RETURN_NOT_OK(projector_.Init());
  RETURN_NOT_OK(delta_projector_.Init());

  if (spec != NULL && spec->has_encoded_ranges()) {
    boost::optional<const Slice &> max_lower_bound;
    BOOST_FOREACH(const EncodedKeyRange *range, spec->encoded_ranges()) {
      if (range->has_lower_bound()) {
        bool exact;
        const Slice &lower_bound = range->lower_bound().encoded_key();
        if (!max_lower_bound.is_initialized() ||
              lower_bound.compare(*max_lower_bound) > 0) {
          if (!iter_->SeekAtOrAfter(lower_bound, &exact)) {
            // Lower bound is after the end of the key range, no rows will
            // pass the predicate so we can stop the scan right away.
            state_ = kFinished;
            return Status::OK();
          }
          max_lower_bound.reset(lower_bound);
        }
      }
      if (range->has_upper_bound()) {
        const Slice &upper_bound = range->upper_bound().encoded_key();
        if (!has_upper_bound() || upper_bound.compare(*upper_bound_) < 0) {
          upper_bound_.reset(upper_bound);
        }
      }
      if (VLOG_IS_ON(1)) {
        Schema key_schema = memrowset_->schema().CreateKeyProjection();
        VLOG_IF(1, max_lower_bound.is_initialized())
            << "Pushed MemRowSet lower bound value "
            << range->lower_bound().Stringify(key_schema);
        VLOG_IF(1, has_upper_bound())
            << "Pushed MemRowSet upper bound value "
            << range->upper_bound().Stringify(key_schema);
      }
    }
    if (max_lower_bound.is_initialized()) {
      bool exact;
      iter_->SeekAtOrAfter(*max_lower_bound, &exact);
    }
  }
  state_ = kScanning;
  return Status::OK();
}

Status MemRowSet::Iterator::SeekAtOrAfter(const Slice &key, bool *exact) {
  DCHECK_NE(state_, kUninitialized) << "not initted";

  if (key.size() > 0) {
    ConstContiguousRow row_slice(&memrowset_->schema(), key);
    memrowset_->schema().EncodeComparableKey(row_slice, &tmp_buf);
  } else {
    // Seeking to empty key shouldn't try to run any encoding.
    tmp_buf.resize(0);
  }

  if (iter_->SeekAtOrAfter(Slice(tmp_buf), exact) ||
      key.size() == 0) {
    return Status::OK();
  } else {
    return Status::NotFound("no match in memrowset");
  }
}

Status MemRowSet::Iterator::NextBlock(RowBlock *dst) {
  // TODO: add dcheck that dst->schema() matches our schema
  // also above TODO applies to a lot of other CopyNextRows cases

  DCHECK_NE(state_, kUninitialized) << "not initted";
  if (PREDICT_FALSE(!iter_->IsValid())) {
    dst->Resize(0);
    return Status::NotFound("end of iter");
  }
  if (PREDICT_FALSE(state_ != kScanning)) {
    dst->Resize(0);
    return Status::OK();
  }
  if (PREDICT_FALSE(dst->row_capacity() == 0)) {
    return Status::OK();
  }

  // Reset rowblock arena to eventually reach appropriate buffer size.
  // Always allocating the full capacity is only a problem for the last block.
  dst->Resize(dst->row_capacity());
  if (dst->arena()) {
    dst->arena()->Reset();
  }

  // Fill
  dst->selection_vector()->SetAllTrue();
  size_t fetched;
  RETURN_NOT_OK(FetchRows(dst, &fetched));
  DCHECK_LE(0, fetched);
  DCHECK_LE(fetched, dst->nrows());

  // Clear unreached bits by resizing
  dst->Resize(fetched);

  return Status::OK();
}

Status MemRowSet::Iterator::FetchRows(RowBlock* dst, size_t* fetched) {
  *fetched = 0;
  do {
    Slice k, v;
    RowBlockRow dst_row = dst->row(*fetched);

    // Copy the row into the destination, including projection
    // and relocating slices.
    // TODO: can we share some code here with CopyRowToArena() from row.h
    // or otherwise put this elsewhere?
    iter_->GetCurrentEntry(&k, &v);
    MRSRow row(memrowset_.get(), v);

    if (mvcc_snap_.IsCommitted(row.insertion_timestamp())) {
      if (has_upper_bound() && out_of_bounds(k)) {
        state_ = kFinished;
        break;
      } else {
        RETURN_NOT_OK(projector_.ProjectRowForRead(row, &dst_row, dst->arena()));

        // Roll-forward MVCC for committed updates.
        RETURN_NOT_OK(ApplyMutationsToProjectedRow(
            row.header_->redo_head, &dst_row, dst->arena()));
      }
    } else {
      // This row was not yet committed in the current MVCC snapshot
      dst->selection_vector()->SetRowUnselected(*fetched);

      // In debug mode, fill the row data for easy debugging
      #ifndef NDEBUG
      if (state_ != kFinished) {
        dst_row.OverwriteWithPattern("MVCCMVCCMVCCMVCCMVCCMVCC"
                                     "MVCCMVCCMVCCMVCCMVCCMVCC"
                                     "MVCCMVCCMVCCMVCCMVCCMVCC");
      }
      #endif
    }

    ++*fetched;
  } while (iter_->Next() && *fetched < dst->nrows());

  return Status::OK();
}

} // namespace tablet
} // namespace kudu