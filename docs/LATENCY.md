# Latency

What one command takes, measured under the rules below. **No baseline has been recorded yet**, so this report holds no numbers. The method is in [`benchmarks/latency/README.md`](../benchmarks/latency/README.md).

## The claims rule

Every statement drawn from these results follows this rule, and so does anything quoting them.

**A report may say:** on this named machine and build, with this run record, this interval had these percentiles at this rate, and a change moved them by more than A/A.

**A report must refuse:**

- "low latency", "microsecond", or "exchange grade", in any form;
- any comparison with an exchange or another system;
- "wire-to-wire" or anything implying a network, since loopback is not one;
- a number without its interval and run record, or a single headline number;
- subtracting one interval from another;
- attributing a marked noise-floor percentile to the engine, or treating max as a guarantee;
- any `not sustained` rate quoted as a capability;
- any Linux or other-machine claim drawn from these runs;
- quoting a `steady_clock` result or the throughput harness's ns/command as latency.

## Intervals

| Interval | From | To |
|---|---|---|
| **Command** | Before one book call | After it returns |
| **Hand-off** | Into the command ring | Back out of the response ring |
| **Round trip** | Scheduled send, over loopback through the shipped engine | Frame flagged last |
| **Durable wait** | A record enters the log ring | The durable position covers it |

The command and hand-off intervals exist so far. The others land with what they time. `--pin-core` covers the sending thread alone, so a hand-off row prints `unpinned` until the matching thread can be pinned too.

## Results

None recorded.

## What can exceed these numbers

Three things can take a command past every percentile here. Their figures come from `order_book_component_benchmarks`, and none is recorded yet.

- A capacity growth.
- A new level far from the touch.
- An image flush on the log's drive.
