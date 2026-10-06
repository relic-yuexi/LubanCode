# Policy callback lifecycle boundary

This slice starts at `a23f98e6`. It connects existing Policy callbacks and owned
capture retirement to the SDK's existing callback barrier. It does not add a
Managed Session facade, authentication, Session ACL enforcement or another policy
table. Public authorization declarations, decisions and scope rules stay fixed.

Authorize and SubscribeChanges must move/swap their by-value provider and callback
parameters into locals after entering the barrier. Those owned values retire
before the barrier exits, including invalid arguments, provider exceptions and
allocation failures after admission. A moved-from small std::function source must
be explicitly emptied while the barrier is still active. This does not claim to
guard argument construction or caller-owned copies outside the SDK invocation.

Provider Authorize/Subscribe, change notifications, subscription closers and
SDK-owned callable capture destructors enter the same SDK lifecycle boundary.
Stored callable owners also guard their own final callable destruction, including
when an arbitrary provider drops its callback on another thread. Captures are
retired outside policy, observer and subscription mutexes, as before.

Blocking Runtime::OpenSession, WaitResult, WaitJob, Session::Close and
Runtime::Shutdown then return the existing lifecycle-reentrant error before
locking. EventStream::Next and CloseChecked gain only a Policy-specific check;
other callback classes keep their existing behavior. Ordinary calls outside a
Policy callback remain usable after the guard restores its previous state.

The Policy callback flag is separate from notification_depth. A notification
still disarms subscriptions without waiting for itself or another notification;
an external unsubscribe still drains real in-flight callbacks. Cross-subscription
unsubscribe, coalescing, revision semantics and idempotent repeated cleanup keep
their original rules. This is not permission caching or a replacement for the
notification protocol.

Callbacks and destructors remain trusted/cooperative. Calling a guarded SDK API
does not permit destroying the last Runtime/Session owner, or the last EventStream
owner still in use, from within the callback. A rejected CloseChecked call does
not defer the later Impl/EventQueue destructor. Do not unload the SDK while its
callbacks remain alive. Deferred destruction is outside this slice.

Existing authorization cases gain real Runtime/Session, event and waiting-call
counterexamples, synchronous/asynchronous notifications, closer/capture retirement
and invalid-input/exception cleanup. Existing case names, markers, timeouts and
cross-unsubscribe/drain tests remain. Native execution and concurrency validation
run only on the exact integrated source in remote CI. Local work is source,
documentation and pure-data checks; no configure, compile, CTest, native process,
HTTP, push or new PR is performed.
