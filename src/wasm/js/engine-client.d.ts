// engine-client.d.ts: the engine client's types, written by hand (spec #127, "The engine client";
// docs/internals/wasm.md). engine-client.js is the only thing a page imports. Nothing is promised
// across releases: a page pins an exact one, and ENGINE_RELEASE names it.

/** The release this client, its worker, and its module were built as, such as "v2.2.0". */
export declare const ENGINE_RELEASE: string;
/** The message protocol's version, which the client and the worker exchange and hold each other to. */
export declare const PROTOCOL_VERSION: number;

export type MissingSupport = "isolation" | "shared-memory" | "threads" | "exceptions";
export type Support = { ok: true } | { ok: false; missing: MissingSupport };

/** Whether this page can run the engine: cross-origin isolation, a SharedArrayBuffer that can be
 * posted, wasm threads, and wasm exceptions, the first missing one named. */
export declare function checkSupport(): Support;

/** What createEngine rejects with when checkSupport fails. */
export declare class EngineSupportError extends Error {
    readonly missing: MissingSupport;
}

/** Where the engine's three files are served from, passed through to the module's locateFile. */
export interface EngineUrls {
    worker: string | URL;
    module: string | URL;
    wasm: string | URL;
}

/** Checks support, rejecting with an EngineSupportError, then starts the engine worker on an
 * empty page store, which the client keeps from then on, and starts the engine, which seeds it. Resolves once the worker is ready, with
 * the engine starting; its state becomes "running" at the engine's ready. */
export declare function createEngine(urls: EngineUrls): Promise<Engine>;

export type EngineState = "starting" | "running" | "stopping" | "down";

/** What a terminal line printed, as the native CLI prints it. */
export interface RunResult {
    stdout: string;
    stderr: string;
    exit: number;
}

export type Side = "BUY" | "SELL";
export type OrderType = "LIMIT" | "MARKET" | "STOP" | "STOP_LIMIT";
export type TimeInForce = "GTC" | "IOC" | "FOK";
export type SelfTradePrevention = "CANCEL_RESTING" | "CANCEL_INCOMING" | "CANCEL_BOTH";

/** An order without its ID, which the engine worker assigns. What is left out takes the
 * terminal's defaults: LIMIT with a price and MARKET without, STOP_LIMIT and STOP with a stop
 * price, GTC with a price and IOC without, and CANCEL_RESTING for an owner with no mode. */
export interface OrderInput {
    side: Side;
    quantity: number;
    price?: number;
    type?: OrderType;
    stopPrice?: number;
    timeInForce?: TimeInForce;
    postOnly?: boolean;
    owner?: number;
    selfTradePrevention?: SelfTradePrevention;
}

/** One event frame of an answer: its fields, and the CLI's line for it. A price of 0 on
 * OrderAccepted is a market order's. */
export type AnswerEvent =
    | { type: "OrderAccepted"; orderId: number; side: Side; price: number; quantity: number; text: string }
    | { type: "OrderRejected"; orderId: number; reason: string; text: string }
    | { type: "TradeExecuted"; incomingOrderId: number; restingOrderId: number; price: number; quantity: number; text: string }
    | { type: "OrderCancelled"; orderId: number; quantity: number; text: string }
    | { type: "CancelRejected"; orderId: number; reason: string; text: string }
    | { type: "OrderExpired"; orderId: number; quantity: number; text: string }
    | { type: "SelfTradePrevented"; incomingOrderId: number; restingOrderId: number; price: number; quantity: number; text: string }
    | { type: "StopTriggered"; orderId: number; stopPrice: number; tradePrice: number; text: string }
    | {
          type: "OrderReplaced";
          orderId: number;
          side: Side;
          price: number;
          quantity: number;
          openQuantity: number;
          priorityKept: boolean;
          text: string;
      }
    | { type: "ReplaceRejected"; orderId: number; reason: string; text: string }
    | { type: "Refused"; orderId: number; code: string; text: string };

/** A sandbox command's answer: a submit's assigned ID, every event, and what send prints for it. */
export interface Answer {
    orderId?: number;
    events: AnswerEvent[];
    text: string;
}

export interface RestingOrder {
    orderId: number;
    quantity: number;
    initialQuantity: number;
    owner?: number;
}

export interface Level {
    price: number;
    quantity: number;
    /** In queue order. */
    orders: RestingOrder[];
}

export interface PendingStop {
    orderId: number;
    side: Side;
    type: "STOP" | "STOP_LIMIT";
    stopPrice: number;
    /** A STOP_LIMIT's limit price. */
    price?: number;
    quantity: number;
    timeInForce: TimeInForce;
    owner?: number;
    selfTradePrevention?: SelfTradePrevention;
    postOnly: boolean;
}

/** A shadow book's snapshot: nothing is pushed. */
export interface Book {
    symbol: string;
    /** Best first. */
    bids: Level[];
    /** Best first. */
    asks: Level[];
    /** In trigger order. */
    stops: PendingStop[];
    lastTradePrice: number | null;
}

export interface ShardStatus {
    shard: number;
    instruments: string[];
    /** The records its log holds. */
    records: number;
}

export interface EngineLine {
    stream: "report" | "diagnostics";
    text: string;
}

export interface EngineExit {
    code: number;
    name: string;
}

export interface EngineEvents {
    state: EngineState;
    line: EngineLine;
    aborted: string;
    exited: EngineExit;
}

export interface Engine {
    readonly state: EngineState;
    /** A terminal line. start, stop, crash, and reset run here: one engine at a time, a first stop
     * clean with its report and a second forcing it. Lines run one at a time, in order; a crash
     * or a reset doesn't wait behind them, and a line it cuts off exits 3. */
    run(line: string): Promise<RunResult>;
    /** The sandbox's commands, rejecting with what refused them, or when the engine isn't running. */
    submit(symbol: string, order: OrderInput): Promise<Answer>;
    cancel(symbol: string, orderId: number): Promise<Answer>;
    replace(symbol: string, orderId: number, price: number, quantity: number): Promise<Answer>;
    book(symbol: string): Promise<Book>;
    status(): Promise<ShardStatus[]>;
    /** Ends the engine worker without warning, as a crash does, then gives a fresh idle worker
     * the page store, so verify and replay run there and start recovers. Everything waiting is
     * reported reset. An aborted engine and a forced stop take the same path. */
    crash(): Promise<void>;
    /** Ends the engine worker, discards the page store, and starts again from the seed. */
    reset(): Promise<void>;
    /** Listens until the returned function is called. */
    on<K extends keyof EngineEvents>(event: K, listener: (value: EngineEvents[K]) => void): () => void;
}
