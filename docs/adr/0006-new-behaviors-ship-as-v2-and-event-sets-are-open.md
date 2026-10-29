# The new behaviors ship as v2.0.0, and event and reason sets are open from then on

The new order behaviors append alternatives to `BookEvent` (`OrderExpired`, `SelfTradePrevented`, `StopTriggered`, `OrderReplaced`, `ReplaceRejected`) and enumerators to `RejectionReason`, and the replay library appends `ReplaceCommand` to `Command` and new `ParseErrorCode`s. Each addition breaks, at compile time, any caller that handles every alternative with no fallback: a `std::visit` over an overload set without a generic case, an `if constexpr` chain ending in a `static_assert` (as `formatEvent` is written), or a `switch` without `default` under `-Wswitch -Werror`. v1.0.0 never said these sets could grow, so we release the behaviors as **v2.0.0** rather than v1.1.0, and README's Public API section states a policy from 2.0 on: these variants and enums are open. New alternatives and enumerators are only ever appended, existing variant indices and enumerator values never change, callers keep a fallback, and later additions are minor releases.

## Considered options

- **v1.1.0, documenting the break in the release notes.** It honours the map's preference for v1.x, but a v1.0 caller with an exhaustive visit would stop compiling on a minor upgrade, which semver forbids and v1.0's docs gave them no reason to expect.
- **Avoid the break:** report the new outcomes through fields on existing events, or through getters on the results. That would bend `OrderCancelled` and `OrderAccepted` into meaning several things, which the behavior tickets rejected one by one.

## Consequences

- The CMake project version becomes 2.0.0. Otherwise the release is additive: a program that uses only v1's commands gets exactly v1's results, and every v1 replay file still parses (its summary gains the new statistics lines).
- Appending, never inserting, is now a rule: a new alternative goes last in its variant, and a new enumerator last in its enum.
