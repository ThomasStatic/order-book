# How exchange binary protocols encode commands and frames

Research for [issue #20](https://github.com/ThomasStatic/order-book/issues/20). Read 2026-10-28.

This survey covers Nasdaq OUCH 5.0 (order entry) and TotalView-ITCH 5.0 (market data), their envelopes SoupBinTCP and MoldUDP64, the FIX Simple Binary Encoding (SBE) standard with the FIX Simple Open Framing Header (SOFH), CME Globex iLink 3 (order entry) and MDP 3.0 (market data), and Binance Spot SBE. For each it records framing, field widths and endianness, instrument identification, session and sequence numbers, request/response correlation, versioning and extension, and error reporting. It records what the sources say and recommends no design.

Citations are reference links (for example [ouch5]), resolved in [Sources](#sources). "Not stated" means the pages read say nothing about it. Nothing has been filled in from memory.

## Facts the decisions wait on

- **Framing is always a binary length prefix, but its width, scope and byte order differ.** SoupBinTCP: 2-byte big-endian length counting the 1-byte packet type plus payload [soup]. MoldUDP64: a 20-byte packet header, then a 2-byte big-endian length before each message [mold]. FIX SOFH: 4-byte length counting the whole message including the 6-byte SOFH, plus a 2-byte encoding type, always big-endian [sbe10e-03]. CME iLink 3 calls its frame SOFH but uses a 2-byte length and encoding type `0xCAFE`, little-endian [cme-ilink-hdr] [cme-ilink-sbe]. Binance streams carry SBE in WebSocket binary frames, with no extra prefix stated [bn-streams].
- **Message type is a 1-byte ASCII letter in OUCH and ITCH** [ouch5] [itch5] **and a `uint16` templateId in an SBE header** [sbe10] (CME [mdp-sbe-hdr], Binance [bn-spot-xml]).
- **Nasdaq is big-endian throughout** [soup] [mold] [ouch5] [itch5]. **SBE defaults to little-endian**, set once per schema [sbe10e-02]. CME [mdp-schema] [cme-ilink-sbe] and Binance [bn-spot-xml] declare little-endian.
- **Instruments.** OUCH carries an 8-byte, space-padded ASCII symbol [ouch5]. ITCH adds a 2-byte Stock Locate code, assigned daily from 1 and found at the same offset in every message [itch5]. CME uses an `Int32` SecurityID that is not reused until the next trade date after the instrument expires or is deleted [mdp-incr-mbp]. Binance sends the symbol as a `varString8` at the end of the message [bn-stream-xml] [bn-spot-xml].
- **Sequencing.** SoupBinTCP sequences only server-to-client messages, implicitly by counting, so no number is carried per message [soup]. MoldUDP64 puts one 8-byte number in each packet, for its first message [mold]. iLink 3 carries a `uInt32` SeqNum on every business message in both directions, scoped by a client-chosen 64-bit UUID, and resets weekly [cme-nos] [cme-session]. MDP 3.0 has a `uInt32` packet sequence number per channel and a `uInt32` RptSeq per instrument [mdp-tech-hdr] [mdp-incr-mbp].
- **Correlation.** OUCH: a client-chosen 4-byte UserRefNum, which must be strictly increasing per port per day, names each order and is echoed on every reply [ouch5]. iLink 3: a client `uInt64` OrderRequestID is echoed on the Execution Report, alongside a 20-character ClOrdID and an exchange `uInt64` OrderID [cme-er-new]. Binance WebSocket API: an arbitrary request `id` is echoed back [bn-ws].
- **Versioning.** OUCH and ITCH carry no version on the wire. OUCH extends its messages with a length-prefixed appendage of tag-value options [ouch5]. ITCH stayed "5.0" through layout changes [itch5]. SBE puts schemaId and version in every message header and allows only append-only changes [sbe10e-05]. Binance bumps the schema `id` on a breaking change, and `version` otherwise [bn-sbe-faq].
- **Errors.** OUCH: a Rejected message (`J`) with a 2-byte reason code, but cancels are never rejected (superfluous ones are silently ignored) [ouch5]. iLink 3: an Execution Report - Reject with a `uInt16` OrdRejReason, a Business Reject with a `uInt16` reason and the rejected message's SeqNum, and session-level `uInt16` ErrorCodes [cme-er-reject] [cme-bizreject] [cme-terminate]. Binance: an SBE `ErrorResponse` with an `int16` code and a text message [bn-spot-xml].

## Scope and source caveats

- **FIX SBE.** `https://www.fixtrading.org/standards/sbe-online/` returned HTTP 404. The standard was read from the FIX Trading Community's own GitHub repository. Two tags differ. At tag `v1.0`, the recommended message header is 8 octets [sbe10]. At tag `v1.0-errata-Nov-2020`, the same file recommends 12 octets, adding `numGroups` and `numVarDataFields`, and its framing text refers to "Version 2.0 of SBE" [sbe10e-03]. Both are reported below. CME and Binance use the 8-octet, four-field header.
- **CME.** The Client Systems Wiki was read through its public Confluence REST API, and the links below are ordinary page URLs. Several MDP pages (Core Schema, Market Data Snapshot, Messaging Concepts) were index pages with no field tables. The schema XML on CME's FTP site did not respond. Some byte widths of CME types (for example PRICE9) are named but not defined on the pages read.
- **Nasdaq.** The PDFs were read as extracted text. The OUCH 5.0 PDF's tables come out interleaved, so offsets were checked against the arithmetic of the neighbouring fields. OUCH's text refers to "Appendix B" for options, but the options table is Appendix A (cancel reasons are B, reject reasons C) [ouch5].
- **Binance.** `spot_prod_latest.xml` in the docs repository contains only the text `spot_3_4.xml`, so schema 3:4 was read. The lifecycle file lists 3:5 as the latest and 3:4 as deprecated on 2026-07-07 [bn-lifecycle].

---

## 1. Nasdaq: SoupBinTCP and MoldUDP64 envelopes

**SoupBinTCP 3.00** (TCP, point to point) [soup]:
- *Framing.* Every logical packet is a 2-byte big-endian length ("the length of the payload plus the length of the packet type - which is 1"), a 1-byte packet type, and a payload. "The SoupBinTCP protocol does not define a maximum payload length" (s1.1). Logical packets may be split or merged by TCP.
- *Data types.* "Character data fields are standard ASCII bytes. Integer fields are binary big-endian values" (s1.5). However, the login packets' sequence numbers are 20-byte ASCII numerals, left-padded with spaces (s2.2.1, s2.3.1).
- *Packet types.* Server: `A` Login Accepted (Session 10 alpha, Sequence Number 20), `J` Login Rejected (1-byte reason), `S` Sequenced Data, `H` Server Heartbeat, `Z` End of Session. Client: `L` Login Request (Username 6, Password 10, Requested Session 10, Requested Sequence Number 20), `U` Unsequenced Data, `R` Client Heartbeat, `O` Logout Request. Either side: `+` Debug (s2).
- *Sequencing.* "Sequenced Data Packets do not contain an explicit sequence number; instead both client and server compute the sequence number locally by counting messages". The first message in each session is 1 (s1.2). The client resumes by logging in with the session and its next expected number (s1.2). Client messages "are not sequenced and may be lost" across a socket failure (s1, s2.3.2).
- *Liveness.* Each side sends a heartbeat after 1 second without sending anything. The server typically closes the socket after 15 seconds of client silence, and after about 30 seconds with no Login Request (s1.3, s2.3.1).
- *Errors.* Only at login: `J` with reason `A` (Not Authorized) or `S` (Session not available), after which the server closes the socket (s2.2.2).

**MoldUDP64 1.00** (UDP multicast, one sender to many receivers) [mold]:
- *Downstream packet.* Header: Session (10 bytes, alphanumeric), Sequence Number (8, the number of the first message in the packet), Message Count (2). Then that many message blocks, each a 2-byte length (excluding itself) and the data. Messages after the first "are implicitly numbered sequentially" ("Header", "Message Block").
- *Byte order.* All MoldUDP64 numbers are "Big Endian ... This need not apply to the data contained the Message Data fields" ("Assumptions").
- *Heartbeat and end of session.* Message Count 0 is a heartbeat, and `0xFFFF` marks the end of the session. Both carry the next expected sequence number ("Heartbeats", "End of Session").
- *Recovery.* A Request Packet (Session 10, Sequence Number 8, Requested Message Count 2) goes to a re-request server, which unicasts a standard downstream packet ("Request Packet").
- *Size.* A message can theoretically be up to 64 KB ("Message").

## 2. Nasdaq OUCH 5.0 (order entry)

- *Transport and framing.* Messages are carried by SoupBinTCP. "Each message type has a fixed message length." Host-to-client messages are sequenced by the lower layer; client-to-host messages are "inherently non-guaranteed" (s1.1). Each message starts with a 1-byte ASCII type: inbound `O` Enter, `U` Replace, `X` Cancel, `M` Modify, `C` Mass Cancel, `D`/`E` Disable/Enable Order Entry, `Q` Account Query; outbound `S`, `A`, `U`, `C`, `D`, `E`, `B`, `J`, `P`, `I`, `T`, `M`, `R`, `X`, `G`, `K`, `Q` (s2, s3).
- *Field widths and endianness.* "All Numeric fields are binary formatted, big-endian numbers", as Longs (8), Integers (4), Shorts (2) and Bytes (1). Alpha fields are "left-justified and padded on the right with spaces". Prices have "an implied 4 decimal places", and the special price `0x7FFFFFFF` flags a market order for a cross (s1.2). Timestamps are 8 bytes of nanoseconds since midnight (s3.1).
- *Enter Order layout* (s2.1). Type 0/1, UserRefNum 1/4, Side 5/1, Quantity 6/4, Symbol 10/8, Price 18/8, Time In Force 26/1, Display 27/1, Capacity 28/1, InterMarket Sweep Eligibility 29/1, CrossType 30/1, ClOrdID 31/14, Appendage Length 45/2, then the Optional Appendage at 47.
- *Instrument.* Symbol is 8 alpha bytes (s2.1). Mass Cancel takes a Symbol that is "Space filled if not specified" (s2.5).
- *Session and sequence.* Each physical port is bound to a Nasdaq-assigned logical OUCH Account. "On a given day, every order entered on OUCH is uniquely identified by the combination of the logical OUCH Account and the participant-created UserRefNum" (s1.1).
- *Correlation and idempotence.* UserRefNum is a 4-byte unsigned "transaction identifier", which "must be both unique and strictly increasing throughout the trading day" and starts at 1. "The system ignores new order requests identified with UserRefNums lower than the last one processed, assuming they are retransmissions" (s1.2). "All Inbound Messages may be repeated benignly" (s2). The optional UserRefIdx tag splits a port into "order flow channel[s]", each with its own UserRefNum rule (s1.2). Every outbound order message carries the UserRefNum. Order Accepted adds an 8-byte, day-unique Order Reference Number assigned by Nasdaq (s3.2), and executions carry an 8-byte Match Number shared by both sides (s3.6). ClOrdID (14 alpha) "will not be checked for day-uniqueness" (s2.1). Replace carries the original and a new UserRefNum (s2.2).
- *Extension.* Optional attributes are TagValue elements: a 1-byte remaining length, a 1-byte OptionTag, then the value. "The options appendage is then constructed by concatenating together the selected set of options" (s1.2). The options table has tags 1 to 30, from SecondaryOrdRefNum and Firm to UserRefIdx (28) and AIQ Strategy and Group ID (29, 30), each with a size, type and default. A tag left out of a Replace resets to its default (Appendix A). On cancel and modify messages, and on most outbound messages, the Appendage Length and appendage are optional and absent unless UserRefIdx was used (s2.3, s3.4 to s3.13). No version field is on the wire. The revision history runs from 1.0 (10/4/2022) to 1.05 (10/7/2025), and 1.03 added message types (Appendix E).
- *Errors.* Rejected (`J`): Timestamp 8, UserRefNum 4, Reason 2 (numeric codes `0x0001` to `0x0040`, such as `0x0017` Invalid Symbol and `0x001D` Invalid Price), ClOrdID 14, and the appendage. "The Order UserRefNum of a Rejected Message cannot be re-used" (s3.8, Appendix C). Cancels have no reject: "There is no 'too late to cancel' message ... Superfluous Cancel Order Messages are silently ignored" (s2.3). A Replace on a dead order, or one with a used UserRefNum, is "silently ignored". An invalid replace of a live order cancels the existing order (s2.2). Cancel Reject (`I`) and Cancel Pending (`P`) exist only for cross orders in the pre-cross late period (s3.9, s3.10). Unsolicited cancels carry a 1-byte reason letter (Appendix B).

## 3. Nasdaq TotalView-ITCH 5.0 (market data)

- *Transport.* Offered over SoupBinTCP, compressed SoupBinTCP, or MoldUDP64. "Each message is variable in length based on the message type" ("Architecture") [itch5].
- *Types.* Integers are "big-endian (network byte order)" and unsigned unless noted. Alpha fields are left-justified and space-padded. "Price (4)" means 4 implied decimals. Timestamps are nanoseconds since midnight ("Data Types").
- *Common prefix.* Message Type 0/1, Stock Locate 1/2, Tracking Number 3/2 ("Nasdaq internal tracking number"), Timestamp 5/6 (s1.1, s1.3).
- *Instrument.* "Instruments are identified by a stock locate code", "dynamically assigned each day, starting with a value of 1, and communicated via the Stock Directory message". It does not change intraday, but "there should be no expectation that the assignment will be the same across multiple days". It sits at the same position in all messages, and is 0 when a message is not stock-specific ("Architecture"). Stock Directory (`R`) maps the locate to the 8-byte Stock symbol (s1.2.1). Add Order (`A`/`F`) also carries the 8-byte Stock, "right padded with spaces" (s1.3).
- *Order identity.* Add Order assigns an 8-byte "day-unique Order Reference Number" (s1.3). Executed (`E`), Cancel (`X`) and Delete (`D`) refer to it. Replace (`U`) gives a new reference number, used "for all subsequent updates" (s1.4.5). Executions carry an 8-byte day-unique Match Number (s1.4.1).
- *Sequence.* Not in ITCH messages. It comes from the envelope (SoupBinTCP or MoldUDP64).
- *Versioning.* There is no version field. The revision log shows layout changes under the same "Version 5.0" label, including "Moved positioning of the Symbol Locate and Tracking Number fields" (August 2, 2013). In May 2018, Nasdaq "made the decision to fall back to the previous version number to avoid customer confusion" (Appendix A).
- *Errors.* Not applicable: a one-way feed.

## 4. FIX Simple Binary Encoding (SBE) and Simple Open Framing Header

- *Scope.* SBE is "one of the possible syntaxes for FIX messages, but not limited to FIX messages", covering "the encoding (wire format) and the message schema" [sbe10e-01]. Its extension rules cover "compatibility at the presentation layer" only [sbe10e-05] ("Objective"). The chapters read define no session, sequence, correlation or reject messages.
- *Framing (SOFH).* SBE "messages have no defined message delimiter" [sbe10e-03] ("Message Framing"). SOFH supplies "an overall message size including headers" and "an identifier of the encoding". It is `messageLength` (`uint32`) then `encodingType` (`uint16`), "always ... big-endian". Encoding types: `0x5BE0` for SBE 1.0 big-endian and `0xEB50` for SBE 1.0 little-endian ("Simple Open Framing Header") [sbe10] [sbe10e-03].
- *Message header.* The header must be named `messageHeader`, its fields must be unsigned integers, and it uses the body's byte order. v1.0 recommends `blockLength`, `templateId`, `schemaId` and `version`, each `uint16`, 8 octets [sbe10]. The errata text adds `numGroups` and `numVarDataFields`, 12 octets [sbe10e-03] ("Message header schema"). blockLength "does not include the length of the message header itself" and may exceed the field total, for alignment ("Root block length").
- *Byte order and primitives.* "Little-Endian order is the default encoding". It is set globally by the schema's `byteOrder` attribute (`littleEndian` or `bigEndian`) [sbe10e-02] ("Byte order") [sbe10e-04]. Sizes: `char`/`int8`/`uint8` 1, `int16`/`uint16` 2, `int32`/`uint32`/`float` 4, `int64`/`uint64`/`double` 8. Optional fields use a null sentinel, for example the type's minimum for signed integers and maximum for unsigned ones, NUL for `char`, and quiet NaN for floats [sbe10e-02] ("Range attributes"). `decimal` is an `int64` mantissa plus an `int8` exponent (9 octets); `decimal64` and `decimal32` have a constant exponent [sbe10e-02] ("Decimal encoding").
- *Strings.* A fixed-length `char` array is "padded by NUL character". A variable-length string is a length followed by the data, which "immediately precedes the data", and an empty string equals null [sbe10e-02] ("Fixed-length character array", "Variable-length string encoding").
- *Body order.* Fixed-length fields, then repeating groups, then variable-length data [sbe10e-03] ("Root level elements"). No padding by default ("No padding by default"). A group has a dimension of `blockLength` (`uint16`) and `numInGroup`. "Implementations should support uint8 and uint16 types for repeating group entry counts" ("Group dimension encoding").
- *Schema identity.* The `<messageSchema>` attribute `id` "Should be unique between counterparties". `version` starts at zero and "is incremented for each version". Each message has a schema-unique `id` (the templateId), and each field's `id` is its FIX tag [sbe10e-04] [sbe10e-02].
- *Extension rules* [sbe10e-05] ("Constraints", "Block size", "Comaptibility strategy"):
  - *Compatible changes.* Fields may be added to the root or a group, but "they must be appended to end of a block". Groups and variable-length fields may be added after existing ones.
  - *Breaking changes.* "Existing fields cannot change data type or move" and "Message header encoding cannot change". A template changed incompatibly "must be assinged a new template 'id'".
  - *Older decoder, newer message.* Because blockLength is on the wire, it "would still know how many octets to consume", so earlier fields still parse.
  - *Newer decoder, older message.* "If sinceVersion is greater than received schema version, then the field is not available". `sinceVersion` is documentation only and "not sent on the wire". `deprecated` marks obsolete elements.

## 5. CME Globex iLink 3 (order entry, SBE over TCP)

- *Framing.* A SOFH of MessageSize (`uInt16`, 2 bytes, "Overall message size including headers") and EncodingType (`uInt16`, "CME SBE Version 1.0 Little-endian: 0xCAFE"), then an SBE header of BlockLength, TemplateID, SchemaID and Version, each `uInt16` [cme-ilink-hdr]. The framing header "will always be encoded in little-endian byte order" [cme-ilink-sbe].
- *Schema.* The schema header shown has `id="8"`, `version="0"`, `package="iLinkBinary"`, `semanticVersion="FIX5.0"`, `byteOrder="littleEndian"`. "Customers and CME Globex can only send messages using the latest version of a schema." The schema is published on CME's SFTP site per environment, updated the "Sunday prior to market open" [cme-ilink-sbe]. Nulls are the unsigned maximum (for example, `uInt32` null 4294967295) [cme-ilink-sbe].
- *Session layer (FIXP-based).* Session messages are Negotiate, Negotiation Response or Reject, Establish, Establishment Ack or Reject, Sequence, Retransmit Request, Retransmission, Retransmit Reject, Not Applied, and Terminate [cme-session]:
  - *Session identity.* A session is a client-chosen 64-bit UUID ("CME Group recommends using the system timestamp"), and messages are "recoverable for that sequence number + UUID combination" [cme-session].
  - *Establish.* Carries `NextSeqNo` (`uInt32`) and `KeepAliveInterval` (`uInt16`, milliseconds) [cme-establish]. The Establishment Ack returns CME's own NextSeqNo plus PreviousUUID and PreviousSeqNo [cme-estack].
  - *Resets.* Sequence numbers reset to 1 at the start of each week [cme-session].
  - *Gaps and recovery.* Not Applied is sent "when CME detects a sequence gap from the customer" [cme-notapplied]. Retransmit Request asks for FromSeqNo (`uInt32`) and MsgCount (`uInt16`), which "Cannot exceed 2500" [cme-retransreq].
- *Business messages.* Each carries SeqNum (tag 9726, `uInt32`) [cme-nos], and outbound ones carry UUID (39001, `uInt64`) [cme-er-new]. New Order - Single: SecurityID (48, `Int32`, "as defined in the market data Security Definition message"), Side, OrderQty (`uInt32`), Price (`PRICENULL9`, 8 bytes), ClOrdID (11, 20 characters), OrderRequestID (2422, `uInt64`), SendingTimeEpoch (`uInt64`, nanoseconds) [cme-nos].
- *Correlation.* OrderRequestID "identif[ies] a request to enter, modify, or delete an order" and is "Echoed back on the Execution Report". Unsolicited reports echo the last accepted OrderRequestID. OrderID (37, `uInt64`) is "Globally unique ... assigned by the exchange" [cme-er-new]. ClOrdID must be unique across open orders per session and firm within a market segment, or it is rejected with Business Reject 133 [cme-ids]. Each request maps to defined responses, for example New Order Single to Execution Report - New, Execution Report - Reject, or Business Reject [cme-applayer].
- *Errors.* There are three kinds:
  - *Order rejects.* Execution Report - Reject (`39=8`, `150=8`): OrdRejReason (103, `uInt16`) and Text (58, String256) [cme-er-reject].
  - *Business rejects.* Business Reject (`35=j`) for messages that pass session rules "but fail the business rules": BusinessRejectReason (380, `uInt16`, codes 0 to 147, such as 5 Required Field Missing and 109 "Incoming message could not be decoded"), RefSeqNum (the rejected message's SeqNum), RefTagID, and Text [cme-bizreject].
  - *Session rejects.* Negotiation Reject and Terminate carry ErrorCodes (39012, `uInt16`) and Reason (String48). Terminate codes include 10/11/12 (invalid sequence numbers), 13 InvalidUUID, 18 InvalidSOFH and 19 DecodingError [cme-neg-reject] [cme-terminate].

## 6. CME MDP 3.0 (market data, SBE over UDP multicast)

- *Packet.* A binary packet header of MsgSeqNum (`uInt32`) and SendingTime (`uInt64`, nanoseconds since the Unix epoch). Sequence numbers are per channel, "increment sequentially with each packet and reset weekly" [mdp-tech-hdr]. "Packets may contain a single or multiple MDP messages" [mdp-packet]. Each message starts with MsgSize (`uInt16`, "Length of entire message, including binary header"), then BlockLength, TemplateID, SchemaID and Version (each `uInt16`) [mdp-sbe-hdr]. The TCP conflated feed instead prefixes encodingType `0xCAFE` [mdp-tech-hdr].
- *Schema.* `id="1"`, `byteOrder="littleEndian"`, "The first version of a schema is version zero" [mdp-schema].
- *Instrument.* SecurityID (48, `Int32`) is "A unique instrument ID value across markets will not be reused until the next trade date following an instrument expiration or deletion" [mdp-incr-mbp]. Security Definition (`35=d`) also carries Symbol, SecurityGroup, Asset, MinPriceIncrement (`PRICE9`) and DisplayFactor (`Decimal9`) [mdp-secdef-fut].
- *Sequences.* RptSeq (83, `uInt32`) is a "Market Data entry sequence number per instrument update" [mdp-incr-mbp]. "Packet loss is detected using the message packet sequence numbers", and after a gap "all books ... may no longer have the correct, latest state" [mdp-recovery-svc]. A snapshot's LastMsgSeqNumProcessed (369) "corresponds to the packet sequence number", and cached incremental updates below it are dropped [mdp-mkt-recovery]. On a Channel Reset, RptSeq "resets to 1 for each instrument" and books are emptied [mdp-chreset].
- *Events.* MatchEventIndicator (5799) is a bitmap of 8 flags; bit 7 marks the "Last message for a given event" [mdp-event].
- *Versioning.* An *appended* extension adds body fields "to the end of the body, before the repeating groups". "The TemplateID will not change, but the schema version will change", and clients may skip the new fields using blockLength. A *non-appended* change "is not backward compatible and the new template must be implemented" [mdp-schema] [cme-ilink-sbe].
- *Errors.* Not applicable on the multicast feed. (Index pages list Request Reject and Request Acknowledgment for TCP recovery, not read.)

## 7. Binance Spot SBE

- *Where SBE is used.* Supported in the REST, WebSocket and FIX APIs [bn-sbe-faq]:
  - *REST.* `Accept: application/sbe` plus `X-MBX-SBE: <ID>:<VERSION>`.
  - *WebSocket API.* `responseFormat=sbe&sbeSchemaId=..&sbeSchemaVersion=..` on the connection URL.
  - *Market data streams.* SBE "in WebSocket binary frames", while subscriptions and responses are JSON in text frames [bn-streams].
- *Header and types.* `byteOrder="littleEndian"`. `messageHeader` is the four `uint16` fields (8 bytes). `groupSizeEncoding` uses a `uint32` `numInGroup` (`groupSize16Encoding` uses `uint16`). Strings are `varString` (`uint16` length) or `varString8` (`uint8` length) in UTF-8 [bn-spot-xml] [bn-stream-xml]. Decimals are "encoded separately as primitive fields", unlike the FIX SBE specification [bn-sbe-faq]: an `int8` exponent field, which "will always precede the mantissa field", and an `int64` mantissa that names it via `mbx:exponent` [bn-stream-xml]. Timestamps are `int64` microseconds [bn-sbe-faq].
- *Instrument.* `symbol` is a `varString8` data field (id 200) at the end of each stream event and order response [bn-stream-xml] [bn-spot-xml].
- *Sequence.* No session sequence number is in the schemas read. Depth events carry `int64` `bookUpdateId`, or `firstBookUpdateId` and `lastBookUpdateId`, and trades carry an `int64` trade `id` [bn-stream-xml].
- *Correlation.* WebSocket API requests carry an `id`, an "Arbitrary ID used to match responses to requests", which the server echoes without interpreting [bn-ws]. In SBE, `WebSocketResponse` (template 50) holds `sbeSchemaIdVersionDeprecated`, `status` (`uint16`), a `rateLimits` group, `id` (`varString8`, always a string), and `result`, a length-prefixed nested SBE message [bn-spot-xml]. Order responses carry `orderId` (`int64`) and `clientOrderId` (`varString8`) (template 300) [bn-spot-xml].
- *Versioning.* "`id` increments on a breaking change, and `version` resets to 0", and `version` increments for non-breaking changes. Deprecation happens "even when the new schema only introduces non-breaking changes". A deprecated schema is supported "for at least 6 months". Deprecation is signalled by the `X-MBX-SBE-DEPRECATED` header (REST), `sbeSchemaIdVersionDeprecated` (WebSocket), or LogonAck (FIX). A retired schema gets HTTP 400 or a rejected message. Schema 3:0 added a `NonRepresentable` enum value, and 3:1 a `NonRepresentableMessage`, to signal data only a newer schema can express [bn-sbe-faq]. Production history: ids 1, 2, 3, with 3:0 to 3:5 released between 2025-04-24 and 2026-07-07 [bn-lifecycle].
- *Errors.* `ErrorResponse` (template 100): `code` (`int16`), optional `serverTime` and `retryAfter`, `msg` (`varString`), and optional `data`. Over WebSocket it is wrapped in `WebSocketResponse` [bn-spot-xml]. Handshake errors are JSON [bn-sbe-faq].

---

## Comparison tables

### Framing

| Protocol | Outer frame | Length field | Message type field |
|---|---|---|---|
| SoupBinTCP | Logical packet over TCP | 2 B, big-endian, counts type + payload | 1 B ASCII packet type |
| MoldUDP64 | UDP packet: 20 B header, then blocks | 2 B, big-endian, per message, excludes itself | (carried by payload) |
| OUCH 5.0 | Inside SoupBinTCP `U`/`S` | From the envelope | 1 B ASCII |
| ITCH 5.0 | Inside SoupBinTCP or MoldUDP64 | From the envelope | 1 B ASCII |
| FIX SBE + SOFH | SOFH | 4 B, always big-endian, whole message | `uint16` templateId in header |
| CME iLink 3 | "SOFH" over TCP | 2 B, little-endian, whole message; encoding `0xCAFE` | `uInt16` TemplateID |
| CME MDP 3.0 | UDP packet: 12 B header, then messages | 2 B MsgSize per message, includes header | `uInt16` TemplateID |
| Binance SBE | WebSocket binary frame or HTTP body | Not stated (frame or body bounds it) | `uint16` templateId |

### Field widths and endianness

| Protocol | Byte order | Strings | Prices | Timestamps |
|---|---|---|---|---|
| OUCH 5.0 | Big-endian | Fixed alpha, space-padded right | 8 B field, 4 implied decimals | 8 B, ns since midnight |
| ITCH 5.0 | Big-endian | Fixed alpha, space-padded right | 4 B, 4 implied decimals | 6 B, ns since midnight |
| FIX SBE | Per schema, default little-endian | Fixed `char[]` NUL-padded, or length-prefixed | `decimal` (int64 + int8), or constant exponent | Not fixed by SBE |
| CME iLink 3 / MDP 3.0 | Little-endian | Fixed (for example String20) | `PRICE9`/`PRICENULL9` (mantissa, exponent -9 in example) | `uInt64` ns since Unix epoch |
| Binance SBE | Little-endian | `varString8`/`varString`, UTF-8 | `int64` mantissa + separate `int8` exponent field | `int64` microseconds |

### Instrument identification

| Protocol | Identifier | Width | Lifetime |
|---|---|---|---|
| OUCH 5.0 | Symbol | 8 B ASCII | Not stated |
| ITCH 5.0 | Stock Locate (plus Stock in some messages) | 2 B integer (8 B ASCII) | Assigned daily from 1; may change across days |
| CME iLink 3 / MDP 3.0 | SecurityID (tag 48) | `Int32` | Not reused until the trade date after expiry or deletion |
| Binance SBE | symbol | `varString8` | Not stated |

### Session and sequence numbers

| Protocol | Session id | Sequence carried | Scope | Reset |
|---|---|---|---|---|
| SoupBinTCP | 10 B alphanumeric | Implicit (counted); only on login packets, as 20 B ASCII | Server to client only | New session; first message is 1 |
| MoldUDP64 | 10 B alphanumeric | 8 B, first message of each packet | Per session | New session |
| OUCH 5.0 | OUCH Account (per port) | UserRefNum, strictly increasing (inbound) | Per account/port per day (or per UserRefIdx) | Daily, from 1 |
| CME iLink 3 | UUID `uInt64`, client-chosen | SeqNum `uInt32` on every business message | Per UUID, each direction | Weekly, to 1 |
| CME MDP 3.0 | Channel | MsgSeqNum `uInt32` per packet; RptSeq `uInt32` per instrument | Per channel; per instrument | Weekly; RptSeq to 1 on Channel Reset |
| Binance SBE | Not stated | Book update ids `int64` on depth events | Per symbol stream | Not stated |

### Request/response correlation

| Protocol | Client-assigned | Exchange-assigned | Echo rule |
|---|---|---|---|
| OUCH 5.0 | UserRefNum (4 B); ClOrdID (14 B, not checked for uniqueness) | Order Reference Number (8 B), Match Number (8 B) | UserRefNum on every order message |
| CME iLink 3 | OrderRequestID (`uInt64`), ClOrdID (20 chars) | OrderID (`uInt64`), ExecID (40 chars) | OrderRequestID echoed on the Execution Report |
| Binance | Request `id` (any type, echoed as a string); clientOrderId | orderId (`int64`) | `id` echoed in the response |

### Versioning and extension

| Protocol | Version on the wire | Extension mechanism | Breaking change |
|---|---|---|---|
| OUCH 5.0 | None | Appendage: 2 B length + TagValue elements (1 B length, 1 B tag, value) | New message types added (rev 1.03) |
| ITCH 5.0 | None | None stated; label stayed 5.0 through offset changes | Same label |
| FIX SBE | schemaId + version in each header | Append to end of block; new groups and var data after existing ones; blockLength skip | New template id |
| CME | SchemaID + Version in each header | "Appended" extension, same TemplateID | "Non-appended": new template |
| Binance | schemaId + version in header; chosen by client | Non-breaking: version + 1; NonRepresentable markers | id + 1, version to 0; at least 6 months' support after deprecation |

### Errors and rejects

| Protocol | Order reject | Cancel reject | Session/transport reject |
|---|---|---|---|
| SoupBinTCP | n/a | n/a | Login Rejected `J`: 1 B reason (`A`, `S`), then socket closed |
| OUCH 5.0 | Rejected `J`: 2 B reason code; UserRefNum consumed | None, except the cross late period (`I`, `P`); superfluous cancels ignored | n/a |
| CME iLink 3 | Execution Report - Reject: OrdRejReason `uInt16` + Text | Order Cancel Reject / Cancel Replace Reject (not read) | Business Reject (`uInt16` reason, RefSeqNum); Negotiation Reject / Terminate ErrorCodes `uInt16` |
| Binance SBE | ErrorResponse: code `int16` + msg | Same | HTTP 400/406 or JSON at handshake |

---

## Sources

Nasdaq:

- [soup]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/soupbintcp.pdf (SoupBinTCP Version 3.00)
- [mold]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf (MoldUDP64 V 1.00, last log entry 8/2/24)
- [ouch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf (OUCH 5.0, "Updated October, 2025")
- [itch5]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf (TotalView-ITCH 5.0, latest log entry April 28, 2023)

FIX Trading Community (GitHub repository FIXTradingCommunity/fix-simple-binary-encoding; the fixtrading.org online page returned 404):

- [sbe10]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0/v1-0-STANDARD/doc/03MessageStructure.md
- [sbe10e-01]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0-errata-Nov-2020/v1-0-STANDARD/doc/01Introduction.md
- [sbe10e-02]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0-errata-Nov-2020/v1-0-STANDARD/doc/02FieldEncoding.md
- [sbe10e-03]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0-errata-Nov-2020/v1-0-STANDARD/doc/03MessageStructure.md
- [sbe10e-04]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0-errata-Nov-2020/v1-0-STANDARD/doc/04MessageSchema.md
- [sbe10e-05]: https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/blob/v1.0-errata-Nov-2020/v1-0-STANDARD/doc/05SchemaExtensionMechanism.md

CME Group Client Systems Wiki (`https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/<id>`):

- [cme-ilink-hdr]: .../pages/716013709 (iLink Message Header)
- [cme-ilink-sbe]: .../pages/714113056 (iLink Simple Binary Encoding)
- [cme-session]: .../pages/714145834 (iLink Binary Order Entry - Session Layer)
- [cme-establish]: .../pages/457672951 (Establish)
- [cme-estack]: .../pages/457227484 (Establishment Acknowledgment)
- [cme-retransreq]: .../pages/457673099 (Retransmit Request)
- [cme-notapplied]: .../pages/457640161 (Not Applied)
- [cme-neg-reject]: .../pages/457227444 (Negotiation Reject)
- [cme-terminate]: .../pages/457327359 (Terminate)
- [cme-nos]: .../pages/715325687 (iLink New Order - Single)
- [cme-er-new]: .../pages/716275899 (iLink Execution Report - New Order)
- [cme-er-reject]: .../pages/715292906 (iLink Execution Report - Reject)
- [cme-bizreject]: .../pages/715686113 (iLink Business Reject)
- [cme-applayer]: .../pages/715325668 (iLink Application Layer)
- [cme-ids]: .../pages/457096319 (iLink - CME Globex Identifiers)
- [mdp-schema]: .../pages/457225459 (MDP 3.0 - Message Schema)
- [mdp-tech-hdr]: .../pages/457638617 (MDP 3.0 - SBE Technical Headers)
- [mdp-sbe-hdr]: .../pages/457325932 (MDP 3.0 - SBE Message Header)
- [mdp-packet]: .../pages/457326496 (MDP 3.0 - Packet Structure with Event Based Messaging)
- [mdp-secdef-fut]: .../pages/457327569 (MDP 3.0 - Market Data Security Definition - Futures)
- [mdp-incr-mbp]: .../pages/457227795 (MDP 3.0 - Market Data Incremental Refresh - MBP and MBOFD)
- [mdp-event]: .../pages/457325420 (MDP 3.0 - Event Based Market Data Messaging)
- [mdp-recovery-svc]: .../pages/457325847 (MDP 3.0 - Recovery Services)
- [mdp-mkt-recovery]: .../pages/457672425 (MDP 3.0 - MBP and MBOFD Market Recovery)
- [mdp-chreset]: .../pages/457326922 (MDP 3.0 - Channel Reset)

Binance:

- [bn-sbe-faq]: https://developers.binance.com/docs/binance-spot-api-docs/faqs/sbe_faq
- [bn-streams]: https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams
- [bn-spot-xml]: https://github.com/binance/binance-spot-api-docs/blob/master/sbe/schemas/spot_3_4.xml
- [bn-stream-xml]: https://github.com/binance/binance-spot-api-docs/blob/master/sbe/schemas/stream_1_0.xml
- [bn-lifecycle]: https://github.com/binance/binance-spot-api-docs/blob/master/sbe/schemas/sbe_schema_lifecycle_prod.json
- [bn-ws]: https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-api.md (request format)
