// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_EXEC_PARTITIONED_AGGREGATION_NODE_H
#define IMPALA_EXEC_PARTITIONED_AGGREGATION_NODE_H

#include <boost/scoped_ptr.hpp>

#include "exec/exec-node.h"
#include "exec/hash-table.h"
#include "runtime/buffered-block-mgr.h"
#include "runtime/buffered-tuple-stream.h"
#include "runtime/descriptors.h"  // for TupleId
#include "runtime/mem-pool.h"
#include "runtime/string-value.h"

namespace llvm {
class BasicBlock;
class Function;
class Value;
}

namespace impala {

class AggFnEvaluator;
class CodegenAnyVal;
class LlvmCodeGen;
class LlvmBuilder;
class RowBatch;
class RuntimeState;
struct StringValue;
class Tuple;
class TupleDescriptor;
class SlotDescriptor;

/// Node for doing partitioned hash aggregation.
/// This node consumes the input (which can be from the child(0) or a spilled partition).
///  1. Each row is hashed and we pick a dst partition (hash_partitions_).
///  2. If the dst partition is not spilled, we probe into the partitions hash table
///  to aggregate/insert the row.
///  3. If the partition is already spilled, the input row is spilled.
///  4. When all the input is consumed, we walk hash_partitions_, put the spilled ones
///  into spilled_partitions_ and the non-spilled ones into aggregated_partitions_.
///  aggregated_partitions_ contain partitions that are fully processed and the result
///  can just be returned. Partitions in spilled_partitions_ need to be repartitioned
///  and we just repeat these steps.
//
/// Each partition contains these structures:
/// 1) Hash Table for aggregated rows. This contains just the hash table directory
///    structure but not the rows themselves. This is NULL for spilled partitions when
///    we stop maintaining the hash table.
/// 2) MemPool for var-len result data for rows in the hash table. If the aggregate
///    function returns a string, we cannot append it to the tuple stream as that
///    structure is immutable. Instead, when we need to spill, we sweep and copy the
///    rows into a tuple stream.
/// 3) Aggregated tuple stream for rows that are/were in the hash table. This stream
///    contains rows that are aggregated. When the partition is not spilled, this stream
///    is pinned and contains the memory referenced by the hash table.
///    In the case where the aggregate function does not return a string (meaning the
///    size of all the slots is known when the row is constructed), this stream contains
///    all the memory for the result rows and the MemPool (2) is not used.
/// 4) Unaggregated tuple stream. Stream to spill unaggregated rows.
///    Rows in this stream always have child(0)'s layout.
///
/// Buffering: Each stream and hash table needs to maintain at least one buffer for
/// some duration of the processing. To minimize the memory requirements of small queries
/// (i.e. memory usage is less than one IO-buffer per partition), the streams and hash
/// tables of each partition start using small (less than IO-sized) buffers, regardless
/// of the level.
///
/// Two-phase aggregation: we support two-phase distributed aggregations, where
/// pre-aggregrations attempt to reduce the size of data before shuffling data across the
/// network to be merged by the merge aggregation node. This exec node supports a
/// streaming mode for pre-aggregations where it maintains a hash table of aggregated
/// rows, but can pass through unaggregated rows (after transforming them into the
/// same tuple format as aggregated rows) when a heuristic determines that it is better
/// to send rows across the network instead of consuming additional memory and CPU
/// resources to expand its hash table. The planner decides whether a given
/// pre-aggregation should use the streaming preaggregation algorithm or the same
/// blocking aggregation algorithm as used in merge aggregations.
/// TODO: make this less of a heuristic by factoring in the cost of the exchange vs the
/// cost of the pre-aggregation.
///
/// If there are no grouping expressions, there is only a single output row for both
/// preaggregations and merge aggregations. This case is handled separately to avoid
/// building hash tables. There is also no need to do streaming preaggregations.
///
/// Handling memory pressure: the node uses two different strategies for responding to
/// memory pressure, depending on whether it is a streaming pre-aggregation or not. If
/// the node is a streaming preaggregation, it stops growing its hash table further by
/// converting unaggregated rows into the aggregated tuple format and passing them
/// through. If the node is not a streaming pre-aggregation, it responds to memory
/// pressure by spilling partitions to disk.
///
/// TODO: Buffer rows before probing into the hash table?
/// TODO: After spilling, we can still maintain a very small hash table just to remove
/// some number of rows (from likely going to disk).
/// TODO: Consider allowing to spill the hash table structure in addition to the rows.
/// TODO: Do we want to insert a buffer before probing into the partition's hash table?
/// TODO: Use a prefetch/batched probe interface.
/// TODO: Return rows from the aggregated_row_stream rather than the HT.
/// TODO: Think about spilling heuristic.
/// TODO: When processing a spilled partition, we have a lot more information and can
/// size the partitions/hash tables better.
/// TODO: Start with unpartitioned (single partition) and switch to partitioning and
/// spilling only if the size gets large, say larger than the LLC.
/// TODO: Simplify or cleanup the various uses of agg_fn_ctx, agg_fn_ctx_, and ctx.
/// There are so many contexts in use that a plain "ctx" variable should never be used.
/// Likewise, it's easy to mixup the agg fn ctxs, there should be a way to simplify this.
/// TODO: support an Init() method with an initial value in the UDAF interface.
class PartitionedAggregationNode : public ExecNode {
 public:
  PartitionedAggregationNode(ObjectPool* pool,
      const TPlanNode& tnode, const DescriptorTbl& descs);

  virtual Status Init(const TPlanNode& tnode, RuntimeState* state);
  virtual Status Prepare(RuntimeState* state);
  virtual void Codegen(RuntimeState* state);
  virtual Status Open(RuntimeState* state);
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos);
  virtual Status Reset(RuntimeState* state);
  virtual void Close(RuntimeState* state);

  static const char* LLVM_CLASS_NAME;

 protected:
  /// Frees local allocations from aggregate_evaluators_ and agg_fn_ctxs
  virtual Status QueryMaintenance(RuntimeState* state);
  virtual void DebugString(int indentation_level, std::stringstream* out) const;

 private:
  struct Partition;

  /// Number of initial partitions to create. Must be a power of 2.
  static const int PARTITION_FANOUT = 16;

  /// Needs to be the log(PARTITION_FANOUT).
  /// We use the upper bits to pick the partition and lower bits in the HT.
  /// TODO: different hash functions here too? We don't need that many bits to pick
  /// the partition so this might be okay.
  static const int NUM_PARTITIONING_BITS = 4;

  /// Maximum number of times we will repartition. The maximum build table we can process
  /// (if we have enough scratch disk space) in case there is no skew is:
  ///  MEM_LIMIT * (PARTITION_FANOUT ^ MAX_PARTITION_DEPTH).
  /// In the case where there is skew, repartitioning is unlikely to help (assuming a
  /// reasonable hash function).
  /// Note that we need to have at least as many SEED_PRIMES in HashTableCtx.
  /// TODO: we can revisit and try harder to explicitly detect skew.
  static const int MAX_PARTITION_DEPTH = 16;

  /// Default initial number of buckets in a hash table.
  /// TODO: rethink this ?
  static const int64_t PAGG_DEFAULT_HASH_TABLE_SZ = 1024;

  /// Codegen doesn't allow for automatic Status variables because then exception
  /// handling code is needed to destruct the Status, and our function call substitution
  /// doesn't know how to deal with the LLVM IR 'invoke' instruction. Workaround that by
  /// placing the Status here so exceptions won't need to destruct it.
  /// TODO: fix IMPALA-1948 and remove this.
  Status process_batch_status_;

  /// Tuple into which Update()/Merge()/Serialize() results are stored.
  TupleId intermediate_tuple_id_;
  TupleDescriptor* intermediate_tuple_desc_;

  /// Row with the intermediate tuple as its only tuple.
  boost::scoped_ptr<RowDescriptor> intermediate_row_desc_;

  /// Tuple into which Finalize() results are stored. Possibly the same as
  /// the intermediate tuple.
  TupleId output_tuple_id_;
  TupleDescriptor* output_tuple_desc_;

  /// Certain aggregates require a finalize step, which is the final step of the
  /// aggregate after consuming all input rows. The finalize step converts the aggregate
  /// value into its final form. This is true if this node contains aggregate that
  /// requires a finalize step.
  const bool needs_finalize_;

  /// True if this is first phase of a two-phase distributed aggregation for which we
  /// are doing a streaming preaggregation.
  bool is_streaming_preagg_;

  /// True if any of the evaluators require the serialize step.
  bool needs_serialize_;

  /// The list of all aggregate operations for this exec node.
  std::vector<AggFnEvaluator*> aggregate_evaluators_;

  /// Cache of the ExprContexts of 'aggregate_evaluators_'. Used in the codegen'ed
  /// version of UpdateTuple() to avoid loading aggregate_evaluators_[i] at runtime.
  /// An entry is NULL if the aggregate evaluator is not codegen'ed or there is no Expr
  /// in the aggregate evaluator (e.g. count(*)).
  std::vector<ExprContext* const*> agg_expr_ctxs_;

  /// FunctionContext for each aggregate function and backing MemPool. String data
  /// returned by the aggregate functions is allocated via these contexts.
  /// These contexts are only passed to the evaluators in the non-partitioned
  /// (non-grouping) case. Otherwise they are only used to clone FunctionContexts for the
  /// partitions.
  /// TODO: we really need to plumb through CHAR(N) for intermediate types.
  std::vector<impala_udf::FunctionContext*> agg_fn_ctxs_;
  boost::scoped_ptr<MemPool> agg_fn_pool_;

  /// Exprs used to evaluate input rows
  std::vector<ExprContext*> grouping_expr_ctxs_;

  /// Exprs used to insert constructed aggregation tuple into the hash table.
  /// All the exprs are simply SlotRefs for the intermediate tuple.
  std::vector<ExprContext*> build_expr_ctxs_;

  /// Indices of grouping exprs with var-len string types in grouping_expr_ctxs_. We need
  /// to do more work for var-len expressions when allocating and spilling rows. All
  /// var-len grouping exprs have type string.
  std::vector<int> string_grouping_exprs_;

  RuntimeState* state_;
  BufferedBlockMgr::Client* block_mgr_client_;

  /// MemPool used to allocate memory for when we don't have grouping and don't initialize
  /// the partitioning structures, or during Close() when creating new output tuples.
  /// For non-grouping aggregations, the ownership of the pool's memory is transferred
  /// to the output batch on eos. The pool should not be Reset() to allow amortizing
  /// memory allocation over a series of Reset()/Open()/GetNext()* calls.
  boost::scoped_ptr<MemPool> mem_pool_;

  /// The current partition and iterator to the next row in its hash table that we need
  /// to return in GetNext()
  Partition* output_partition_;
  HashTable::Iterator output_iterator_;

  typedef Status (*ProcessBatchNoGroupingFn)(PartitionedAggregationNode*, RowBatch*);
  /// Jitted ProcessBatchNoGrouping function pointer. Null if codegen is disabled.
  ProcessBatchNoGroupingFn process_batch_no_grouping_fn_;

  typedef Status (*ProcessBatchFn)(
      PartitionedAggregationNode*, RowBatch*, TPrefetchMode::type, HashTableCtx*);
  /// Jitted ProcessBatch function pointer. Null if codegen is disabled.
  ProcessBatchFn process_batch_fn_;

  typedef Status (*ProcessBatchStreamingFn)(PartitionedAggregationNode*, bool,
      TPrefetchMode::type, RowBatch*, RowBatch*, HashTableCtx*, int[PARTITION_FANOUT]);
  /// Jitted ProcessBatchStreaming function pointer.  Null if codegen is disabled.
  ProcessBatchStreamingFn process_batch_streaming_fn_;

  /// Time spent processing the child rows
  RuntimeProfile::Counter* build_timer_;

  /// Total time spent resizing hash tables.
  RuntimeProfile::Counter* ht_resize_timer_;

  /// Time spent returning the aggregated rows
  RuntimeProfile::Counter* get_results_timer_;

  /// Total number of hash buckets across all partitions.
  RuntimeProfile::Counter* num_hash_buckets_;

  /// Total number of partitions created.
  RuntimeProfile::Counter* partitions_created_;

  /// Level of max partition (i.e. number of repartitioning steps).
  RuntimeProfile::HighWaterMarkCounter* max_partition_level_;

  /// Number of rows that have been repartitioned.
  RuntimeProfile::Counter* num_row_repartitioned_;

  /// Number of partitions that have been repartitioned.
  RuntimeProfile::Counter* num_repartitions_;

  /// Number of partitions that have been spilled.
  RuntimeProfile::Counter* num_spilled_partitions_;

  /// The largest fraction after repartitioning. This is expected to be
  /// 1 / PARTITION_FANOUT. A value much larger indicates skew.
  RuntimeProfile::HighWaterMarkCounter* largest_partition_percent_;

  /// Time spent in streaming preagg algorithm.
  RuntimeProfile::Counter* streaming_timer_;

  /// The number of rows passed through without aggregation.
  RuntimeProfile::Counter* num_passthrough_rows_;

  /// The estimated reduction of the preaggregation.
  RuntimeProfile::Counter* preagg_estimated_reduction_;

  /// Expose the minimum reduction factor to continue growing the hash tables.
  RuntimeProfile::Counter* preagg_streaming_ht_min_reduction_;

  /// The estimated number of input rows from the planner.
  int64_t estimated_input_cardinality_;

  /////////////////////////////////////////
  /// BEGIN: Members that must be Reset()

  /// Result of aggregation w/o GROUP BY.
  /// Note: can be NULL even if there is no grouping if the result tuple is 0 width
  /// e.g. select 1 from table group by col.
  Tuple* singleton_output_tuple_;
  bool singleton_output_tuple_returned_;

  /// Row batch used as argument to GetNext() for the child node preaggregations. Store
  /// in node to avoid reallocating for every GetNext() call when streaming.
  boost::scoped_ptr<RowBatch> child_batch_;

  /// If true, no more rows to output from partitions.
  bool partition_eos_;

  /// True if no more rows to process from child.
  bool child_eos_;

  /// Used for hash-related functionality, such as evaluating rows and calculating hashes.
  boost::scoped_ptr<HashTableCtx> ht_ctx_;

  /// Object pool that holds the Partition objects in hash_partitions_.
  boost::scoped_ptr<ObjectPool> partition_pool_;

  /// Current partitions we are partitioning into.
  std::vector<Partition*> hash_partitions_;

  /// Cache for hash tables in 'hash_partitions_'.
  HashTable* hash_tbls_[PARTITION_FANOUT];

  /// All partitions that have been spilled and need further processing.
  std::list<Partition*> spilled_partitions_;

  /// All partitions that are aggregated and can just return the results in GetNext().
  /// After consuming all the input, hash_partitions_ is split into spilled_partitions_
  /// and aggregated_partitions_, depending on if it was spilled or not.
  std::list<Partition*> aggregated_partitions_;

  /// END: Members that must be Reset()
  /////////////////////////////////////////

  /// The hash table and streams (aggregated and unaggregated) for an individual
  /// partition. The streams of each partition always (i.e. regardless of level)
  /// initially use small buffers. Streaming pre-aggregations do not spill and do not
  /// require an unaggregated stream.
  struct Partition {
    Partition(PartitionedAggregationNode* parent, int level)
      : parent(parent), is_closed(false), level(level) {}

    ~Partition();

    /// Initializes aggregated_row_stream and unaggregated_row_stream (if a spilling
    /// aggregation), reserving one buffer for each. The buffers backing these streams
    /// are reserved, so this function will not fail with a continuable OOM. If we fail
    /// to init these buffers, the mem limit is too low to run this algorithm.
    Status InitStreams();

    /// Initializes the hash table. Returns false on OOM.
    bool InitHashTable();

    /// Called in case we need to serialize aggregated rows. This step effectively does
    /// a merge aggregation in this node.
    Status SerializeStreamForSpilling();

    /// Closes this partition. If finalize_rows is true, this iterates over all rows
    /// in aggregated_row_stream and finalizes them (this is only used in the cancellation
    /// path).
    void Close(bool finalize_rows);

    /// Spills this partition, unpinning streams and cleaning up hash tables as necessary.
    Status Spill();

    bool is_spilled() const { return hash_tbl.get() == NULL; }

    PartitionedAggregationNode* parent;

    /// If true, this partition is closed and there is nothing left to do.
    bool is_closed;

    /// How many times rows in this partition have been repartitioned. Partitions created
    /// from the node's children's input is level 0, 1 after the first repartitionining,
    /// etc.
    const int level;

    /// Hash table for this partition.
    /// Can be NULL if this partition is no longer maintaining a hash table (i.e.
    /// is spilled).
    boost::scoped_ptr<HashTable> hash_tbl;

    /// Clone of parent's agg_fn_ctxs_ and backing MemPool.
    std::vector<impala_udf::FunctionContext*> agg_fn_ctxs;
    boost::scoped_ptr<MemPool> agg_fn_pool;

    /// Tuple stream used to store aggregated rows. When the partition is not spilled,
    /// (meaning the hash table is maintained), this stream is pinned and contains the
    /// memory referenced by the hash table. When it is spilled, aggregate rows are
    /// just appended to this stream.
    boost::scoped_ptr<BufferedTupleStream> aggregated_row_stream;

    /// Unaggregated rows that are spilled. Always NULL for streaming pre-aggregations.
    boost::scoped_ptr<BufferedTupleStream> unaggregated_row_stream;
  };

  /// Stream used to store serialized spilled rows. Only used if needs_serialize_
  /// is set. This stream is never pinned and only used in Partition::Spill as a
  /// a temporary buffer.
  boost::scoped_ptr<BufferedTupleStream> serialize_stream_;

  /// Accessor for 'hash_tbls_' that verifies consistency with the partitions.
  HashTable* ALWAYS_INLINE GetHashTable(int partition_idx) {
    HashTable* ht = hash_tbls_[partition_idx];
    DCHECK_EQ(ht, hash_partitions_[partition_idx]->hash_tbl.get());
    return ht;
  }

  /// Materializes 'row_batch' in either grouping or non-grouping case.
  Status GetNextInternal(RuntimeState* state, RowBatch* row_batch, bool* eos);

  /// Helper function called by GetNextInternal() to ensure that string data referenced in
  /// 'row_batch' will live as long as 'row_batch's tuples. 'first_row_idx' indexes the
  /// first row that should be processed in 'row_batch'.
  Status HandleOutputStrings(RowBatch* row_batch, int first_row_idx);

  /// Copies string data from the specified slot into 'pool', and sets the StringValues'
  /// ptrs to the copied data. Copies data from all tuples in 'row_batch' from
  /// 'first_row_idx' onwards. 'slot_desc' must have a var-len string type.
  Status CopyStringData(const SlotDescriptor* slot_desc, RowBatch* row_batch,
      int first_row_idx, MemPool* pool);

  /// Constructs singleton output tuple, allocating memory from pool.
  Tuple* ConstructSingletonOutputTuple(
      const std::vector<impala_udf::FunctionContext*>& agg_fn_ctxs, MemPool* pool);

  /// Copies grouping values stored in 'ht_ctx_' that were computed over 'current_row_'
  /// using 'grouping_expr_ctxs_'. Aggregation expr slots are set to their initial values.
  /// Returns NULL if there was not enough memory to allocate the tuple or an error
  /// occurred. When returning NULL, sets *status.  Allocates tuple and var-len data for
  /// grouping exprs from stream. Var-len data for aggregate exprs is allocated from the
  /// FunctionContexts, so is stored outside the stream. If stream's small buffers get
  /// full, it will attempt to switch to IO-buffers.
  Tuple* ConstructIntermediateTuple(
      const std::vector<impala_udf::FunctionContext*>& agg_fn_ctxs,
      BufferedTupleStream* stream, Status* status) noexcept;

  /// Constructs intermediate tuple, allocating memory from pool instead of the stream.
  /// Returns NULL and sets status if there is not enough memory to allocate the tuple.
  Tuple* ConstructIntermediateTuple(
      const std::vector<impala_udf::FunctionContext*>& agg_fn_ctxs, MemPool* pool,
      Status* status) noexcept;

  /// Returns the number of bytes of variable-length data for the grouping values stored
  /// in 'ht_ctx_'.
  int GroupingExprsVarlenSize();

  /// Initializes intermediate tuple by copying grouping values stored in 'ht_ctx_' that
  /// that were computed over 'current_row_' using 'grouping_expr_ctxs_'. Writes the
  /// var-len data into buffer. 'buffer' points to the start of a buffer of at least the
  /// size of the variable-length data: 'varlen_size'.
  void CopyGroupingValues(Tuple* intermediate_tuple, uint8_t* buffer, int varlen_size);

  /// Initializes the aggregate function slots of an intermediate tuple.
  /// Any var-len data is allocated from the FunctionContexts.
  void InitAggSlots(const vector<impala_udf::FunctionContext*>& agg_fn_ctxs,
      Tuple* intermediate_tuple);

  /// Updates the given aggregation intermediate tuple with aggregation values computed
  /// over 'row' using 'agg_fn_ctxs'. Whether the agg fn evaluator calls Update() or
  /// Merge() is controlled by the evaluator itself, unless enforced explicitly by passing
  /// in is_merge == true.  The override is needed to merge spilled and non-spilled rows
  /// belonging to the same partition independent of whether the agg fn evaluators have
  /// is_merge() == true.
  /// This function is replaced by codegen (which is why we don't use a vector argument
  /// for agg_fn_ctxs).. Any var-len data is allocated from the FunctionContexts.
  void UpdateTuple(impala_udf::FunctionContext** agg_fn_ctxs, Tuple* tuple, TupleRow* row,
      bool is_merge = false) noexcept;

  /// Called on the intermediate tuple of each group after all input rows have been
  /// consumed and aggregated. Computes the final aggregate values to be returned in
  /// GetNext() using the agg fn evaluators' Serialize() or Finalize().
  /// For the Finalize() case if the output tuple is different from the intermediate
  /// tuple, then a new tuple is allocated from 'pool' to hold the final result.
  /// Grouping values are copied into the output tuple and the the output tuple holding
  /// the finalized/serialized aggregate values is returned.
  /// TODO: Coordinate the allocation of new tuples with the release of memory
  /// so as not to make memory consumption blow up.
  Tuple* GetOutputTuple(const std::vector<impala_udf::FunctionContext*>& agg_fn_ctxs,
      Tuple* tuple, MemPool* pool);

  /// Do the aggregation for all tuple rows in the batch when there is no grouping.
  /// This function is replaced by codegen.
  Status ProcessBatchNoGrouping(RowBatch* batch);

  /// Processes a batch of rows. This is the core function of the algorithm. We partition
  /// the rows into hash_partitions_, spilling as necessary.
  /// If AGGREGATED_ROWS is true, it means that the rows in the batch are already
  /// pre-aggregated.
  /// 'prefetch_mode' specifies the prefetching mode in use. If it's not PREFETCH_NONE,
  ///     hash table buckets will be prefetched based on the hash values computed. Note
  ///     that 'prefetch_mode' will be substituted with constants during codegen time.
  //
  /// This function is replaced by codegen. We pass in ht_ctx_.get() as an argument for
  /// performance.
  template<bool AGGREGATED_ROWS>
  Status IR_ALWAYS_INLINE ProcessBatch(RowBatch* batch,
      TPrefetchMode::type prefetch_mode, HashTableCtx* ht_ctx);

  /// Evaluates the rows in 'batch' starting at 'start_row_idx' and stores the results in
  /// the expression values cache in 'ht_ctx'. The number of rows evaluated depends on
  /// the capacity of the cache. 'prefetch_mode' specifies the prefetching mode in use.
  /// If it's not PREFETCH_NONE, hash table buckets for the computed hashes will be
  /// prefetched. Note that codegen replaces 'prefetch_mode' with a constant.
  template<bool AGGREGATED_ROWS>
  void EvalAndHashPrefetchGroup(RowBatch* batch, int start_row_idx,
      TPrefetchMode::type prefetch_mode, HashTableCtx* ht_ctx);

  /// This function processes each individual row in ProcessBatch(). Must be inlined into
  /// ProcessBatch for codegen to substitute function calls with codegen'd versions.
  /// May spill partitions if not enough memory is available.
  template <bool AGGREGATED_ROWS>
  Status IR_ALWAYS_INLINE ProcessRow(TupleRow* row, HashTableCtx* ht_ctx);

  /// Accessor for the expression contexts of an AggFnEvaluator. Returns an array of
  /// pointers the the AggFnEvaluator's expression contexts. Used only in codegen'ed
  /// version of UpdateTuple().
  ExprContext* const* IR_ALWAYS_INLINE GetAggExprContexts(int i) const;

  /// Create a new intermediate tuple in partition, initialized with row. ht_ctx is
  /// the context for the partition's hash table and hash is the precomputed hash of
  /// the row. The row can be an unaggregated or aggregated row depending on
  /// AGGREGATED_ROWS. Spills partitions if necessary to append the new intermediate
  /// tuple to the partition's stream. Must be inlined into ProcessBatch for codegen
  /// to substitute function calls with codegen'd versions.  insert_it is an iterator
  /// for insertion returned from HashTable::FindBuildRowBucket().
  template<bool AGGREGATED_ROWS>
  Status IR_ALWAYS_INLINE AddIntermediateTuple(Partition* partition,
      TupleRow* row, uint32_t hash, HashTable::Iterator insert_it);

  /// Append a row to a spilled partition. May spill partitions if needed to switch to
  /// I/O buffers. Selects the correct stream according to the argument. Inlined into
  /// ProcessBatch().
  template<bool AGGREGATED_ROWS>
  Status IR_ALWAYS_INLINE AppendSpilledRow(Partition* partition, TupleRow* row);

  /// Append a row to a stream of a spilled partition. May spill partitions if needed
  /// to append the row.
  Status AppendSpilledRow(BufferedTupleStream* stream, TupleRow* row);

  /// Reads all the rows from input_stream and process them by calling ProcessBatch().
  template<bool AGGREGATED_ROWS>
  Status ProcessStream(BufferedTupleStream* input_stream);

  /// Output 'singleton_output_tuple_' and transfer memory to 'row_batch'.
  void GetSingletonOutput(RowBatch* row_batch);

  /// Get rows for the next rowbatch from the next partition. Sets 'partition_eos_' to
  /// true if all rows from all partitions have been returned or the limit is reached.
  Status GetRowsFromPartition(RuntimeState* state, RowBatch* row_batch);

  /// Get output rows from child for streaming pre-aggregation. Aggregates some rows with
  /// hash table and passes through other rows converted into the intermediate
  /// tuple format. Sets 'child_eos_' once all rows from child have been returned.
  Status GetRowsStreaming(RuntimeState* state, RowBatch* row_batch);

  /// Return true if we should keep expanding hash tables in the preagg. If false,
  /// the preagg should pass through any rows it can't fit in its tables.
  bool ShouldExpandPreaggHashTables() const;

  /// Streaming processing of in_batch from child. Rows from child are either aggregated
  /// into the hash table or added to 'out_batch' in the intermediate tuple format.
  /// 'in_batch' is processed entirely, and 'out_batch' must have enough capacity to
  /// store all of the rows in 'in_batch'.
  /// 'needs_serialize' is an argument so that codegen can replace it with a constant,
  ///     rather than using the member variable 'needs_serialize_'.
  /// 'prefetch_mode' specifies the prefetching mode in use. If it's not PREFETCH_NONE,
  ///     hash table buckets will be prefetched based on the hash values computed. Note
  ///     that 'prefetch_mode' will be substituted with constants during codegen time.
  /// 'remaining_capacity' is an array with PARTITION_FANOUT entries with the number of
  ///     additional rows that can be added to the hash table per partition. It is updated
  ///     by ProcessBatchStreaming() when it inserts new rows.
  /// 'ht_ctx' is passed in as a way to avoid aliasing of 'this' confusing the optimiser.
  Status ProcessBatchStreaming(bool needs_serialize, TPrefetchMode::type prefetch_mode,
      RowBatch* in_batch, RowBatch* out_batch, HashTableCtx* ht_ctx,
      int remaining_capacity[PARTITION_FANOUT]);

  /// Tries to add intermediate to the hash table 'hash_tbl' of 'partition' for streaming
  /// aggregation. The input row must have been evaluated with 'ht_ctx', with 'hash' set
  /// to the corresponding hash. If the tuple already exists in the hash table, update
  /// the tuple and return true. Otherwise try to create a new entry in the hash table,
  /// returning true if successful or false if the table is full. 'remaining_capacity'
  /// keeps track of how many more entries can be added to the hash table so we can avoid
  /// retrying inserts. It is decremented if an insert succeeds and set to zero if an
  /// insert fails. If an error occurs, returns false and sets 'status'.
  bool IR_ALWAYS_INLINE TryAddToHashTable(HashTableCtx* ht_ctx,
      Partition* partition, HashTable* hash_tbl, TupleRow* in_row, uint32_t hash,
      int* remaining_capacity, Status* status);

  /// Initializes hash_partitions_. 'level' is the level for the partitions to create.
  /// Also sets ht_ctx_'s level to 'level'.
  Status CreateHashPartitions(int level);

  /// Ensure that hash tables for all in-memory partitions are large enough to fit
  /// 'num_rows' additional hash table entries. If there is not enough memory to
  /// resize the hash tables, may spill partitions.
  Status CheckAndResizeHashPartitions(int num_rows, const HashTableCtx* ht_ctx);

  /// Iterates over all the partitions in hash_partitions_ and returns the number of rows
  /// of the largest spilled partition (in terms of number of aggregated and unaggregated
  /// rows).
  int64_t LargestSpilledPartition() const;

  /// Prepares the next partition to return results from. On return, this function
  /// initializes output_iterator_ and output_partition_. This either removes
  /// a partition from aggregated_partitions_ (and is done) or removes the next
  /// partition from aggregated_partitions_ and repartitions it.
  Status NextPartition();

  /// Picks a partition from hash_partitions_ to spill.
  Status SpillPartition();

  /// Moves the partitions in hash_partitions_ to aggregated_partitions_ or
  /// spilled_partitions_. Partitions moved to spilled_partitions_ are unpinned.
  /// input_rows is the number of input rows that have been repartitioned.
  /// Used for diagnostics.
  Status MoveHashPartitions(int64_t input_rows);

  /// Calls Close() on every Partition in 'aggregated_partitions_',
  /// 'spilled_partitions_', and 'hash_partitions_' and then resets the lists,
  /// the vector and the partition pool.
  void ClosePartitions();

  /// Calls finalizes on all tuples starting at 'it'.
  void CleanupHashTbl(const std::vector<impala_udf::FunctionContext*>& agg_fn_ctxs,
      HashTable::Iterator it);

  /// Codegen UpdateSlot(). Returns non-OK status if codegen is unsuccessful.
  /// Assumes is_merge = false;
  Status CodegenUpdateSlot(LlvmCodeGen* codegen, AggFnEvaluator* evaluator,
      SlotDescriptor* slot_desc, llvm::Function** fn);

  /// Codegen a call to a function implementing the UDA interface with input values
  /// from 'input_vals'. 'dst_val' should contain the previous value of the aggregate
  /// function, and 'updated_dst_val' is set to the new value after the Update or Merge
  /// operation is applied. The instruction sequence for the UDA call is inserted at
  /// the insert position of 'builder'.
  Status CodegenCallUda(LlvmCodeGen* codegen, LlvmBuilder* builder,
      AggFnEvaluator* evaluator, llvm::Value* agg_fn_ctx,
      const std::vector<CodegenAnyVal>& input_vals, const CodegenAnyVal& dst_val,
      CodegenAnyVal* updated_dst_val);

  /// Codegen UpdateTuple(). Returns non-OK status if codegen is unsuccessful.
  Status CodegenUpdateTuple(LlvmCodeGen* codegen, llvm::Function** fn);

  /// Codegen the non-streaming process row batch loop. The loop has already been
  /// compiled to IR and loaded into the codegen object. UpdateAggTuple has also been
  /// codegen'd to IR. This function will modify the loop subsituting the statically
  /// compiled functions with codegen'd ones. 'process_batch_fn_' or
  /// 'process_batch_no_grouping_fn_' will be updated with the codegened function
  /// depending on whether this is a grouping or non-grouping aggregation.
  /// Assumes AGGREGATED_ROWS = false.
  Status CodegenProcessBatch(LlvmCodeGen* codegen, TPrefetchMode::type prefetch_mode);

  /// Codegen the materialization loop for streaming preaggregations.
  /// 'process_batch_streaming_fn_' will be updated with the codegened function.
  Status CodegenProcessBatchStreaming(
      LlvmCodeGen* codegen, TPrefetchMode::type prefetch_mode);

  /// We need two buffers per partition, one for the aggregated stream and one
  /// for the unaggregated stream. We need an additional buffer to read the stream
  /// we are currently repartitioning.
  /// If we need to serialize, we need an additional buffer while spilling a partition
  /// as the partitions aggregate stream needs to be serialized and rewritten.
  /// We do not spill streaming preaggregations, so we do not need to reserve any buffers.
  int MinRequiredBuffers() const {
    if (is_streaming_preagg_) return 0;
    return 2 * PARTITION_FANOUT + 1 + (needs_serialize_ ? 1 : 0);
  }
};

}

#endif
