# Order book

A limit order book and matching engine: it accepts orders, matches them by price-time priority, and reports what happened as events.

## Orders

**Order request**:
What a caller submits to the book: the order's identity, side, size, price, and the instructions that say how it may trade.
_Avoid_: new order, submission (a submission is the act of sending one)

**Resting order**:
An order the book holds at a price level, waiting to be matched. Only limit orders rest.
_Avoid_: open order, working order, live order

**Order type**:
Whether an order has a limit price and whether it waits for a trigger: limit, market, stop, or stop-limit (FIX OrdType).
_Avoid_: kind

**Limit order**:
An order that trades only at its limit price or better.

**Market order**:
An order with no limit price, which takes the best prices available. It never rests: whatever it cannot trade at once expires.

**Stop order**:
An order that waits until it triggers, then becomes a market order.
_Avoid_: stop-loss, stop-market

**Stop-limit order**:
An order that waits until it triggers, then becomes a limit order at its limit price. Its stop price and limit price may stand in any relation.

**Pending stop**:
A stop or stop-limit order the book holds until it triggers or is cancelled. It is not a resting order: it sits at no price level and has no place in time priority until it triggers. Its ID is live, as a resting order's is, so no other order may use it meanwhile.
_Avoid_: waiting stop, untriggered stop, resting stop

**Cascade**:
The stops a single command triggers and runs after the submitting order has finished, including those triggered by each other's trades. The command reports the cascade's events together with its own.

**Stop price**:
The price at which a stop or stop-limit order triggers (FIX StopPx). Distinct from the limit price.
_Avoid_: trigger price, stop limit

**Last trade price**:
The price of the most recent trade on this book. It is what stop and stop-limit orders trigger on. A prevented self-trade is not a trade and does not set it.
_Avoid_: last price, market price, reference price

**Trigger**:
Said of a stop or stop-limit order when a trade on this book is at or through its stop price: at or above it for a buy, at or below it for a sell.
_Avoid_: activate, elect, fire

**Time in force**:
How long an order may remain working: good-till-cancelled, immediate-or-cancel, or fill-or-kill (FIX TimeInForce).
_Avoid_: validity, duration

**Good-till-cancelled (GTC)**:
An order whose unfilled remainder rests until it is filled or cancelled. Every resting order is GTC.

**Immediate-or-cancel (IOC)**:
An order that trades what it can at once; the rest expires.
_Avoid_: fill-and-kill, FAK

**Fill-or-kill (FOK)**:
An order that trades its whole quantity at once or not at all, and then its whole quantity expires.
_Avoid_: all-or-none

**Expire**:
Said of an accepted order's unfilled remainder that the book drops instead of resting, because the order may not wait, because resting it would mean taking liquidity it may not take, because self-trade prevention stopped it, or because it is a stop or stop-limit order that arrived already past its trigger. Expiring is not cancelling: no cancel was requested, and the remainder never rested since the order was accepted or last replaced.
_Avoid_: cancel, kill, eliminate

**Post-only**:
An instruction that the order may only add liquidity, never take it (FIX ExecInst "participate, don't initiate"). A post-only order that would take liquidity on arrival expires whole; it is never repriced.
_Avoid_: maker-only, book-or-cancel, add-liquidity-only

**Owner**:
Whoever an order belongs to, for self-trade prevention only: two orders with the same owner may not trade with each other. An order may have no owner, and then it can trade with anyone. The book gives the owner no other meaning.
_Avoid_: account, trader, firm, participant

**Self-trade prevention (STP)**:
Stopping an incoming order from trading with a resting order that has the same owner. The incoming order's instruction says which order gives way: the resting order is cancelled whole and the incoming order goes on matching, the incoming order's remainder expires, or both. A prevented match is not a trade.
_Avoid_: self-match prevention, SMP, wash-trade prevention

**Open quantity**:
What an order may still trade: its total quantity less what has already traded. A replace sets a new total, so it sets a new open quantity too.
_Avoid_: leaves quantity, outstanding quantity

**Time priority**:
An order's place in the queue at its price level: among resting orders at one price, the one that has held its place longest trades first.
_Avoid_: queue priority, FIFO position

**Replace**:
Changing a resting order's price, quantity, or instructions in one command, under the same order ID (FIX cancel/replace). The quantity given is the new total, including what has already traded. The order keeps its time priority only when its price is unchanged and its quantity is not increased; otherwise it goes to the back of the queue at its new price, and may trade on the way, as if it had just arrived. Its side never changes.
_Avoid_: amend, modify, cancel-replace (except when citing FIX)

**Malformed**:
Said of an order or order request whose fields, alone or in combination, could never be accepted, whatever the book holds.
_Avoid_: invalid (too broad: a well-formed order can still be rejected)

## Books and records

**Instrument**:
One tradable thing, with its own book. Orders never cross instruments, and the book gives an instrument no other meaning: no tick size, lot size, or decimal price. Order IDs and trade IDs mean something only within one instrument.
_Avoid_: product, security, market, symbol (a symbol names an instrument)

**Symbol**:
The short name that identifies an instrument, and its only identity in any command or record. It never changes for the life of the instrument.
_Avoid_: ticker, instrument ID, security ID

**Instrument list**:
The instruments an engine may trade, fixed when it starts. A later start may add to it but never remove from it.
_Avoid_: universe, security master

**Shard**:
A group of instruments handled together by one matching thread, sharing nothing with any other shard. Every instrument belongs to exactly one shard.
_Avoid_: partition, worker

**Matching thread**:
The one thread allowed to touch a shard's books, from when a book is built or restored until it is destroyed.
_Avoid_: business thread, engine thread, worker thread

**Placement**:
Which shard each instrument belongs to. It never changes while the engine runs, and changes between runs only after a clean stop.
_Avoid_: assignment, allocation, mapping

**Command sequence**:
The order in which an instrument's matching thread executes that instrument's commands. It alone determines the instrument's book and events, and the command log records it. It includes well-formed commands the book rejects, never malformed ones, which are refused before they enter it. Nothing orders one instrument's commands against another's.
_Avoid_: accepted commands, order flow

**Command queue**:
Where a command waits on its way to a shard's matching thread. It is bounded. A command in it will execute and cannot be withdrawn, even if its connection closes. It enters the command sequence when the matching thread takes it.
_Avoid_: inbox, mailbox, buffer

**Doorbell**:
How a producer wakes a sleeping thread that consumes a queue: a shard's matching thread, which producers ring after each batch, or its log writer, which the matching thread rings after each take. There is one for each.
_Avoid_: signal, event, notifier

**Stop marker**:
A producer's last element on a command queue, saying it will send nothing more. It sits beside a command and is never one, so it can never reach the log.
_Avoid_: poison pill, sentinel

**Hold marker**:
What a matching thread puts on a response queue before each batch of commands it takes, naming the log position they all need. A gateway thread drains no further on that queue until the durable position reaches it, so the queue itself holds their responses back.
_Avoid_: barrier, fence, watermark

**Response queue**:
Where a command's events wait on their way back from a matching thread, and where they stay until the command is durable. It is bounded, and a matching thread with no room waits rather than lose an event.
_Avoid_: outbox, result queue

**Parked**:
Said of a connection that is no longer being read because the command queue its next command needs is full. It resumes when that queue has room; being parked refuses and loses nothing.
_Avoid_: blocked, throttled, suspended

**Capacity**:
How much a book is prepared to hold without allocating memory: resting orders, pending stops, and price levels. It changes no outcome, so a book past its capacity grows and never refuses a command.
`OrderBook::holding` gives what a book holds now in the same three numbers, which is not its capacity.
_Avoid_: limit (a limit refuses something), size (what a book holds now)

**Warm**:
Said of a book prepared up to its capacity, and of a shard whose books all are. A shard reports ready only once it is warm.
_Avoid_: hot, primed, preloaded

**Snapshot**:
A summary of a book's depth at one moment: its best levels on each side, with their aggregate quantities. It describes a book; it cannot rebuild one.
_Avoid_: checkpoint, dump

**Book image**:
The complete state of one book at one moment, from which an identical book can be rebuilt. It says nothing of which instrument it belongs to or where in a command log it stands.
_Avoid_: state image, dump

**Checkpoint**:
One instrument's book image, published with its symbol and log position once it has been proved to restore. Each book has its own; no checkpoint covers a shard or the engine.
_Avoid_: snapshot, dump

**Command log**:
The sequence of commands a book or engine was given, recorded so that replaying it reproduces the same books and events. It records commands only, never their results.
_Avoid_: journal, event log, audit log

**Log position**:
A place in a command log, counted in commands from the log's start. A checkpoint at a log position reflects every command for its instrument before that place, and none at or after it.
_Avoid_: offset, sequence

**Log writer**:
The one thread allowed to write a shard's command log. It never touches a book, and the matching thread never touches the log.
_Avoid_: journaler, log thread

**Durability level**:
What a response promises about its command: that it survives the engine crashing, or that it survives the machine crashing too.
_Avoid_: sync mode, fsync policy

**Durable position**:
How far a shard's command log is safe at its durability level. A command may already have matched past this point, but its response is not released until the durable position has passed it, so a crash can lose only commands nobody was told about.
_Avoid_: commit position, safe position, synced position

**Torn tail**:
The incomplete end a crash can leave on a command log: a record cut short, or anything else with no valid record after it. It is expected after a crash, unlike damage with valid records following, which is expected only as a torn write.
_Avoid_: truncated log, corrupt tail

**Torn write**:
A log write a machine crash met on its way, of which some pages or sectors reached the drive and others did not, so it can leave a hole that valid records follow. Recovery cuts one at the hole if it lies within 1 MiB, the most one write holds, of the end of the last file; further back, no crash explains it.
_Avoid_: partial write

**Log file**:
One piece of a shard's command log, of a fixed size. Once closed it is complete and safe on disk, and it is what a checkpoint is rolled forward from.
_Avoid_: segment, journal file

**Housekeeping thread**:
The engine's one thread that makes each shard's next log file before it is needed, so no matching thread ever waits for one, and a log writer only when the drive is slow.
_Avoid_: file manager, allocator thread

**Clean stop**:
A stop that answers everything queued, ends the log, and leaves every book's checkpoint at its log's end, so recovery has nothing to replay. It is the only stop after which placement may change.
_Avoid_: graceful shutdown, drain

**Forced stop**:
A stop that ends the engine at once, without any of a clean stop's steps, so recovery does the rest. Nothing a client was told is lost, but what was still queued gets no answer.
_Avoid_: hard stop, kill

**Poison command**:
A command that breaks a book the same way on every replay, so recovery cannot finish until the log is cut before it.
_Avoid_: bad command, toxic command

**Roll forward**:
Making a book's next checkpoint from its last one by replaying the command log after it, away from the matching thread, so the live book is never read or paused.
_Avoid_: compaction, shadow book

**Replay**:
Feeding a recorded sequence of commands to fresh books, which produces the same books and events as the original run.
_Avoid_: recovery (recovery is a replay that starts from a checkpoint)

**Restore**:
Rebuilding a book from a book image, without running any command.
_Avoid_: load, recover

**Recovery**:
Bringing the books back after a stop or crash: restoring each from its checkpoint, its newest that restores or the older one, then replaying the command log after it, each shard from its oldest book's position, skipping each command a book's checkpoint already holds.
_Avoid_: restore, restart

## The gateway

**Gateway**:
The part of the engine that clients talk to. It takes commands from connections, refuses those that may not enter, and sends each command's events back to whoever sent it.
_Avoid_: server, front end

**Gateway thread**:
One of the threads that serve connections. A connection stays on one gateway thread for its whole life.
_Avoid_: I/O thread, network thread

**Connection**:
One ordered stream of bytes each way between a client and the gateway: a TCP connection natively, an in-page connection in a browser tab, which behaves the same. Nothing about it outlives it: no identity, no numbering, nothing to resume. Its resting orders are unaffected when it closes, and the book never knows which connection sent a command.
_Avoid_: session (nothing here survives a reconnect, and the word also names a trading session), client (a client may open several)

**Frame**:
One message on a connection, in either direction.
_Avoid_: packet, message

**Correlation**:
A number the client picks for a command, which every response to that command echoes. The engine never reads it or requires it to be unique.
_Avoid_: request ID, client order ID

**In flight**:
Said of a command handed to a command queue whose response has not been fully received back. A command refused at entry is never in flight.
_Avoid_: pending (a pending stop is something else), outstanding

**Backlogged**:
Said of a connection that is no longer being read because its client has fallen behind on its responses. It resumes once the client catches up, and is closed if it falls too far behind. Parked means the engine cannot take more; backlogged means the client is not taking its answers.
_Avoid_: parked, throttled, slow consumer

**Refused**:
Said of a command the gateway turns away at entry, so that it never reaches a command queue or the command log: its instrument is unknown, it is malformed, or the engine is stopping. A refused command has certainly not run.
_Avoid_: rejected (a rejection is the book's answer to a command in its command sequence, though a malformed command is refused with the same reason the book would give)

**Protocol error**:
A fault in a connection's frames rather than in a command: nothing can answer it as a command, so the gateway answers what is in flight, reports the error, and closes the connection.
_Avoid_: reject, parse error (a parse error belongs to a file)

## In the browser

**Engine worker**:
The Web Worker that holds the engine when it runs in a browser tab. The page talks to the engine only through it, so nothing the page does can hold the engine up, and ending it ends the whole engine at once.
_Avoid_: engine thread, wasm worker

**Page store**:
The copy of the engine's command logs and checkpoints that the page keeps, outside the engine worker, so ending the worker leaves it whole. Each new engine worker starts from it, and a reload loses it.
_Avoid_: drive, mirror

**Engine client**:
The part of the page that speaks to the engine worker. It holds the page store, hands it to each new engine worker, and is the only way the rest of the page reaches the engine.
_Avoid_: bindings, SDK, wrapper

**Shadow book**:
A copy of one instrument's book that the engine worker keeps by running every logged command again, so the page can see the whole book (every level, owner, and pending stop) without asking the engine. Because each answer reaches the page only after its log write, a shadow book already holds every command the page has seen answered.
_Avoid_: mirror book, snapshot, replica

## Measurement

**Service time**:
How long a piece of work takes from the moment it actually started. It says nothing about any wait before that.
_Avoid_: latency (which could mean this or response time)

**Response time**:
How long a command takes from the moment it was scheduled to be sent, so it includes any wait behind earlier commands or a stall.
_Avoid_: latency (which could mean this or service time)

**Noise floor**:
What the machine itself costs a thread that does nothing but watch the clock. A result at or beyond it describes the machine, not the engine.
_Avoid_: jitter, hiccups
