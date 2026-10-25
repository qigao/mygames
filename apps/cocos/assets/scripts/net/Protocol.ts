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
export type ServerFrame =
    ServerHello | ServerPong | ServerError | RoomAck | RoomError | RoomSnapshot |
    SessionReady | SessionError;

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
    'room.create' | 'room.join' | 'room.ready' | 'room.leave' | 'room.snapshot.get';
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
