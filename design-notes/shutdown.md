# Shutdown: one force mode in the runtime, graceful as an application pattern

Status: LIVING DOCUMENT — decided 2026-10-14, after the stop-semantics review
sequence (run() self-questions → suggested rewrite review → the call, user
rulings: ONE stop mode, no grace for new requests, rest delegated).
Related: `sharding.md` §4/§6 (control plane, the waker ban), `ready-ring.md`
(the loop this extends), `buffer-pool.md` (the parked-waiter CHECK).

## The contract

`run(std::stop_token st = {})` has exactly ONE stop semantic: **force-drain**.

1. `run()` returns only when quiesced (`ready_list_` empty && `inflight_ == 0`).
   There is no other exit. Stop never abandons work.
2. The token is read at exactly ONE point — the loop's arming check, after the
   resume phase. A stop request latches `canceling_all_` (unconditionally: with
   nothing in flight the cancel SQE completes `-ENOENT` under `kIgnoreTag`) and
   stages `CANCEL_ALL|ANY`. Everything downstream — fail-fast staging, the
   bounded wait, the ECANCELED classification in `await_resume` — is driven by
   the latch, never by the token again. The token is an edge adapter in,
   internal latch out.
3. **Fail-fast staging**: while latched, `IoAwaitable::await_suspend` refuses
   to stage (no SQE, no `op_started`), completes the awaitable inline with
   `-ECANCELED` (`await_suspend` returns `false` = never suspended). During a
   drain `inflight_` is monotone non-increasing → quiesce is a termination
   PROOF, not a hope. `inflight_`-accounting ops staged before the latch was
   observed complete normally, then their tasks hit fail-fast on re-await.
4. **Bounded wait**: whenever the loop blocks with a stop conceivably
   observable (`st.stop_possible()` or latched), the block carries a 50 ms
   in-kernel deadline (`io_uring_submit_and_wait_timeout`; `EXT_ARG` kernels
   6.8/7.0 — the timespec rides inside `enter`, no timerfd, no extra SQE, no
   `inflight_` interaction). `-ETIME` = the normal idle wake, normalized to 0.
   The unbounded `submit_and_wait` survives only when NO stop can ever be
   observed: `!st.stop_possible() && !canceling_all_` (default token, or every
   source destroyed unrequested). The tick is an internal `constexpr`, pinned
   by test — one stop mode, no knobs.
5. Baton/Semaphore/pool parks are NOT fail-fast: a park that survives into a
   drain hits the quiesce CHECK ("parked waiter with no one left to wake it")
   — the deadlock detector doing its job.

## Why the runtime cannot own "graceful for admitted work"

"Only already-admitted requests may progress" is application semantics. A
handler is a pipeline (read → process → write); classifying "new work" vs
"admitted work" requires classifying individual ops. Fail-fast classifies
temporally (staged-after-latch) — that kills step 2 of an admitted pipeline.
The kernel classifies nothing: to `reap()`, an accept CQE and a response-write
CQE are identical 16-byte records. And the admission boundary itself
(spawn? first read? headers?) is the app's model to define. Hence:

- **Happy path never touches the token.** The app drains itself to quiesce and
  `run()` returns through the ordinary quiesce exit with nothing ever
  cancelled. The token is purely the force channel — watchdog escalation,
  `jthread` destructor, Ctrl-C. Because graceful precedes any token request,
  the one-bit token IS the "escalate now" channel; no second bit needed.

## The application pattern (example 03 shape)

Two channels, two layers:

| concern | channel | mechanism |
|---|---|---|
| graceful trigger | app | atomic flag set by main (NOT the token — it is still un-requested) |
| door (stop admitting) | app | accept with linked timeout; each wake checks the flag → close listener. Backlog refused; admission boundary = successful accept |
| "no grace for new" (keep-alive) | app | handler reads with linked timeout; on flag, finish current response, close — the NEXT request is new work |
| tracking admitted work | app | counter + Baton composed-exit (the primitives' own discipline) |
| escalation | token | main: timed wait for drain, then `request_stop()` → force-drain |

Mechanical facts that dictate this shape: a stop request produces no CQE (the
50 ms tick wakes the LOOP; a suspended task needs its own wake — the linked
timeout); a pending accept keeps `inflight_ ≥ 1` forever (quiesce can never
fire while a listener is armed). `std::jthread`'s choreography (request stop,
join for orderly return) is this contract verbatim.

## Costs consciously accepted

- **Dead source = un-stoppable** (row 3 of the token's 3-valued state machine):
  destroying every `stop_source` without requesting flips `stop_possible()` to
  false; the loop takes the unbounded wait forever. Correct — nobody can ever
  signal it — but the opposite of an ownership-transfer intuition. Pinned by
  test, documented here.
- **Tick while armed**: a live un-requested stop source makes every
  blocked-idle loop wake ~20×/s. Negligible vs 143k req/s/core; the price of
  no-waker-machinery (sharding.md §6).
- **Unkillable remainder**: a task that observes `-ECANCELED` and re-awaits
  forever spins in Phase 1 (fail-fast = inline resume); one that never awaits
  again just never finishes. No mechanism can win — `-ECANCELED` during drain
  is terminal, documented contract.
- Pre-latch staged ops complete normally during a drain (the kernel cannot be
  told otherwise); their tasks fail fast on the next await.

## Deferred (with triggers)

- **Runtime-classified graceful (admission generations)**: spawn-epoch +
  current-task tracking at the single resume point + an in-flight op registry
  for door cancellation — feasible BECAUSE all resumes go through Phase 1.
  Revisit when a second real graceful-shutdown user feels the two-channel
  duplication AND `when_all` exists.
- **Two-level Shutdown type** (`request_graceful()/request_force()`): only
  when the deadline-only escalation genuinely hurts.

## Tests (RunLoopTest)

- `StopAsLastActStillReturns` — the old quiesce-guard hang, deterministic.
- `StopDuringBlockedWaitDrainsBounded` — tick wake + `-ETIME` convention +
  drain reaches a parked op (ECANCELED, not a hang).
- `RetryLoopDrainsInsteadOfLivelocking` — fail-fast termination proof.
- `SourceDestroyedWithoutRequestStillQuiesces` — row 3.
