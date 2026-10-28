# How FIX and major venues define the new order behaviors

Research for [issue #5](https://github.com/ThomasStatic/order-book/issues/5) on the [wayfinder map, #4](https://github.com/ThomasStatic/order-book/issues/4). Read 2026-10-27/28.

This survey describes what FIX and four venues define for immediate-or-cancel (IOC), fill-or-kill (FOK), market orders, post-only, stop and stop-limit, self-trade prevention (STP), and cancel-replace. It records what the sources say and recommends no design. Each behavior has a comparison table and a short summary of where the sources agree and where they differ.

Citations are reference links (for example [cme-qual]), resolved in [Sources](#sources) at the end. A gap marked "not stated" means the pages read say nothing about it. Nothing has been filled in from memory.

## Scope and source caveats

- **FIX.** `fiximate.fixtrading.org` now redirects (HTTP 308) to Orchimate (`orchimate.org`), which hosts the FIX Trading Community's FIX Latest Orchestra repository. The legacy FIX 5.0SP2 FIXimate URL redirects to the FIX Latest landing page, so the 4.4 and 5.0SP2 dictionaries could not be read on a fixtrading.org-hosted page. For 4.4 and 5.0SP2, this survey uses the OnixS FIX Dictionary, a third-party rendering of the published specification, and labels it as such. Orchimate's page text truncates long elaborations at 125 characters, so the full FIX Latest wording of some values (IOC and FOK among them) was not visible.
- **CME Globex.** The CME Client Systems Wiki renders its pages client-side, so plain fetches returned empty bodies. Its pages were read as rendered page bodies through the wiki's public Confluence REST API (`/wiki/rest/api/content/<id>?expand=body.view`). The links below are the ordinary page URLs. The cmegroup.com SMP FAQ timed out twice and was not read.
- **Nasdaq.** The OUCH 5.0 and TotalView-ITCH 5.0 PDFs were read. The Nasdaq rulebook (`listingcenter.nasdaq.com`) returned HTTP 403, so the current text of Rule 4702 (order types, including Post-Only) could not be read. Post-only behavior below comes from the SEC's 2016 approval order (Federal Register) and from OUCH's cancel reasons, and may predate later amendments.
- **Eurex.** The T7 Release 15.0 Functional Reference Part 1 (Version 2, 2 October 2026) and the T7 Release 15.0 ETI Manual (the file served was Version 3, 23 September 2026) were read. T7 is shared by Eurex, Xetra, EEX, and others. Only statements that apply to Eurex, or to all of T7, are attributed to Eurex here.
- **Binance.** Spot API documentation (REST, FIX, enums, errors, and FAQs) on developers.binance.com.

---

## 1. Immediate-or-cancel (IOC)

| Source | How expressed | Accept / reject / cancel | Unfilled remainder | Reported |
|---|---|---|---|---|
| FIX 4.4 / 5.0SP2 / Latest | `TimeInForce(59)=3` [fix44-tif] [fixlat-tif] | Executes in whole or in part as soon as it is represented in the market [fix44-glossary] | Any portion not executed at once is cancelled [fix44-glossary] | `ExecType(150)` has both `4` Canceled and `C` Expired [fixlat-exectype]. The pages read do not say which one reports an IOC remainder |
| CME Globex | Called FAK (Fill and Kill): `59=3` [cme-elim] [cme-qual] | Executes immediately against resting orders. Accepted only in the Open market state, rejected in any other [cme-qual]. Optional MinQty: if the minimum cannot be filled at once, the whole order is cancelled [cme-qual] | Cancelled [cme-qual] | Execution Report - Elimination, `35=8 39=C 150=C` (Expired) [cme-elim] |
| Nasdaq (OUCH 5.0) | `Time In Force = 3` (IOC), mapped to FIX tag 59 [ouch5] | Not stated beyond the TIF value | Cancelled | Order Canceled message (`C`) with reason `I` (Immediate or Cancel Order). The cancelled quantity is incremental, not cumulative [ouch5]. An Order Accepted with Order State `D` (Order Dead) means accepted and then automatically cancelled [ouch5] |
| Eurex (T7) | Validity IOC [t7-fr] | Processed on entry, then deleted without ever going to the book. In the Book or auction instrument states, IOC orders are deleted instead of booked [t7-fr] | Deleted [t7-fr] | Execution Report: `OrdStatus=4` Cancelled, `ExecType=4` (or `F` Trade if partly filled), `ExecRestatementReason(378)=105` "IOC order cancelled" [t7-eti] |
| Binance Spot | `timeInForce=IOC` (FIX `59=3`) [bn-enums] [bn-fix] | Fills as much as possible [bn-enums] | Expires [bn-enums] | Status `EXPIRED` (cancelled under the order type's rules, for example a partially filled IOC) [bn-enums] |

**Agree.** Everyone uses FIX `TimeInForce=3` or maps to it, and nobody books the remainder: it is removed in the same transaction.

**Differ.**
- *Naming.* CME calls IOC "FAK".
- *How the remainder is reported.* It is reported as expired at CME (`39=C`) and Binance (`EXPIRED`), but as cancelled at Eurex (`39=4`, reason 105) and at Nasdaq (Canceled, reason `I`).
- *Market states.* CME rejects FAK outside the Open state. Eurex accepts IOC in Book or auction states but deletes it unmatched.
- *Minimum quantity.* CME lets an IOC carry a minimum fill quantity.

## 2. Fill-or-kill (FOK)

| Source | How expressed | Accept / reject / cancel | Unfilled remainder | Reported |
|---|---|---|---|---|
| FIX | `TimeInForce(59)=4` [fix44-tif] [fixlat-tif] | Executes in its entirety as soon as it is represented, or the whole order is cancelled. No partial fill [fix44-glossary] | None, because the order is all-or-nothing | Not stated in the pages read |
| CME Globex | Futures and options: FAK (`59=3`) with `MinQty(110)` equal to `OrderQty(38)` [cme-qual] [cme-elim]. iLink 3 accepts `59=4` only for BrokerTec and EBS [cme-elim] | Accepted only in the Open state [cme-qual]. If the quantity in tag 110 cannot be executed at once, the entire order is eliminated [cme-elim] | The whole order is eliminated | Execution Report - Elimination (`39=C`) [cme-elim] |
| Nasdaq (OUCH 5.0) | No FOK time-in-force. OUCH's TIF values are 0, 3, 5, 6, and E [ouch5] | Not offered as a TIF. OUCH has a `MinQty` option (it "must be a round lot"), but does not define its matching semantics [ouch5] | n/a | n/a |
| Eurex (T7) | Validity FOK [t7-fr] | On entry, T7 checks whether the order is fully executable. If it is, the order executes. If not, it is deleted without any execution [t7-fr]. FOK is available "for all cash markets on T7 and for EEX" [t7-fr], and the ETI table marks FOK as cash-only [t7-eti], so **Eurex derivatives do not offer FOK** in these documents. In synthetic matching T7 may miss some combinations and delete an FOK that could have filled [t7-fr] | None | `OrdStatus=4`, `ExecType=4`, `ExecRestatementReason=107` "FOK order cancelled" [t7-eti] |
| Binance Spot | `timeInForce=FOK` (FIX `59=4`) [bn-enums] [bn-fix] | Expires unless the full quantity can be filled immediately [bn-enums] | None | Status `EXPIRED` [bn-enums] |

**Agree.** Wherever FOK exists it is all-or-nothing and immediate, and a kill leaves the book untouched.

**Differ.**
- *Availability.* Nasdaq OUCH has no FOK TIF, and Eurex's derivatives market does not offer FOK.
- *Encoding.* CME futures express FOK as IOC plus a minimum quantity, not as `59=4`.
- *Reporting.* A killed FOK is reported as expired at CME and Binance, and as cancelled at Eurex.
- *Interaction with STP.* Eurex documents an extra rule: SMP is applied to an FOK only if the order is "potentially fully executable considering all price levels"; otherwise the FOK is deleted with no effect on the book [t7-fr]. (See section 6.)

## 3. Market orders

| Source | How expressed | Accept / reject | Price protection | Unfilled remainder | Reported |
|---|---|---|---|---|---|
| FIX | `OrdType(40)=1` Market, "an order without a price" [fixlat-ordtype]. In 4.4: buy or sell at "the most advantageous price obtainable" [fix44-glossary]. Related values: `K` Market With Left Over as Limit (the unexecuted quantity becomes a limit order at the last executed price) and `T` Market within price band (becomes a limit at the band bound if it cannot execute within the band) [fixlat-ordtype] [fix44-ordtype] | Not stated | Not part of `1`. `T` builds in a band | Not stated for `1`. Defined for `K` and `T` | Not stated |
| CME Globex | No unprotected market order. `40=1` is **Market with protection** and `40=K` is **Market-limit** [cme-ilink-types] | Both are rejected when no opposing bid or offer is working, and in any state except Open. Market-limit is also rejected if its designated limit is farther than the price bands from the Last Best Price [cme-ilink-types] | Protection price limit = best opposite price plus protection points (bids) or minus them (offers) [cme-types] | Market with protection: the remainder rests as a limit order at the protection price limit [cme-types]. Market-limit: it "becomes a limit order", and the remainder rests on the book. The worked example rests it at the first execution price, 90025 [cme-types] [cme-ilink-types] | Execution Report confirmation, then Fill Notices [cme-ilink-types] |
| Nasdaq (OUCH 5.0) | No market order type for continuous trading. A market order **for a cross** is flagged with the special price $214,748.3647. Prices of $200,000.00 or the maximum integer are also treated as market orders [ouch5] | Reject reason `0x002C` "Risk: Market Order Not Allowed" exists [ouch5] | Not stated in OUCH | Not stated in OUCH | Not stated |
| Eurex (T7) | Price condition Market (no limit price). Allowed for simple instruments only [t7-fr] | Options with `checkMarketOrder`: rejected in Book state, and in Continuous when there is no valid opposite bid or ask; resting market orders are re-validated, and cancelled if necessary, on the transition to Continuous [t7-fr] | Every Eurex and EEX market order is protected by the Market Order Matching Range, a price interval around the current market price outside which it may not match [t7-fr] | "When not matched they may be saved on the book like limit orders". Because of the range, a market order can rest even while the opposite side is not empty [t7-fr] | Not stated in the parts read |
| Binance Spot | `type=MARKET` with `quantity` or `quoteOrderQty`. FIX `40=1` [bn-trade] [bn-fix] | No `timeInForce` parameter for MARKET [bn-trade] | Not stated on the pages read | Expires. `EXPIRED` covers "partially filled ... MARKET" orders [bn-enums] | Status `EXPIRED` |

**Agree.** A market order carries no limit price and takes the best available prices.

**Differ.** The sources split three ways on the remainder:
- *It expires* (Binance).
- *It rests on the book* (Eurex, as a market order, inside a matching range).
- *It becomes a limit order* (CME at the protection limit or the first fill price; FIX `K` at the last executed price).

They also differ on protection and on whether the order exists at all:
- *Protection.* CME applies a protection band to every market order; Eurex applies a matching range; FIX `1` has none.
- *Availability.* CME rejects a market order when the opposite side is empty. Nasdaq OUCH only offers market orders for crosses.

## 4. Post-only

| Source | How expressed | Rule on entry | If it would trade | Reported |
|---|---|---|---|---|
| FIX | No post-only time-in-force. The closest instruction is `ExecInst(18)=6` Participate don't initiate: an order "that may participate in a transaction initiated by another party, but may not initiate a transaction" [fix44-glossary] [fix44-execinst] [fixlat-execinst]. (`TimeInForce=5` GTX, Good Till Crossing, is about auctions: it is "canceled prior to the market entering into an auction or crossing phase", not post-only [fixlat-tif].) Also `ExecInst=Z` Cancel if not best [fix44-execinst] | Not stated | Not stated | Not stated |
| CME Globex (futures and options) | None found in the pages read. iLink 3's `ExecutionMode(5906)=P` (Passive, "will rest in the market without executing unless matched with an Aggressive Order") applies to US and EU Repo only, and a cancel-replace with `5906=P` is rejected for futures and options [cme-elim] [cme-ocr]. BrokerTec's `ExecInst` "Only Best" is fixed-income only [cme-elim] | n/a | n/a | n/a |
| Nasdaq | OUCH option `PostOnly` (tag 12) = `P` [ouch5] | Per the 2016 SEC approval order: if it would lock or cross a Protected Quotation, it is repriced, ranked, and displayed one minimum increment away from the opposite best price. If it would lock or cross only a non-displayed order on Nasdaq's book, it is posted, ranked, and displayed at its limit price [fr-2016-27600] | It **executes** instead of posting if the price improvement is at least $0.01 per share (orders at $1.00 or more), or at least the fees plus forgone rebate (orders below $1.00) [fr-2016-27600]. OUCH also has cancel reasons for post-only orders cancelled instead of being "price slid" [ouch5] | Order Canceled with reason `F` (would have been price slid for NMS) or `G` (would have been price slid due to a contra-side displayed order) [ouch5]. Accepted price "will always be better than or equal to the entered price" [ouch5] |
| Eurex (T7) | Book-Or-Cancel (BOC), limit only. Market BOC is not supported, and BOC takes no other order restriction [t7-fr] | Never executed on entry. If it is not executable, it is accepted and booked [t7-fr] | If it could be partially or fully executed on entry, it is deleted at once without any execution. A modification that makes a booked BOC executable also deletes it [t7-fr] | `OrdStatus=4`, `ExecType=4`, `ExecRestatementReason=212` "BOC order cancelled" [t7-eti] |
| Binance Spot | `type=LIMIT_MAKER`. In FIX it is `40=2` with `ExecInst(18)=6` [bn-trade] [bn-fix] | A LIMIT order that is rejected if it would immediately match and trade as a taker [bn-trade] | **Rejected** with `-2010` "Order would immediately match and take." [bn-errors] | Rejection |

**Agree.** A post-only order may only add liquidity, and it is never filled as the taker in the form it was submitted.

**Differ.**
- *What happens when it would trade.* Eurex deletes it after acceptance, Binance rejects it, and Nasdaq reprices it to one tick away (or cancels it, or executes it when the price improvement is large enough). Nasdaq's version is not strictly "never take".
- *FIX vocabulary.* FIX has no dedicated value. Binance maps post-only onto `ExecInst=6`, Participate don't initiate.
- *Availability.* CME futures and options have no post-only in the documentation read.

## 5. Stop and stop-limit

| Source | Types | Accept / reject on entry | Trigger price and comparison | What a triggered order becomes | Reported |
|---|---|---|---|---|---|
| FIX | `OrdType=3` Stop, `4` Stop limit, stop price in `StopPx(99)` [fix44-ordtype] [fix-stoppx]. FIX Latest also defines `R` Stop on Bid or Offer and `S` Stop Limit on Bid or Offer (triggered by quote movement) [fixlat-ordtype], and `TriggerPriceType(1107)` = Best Offer, Last Trade, Best Bid, Best Bid or Last Trade, Best Offer or Last Trade, Best Mid [fixlat-triggerpricetype] | Not stated | 4.4 glossary: a buy stop triggers when the security trades at or above the stop price, a sell stop at or below [fix44-glossary]. FIX Latest `3`: "triggered as a result of a trade in the market" [fixlat-ordtype] | Stop becomes a market order. Stop limit becomes a limit order at its limit price [fix44-glossary] [fixlat-ordtype] | `ExecType=L` "Triggered or Activated by System" [fixlat-exectype] |
| CME Globex | Futures only: Stop-Limit (`40=4`) and Stop with Protection (`40=3`) [cme-types] [cme-ilink-types] | Continuous state: a buy stop must be above the last trade price and a sell stop below it, with the settlement price used if there is no last trade. Pre-Open and No Cancel states: compared against the settlement price [cme-types]. Rejected in any state except Pre-Open, Open, and No Cancel [cme-ilink-types]. For a buy stop-limit, the trigger is below the limit price; for a sell, above it [cme-ilink-types] | A trade in the market at the trigger price: "activated when the market trades at or through the stop trigger price" (stop with protection) [cme-types] | Stop-limit: a limit order at its limit price, which may fill at any level between trigger and limit, with the remainder resting at the limit. Stop with protection: a market order with protection limit = trigger plus or minus protection points, with the remainder resting at that limit [cme-types] | On acceptance, a stop with protection is acknowledged with `40=4`. On triggering, an Execution Report confirmation is sent with `40=2`, and later reports keep `40=2` [cme-ilink-types] |
| Nasdaq (OUCH 5.0) | OUCH 5.0 defines no stop order type and no stop-price field [ouch5] | n/a | n/a | n/a | n/a |
| Eurex (T7) | Stop Market and Stop Limit, for futures only (no stops for options) [t7-fr] | A buy stop is accepted only if its stop limit is above the Best Available Buy Limit; a sell stop only if it is below the Best Available Sell Limit. The best available limit is normally the limit price of the best limit order on that side of the book. All T7 exchanges reject stops that fail this check [t7-fr] | Eurex uses the **last trade price** only: a buy triggers when the new trade price is at or above the stop limit, a sell when it is at or below. Eurex stops are not triggered by a closing-auction price [t7-fr] | Converted to an incoming regular limit or market order. It gets a new priority timestamp and keeps its exchange order ID, and an immediate match is not guaranteed. Stops trigger at the end of the transaction that created the condition, one per transaction, alternating buy and sell lists ordered by stop limit then time [t7-fr] | `ExecType=L` (Triggered), `OrdStatus=0` if nothing executes [t7-eti] |
| Binance Spot | `STOP_LOSS` and `TAKE_PROFIT` (market when triggered), `STOP_LOSS_LIMIT` and `TAKE_PROFIT_LIMIT` (limit), and optional `trailingDelta`. FIX `40=3` and `40=4` map to these [bn-trade] [bn-fix] | Rejected with "Order would trigger immediately." when the stop price is invalid relative to the last traded price [bn-errors] | FIX documents `TriggerPriceType(1107)=2` LAST_TRADE [bn-fix]. The REST pages say "market price". BUY STOP_LOSS triggers when market price is at or above the stop price, SELL STOP_LOSS when it is at or below; TAKE_PROFIT is the reverse [bn-trade] [bn-trailing] | STOP_LOSS and TAKE_PROFIT "execute a MARKET order" [bn-trade]. The `_LIMIT` variants start working at their limit price [bn-trailing] | Not stated in the pages read |

**Agree.**
- *Trigger.* FIX, CME, Eurex, and Binance all trigger stops on the last trade price.
- *Comparison.* Buy stops trigger at or above the stop price and sell stops at or below.
- *Conversion.* A stop becomes a market order and a stop-limit becomes a limit order at its limit price.
- *Entry check.* CME, Eurex, and Binance refuse a stop that would trigger at once.
- *Reporting.* Triggering is reported as `ExecType=L` at Eurex and in FIX.

**Differ.**
- *The entry check's reference.* CME compares against the last trade (or the settlement price). Eurex compares against the best limit on the order's own side of the book.
- *Unprotected stops.* CME has no plain stop-market; its "stop" carries protection points and rests at the protection limit.
- *Coverage.* Eurex and CME offer stops on futures only, and Nasdaq OUCH has none.
- *Quote-based triggers.* FIX offers them (`R`, `S`, `TriggerPriceType`), but no venue read uses them for these products.
- *Ordering.* Eurex specifies trigger timing and ordering (end of transaction, round-robin, new time priority). The other venues read do not say.

## 6. Self-trade prevention (STP)

| Source | Identifies a self-match by | Modes and who is cancelled | Whose instruction governs and the default | After the prevented match | Reported |
|---|---|---|---|---|---|
| FIX | `SelfMatchPreventionID(2362)`, same ID and same firm [fixlat-smpid]. Added in extension pack EP211, with the instruction in EP280. Not in base FIX 4.4 or 5.0SP2 (OnixS's 5.0SP2 dictionary has no tag 2362 page) | `SelfMatchPreventionInstruction(2964)`: `1` Cancel aggressive (incoming), `2` Cancel passive (resting), `3` Cancel aggressive and passive [fixlat-smpcs] | The instruction applies "when the incoming (aggressive) order" shares the ID with a resting order [fixlat-smpinst]. Default not stated | Not stated | No dedicated `ExecType` [fixlat-exectype] |
| CME Globex | Registered SMP ID in `2362`, same ID on both sides, across firms and clearing firms that share ownership [cme-smp] | `8000=N` cancels the aggressing ("newest") order. `8000=O` cancels the resting ("oldest") order or orders [cme-smp] | The aggressing order's tag 8000 always decides, and the resting order's tag 8000 is ignored. **Default: cancel resting** [cme-smp] | Cancel resting: the incoming order continues processing [cme-smp], and in the worked example its remainder rests. Cancel aggressing: the aggressor is cancelled regardless of quantity [cme-smp]. FIFO markets cancel the orders that would have executed; other algorithms cancel the orders that could have executed at that price level, so a resting order that would not have been reached can be cancelled [cme-smp]. Not applied in Opening resolution or to implied trades. Mass quotes: only resting quotes are cancelled [cme-smp] | Execution Report cancel (`39=4`) with `378=103` "Cancel Oldest (Resting) due to SMP" or `107` "Cancel Newest (Aggressing)". An aggressing market order with `8000=O` is acknowledged and then cancelled with `378=111` System Cancel [cme-smp] |
| Nasdaq (OUCH 5.0) | `AIQ Strategy` option (tag 29) and `AIQ Group ID` (tag 30). Matching levels: Firm, Organization, Affiliate, Match Any [ouch5]. Order-by-order SMP launched in September 2026 [nasdaq-etu-2026-4] | Per level: Decrement Both, Decrement Both No Details, Cancel Oldest, Cancel Newest, Use Remover. `N` disables it [ouch5] | `*` uses the port default [ouch5]. OUCH does not say whose strategy governs when the two orders differ; the name "Use Remover" suggests the remover's | Not stated | AIQ Canceled message (`D`): shares decremented, quantity prevented from trading, the price the trade would have occurred at, and the liquidity flag; Cancel reason `Q`. For Decrement Both the decremented and prevented quantities are always equal [ouch5]. Reject `0x0040` Invalid AIQ [ouch5] |
| Eurex (T7) | Type A: same business unit and same user-supplied SMP ID (`MatchInstCrossID(28744)`). Market-wide SMP (Eurex and EEX): same market-wide SMP ID, even across participants. Only for products Eurex enables [t7-fr] [t7-eti] | CAP, Cancel Aggressive and Passive: both orders are reduced until at least one is consumed. CA, Cancel Aggressive: the incoming remainder is cancelled. CP, Cancel Passive: the resting order is deleted entirely, whatever the incoming size [t7-fr] | The incoming aggressive order's instruction governs. The default when only an ID is given is set per exchange; Eurex's default was not found [t7-fr] | Time-priority books check order by order. Pro-rata books check per price level upfront, before allocating. After CAP or CP, the incoming order keeps matching and its remainder is booked. Continuous trading only, not auctions [t7-fr] | `CrossedIndicator(2523)=1` "Cross rejected" in the order response or notification, and the order's total quantity is reduced [t7-eti] |
| Binance Spot | Same account, or the same `tradeGroupId` [bn-stp] | `NONE`. `EXPIRE_TAKER`: the taker's remainder expires. `EXPIRE_MAKER`: the maker expires. `EXPIRE_BOTH`. `DECREMENT`: both orders' prevented quantity rises, and the smaller order (or both, if equal) expires. `TRANSFER` [bn-stp] | The taker's mode governs, except `TRANSFER`, which needs both orders to use it (otherwise it acts as `DECREMENT`). Each symbol sets a default and an allowed list [bn-stp] | `EXPIRE_MAKER`: the taker goes on and can rest on the book [bn-stp] | `executionType=TRADE_PREVENTION`, status `EXPIRED_IN_MATCH`, `preventedMatchId`, `preventedQuantity` [bn-stp] [bn-enums] |

**Agree.**
- *Identity.* An opt-in ID marks orders as belonging to one owner.
- *Instruction values.* Each venue offers some mix of the same three outcomes FIX names: cancel incoming, cancel resting, and cancel both.
- *Precedence.* Where the venue says, the aggressor's instruction governs (CME, Eurex, Binance).
- *Continuation.* After the resting order is removed, the aggressor keeps matching (CME, Eurex, Binance).

**Differ.**
- *Defaults.* CME cancels the resting order. Binance and Eurex set a default per symbol or exchange, and Nasdaq uses the port default.
- *"Both".* For FIX and CME (via its instructions) it means cancel both. For Eurex CAP, and for Binance and Nasdaq "decrement", it means *decrement* both by the overlap until one is gone.
- *Extra modes.* Binance adds `TRANSFER`, and Nasdaq adds "Use Remover" and four ownership levels.
- *Pro-rata books.* At CME and Eurex an order that would never have matched can be cancelled.
- *Reporting.* It is reported as a cancel with a reason at CME and Nasdaq, as an expiry (`EXPIRED_IN_MATCH`) at Binance, and as a quantity reduction with a flag at Eurex.

## 7. Cancel-replace (modify)

| Source | Message and identifiers | What may change | Priority retention | Quantity semantics | Reported |
|---|---|---|---|---|---|
| FIX 4.4 / 5.0SP2 | `OrderCancelReplaceRequest` (`35=G`). `ClOrdID(11)` "must change and be unique". `OrigClOrdID(41)` is the ClOrdID of the last non-rejected order, not the first. The sender chains optimistically and the receiver pessimistically. `OrderID` need not change [fix44-ocrr] [fix50-ocrr] | Any field except the `<Instrument>` block. Currency and FinancingDetails must match. Side may change only between bilaterally agreed pairs (for example Buy and Buy Minus). The sell side may restrict more and reject with `CxlRejReason=2`. All unchanged fields are resent, and every `ExecInst` must be re-declared [fix44-ocrr] [fix50-ocrr] | Not addressed [fix44-ocrr] [fix50-ocrr] | `OrderQty` is the total intended quantity, including what has already executed. Not to be used to cancel the remainder; use Order Cancel Request [fix44-ocrr] [fix50-ocrr] | Execution Report `ExecType` Pending Replace, then Replaced, or Order Cancel Reject (`35=9`) [fix44-ocrr] |
| CME Globex (iLink 3) | `35=G` naming the order by `OrderID(37)` / `ClOrdID(11)` [cme-ocr] | Side change and instrument change: Order Cancel Reject. Everything else in the table may change [cme-ocr-mod] | **Lost** on account change, quantity increase, order type change, any price change, adding a stop trigger price, or adding a minimum fill quantity. **Kept** on quantity decrease, ClOrdID, TIF, GTD expiry, display quantity, SMP ID or instruction, and several administrative fields [cme-ocr-mod] [cme-func] | Optional in-flight mitigation (`OFMOverride(9768)=1`, set on the first replace and then fixed for the chain). With it, open = new quantity minus filled (cancelled if the new quantity is below the filled quantity). Without it, open = new quantity [cme-ifm] [cme-func] | Execution Report - Modify (`35=8 39=5`) or Order Cancel Reject (`35=9`) [cme-ocr-mod] |
| Nasdaq (OUCH 5.0) | Replace Order Request (`U`): `OrigUserRefNum` plus a new, unique, increasing `UserRefNum` [ouch5]. Separate Cancel (`X`, reduce to an "intended order size") and Modify (`M`) messages [ouch5] | Most Enter Order fields except Firm and GroupID. An optional field left out of a replace resets to its default [ouch5]. Side changes are allowed only S to E, S to T, and E to T, via Modify [ouch5]. ITCH says side, symbol, and attribution cannot change in an Order Replace [itch5] | **A replace always loses time priority** ("always gives it a new timestamp"). To shrink and keep priority, send Cancel with a smaller intended size [ouch5]. Modify does not affect priority (with a noted exception) and cannot increase shares [ouch5] | Shares = total liable for the whole chain, including earlier executions and SMP-decremented shares. The Replaced message reports the shares left exposed [ouch5] | Order Replaced (`U`) with a new Order Reference Number, or Rejected (`J`). **If the replace details are invalid, the existing order is cancelled** (Canceled message). If the original is no longer live, the replace is silently ignored [ouch5]. In ITCH, the Order Replace message (`U`) carries a new order reference number [itch5] |
| Eurex (T7) | Modify by client order ID or exchange order ID. The exchange order ID never changes [t7-fr] | Limit price, total quantity, stop limit, and other attributes may change. Instrument, side, the Persistent and Lean flags, and order category may not; the trader must delete and re-enter [t7-fr] | **Lost** (new priority timestamp) on a price change, market to limit or back, a quantity increase, a stop-price change, or a trailing-stop mode change. Eurex does not reset priority on a validity extension. Every other change keeps priority. If priority changed, T7 re-matches the order as if it were new [t7-fr] | Executed quantity carries forward and open quantity is recalculated. Modifying total quantity below the executed quantity deletes the order [t7-fr] | Execution Report `ExecType=5` Replaced, with restatement reasons such as `102` Order replaced [t7-eti] |
| Binance Spot | Cancel-replace: `POST /api/v3/order/cancelReplace` (FIX `XCN`). Cancel first, then a new order with a new `orderId`. `cancelReplaceMode` is STOP_ON_FAILURE or ALLOW_FAILURE [bn-trade] [bn-fix]. Amend keep priority: `PUT /api/v3/order/amend/keepPriority` (FIX `XAK`) [bn-trade] [bn-fix] | Cancel-replace: anything, since it is a new order. Amend: only a **quantity decrease** (the new quantity must be greater than 0 and below the current quantity) and the client order ID [bn-amend] [bn-trade] | Cancel-replace returns a new order; the docs do not claim priority is kept [bn-trade]. Amend "keeps its time priority among existing orders at the same price" [bn-amend] | Not stated what happens when the amended quantity is at or below the executed quantity [bn-amend] | Cancel-replace: separate cancel and new-order responses, and partial failure `-2021` [bn-errors]. Amend: `executionReport` with `x=REPLACED` [bn-amend] |

**Agree.**
- *Side and instrument.* Neither can change, except FIX's bilateral Side pairs and Nasdaq's short-sale side variants.
- *Priority.* Reducing quantity keeps priority wherever a venue offers that path; a price change or a quantity increase loses it (CME, Eurex).
- *Quantity.* Replace quantity is the total including executed (FIX, Nasdaq, Eurex, and CME with in-flight mitigation).

**Differ.**
- *Whether "replace" can keep priority.* At Nasdaq a Replace *always* loses priority, and a separate Cancel or Modify keeps it. At Binance a priority-keeping change is a separate decrease-only amend.
- *FIX.* It does not address priority at all.
- *A failed replace.* Nasdaq cancels the existing order when the replace details are invalid, while FIX, CME, and Eurex reject and leave the order unchanged.
- *Overfills.* Eurex deletes an order replaced below its executed quantity. CME cancels the open quantity only when in-flight mitigation is on; with mitigation off, the docs do not state the outcome.
- *Identifiers.* FIX and CME chain ClOrdIDs, Nasdaq chains UserRefNums, and Eurex and Binance amend keep the exchange order ID.

---

## Sources

FIX (4.4 and 5.0SP2 via the OnixS FIX Dictionary, a third-party rendering; FIX Latest via Orchimate, which hosts the FIX Trading Community repository):

- [fix44-tif]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_59.html
- [fix44-glossary]: https://www.onixs.biz/fix-dictionary/4.4/glossary.html
- [fix44-ordtype]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_40.html
- [fix44-execinst]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_18.html
- [fix44-ocrr]: https://www.onixs.biz/fix-dictionary/4.4/msgType_G_71.html
- [fix50-ocrr]: https://www.onixs.biz/fix-dictionary/5.0.sp2/msgtype_g_71.html
- [fix-stoppx]: https://www.onixs.biz/fix-dictionary/5.0.sp2/tagnum_99.html
- [fixlat-tif]: https://orchimate.org/fixtrading/fix-latest/codeSets/TimeInForceCodeSet
- [fixlat-ordtype]: https://orchimate.org/fixtrading/fix-latest/codeSets/OrdTypeCodeSet
- [fixlat-execinst]: https://orchimate.org/fixtrading/fix-latest/codeSets/ExecInstCodeSet
- [fixlat-exectype]: https://orchimate.org/fixtrading/fix-latest/codeSets/ExecTypeCodeSet
- [fixlat-triggerpricetype]: https://orchimate.org/fixtrading/fix-latest/codeSets/TriggerPriceTypeCodeSet
- [fixlat-smpid]: https://orchimate.org/fixtrading/fix-latest/fields/SelfMatchPreventionID
- [fixlat-smpinst]: https://orchimate.org/fixtrading/fix-latest/fields/SelfMatchPreventionInstruction
- [fixlat-smpcs]: https://orchimate.org/fixtrading/fix-latest/codeSets/SelfMatchPreventionInstructionCodeSet

CME Globex (CME Group Client Systems Wiki):

- [cme-types]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457087412/Order+Types+for+Futures+and+Options
- [cme-ilink-types]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457227032 (iLink Order Types)
- [cme-qual]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457218264/Order+Qualifiers
- [cme-elim]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/716177614 (iLink Execution Report - Elimination)
- [cme-func]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457414497/Order+Functionalities
- [cme-ocr]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/715882680 (iLink Order Cancel Replace Request)
- [cme-ocr-mod]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/716079235 (iLink Order Cancel Replace - Tag Value Modification)
- [cme-ifm]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457574507 (iLink Order Cancel-Replace and In-Flight Mitigation)
- [cme-smp]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457640025 (CME Globex Self-Match Prevention)

Nasdaq:

- [ouch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf (OUCH 5.0 Order Entry Specification, headed "Updated October, 2025")
- [itch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf (TotalView-ITCH 5.0; latest log entry April 28, 2023)
- [nasdaq-etu-2026-4]: https://www.nasdaqtrader.com/TraderNews.aspx?id=ETU2026-4 (Equity Technical Update 2026-4, Order-by-Order Self Match Prevention)
- [fr-2016-27600]: https://www.govinfo.gov/content/pkg/FR-2016-11-17/pdf/2016-27600.pdf (SEC approval order for SR-NASDAQ-2016-111 and SR-BX-2016-046, Post-Only Orders; 81 FR 81184, November 17, 2016)

Eurex / Deutsche Boerse T7:

- [t7-fr]: https://www.eurexchange.com/resource/blob/5409264/38a8e7dbc9c0455ca09043e6fc68b616/data/T7_Release_15.0_-_Functional_Reference-Part_1_Version_1.pdf (redirects to the Version 2 file on cashmarket.deutsche-boerse.com; T7 Release 15.0 Functional Reference Part 1, Version 2, October 2, 2026)
- [t7-eti]: https://www.cashmarket.deutsche-boerse.com/resource/blob/5409190/6ed9cba06592d3c153b0a4dd92a3399b/data/T7_R.15.0_Enhanced_Trading_Interface_-_Manual_Version_1.pdf (served file: T7 Release 15.0 ETI Manual, Version 3, September 23, 2026)

Binance Spot API:

- [bn-enums]: https://developers.binance.com/docs/binance-spot-api-docs/enums
- [bn-trade]: https://developers.binance.com/docs/binance-spot-api-docs/rest-api/trading-endpoints
- [bn-stp]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/stp_faq
- [bn-amend]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/order_amend_keep_priority
- [bn-trailing]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/trailing-stop-faq
- [bn-fix]: https://developers.binance.com/docs/binance-spot-api-docs/fix-api
- [bn-errors]: https://developers.binance.com/docs/binance-spot-api-docs/errors

### Not reached

- FIXimate (`fiximate.fixtrading.org`) for FIX 4.4 and 5.0SP2: it redirects to Orchimate's FIX Latest. The 4.4 and 5.0SP2 field text above therefore comes from OnixS, and Orchimate's long elaborations (IOC, FOK, Market) were truncated in what was readable.
- The Nasdaq rulebook, Rule 4702 (`listingcenter.nasdaq.com`): HTTP 403. The current Post-Only text and any post-2016 amendments were not verified.
- The CME Globex SMP FAQ on cmegroup.com: timed out twice. The Client Systems Wiki SMP page was used instead.
- Eurex's own default SMP instruction, and the CME futures behavior when a replace without in-flight mitigation lowers quantity below the filled quantity: not found in the pages read.

<!-- Reference-link definitions for the citations above. -->
[fix44-tif]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_59.html
[fix44-glossary]: https://www.onixs.biz/fix-dictionary/4.4/glossary.html
[fix44-ordtype]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_40.html
[fix44-execinst]: https://www.onixs.biz/fix-dictionary/4.4/tagNum_18.html
[fix44-ocrr]: https://www.onixs.biz/fix-dictionary/4.4/msgType_G_71.html
[fix50-ocrr]: https://www.onixs.biz/fix-dictionary/5.0.sp2/msgtype_g_71.html
[fix-stoppx]: https://www.onixs.biz/fix-dictionary/5.0.sp2/tagnum_99.html
[fixlat-tif]: https://orchimate.org/fixtrading/fix-latest/codeSets/TimeInForceCodeSet
[fixlat-ordtype]: https://orchimate.org/fixtrading/fix-latest/codeSets/OrdTypeCodeSet
[fixlat-execinst]: https://orchimate.org/fixtrading/fix-latest/codeSets/ExecInstCodeSet
[fixlat-exectype]: https://orchimate.org/fixtrading/fix-latest/codeSets/ExecTypeCodeSet
[fixlat-triggerpricetype]: https://orchimate.org/fixtrading/fix-latest/codeSets/TriggerPriceTypeCodeSet
[fixlat-smpid]: https://orchimate.org/fixtrading/fix-latest/fields/SelfMatchPreventionID
[fixlat-smpinst]: https://orchimate.org/fixtrading/fix-latest/fields/SelfMatchPreventionInstruction
[fixlat-smpcs]: https://orchimate.org/fixtrading/fix-latest/codeSets/SelfMatchPreventionInstructionCodeSet
[cme-types]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457087412/Order+Types+for+Futures+and+Options
[cme-ilink-types]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457227032
[cme-qual]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457218264/Order+Qualifiers
[cme-elim]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/716177614
[cme-func]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457414497/Order+Functionalities
[cme-ocr]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/715882680
[cme-ocr-mod]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/716079235
[cme-ifm]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457574507
[cme-smp]: https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457640025
[ouch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf
[itch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
[nasdaq-etu-2026-4]: https://www.nasdaqtrader.com/TraderNews.aspx?id=ETU2026-4
[fr-2016-27600]: https://www.govinfo.gov/content/pkg/FR-2016-11-17/pdf/2016-27600.pdf
[t7-fr]: https://www.eurexchange.com/resource/blob/5409264/38a8e7dbc9c0455ca09043e6fc68b616/data/T7_Release_15.0_-_Functional_Reference-Part_1_Version_1.pdf
[t7-eti]: https://www.cashmarket.deutsche-boerse.com/resource/blob/5409190/6ed9cba06592d3c153b0a4dd92a3399b/data/T7_R.15.0_Enhanced_Trading_Interface_-_Manual_Version_1.pdf
[bn-enums]: https://developers.binance.com/docs/binance-spot-api-docs/enums
[bn-trade]: https://developers.binance.com/docs/binance-spot-api-docs/rest-api/trading-endpoints
[bn-stp]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/stp_faq
[bn-amend]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/order_amend_keep_priority
[bn-trailing]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/trailing-stop-faq
[bn-fix]: https://developers.binance.com/docs/binance-spot-api-docs/fix-api
[bn-errors]: https://developers.binance.com/docs/binance-spot-api-docs/errors
