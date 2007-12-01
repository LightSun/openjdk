/*
 * Copyright 2001-2006 Sun Microsystems, Inc.  All Rights Reserved.
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
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */

class TaskQueueSuper: public CHeapObj {
protected:
  // The first free element after the last one pushed (mod _n).
  // (For now we'll assume only 32-bit CAS).
  volatile juint _bottom;

  // log2 of the size of the queue.
  enum SomeProtectedConstants {
    Log_n = 14
  };

  // Size of the queue.
  juint n() { return (1 << Log_n); }
  // For computing "x mod n" efficiently.
  juint n_mod_mask() { return n() - 1; }

  struct Age {
    jushort _top;
    jushort _tag;

    jushort tag() const { return _tag; }
    jushort top() const { return _top; }

    Age() { _tag = 0; _top = 0; }

    friend bool operator ==(const Age& a1, const Age& a2) {
      return a1.tag() == a2.tag() && a1.top() == a2.top();
    }

  };
  Age _age;
  // These make sure we do single atomic reads and writes.
  Age get_age() {
    jint res = *(volatile jint*)(&_age);
    return *(Age*)(&res);
  }
  void set_age(Age a) {
    *(volatile jint*)(&_age) = *(int*)(&a);
  }

  jushort get_top() {
    return get_age().top();
  }

  // These both operate mod _n.
  juint increment_index(juint ind) {
    return (ind + 1) & n_mod_mask();
  }
  juint decrement_index(juint ind) {
    return (ind - 1) & n_mod_mask();
  }

  // Returns a number in the range [0.._n).  If the result is "n-1", it
  // should be interpreted as 0.
  juint dirty_size(juint bot, juint top) {
    return ((jint)bot - (jint)top) & n_mod_mask();
  }

  // Returns the size corresponding to the given "bot" and "top".
  juint size(juint bot, juint top) {
    juint sz = dirty_size(bot, top);
    // Has the queue "wrapped", so that bottom is less than top?
    // There's a complicated special case here.  A pair of threads could
    // perform pop_local and pop_global operations concurrently, starting
    // from a state in which _bottom == _top+1.  The pop_local could
    // succeed in decrementing _bottom, and the pop_global in incrementing
    // _top (in which case the pop_global will be awarded the contested
    // queue element.)  The resulting state must be interpreted as an empty
    // queue.  (We only need to worry about one such event: only the queue
    // owner performs pop_local's, and several concurrent threads
    // attempting to perform the pop_global will all perform the same CAS,
    // and only one can succeed.  Any stealing thread that reads after
    // either the increment or decrement will seen an empty queue, and will
    // not join the competitors.  The "sz == -1 || sz == _n-1" state will
    // not be modified  by concurrent queues, so the owner thread can reset
    // the state to _bottom == top so subsequent pushes will be performed
    // normally.
    if (sz == (n()-1)) return 0;
    else return sz;
  }

public:
  TaskQueueSuper() : _bottom(0), _age() {}

  // Return "true" if the TaskQueue contains any tasks.
  bool peek();

  // Return an estimate of the number of elements in the queue.
  // The "careful" version admits the possibility of pop_local/pop_global
  // races.
  juint size() {
    return size(_bottom, get_top());
  }

  juint dirty_size() {
    return dirty_size(_bottom, get_top());
  }

  // Maximum number of elements allowed in the queue.  This is two less
  // than the actual queue size, for somewhat complicated reasons.
  juint max_elems() { return n() - 2; }

};

template<class E> class GenericTaskQueue: public TaskQueueSuper {
private:
  // Slow paths for push, pop_local.  (pop_global has no fast path.)
  bool push_slow(E t, juint dirty_n_elems);
  bool pop_local_slow(juint localBot, Age oldAge);

public:
  // Initializes the queue to empty.
  GenericTaskQueue();

  void initialize();

  // Push the task "t" on the queue.  Returns "false" iff the queue is
  // full.
  inline bool push(E t);

  // If succeeds in claiming a task (from the 'local' end, that is, the
  // most recently pushed task), returns "true" and sets "t" to that task.
  // Otherwise, the queue is empty and returns false.
  inline bool pop_local(E& t);

  // If succeeds in claiming a task (from the 'global' end, that is, the
  // least recently pushed task), returns "true" and sets "t" to that task.
  // Otherwise, the queue is empty and returns false.
  bool pop_global(E& t);

  // Delete any resource associated with the queue.
  ~GenericTaskQueue();

private:
  // Element array.
  volatile E* _elems;
};

template<class E>
GenericTaskQueue<E>::GenericTaskQueue():TaskQueueSuper() {
  assert(sizeof(Age) == sizeof(jint), "Depends on this.");
}

template<class E>
void GenericTaskQueue<E>::initialize() {
  _elems = NEW_C_HEAP_ARRAY(E, n());
  guarantee(_elems != NULL, "Allocation failed.");
}

template<class E>
bool GenericTaskQueue<E>::push_slow(E t, juint dirty_n_elems) {
  if (dirty_n_elems == n() - 1) {
    // Actually means 0, so do the push.
    juint localBot = _bottom;
    _elems[localBot] = t;
    _bottom = increment_index(localBot);
    return true;
  } else
    return false;
}

template<class E>
bool GenericTaskQueue<E>::
pop_local_slow(juint localBot, Age oldAge) {
  // This queue was observed to contain exactly one element; either this
  // thread will claim it, or a competing "pop_global".  In either case,
  // the queue will be logically empty afterwards.  Create a new Age value
  // that represents the empty queue for the given value of "_bottom".  (We
  // must also increment "tag" because of the case where "bottom == 1",
  // "top == 0".  A pop_global could read the queue element in that case,
  // then have the owner thread do a pop followed by another push.  Without
  // the incrementing of "tag", the pop_global's CAS could succeed,
  // allowing it to believe it has claimed the stale element.)
  Age newAge;
  newAge._top = localBot;
  newAge._tag = oldAge.tag() + 1;
  // Perhaps a competing pop_global has already incremented "top", in which
  // case it wins the element.
  if (localBot == oldAge.top()) {
    Age tempAge;
    // No competing pop_global has yet incremented "top"; we'll try to
    // install new_age, thus claiming the element.
    assert(sizeof(Age) == sizeof(jint) && sizeof(jint) == sizeof(juint),
           "Assumption about CAS unit.");
    *(jint*)&tempAge = Atomic::cmpxchg(*(jint*)&newAge, (volatile jint*)&_age, *(jint*)&oldAge);
    if (tempAge == oldAge) {
      // We win.
      assert(dirty_size(localBot, get_top()) != n() - 1,
             "Shouldn't be possible...");
      return true;
    }
  }
  // We fail; a completing pop_global gets the element.  But the queue is
  // empty (and top is greater than bottom.)  Fix this representation of
  // the empty queue to become the canonical one.
  set_age(newAge);
  assert(dirty_size(localBot, get_top()) != n() - 1,
         "Shouldn't be possible...");
  return false;
}

template<class E>
bool GenericTaskQueue<E>::pop_global(E& t) {
  Age newAge;
  Age oldAge = get_age();
  juint localBot = _bottom;
  juint n_elems = size(localBot, oldAge.top());
  if (n_elems == 0) {
    return false;
  }
  t = _elems[oldAge.top()];
  newAge = oldAge;
  newAge._top = increment_index(newAge.top());
  if ( newAge._top == 0 ) newAge._tag++;
  Age resAge;
  *(jint*)&resAge = Atomic::cmpxchg(*(jint*)&newAge, (volatile jint*)&_age, *(jint*)&oldAge);
  // Note that using "_bottom" here might fail, since a pop_local might
  // have decremented it.
  assert(dirty_size(localBot, newAge._top) != n() - 1,
         "Shouldn't be possible...");
  return (resAge == oldAge);
}

template<class E>
GenericTaskQueue<E>::~GenericTaskQueue() {
  FREE_C_HEAP_ARRAY(E, _elems);
}

// Inherits the typedef of "Task" from above.
class TaskQueueSetSuper: public CHeapObj {
protected:
  static int randomParkAndMiller(int* seed0);
public:
  // Returns "true" if some TaskQueue in the set contains a task.
  virtual bool peek() = 0;
};

template<class E> class GenericTaskQueueSet: public TaskQueueSetSuper {
private:
  int _n;
  GenericTaskQueue<E>** _queues;

public:
  GenericTaskQueueSet(int n) : _n(n) {
    typedef GenericTaskQueue<E>* GenericTaskQueuePtr;
    _queues = NEW_C_HEAP_ARRAY(GenericTaskQueuePtr, n);
    guarantee(_queues != NULL, "Allocation failure.");
    for (int i = 0; i < n; i++) {
      _queues[i] = NULL;
    }
  }

  bool steal_1_random(int queue_num, int* seed, E& t);
  bool steal_best_of_2(int queue_num, int* seed, E& t);
  bool steal_best_of_all(int queue_num, int* seed, E& t);

  void register_queue(int i, GenericTaskQueue<E>* q);

  GenericTaskQueue<E>* queue(int n);

  // The thread with queue number "queue_num" (and whose random number seed
  // is at "seed") is trying to steal a task from some other queue.  (It
  // may try several queues, according to some configuration parameter.)
  // If some steal succeeds, returns "true" and sets "t" the stolen task,
  // otherwise returns false.
  bool steal(int queue_num, int* seed, E& t);

  bool peek();
};

template<class E>
void GenericTaskQueueSet<E>::register_queue(int i, GenericTaskQueue<E>* q) {
  assert(0 <= i && i < _n, "index out of range.");
  _queues[i] = q;
}

template<class E>
GenericTaskQueue<E>* GenericTaskQueueSet<E>::queue(int i) {
  return _queues[i];
}

template<class E>
bool GenericTaskQueueSet<E>::steal(int queue_num, int* seed, E& t) {
  for (int i = 0; i < 2 * _n; i++)
    if (steal_best_of_2(queue_num, seed, t))
      return true;
  return false;
}

template<class E>
bool GenericTaskQueueSet<E>::steal_best_of_all(int queue_num, int* seed, E& t) {
  if (_n > 2) {
    int best_k;
    jint best_sz = 0;
    for (int k = 0; k < _n; k++) {
      if (k == queue_num) continue;
      jint sz = _queues[k]->size();
      if (sz > best_sz) {
        best_sz = sz;
        best_k = k;
      }
    }
    return best_sz > 0 && _queues[best_k]->pop_global(t);
  } else if (_n == 2) {
    // Just try the other one.
    int k = (queue_num + 1) % 2;
    return _queues[k]->pop_global(t);
  } else {
    assert(_n == 1, "can't be zero.");
    return false;
  }
}

template<class E>
bool GenericTaskQueueSet<E>::steal_1_random(int queue_num, int* seed, E& t) {
  if (_n > 2) {
    int k = queue_num;
    while (k == queue_num) k = randomParkAndMiller(seed) % _n;
    return _queues[2]->pop_global(t);
  } else if (_n == 2) {
    // Just try the other one.
    int k = (queue_num + 1) % 2;
    return _queues[k]->pop_global(t);
  } else {
    assert(_n == 1, "can't be zero.");
    return false;
  }
}

template<class E>
bool GenericTaskQueueSet<E>::steal_best_of_2(int queue_num, int* seed, E& t) {
  if (_n > 2) {
    int k1 = queue_num;
    while (k1 == queue_num) k1 = randomParkAndMiller(seed) % _n;
    int k2 = queue_num;
    while (k2 == queue_num || k2 == k1) k2 = randomParkAndMiller(seed) % _n;
    // Sample both and try the larger.
    juint sz1 = _queues[k1]->size();
    juint sz2 = _queues[k2]->size();
    if (sz2 > sz1) return _queues[k2]->pop_global(t);
    else return _queues[k1]->pop_global(t);
  } else if (_n == 2) {
    // Just try the other one.
    int k = (queue_num + 1) % 2;
    return _queues[k]->pop_global(t);
  } else {
    assert(_n == 1, "can't be zero.");
    return false;
  }
}

template<class E>
bool GenericTaskQueueSet<E>::peek() {
  // Try all the queues.
  for (int j = 0; j < _n; j++) {
    if (_queues[j]->peek())
      return true;
  }
  return false;
}

// A class to aid in the termination of a set of parallel tasks using
// TaskQueueSet's for work stealing.

class ParallelTaskTerminator: public StackObj {
private:
  int _n_threads;
  TaskQueueSetSuper* _queue_set;
  jint _offered_termination;

  bool peek_in_queue_set();
protected:
  virtual void yield();
  void sleep(uint millis);

public:

  // "n_threads" is the number of threads to be terminated.  "queue_set" is a
  // queue sets of work queues of other threads.
  ParallelTaskTerminator(int n_threads, TaskQueueSetSuper* queue_set);

  // The current thread has no work, and is ready to terminate if everyone
  // else is.  If returns "true", all threads are terminated.  If returns
  // "false", available work has been observed in one of the task queues,
  // so the global task is not complete.
  bool offer_termination();

  // Reset the terminator, so that it may be reused again.
  // The caller is responsible for ensuring that this is done
  // in an MT-safe manner, once the previous round of use of
  // the terminator is finished.
  void reset_for_reuse();

};

#define SIMPLE_STACK 0

template<class E> inline bool GenericTaskQueue<E>::push(E t) {
#if SIMPLE_STACK
  juint localBot = _bottom;
  if (_bottom < max_elems()) {
    _elems[localBot] = t;
    _bottom = localBot + 1;
    return true;
  } else {
    return false;
  }
#else
  juint localBot = _bottom;
  assert((localBot >= 0) && (localBot < n()), "_bottom out of range.");
  jushort top = get_top();
  juint dirty_n_elems = dirty_size(localBot, top);
  assert((dirty_n_elems >= 0) && (dirty_n_elems < n()),
         "n_elems out of range.");
  if (dirty_n_elems < max_elems()) {
    _elems[localBot] = t;
    _bottom = increment_index(localBot);
    return true;
  } else {
    return push_slow(t, dirty_n_elems);
  }
#endif
}

template<class E> inline bool GenericTaskQueue<E>::pop_local(E& t) {
#if SIMPLE_STACK
  juint localBot = _bottom;
  assert(localBot > 0, "precondition.");
  localBot--;
  t = _elems[localBot];
  _bottom = localBot;
  return true;
#else
  juint localBot = _bottom;
  // This value cannot be n-1.  That can only occur as a result of
  // the assignment to bottom in this method.  If it does, this method
  // resets the size( to 0 before the next call (which is sequential,
  // since this is pop_local.)
  juint dirty_n_elems = dirty_size(localBot, get_top());
  assert(dirty_n_elems != n() - 1, "Shouldn't be possible...");
  if (dirty_n_elems == 0) return false;
  localBot = decrement_index(localBot);
  _bottom = localBot;
  // This is necessary to prevent any read below from being reordered
  // before the store just above.
  OrderAccess::fence();
  t = _elems[localBot];
  // This is a second read of "age"; the "size()" above is the first.
  // If there's still at least one element in the queue, based on the
  // "_bottom" and "age" we've read, then there can be no interference with
  // a "pop_global" operation, and we're done.
  juint tp = get_top();
  if (size(localBot, tp) > 0) {
    assert(dirty_size(localBot, tp) != n() - 1,
           "Shouldn't be possible...");
    return true;
  } else {
    // Otherwise, the queue contained exactly one element; we take the slow
    // path.
    return pop_local_slow(localBot, get_age());
  }
#endif
}

typedef oop Task;
typedef GenericTaskQueue<Task>         OopTaskQueue;
typedef GenericTaskQueueSet<Task>      OopTaskQueueSet;

typedef oop* StarTask;
typedef GenericTaskQueue<StarTask>     OopStarTaskQueue;
typedef GenericTaskQueueSet<StarTask>  OopStarTaskQueueSet;

typedef size_t ChunkTask;  // index for chunk
typedef GenericTaskQueue<ChunkTask>    ChunkTaskQueue;
typedef GenericTaskQueueSet<ChunkTask> ChunkTaskQueueSet;

class ChunkTaskQueueWithOverflow: public CHeapObj {
 protected:
  ChunkTaskQueue              _chunk_queue;
  GrowableArray<ChunkTask>*   _overflow_stack;

 public:
  ChunkTaskQueueWithOverflow() : _overflow_stack(NULL) {}
  // Initialize both stealable queue and overflow
  void initialize();
  // Save first to stealable queue and then to overflow
  void save(ChunkTask t);
  // Retrieve first from overflow and then from stealable queue
  bool retrieve(ChunkTask& chunk_index);
  // Retrieve from stealable queue
  bool retrieve_from_stealable_queue(ChunkTask& chunk_index);
  // Retrieve from overflow
  bool retrieve_from_overflow(ChunkTask& chunk_index);
  bool is_empty();
  bool stealable_is_empty();
  bool overflow_is_empty();
  juint stealable_size() { return _chunk_queue.size(); }
  ChunkTaskQueue* task_queue() { return &_chunk_queue; }
};

#define USE_ChunkTaskQueueWithOverflow
