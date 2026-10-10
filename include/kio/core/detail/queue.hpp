#pragma once

// ============================================================================
// kio::detail::CoroQueue -- intrusive lock-free MPSC queue of coroutine frames
// ============================================================================
//
// The single hand-off point between threads in kio. Everything else about
// concurrency in this library funnels through here.
//
// WHO MAY TOUCH WHAT
// ------------------
//
//   producers: ANY thread   -- calls enqueue()
//   consumer:  EXACTLY ONE  -- calls dequeue() / drain() / empty()
//
// INVARIANT: dequeue()/drain()/empty() are called from one thread only. The
// worker that owns an IO is the sole consumer of that IO's queue. There is no
// lock here because there is no need for one; adding a second consumer would
// break it, and no code does that.
//
// INVARIANT: `tail_` and the `stub_` link field are touched by the consumer
// alone and are therefore plain (non-atomic) -- except stub_.next, which a
// producer writes, which is why it is atomic. `head_` is the shared boundary:
// producers RMW it, the consumer only loads it.
//
// WHY "INTRUSIVE"
// ---------------
//
// The queued items are `TaskPromiseBase*` -- a pointer into the middle of a
// coroutine frame (the promise subobject). No allocation, no separate node
// object, no free list: when the consumer resumes a coroutine and it runs to
// completion, the frame frees itself (see promise_base.hpp), taking the queue
// link with it. The link field is `TaskPromiseBase::next`.
//
// THE ALGORITHM (Vyukov intrusive MPSC)
// -------------------------------------
//
// A classic singly-linked queue cannot be made lock-free with a single link
// per node: the producer must change the OLD tail's link, which the consumer
// may be reading at the same time. The fix is a permanently-resident dummy
// node -- here `stub_` -- so that "the tail" and "the node whose link we must
// write" are never the same object.
//
//   producers push to the RIGHT of head_, consumer pops from the LEFT of tail_:
//
//        consumer                                          producer
//           ^                                                 |
//     [ node ][ node ][ node ] <-------------------------------+
//           ^                                                 |
//          tail_                                             head_
//
//   Enqueue (2 atomic ops, no retry loop, no CAS):
//
//       node->next = nullptr;            // init
//       prev = head_.exchange(node);     // claim our predecessor
//       prev->next = node;               // link it in  <-- the hard part
//
//   Between the exchange and the link there is a WINDOW in which the node is
//   reachable from head_ but not from tail_.next. A naive consumer that reads
//   tail_.next == nullptr during that window concludes "empty", parks the
//   worker, and strands the node -- the task then never runs. That is not a
//   theoretical race; it is the window that enqueue() spends real time in on
//   every single push.
//
//   The stub closes the window. When the consumer sees "tail looks like the
//   last node, but head has moved on", it enqueues the stub ITSELF:
//
//       prev = head_.exchange(&stub_);   // prev == tail, in the ambiguous case
//       prev->next = &stub_;             // tail now definitively has a link
//
//   If the producer finishes afterwards, its `prev->next = node` overwrites
//   that stub link and the consumer's re-read of tail->next yields the node.
//   If the consumer arrives first, it sees tail->next == &stub_ == head, and
//   correctly reports "still empty" -- because then the producer really has
//   not finished. It does not matter that this costs the producer a tick of
//   latency: IO's park is bounded, so the worker comes back and looks again.
//
//   INVARIANT: at most one stub is ever in flight at a time, because only the
//   single consumer calls enqueue() on it. Two concurrent stub insertions would
//   corrupt tail_, and the single-consumer rule is what prevents that.
//
//   INVARIANT: the consumer must not return a node until it is reachable from
//   tail_, i.e. until the producer's link store is visible. Every `return tail`
//   below is preceded by a load of `next` that is null only when the node
//   cannot yet be safely handed out.
// ============================================================================

#include <atomic>
#include <cstddef>
#include <limits>

#include "kio/core/task.hpp"

namespace kio::detail
{
    /// MPSC intrusive queue for coroutines
    class CoroQueue
    {
    public:
        // Starts with head_ == &stub_ and tail_ == &stub_: an empty queue whose
        // tail has a link slot, so the first enqueue never has to special-case
        // the initial state.
        CoroQueue()
        {
            stub_.next.store(nullptr, std::memory_order_relaxed);
            head_.store(&stub_, std::memory_order_relaxed);
            tail_ = &stub_;
        }

        // Called from any thread.
        //
        // The relaxed init of node->next is safe because nobody can observe the
        // node until the release store to prev->next publishes it. The exchange
        // is acq_rel: the acquire half pairs with the consumer's acquire load
        // of head_ (used only as a liveness check, see dequeue).
        void enqueue(TaskPromiseBase* node)
        {
            node->next.store(nullptr, std::memory_order_relaxed);
            TaskPromiseBase* prev = head_.exchange(node, std::memory_order_acq_rel);
            prev->next.store(node, std::memory_order_release);
        }

        // Called from the single consumer thread.
        //
        // Returns nullptr when nothing can be safely dequeued RIGHT NOW. That
        // is not the same as "the queue is empty forever" -- it also covers
        // "a producer is mid-enqueue". Callers treat nullptr as "check
        // empty(), then maybe park"; that second check is what distinguishes
        // the two cases.
        TaskPromiseBase* dequeue()
        {
            TaskPromiseBase* tail = tail_;
            TaskPromiseBase* next = tail->next.load(std::memory_order_acquire);

            if (tail == &stub_)
            {
                // First real node, or nothing. The stub itself must never be
                // handed out -- a consumer has no business resuming it.
                if (next == nullptr)
                {
                    return nullptr;
                }
                tail_ = next;
                tail = next;
                next = next->next.load(std::memory_order_acquire);
            }

            if (next != nullptr)
            {
                // Comfortable case: at least two real nodes are linked, so the
                // consumer may advance tail_ and return the old tail without
                // touching head_ at all.
                tail_ = next;
                return tail;
            }

            // next == nullptr: `tail` may or may not be the last node.
            // If head_ still points at tail, no producer has claimed a
            // successor yet and we are free to report empty. If head_ has
            // moved, a producer is mid-enqueue -- fall through to the stub
            // trick instead of returning nullptr and stranding the node.
            if (const TaskPromiseBase* head = head_.load(std::memory_order_acquire); tail != head)
            {
                return nullptr;
            }

            enqueue(&stub_);
            next = tail->next.load(std::memory_order_acquire);
            if (next != nullptr)
            {
                tail_ = next;
                return tail;
            }
            return nullptr;
        }

        // Pop up to max_count nodes and hand each to fn.
        //
        // Bounded on purpose: the worker drains in batches so that a producer
        // storm cannot starve the event loop's submit/wait cycle. Draining
        // forever would let a hot queue delay CQ processing without bound.
        //
        // Returns how many were actually dequeued, which the worker uses to
        // decide whether it did useful work this round.
        template <typename Fn>
        std::size_t drain(Fn&& fn, const std::size_t max_count = std::numeric_limits<std::size_t>::max())
        {
            std::size_t count = 0;
            while (count < max_count)
            {
                TaskPromiseBase* node = dequeue();
                if (node == nullptr)
                {
                    break;
                }
                fn(node);
                ++count;
            }
            return count;
        }

        /// Consumer-side only. tail_ always points at the next node dequeue() will
        /// return, so the queue is empty only if that is the stub with nothing linked
        /// behind it. Comparing head_ to tail_ instead reported "empty" while exactly
        /// one real node was still pending (head_ == tail_ == that node), which let
        /// the worker park with a task stranded in the queue.
        ///
        /// A producer between its exchange and its link store reads as empty; that
        /// is fine. It costs at most one heartbeat of latency, because IO's park
        /// is bounded and the worker comes back and re-checks.

        /// INVARIANT: this is the ONLY safe emptiness test. It is a
        /// "nothing is dequeueable RIGHT NOW" predicate, not a memory barrier:
        /// it does not prevent a producer that has already exchanged head_ from
        /// completing immediately afterwards. Delivery is guaranteed by time,
        /// not by this load.
        bool empty() const noexcept
        {
            return tail_ == &stub_ && stub_.next.load(std::memory_order_acquire) == nullptr;
        }

    private:
        // Three cache lines, three owners. A producer and the consumer writing
        // the same line would ping-pong it on every push; separating them keeps
        // each side's store uncontended.
        //
        // head_: written by producers (exchange), read by the consumer only in
        //        the "is tail really the last node?" check.
        alignas(64) std::atomic<TaskPromiseBase*> head_;

        // tail_: consumer-private. Never touched by a producer.
        alignas(64) TaskPromiseBase* tail_;

        // Dummy node to prevent empty-queue race conditions. Producers store to
        // stub_.next when it is the last node, so keep it off the consumer's tail_ line.
        //
        // Note the mixed access: `next` is atomic because a producer writes it
        // during the enqueue(&stub_) trick, while the rest of the stub is
        // conceptually consumer-owned. Aligning it separately from tail_ stops
        // a producer's link store from invalidating the consumer's tail_.
        alignas(64) TaskPromiseBase stub_{};
    };
} // namespace kio::detail
