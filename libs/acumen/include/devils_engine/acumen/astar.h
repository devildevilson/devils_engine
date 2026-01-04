#ifndef DEVILS_ENGINE_ACUMEN_ASTAR_H
#define DEVILS_ENGINE_ACUMEN_ASTAR_H

#include <vector>
#include <functional>
#include <cstdint>
#include "devils_engine/utils/memory_pool.h"

#define ASTAR_SEARCH_NODE_DEFAULT_SIZE 100

namespace devils_engine {
template <typename T>
struct astar {
  using float_t = double;

  struct container;

  // тут по идее указатель ненужен если у нас и так интерфейс куда мы можем указать пихнуть
  class interface {
  public:
    virtual ~interface() noexcept = default;
    virtual float_t neighbor_cost(const T&, const T&, const void*) const = 0;
    virtual float_t goal_cost(const T&, const T&, const void*) const = 0;
    virtual bool is_same(const T&, const T&, const void*) const = 0;
    virtual void fill_successors(container*, const T&, const void*) const = 0;
  };

  enum class state {
    not_initialised,
    searching,
    succeeded,
    failed,
    out_of_memory,
    invalid
  };

  struct node {
    node* parent; // used during the search to record the parent of successor nodes
    node* child;  // used after the search for the application to view the search in reverse

    float_t g; // cost of this node + it's predecessors
    float_t h; // heuristic estimate of distance to goal
    float_t f; // sum of cumulative cost of predecessors and self and heuristic

    T data;

    inline node() : parent(nullptr), child(nullptr), g(0.0), h(0.0), f(0.0) {}
    inline node(T data) : parent(nullptr), child(nullptr), g(0.0), h(0.0), f(0.0), data(std::move(data)) {}
  };

  struct container {
    std::vector<node*> openlist;   // heap
    std::vector<node*> closedlist; // vector
    std::vector<node*> successors;
    utils::memory_pool<node, ASTAR_SEARCH_NODE_DEFAULT_SIZE * sizeof(node)> node_pool;

    void add_successor(T data);
    void free_solution(node* start, node* goal) noexcept;
    void clear_memory(node* start, node* goal) noexcept;
    void free_all(node* start, node* goal) noexcept;
    void free_unused() noexcept;
  };

  class algorithm {
  public:
    algorithm(container* c, const interface* i, T start, T end, const void* p) noexcept;

    void cancel() noexcept;
    state step() noexcept;
    size_t step_count() const noexcept;

    void free_solution() noexcept;
    std::vector<node*> solution() noexcept;
    size_t solution(node** nodes, const size_t max_nodes) noexcept;
    node* solution_raw() noexcept;
    float_t solution_cost() const noexcept;

    node* goal_node() noexcept;
  private:
    container* c;
    const interface* i;
    const void* p;

    node* start;
    node* current;
    node* goal;

    bool canceled;
    state current_state;
    size_t steps;
  };
};
}

namespace devils_engine {
template <typename T>
struct node_compare {
  bool operator() (const astar<T>::node* first, const astar<T>::node* second) const noexcept {
    return first->f > second->f;
  }
};

template <typename T>
void astar<T>::container::add_successor(T data) {
  auto n = node_pool.create(std::move(data));
  successors.push_back(n);
}

template <typename T>
void astar<T>::container::free_solution(node* start, node* goal) noexcept {
  node* n = start;

  if (start->child != nullptr) {
    do {
      node* del = n;
      n = n->child;
      node_pool.destroy(del);

      del = nullptr;
    } while (n != goal);

    node_pool.destroy(n); // Delete the goal
  }
  else {
    node_pool.destroy(start);
    node_pool.destroy(goal);
  }
}

template <typename T>
void astar<T>::container::clear_memory(node* start, node* goal) noexcept {
  free_all(start, goal);

  openlist.shrink_to_fit();
  closedlist.shrink_to_fit();
  successors.shrink_to_fit();
  node_pool.clear();
}

template <typename T>
void astar<T>::container::free_all(node* start, node* goal) noexcept {
  for (size_t i = 0; i < openlist.size(); ++i) {
    node_pool.destroy(openlist[i]);
  }
  openlist.clear();

  for (size_t i = 0; i < closedlist.size(); ++i) {
    node_pool.destroy(closedlist[i]);
  }
  closedlist.clear();

  if (start != nullptr) node_pool.destroy(start);
  // по идее goal тоже отсутствует в openlist или closedlist

  start = nullptr;
  goal = nullptr;
}

template <typename T>
void astar<T>::container::free_unused() noexcept {
  for (size_t i = 0; i < openlist.size(); ++i) {
    if (openlist[i]->child == nullptr) node_pool.destroy(openlist[i]);
  }
  openlist.clear();

  for (size_t i = 0; i < closedlist.size(); ++i) {
    if (closedlist[i]->child == nullptr) node_pool.destroy(closedlist[i]);
  }
  closedlist.clear();
}

template <typename T>
astar<T>::algorithm::algorithm(astar<T>::container* c, const astar<T>::interface* i, T start, T end, const void* p) noexcept :
  c(c), i(i), p(p), start(nullptr), current(nullptr), goal(nullptr), canceled(false), current_state(state::searching), steps(0)
{
  this->start = c->node_pool.create(std::move(start));
  this->goal = c->node_pool.create(std::move(end));

  this->start->g = 0.0;
  this->start->h = i->goal_cost(start, end, p);
  this->start->f = this->start->g + this->start->h;

  c->openlist.push_back(this->start);
  std::push_heap(c->openlist.begin(), c->openlist.end(), node_compare<T>());
}

template <typename T>
void astar<T>::algorithm::cancel() noexcept { canceled = true; }
template <typename T>
astar<T>::state astar<T>::algorithm::step() noexcept {
  // Next I want it to be safe to do a searchstep once the search has succeeded...
  if (current_state == state::succeeded || current_state == state::failed) return current_state;

  // Failure is defined as emptying the open list as there is nothing left to search...
  // New: Allow user abort
  if (c->openlist.empty() || canceled) {
    c->free_all(start, goal);
    current_state = state::failed;
    return current_state;
  }

  // Incremement step count
  ++steps;

  // Pop the best node (the one with the lowest f)
  auto n = c->openlist.front(); // get pointer to the node
  std::pop_heap(c->openlist.begin(), c->openlist.end(), node_compare<T>());
  c->openlist.pop_back();

  // Check for the goal, once we pop that we're done
  if (i->is_same(n->data, goal->data, p)) {
    // The user is going to use the Goal Node he passed in
    // so copy the parent pointer of n
    // лучше здесь взять n вместо goal, goal удалить
    //auto tmp = goal;
    //goal = n;
    //c->node_pool.destroy(tmp);
    goal->parent = n->parent;
    goal->g = n->g;
    // как это сделать более аккуратно я не знаю
    // но у нас у цели по умолчанию не будет экшона, а у n должен быть
    // экшон некст стейт == текущему в ноде
    goal->data = n->data;

    // A special case is that the goal was passed in as the start state
    // so handle that here
    if (!i->is_same(n->data, start->data, p)) {
      c->node_pool.destroy(n);

      // set the child pointers in each node (except Goal which has no child)
      auto nodeChild = goal;
      auto nodeParent = goal->parent;

      do {
        nodeParent->child = nodeChild;

        nodeChild = nodeParent;
        nodeParent = nodeParent->parent;

      } while (nodeChild != start); // Start is always the first node by definition
    }

    // delete nodes that aren't needed for the solution
    c->free_unused();
    current_state = state::succeeded;
    return current_state;
  }
  else { // not goal

    // We now need to generate the successors of this node
    // The user helps us to do this, and we keep the new nodes in
    // m_Successors ...

    c->successors.clear(); // empty vector of successor nodes to n

    // User provides this functions and uses AddSuccessor to add each successor of
    // node 'n' to m_Successors
    i->fill_successors(c, n->data, p);

    // Now handle each successor to the current node ...
    for (auto successor = c->successors.begin(); successor != c->successors.end(); ++successor) {
      // The g value for this successor ...
      const float_t n_cost = i->neighbor_cost(n->data, (*successor)->data, p); // тут видимо иногда неверно приходит значение
      const float_t newg = n->g + n_cost;

      // Now we need to find whether the node is on the open or closed lists
      // If it is but the node that is already on them is better (lower g)
      // then we can forget about this successor

      // First linear search of open list to find node
      auto openlist_result = c->openlist.begin();
      for (; openlist_result != c->openlist.end(); ++openlist_result) {
        if (i->is_same((*openlist_result)->data, (*successor)->data, p)) break;
      }

      if (openlist_result != c->openlist.end()) {
        // we found this state on open

        if ((*openlist_result)->g <= newg) {
          c->node_pool.destroy(*successor);
          *successor = nullptr;

          // the one on Open is cheaper than this one
          continue;
        }
      }

      auto closedlist_result = c->closedlist.begin();
      for (; closedlist_result != c->closedlist.end(); ++closedlist_result) {
        if (i->is_same((*closedlist_result)->data, (*successor)->data, p)) break;
      }

      if (closedlist_result != c->closedlist.end()) {
        // we found this state on closed

        if ((*closedlist_result)->g <= newg) {
          // the one on Closed is cheaper than this one
          c->node_pool.destroy(*successor);
          *successor = nullptr;

          continue;
        }
      }

      // This node is the best node so far with this particular state
      // so lets keep it and set up its AStar specific data ...

      (*successor)->parent = n;
      (*successor)->g = newg;
      (*successor)->h = i->goal_cost((*successor)->data, goal->data, p);
      (*successor)->f = (*successor)->g + (*successor)->h;

      // Successor in closed list
      // 1 - Update old version of this node in closed list
      // 2 - Move it from closed to open list
      // 3 - Sort heap again in open list

      if (closedlist_result != c->closedlist.end()) {
        // Update closed node with successor node AStar data
        *(*closedlist_result) = *(*successor);

        // Free successor node
        c->node_pool.destroy(*successor);
        *successor = nullptr;

        // Push closed node into open list
        c->openlist.push_back(*closedlist_result);

        // Remove closed node from closed list
        //closedlist.erase(closedlist_result);
        std::swap(*closedlist_result, c->closedlist.back());
        c->closedlist.pop_back();

        // Sort back element into heap
        std::push_heap(c->openlist.begin(), c->openlist.end(), node_compare<T>());

        // Fix thanks to ...
        // Greg Douglas <gregdouglasmail@gmail.com>
        // who noticed that this code path was incorrect
        // Here we have found a new state which is already CLOSED
      }
      else if (openlist_result != c->openlist.end()) {
        // Successor in open list
        // 1 - Update old version of this node in open list
        // 2 - sort heap again in open list

        // Update open node with successor node AStar data
        *(*openlist_result) = *(*successor);

        // Free successor node
        c->node_pool.destroy(*successor);
        *successor = nullptr;

        // re-make the heap
        // make_heap rather than sort_heap is an essential bug fix
        // thanks to Mike Ryynanen for pointing this out and then explaining
        // it in detail. sort_heap called on an invalid heap does not work
        std::make_heap(c->openlist.begin(), c->openlist.end(), node_compare<T>());
      }
      else {
        // New successor
        // 1 - Move it from successors to open list
        // 2 - sort heap again in open list

        // Push successor node into open list
        c->openlist.push_back(*successor);

        // Sort back element into heap
        std::push_heap(c->openlist.begin(), c->openlist.end(), node_compare<T>());
      }
    }

    // push n onto Closed, as we have expanded it now
    c->closedlist.push_back(n);

  } // end else (not goal so expand)

  return current_state; // Succeeded bool is false at this point.
}

template <typename T>
size_t astar<T>::algorithm::step_count() const noexcept { return steps; }

template <typename T>
void astar<T>::algorithm::free_solution() noexcept {
  c->free_solution(start, goal);
  start = nullptr;
  goal = nullptr;
}

template <typename T>
std::vector<typename astar<T>::node*> astar<T>::algorithm::solution() noexcept {
  if (i->is_same(start->data, goal->data, p)) return { start };

  std::vector<node*> sol;

  node* n = start;
  sol.push_back(n);
  while (n != goal) {
    n = n->child;
    sol.push_back(n);
  }

  return sol;
}

template <typename T>
size_t astar<T>::algorithm::solution(astar<T>::node** nodes, const size_t max_nodes) noexcept {
  if (max_nodes == 0) return 0;

  if (i->is_same(start->data, goal->data, p)) {
    nodes[0] = start;
    return 1;
  }

  size_t counter = 0;
  node* n = start;
  nodes[counter] = n;
  counter += 1;
  while (n != goal && counter < max_nodes) {
    n = n->child;
    nodes[counter] = n;
    counter += 1;
  }

  return counter;
}

template <typename T>
astar<T>::node* astar<T>::algorithm::solution_raw() noexcept {
  return start;
}

template <typename T>
astar<T>::float_t astar<T>::algorithm::solution_cost() const noexcept {
  if (goal != nullptr && current_state == state::succeeded) {
    return goal->g;
  }

  return -1.0f;
}

template <typename T>
astar<T>::node* astar<T>::algorithm::goal_node() noexcept { return goal; }
}

#endif