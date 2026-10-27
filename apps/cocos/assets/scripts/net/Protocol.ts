/** MyGames v1 developer-room wire contract. NOT a production auth protocol. */
export const PROTOCOL_VERSION = 1 as const;
export const ROOM_SEATS = 3 as const;

export interface WireEnvelope {
    v: number;
    type: string;
}
export interface ServerHello extends WireEnvelope {
    v: 1;
    type: 'server.hello';
    capabilities: string[];
    player_id?: string;
    resume_token?: string;
}
export interface ServerPong extends WireEnvelope { v: 1; type: 'server.pong' }
export interface ServerError extends WireEnvelope {
    v: 1;
    type: 'server.error';
    code: string;
}
export interface RoomAck extends WireEnvelope {
    v: 1;
    type: 'room.ack';
    request_id: string;
    room_id: string;
    revision: number;
    seat: number; // 0..2, or 3 after a successful room.leave
}
export interface RoomError extends WireEnvelope {
    v: 1;
    type: 'room.error';
    request_id: string;
    code: string;
}
export interface RoomSnapshot extends WireEnvelope {
    v: 1;
    type: 'room.snapshot';
    room_id: string;
    room_epoch: number;
    revision: number;
    phase: 'waiting' | 'starting';
    owner_id: string;
    seats: Array<{player_id: string; ready: boolean} | null>;
}
export interface SessionReady extends WireEnvelope {
    v: 1;
    type: 'session.ready';
    player_id: string;
    resume_token: string;
    generation: number;
    resumed: true;
}
export interface SessionError extends WireEnvelope {
    v: 1;
    type: 'session.error';
    code: string;
}
export type GameAction = 'start' | 'bid' | 'play' | 'pass';
export type GameClientCommand =
    'game.start' | 'game.bid' | 'game.play' | 'game.pass' |
    'game.snapshot.get' | 'game.hand.get' |
    'game.result.get' | 'game.standings.get';
export type GamePhase = 'bidding' | 'redeal_required' | 'playing' | 'finished';
export interface GameAck extends WireEnvelope {
    v: 1;
    type: 'game.ack';
    action: GameAction;
    request_id: string;
    room_id: string;
    room_epoch: string;
    game_id: string;
    phase: 1 | 2 | 3 | 4;
    turn_id: string;
    seq: string;
}
export interface GameError extends WireEnvelope {
    v: 1;
    type: 'game.error';
    request_id: string;
    code: string;
}
export interface GameBiddingSnapshot extends WireEnvelope {
    v: 1;
    type: 'game.snapshot';
    request_id: string;
    room_id: string;
    room_epoch: string;
    game_id: string;
    phase: 'bidding' | 'redeal_required';
    bid_turn_id: string;
    bid_turn_seat: number;
    highest_bid: number;
    highest_seat: number;
}
/* Exact public settlement. Zero-sum game points, never money. */
export interface GameScore {
    base: number;
    bombs: number;
    spring: 'none' | 'landlord' | 'farmers';
    multiplier: number;
    unit: number;
    delta: [number, number, number];
}
export interface GamePlayingSnapshot extends WireEnvelope {
    v: 1;
    type: 'game.snapshot';
    request_id: string;
    room_id: string;
    room_epoch: string;
    game_id: string;
    phase: 'playing' | 'finished';
    highest_bid: number;
    landlord_seat: number;
    revealed_kitty_hex: string;
    turn_id: string;
    turn_seat: number;
    last_cards_hex: string;
    last_seat: number;
    remaining: [number, number, number];
    winner_seat: number;
    score?: GameScore; // Required ONLY for FINISHED
}
export type GameSnapshot = GameBiddingSnapshot | GamePlayingSnapshot;
export interface GameHand extends WireEnvelope {
    v: 1;
    type: 'game.hand';
    request_id: string;
    room_id: string;
    game_id: string;
    cards_hex: string;
}
/* Historical scores are public to the CURRENT authenticated room members
 * only; they contain no deck, hand, ticket or money. All 64-bit IDs and
 * cumulative signed totals arrive as LOSSLESS canonical strings. */
export interface GameResult extends WireEnvelope {
    v: 1;
    type: 'game.result';
    request_id: string;
    room_id: string;
    game_id: string;
    finish_seq: string;
    players: [string, string, string];
    score: GameScore;
}
export interface GameStandingMember {
    player_id: string;
    points: string;
    games: string;
}
export interface GameStandings extends WireEnvelope {
    v: 1;
    type: 'game.standings';
    request_id: string;
    room_id: string;
    room_epoch: string;
    checkpoint_seq: string;
    players: [GameStandingMember, GameStandingMember, GameStandingMember];
}
export type GameFrame = GameAck | GameError | GameSnapshot | GameHand |
    GameResult | GameStandings;
export type ServerFrame =
    ServerHello | ServerPong | ServerError | RoomAck | RoomError | RoomSnapshot |
    SessionReady | SessionError | GameFrame;

function record(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function safeText(value: unknown, max = 48): value is string {
    return typeof value === 'string' &&
        value.length >= 1 && value.length <= max &&
        /^[A-Za-z0-9_.:-]+$/.test(value);
}
function safeNumber(value: unknown): value is number {
    return Number.isSafeInteger(value) && typeof value === 'number' && value >= 0;
}
export function validResumeToken(value: unknown): value is string {
    return typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
}
function safePlayer(value: unknown): value is string {
    return typeof value === 'string' && /^[1-9][0-9]*$/.test(value) &&
        value.length <= 20;
}
export function parseServerFrame(data: string): ServerFrame {
    let value: unknown;
    try {
        value = JSON.parse(data);
    } catch {
        throw new Error('Invalid JSON from server');
    }
    if (!record(value) || value.v !== PROTOCOL_VERSION || typeof value.type !== 'string') {
        throw new Error('Incompatible wire protocol');
    }
    if (value.type.startsWith('game.')) return parseGameFrame(value);
    switch (value.type) {
    case 'server.hello':
        if (!Array.isArray(value.capabilities) ||
            !value.capabilities.every((x: unknown) => typeof x === 'string' && x.length <= 32) ||
            (value.player_id !== undefined && !safePlayer(value.player_id)) ||
            (value.resume_token !== undefined && !validResumeToken(value.resume_token))) {
            throw new Error('Invalid server hello');
        }
        return value as unknown as ServerHello;
    case 'server.pong':
        return value as unknown as ServerPong;
    case 'session.ready':
        if (!safePlayer(value.player_id) ||
            !validResumeToken(value.resume_token) ||
            !safeNumber(value.generation) || value.generation < 2 ||
            value.resumed !== true) throw new Error('Invalid session recovery');
        return value as unknown as SessionReady;
    case 'session.error':
        if (!safeText(value.code)) throw new Error('Invalid session error');
        return value as unknown as SessionError;
    case 'server.error':
        if (!safeText(value.code)) throw new Error('Invalid server error');
        return value as unknown as ServerError;
    case 'room.ack':
        if (!safeText(value.request_id) ||
            !safeText(value.room_id, 39) ||
            !safeNumber(value.revision) ||
            !safeNumber(value.seat) || value.seat > ROOM_SEATS) {
            throw new Error('Invalid room acknowledgement');
        }
        return value as unknown as RoomAck;
    case 'room.error':
        if (!safeText(value.request_id) || !safeText(value.code)) {
            throw new Error('Invalid room error');
        }
        return value as unknown as RoomError;
    case 'room.snapshot':
        if (!safeText(value.room_id, 39) ||
            !safeNumber(value.room_epoch) || value.room_epoch < 1 ||
            !safeNumber(value.revision) || value.revision < 1 ||
            (value.phase !== 'waiting' && value.phase !== 'starting') ||
            !safePlayer(value.owner_id) ||
            !Array.isArray(value.seats) || value.seats.length !== ROOM_SEATS ||
            !value.seats.every((seat: unknown) =>
                seat === null || (record(seat) &&
                    safePlayer(seat.player_id) && typeof seat.ready === 'boolean'))) {
            throw new Error('Invalid room snapshot');
        }
        return value as unknown as RoomSnapshot;
    default:
        throw new Error('Unknown server frame type');
    }
}
export function encodeResume(token: string): string {
    if (!validResumeToken(token)) throw new Error('Invalid resume credential');
    return JSON.stringify({v: PROTOCOL_VERSION, type: 'session.resume', resume_token: token});
}
export function encodePing(): string {
    return JSON.stringify({v: PROTOCOL_VERSION, type: 'client.ping'});
}
export type RoomCommandType =
    'room.create' | 'room.join' | 'room.ready' | 'room.leave' |
    'room.start' | 'room.snapshot.get';
export function encodeRoomCommand(type: RoomCommandType, requestId: string,
                                  options: {room_id?: string; ready?: boolean} = {}): string {
    if (!safeText(requestId) || !type) throw new Error('Invalid request id');
    const data: Record<string, string | number | boolean> = {
        v: PROTOCOL_VERSION, type, request_id: requestId
    };
    if (type === 'room.join') {
        if (!safeText(options.room_id, 39)) throw new Error('Invalid room id');
        data.room_id = options.room_id;
    } else if (type === 'room.ready') {
        if (typeof options.ready !== 'boolean') throw new Error('Invalid ready value');
        data.ready = options.ready;
    }
    return JSON.stringify(data);
}


/** Explicitly declared game frames cannot smuggle private fields into UI. */
function exactFields(value: Record<string, unknown>, names: readonly string[]): boolean {
    return Object.keys(value).length === names.length &&
        Object.keys(value).every((key) => names.includes(key));
}
/** Unsigned *canonical* 64-bit decimal represented as a string, not JS number. */
export function canonicalU64(value: unknown): value is string {
    if (typeof value !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value))
        return false;
    return BigInt(value) <= 18446744073709551615n;
}
/** 54 physical cards occupy 14 lowercase hex digits (top nibble <= 3). */
export function cardMaskHex(value: unknown, allowEmpty = true): value is string {
    if (typeof value !== 'string' || !/^[0-3][0-9a-f]{13}$/.test(value))
        return false;
    return allowEmpty || value !== '00000000000000';
}
export function cardsFromMask(hex: string): number[] {
    if (!cardMaskHex(hex)) throw new Error('Invalid physical card mask');
    const bits = BigInt('0x' + hex);
    const cards: number[] = [];
    for (let id = 53; id >= 0; id--) {
        if ((bits & (1n << BigInt(id))) !== 0n) cards.push(id);
    }
    return cards;
}
export function cardMaskFromIds(ids: readonly number[]): string {
    if (ids.length === 0 || ids.length > 20) throw new Error('Select 1–20 cards');
    let mask = 0n;
    for (const id of ids) {
        if (!Number.isInteger(id) || id < 0 || id > 53)
            throw new Error('Invalid physical card ID');
        const card = 1n << BigInt(id);
        if ((mask & card) !== 0n) throw new Error('Duplicate physical card');
        mask |= card;
    }
    return mask.toString(16).padStart(14, '0');
}

export function canonicalSignedI64(value: unknown): value is string {
    if (typeof value !== 'string' ||
        !/^(?:0|-[1-9][0-9]{0,18}|[1-9][0-9]{0,18})$/.test(value))
        return false;
    const n = BigInt(value);
    return n >= -9223372036854775808n && n <= 9223372036854775807n;
}
function canonicalCount(value: unknown): value is string {
    return value === '0' || canonicalU64(value);
}
function validGameScore(
    value: unknown, base: number, landlord: number, winner: number
): value is GameScore {
    if (!record(value) ||
        !exactFields(value,['base','bombs','spring','multiplier','unit','delta']) ||
        !Number.isInteger(value.base) || value.base !== base ||
        !Number.isInteger(value.bombs) || (value.bombs as number) < 0 ||
        (value.bombs as number) > 14 ||
        (value.spring !== 'none' &&
         value.spring !== 'landlord' && value.spring !== 'farmers') ||
        !Number.isInteger(value.multiplier) || !Number.isInteger(value.unit) ||
        !Array.isArray(value.delta) || value.delta.length !== 3 ||
        !value.delta.every((x: unknown) =>
            typeof x === 'number' && Number.isSafeInteger(x)) ||
        (value.delta as number[]).reduce((a: number,b: number) => a+b,0) !== 0 ||
        value.multiplier !== 2 ** ((value.bombs as number) +
                        (value.spring !== 'none' ? 1 : 0)) ||
        value.unit !== (value.base as number) * (value.multiplier as number) ||
        (value.spring === 'landlord' && winner !== landlord) ||
        (value.spring === 'farmers' && winner === landlord))
        return false;
    const points = value.delta as number[];
    const unit = value.unit as number;
    return seatIndex(landlord,false) && seatIndex(winner,false) &&
        points[landlord] === (winner === landlord ? unit*2 : -unit*2) &&
        points.every((point,i) =>
            i === landlord || point === (winner === landlord ? -unit : unit));
}

function parseGameFrame(value: Record<string, unknown>): GameFrame {
    const common = ['v','type','request_id','room_id','room_epoch','game_id'];
    if (value.type === 'game.result') {
        if (!exactFields(value,['v','type','request_id','room_id','game_id',
                                'finish_seq','players','score']) ||
            !safeText(value.request_id) || !safeText(value.room_id,39) ||
            !canonicalU64(value.game_id) ||
            !canonicalU64(value.finish_seq) ||
            !Array.isArray(value.players) || value.players.length !== 3 ||
            !value.players.every((id: unknown) => safePlayer(id)) ||
            new Set(value.players).size !== 3 ||
            !record(value.score) ||
            !validGameScore(value.score, value.score.base as number,
                            value.score.landlord_seat as number,
                            value.score.winner_seat as number))
            throw new Error('Invalid historical game result');
        return value as unknown as GameResult;
    }
    if (value.type === 'game.standings') {
        if (!exactFields(value,['v','type','request_id','room_id',
                                'room_epoch','checkpoint_seq','players']) ||
            !safeText(value.request_id) || !safeText(value.room_id,39) ||
            !canonicalU64(value.room_epoch) ||
            !canonicalU64(value.checkpoint_seq) ||
            !Array.isArray(value.players) || value.players.length !== 3)
            throw new Error('Invalid room standings');
        const entries = value.players as unknown[];
        if (!entries.every((entry: unknown) =>
            record(entry) && exactFields(entry,['player_id','points','games']) &&
            safePlayer(entry.player_id) &&
            canonicalSignedI64(entry.points) && canonicalCount(entry.games)))
            throw new Error('Invalid player standings');
        const participants = entries as GameStandingMember[];
        if (new Set(participants.map(p=>p.player_id)).size !== 3 ||
            participants.some(p=>p.games !== participants[0].games) ||
            participants.reduce((sum,p)=>sum+BigInt(p.points),0n) !== 0n)
            throw new Error('Unbalanced historical standings');
        return value as unknown as GameStandings;
    }
    if (value.type === 'game.ack') {
        if (!exactFields(value, [...common,'action','phase','turn_id','seq']) ||
            !safeText(value.request_id) || !safeText(value.room_id, 39) ||
            !canonicalU64(value.room_epoch) || !canonicalU64(value.game_id) ||
            !canonicalU64(value.turn_id) || !canonicalU64(value.seq) ||
            (typeof value.action !== 'string' ||
             !['start','bid','play','pass'].includes(value.action)) ||
            !Number.isInteger(value.phase) ||
            ![1,2,3,4].includes(value.phase as number))
            throw new Error('Invalid durable game ACK');
        return value as unknown as GameAck;
    }
    if (value.type === 'game.error') {
        if (!exactFields(value, ['v','type','request_id','code']) ||
            !safeText(value.request_id) || !safeText(value.code))
            throw new Error('Invalid game error');
        return value as unknown as GameError;
    }
    if (value.type === 'game.hand') {
        if (!exactFields(value, ['v','type','request_id','room_id','game_id','cards_hex']) ||
            !safeText(value.request_id) || !safeText(value.room_id, 39) ||
            !canonicalU64(value.game_id) || !cardMaskHex(value.cards_hex))
            throw new Error('Invalid private game hand');
        return value as unknown as GameHand;
    }
    if (value.type === 'game.snapshot') {
        if (!safeText(value.request_id) || !safeText(value.room_id, 39) ||
            !canonicalU64(value.room_epoch) || !canonicalU64(value.game_id))
            throw new Error('Invalid public game snapshot');
        if (value.phase === 'bidding' || value.phase === 'redeal_required') {
            if (!exactFields(value, [...common,'phase','bid_turn_id','bid_turn_seat',
                                      'highest_bid','highest_seat']) ||
                !canonicalU64(value.bid_turn_id) ||
                !seatIndex(value.bid_turn_seat, true) ||
                !Number.isInteger(value.highest_bid) ||
                (value.highest_bid as number) < 0 || (value.highest_bid as number) > 3 ||
                !seatIndex(value.highest_seat, true) ||
                ((value.highest_bid === 0) !== (value.highest_seat === 3)))
                throw new Error('Invalid bidding snapshot');
            return value as unknown as GameBiddingSnapshot;
        }
        if (value.phase === 'playing' || value.phase === 'finished') {
            if (!exactFields(value, [...common,'phase','highest_bid','landlord_seat',
                                      'revealed_kitty_hex','turn_id','turn_seat',
                                      'last_cards_hex','last_seat','remaining',
                                      'winner_seat',
                                      ...(value.phase === 'finished' ? ['score'] : [])]) ||
                !canonicalU64(value.turn_id) ||
                !Number.isInteger(value.highest_bid) ||
                (value.highest_bid as number) < 1 || (value.highest_bid as number) > 3 ||
                !seatIndex(value.landlord_seat, false) ||
                !seatIndex(value.turn_seat, true) ||
                !seatIndex(value.last_seat, true) ||
                !seatIndex(value.winner_seat, true) ||
                !cardMaskHex(value.revealed_kitty_hex, false) ||
                cardsFromMask(value.revealed_kitty_hex as string).length !== 3 ||
                !cardMaskHex(value.last_cards_hex) ||
                !Array.isArray(value.remaining) || value.remaining.length !== 3 ||
                !value.remaining.every((n: unknown) =>
                    typeof n === 'number' && Number.isInteger(n) && n >= 0 && n <= 20) ||
                (value.phase === 'playing' &&
                 (value.winner_seat !== 3 || value.turn_seat === 3)) ||
                (value.phase === 'finished' &&
                 (value.winner_seat === 3 || value.turn_seat !== 3)))
                throw new Error('Invalid playing snapshot');
            if (value.phase === 'finished' &&
                !validGameScore(value.score, value.highest_bid as number,
                                value.landlord_seat as number,
                                value.winner_seat as number))
                throw new Error('Invalid authoritative score');
            return value as unknown as GamePlayingSnapshot;
        }
        throw new Error('Invalid game phase');
    }
    throw new Error('Unknown game frame');
}
function seatIndex(value: unknown, sentinel: boolean): boolean {
    return typeof value === 'number' && Number.isInteger(value) &&
        value >= 0 && value <= (sentinel ? 3 : 2);
}

export function encodeGameCommand(
    type: GameClientCommand, requestId: string,
    options: {game_id?: string; turn_id?: string; bid?: number; cards_hex?: string} = {}
): string {
    if (!safeText(requestId)) throw new Error('Invalid game request ID');
    const payload: Record<string, number | string> = {
        v: PROTOCOL_VERSION, type, request_id: requestId
    };
    if (type === 'game.start' || type === 'game.standings.get') {
        if (Object.keys(options).length)
            throw new Error('Game ID/room identity is server-owned');
    } else if (type === 'game.bid') {
        if (!canonicalU64(options.game_id) || !canonicalU64(options.turn_id) ||
            !Number.isInteger(options.bid) || (options.bid ?? -1) < 0 ||
            (options.bid ?? 4) > 3 ||
            options.cards_hex !== undefined)
            throw new Error('Invalid bid');
        payload.game_id = options.game_id;
        payload.turn_id = options.turn_id;
        payload.bid = options.bid!;
    } else if (type === 'game.play') {
        if (!canonicalU64(options.game_id) || !canonicalU64(options.turn_id) ||
            !cardMaskHex(options.cards_hex, false) || options.bid !== undefined ||
            cardsFromMask(options.cards_hex).length > 20)
            throw new Error('Invalid move');
        payload.game_id = options.game_id;
        payload.turn_id = options.turn_id;
        payload.cards_hex = options.cards_hex;
    } else if (type === 'game.pass') {
        if (!canonicalU64(options.game_id) || !canonicalU64(options.turn_id) ||
            options.cards_hex !== undefined || options.bid !== undefined)
            throw new Error('Invalid pass');
        payload.game_id = options.game_id;
        payload.turn_id = options.turn_id;
    } else if (type === 'game.snapshot.get' || type === 'game.hand.get' ||
               type === 'game.result.get') {
        if (!canonicalU64(options.game_id) || options.turn_id !== undefined ||
            options.cards_hex !== undefined || options.bid !== undefined)
            throw new Error('Invalid game query');
        payload.game_id = options.game_id;
    } else {
        throw new Error('Unknown game command');
    }
    return JSON.stringify(payload);
}
