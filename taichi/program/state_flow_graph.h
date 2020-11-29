#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "taichi/ir/ir.h"
#include "taichi/lang_util.h"
#include "taichi/program/async_utils.h"
#include "taichi/program/program.h"
#include "taichi/util/bit.h"

TLANG_NAMESPACE_BEGIN

class IRBank;
class AsyncEngine;

class StateFlowGraph {
 public:
  struct Node;
  using StateToNodesMap =
      llvm::SmallVector<std::pair<AsyncState, llvm::SmallSet<Node *, 8>>, 4>;

  // Each node is a task
  // Note: after SFG is done, each node here should hold a TaskLaunchRecord.
  // Optimization should happen fully on the SFG, instead of the queue in
  // AsyncEngine.
  // Before we migrate, the SFG is only used for visualization. Therefore we
  // only store a kernel_name, which is used as the label of the GraphViz node.
  struct Node {
    TaskLaunchRecord rec;
    TaskMeta *meta{nullptr};
    // Incremental ID to identify the i-th launch of the task.
    bool is_initial_node{false};

    // Returns the position in nodes_. Invoke StateFlowGraph::reid_nodes() to
    // keep it up-to-date.
    int node_id{0};

    // Returns the position in get_pending_tasks() or extract_pending_tasks().
    // For executed tasks (including the initial node), pending_node_id is -1.
    int pending_node_id{0};

    // Performance hits
    // * std::unordered_multimap: Slow on clang + libc++, see #1855
    // * std::unordered_map<., std::unordered_set<.>>: slow due to frequent
    //   allocation, see #1927.
    StateToNodesMap input_edges, output_edges;

    bool pending() const {
      return pending_node_id >= 0;
    }

    bool executed() const {
      return pending_node_id == -1;
    }

    void mark_executed() {
      pending_node_id = -1;
    }

    std::string string() const;

    // Note: there are two types of edges A->B:
    //   ------
    //   Dependency edge: A must execute before B
    //
    //   Flow edge: A generates some data that is consumed by B. This also
    //   implies that A must execute before B
    //
    //   Therefore Flow edge = Dependency edge + possible state flow

    bool has_state_flow(AsyncState state, const Node *destination) const {
      // True: (Flow edge) the state generated by this node is used by the
      // destination node.

      // False: (Dependency edge) the destination node does not
      // use the generated state, but its execution must happen after this case.
      // This usually means a write-after-read (WAR) dependency on state.

      // Note:
      // Read-after-write leads to flow edges
      // Write-after-write leads to dependency edges
      // Write-after-read leads to dependency edges
      //
      // So an edge is a data flow edge iff the destination node reads the
      // state.
      //

      return destination->meta->input_states.find(state) !=
             destination->meta->input_states.end();
    }

    void disconnect_all();

    void disconnect_with(Node *other);
  };

  StateFlowGraph(AsyncEngine *engine, IRBank *ir_bank);

  std::vector<Node *> get_pending_tasks() const;

  // Returns get_pending_tasks()[begin, end).
  std::vector<Node *> get_pending_tasks(int begin, int end) const;

  std::vector<std::unique_ptr<Node>> extract_pending_tasks();

  void clear();

  void mark_pending_tasks_as_executed();

  void print();

  // Returns a string representing a DOT graph.
  //
  // |embed_states_threshold|: We can choose to embed the states into the task
  // node itself, if there aren't too many output states. This defines the
  // maximum number of output states a task can have for the states to be
  // embedded in the node.
  //
  // TODO: In case we add more and more DOT configs, create a struct?
  std::string dump_dot(const std::optional<std::string> &rankdir,
                       int embed_states_threshold = 0);

  void insert_tasks(const std::vector<TaskLaunchRecord> &rec,
                    bool filter_listgen);

  void insert_node(std::unique_ptr<Node> &&node);

  void insert_edge(Node *from, Node *to, AsyncState state);

  // Compute transitive closure for tasks in get_pending_tasks()[begin, end).
  std::pair<std::vector<bit::Bitset>, std::vector<bit::Bitset>>
  compute_transitive_closure(int begin, int end);

  // Fuse tasks in get_pending_tasks()[begin, end),
  // return the indices to delete.
  std::unordered_set<int> fuse_range(int begin, int end);

  bool fuse();

  bool optimize_listgen();

  bool demote_activation();

  bool optimize_dead_store();

  void delete_nodes(const std::unordered_set<int> &indices_to_delete);

  void reid_nodes();

  void reid_pending_nodes();

  void replace_reference(Node *node_a,
                         Node *node_b,
                         bool only_output_edges = false);

  void topo_sort_nodes();

  void verify(bool also_verify_ir = false) const;

  // Extract all pending tasks and insert them in topological/original order.
  void rebuild_graph(bool sort);

  // Extract all tasks to execute.
  std::vector<TaskLaunchRecord> extract_to_execute();

  std::size_t size() const {
    return nodes_.size();
  }

  int num_pending_tasks() const {
    return nodes_.size() - first_pending_task_index_;
  }

  // Recursively mark as dirty the list state of "snode" and all its children
  void mark_list_as_dirty(SNode *snode);

  void benchmark_rebuild_graph();

 private:
  std::vector<std::unique_ptr<Node>> nodes_;
  Node *initial_node_;  // The initial node holds all the initial states.
  int first_pending_task_index_;
  TaskMeta initial_meta_;
  std::unordered_map<AsyncState, Node *> latest_state_owner_;
  StateToNodesMap latest_state_readers_;
  std::unordered_map<std::string, int> task_name_to_launch_ids_;
  IRBank *ir_bank_;
  std::unordered_map<SNode *, bool> list_up_to_date_;
  [[maybe_unused]] AsyncEngine *engine_;
  Program *program_;
};

TLANG_NAMESPACE_END
