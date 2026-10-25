/** MyGames development wire contract; not yet an authenticated game protocol. */
export const PROTOCOL_VERSION = 1 as const;

export interface WireEnvelope {
    v: number;
    type: string;
}

export interface ServerHello extends WireEnvelope {
    v: 1;
    type: 'server.hello';
    capabilities: string[];
}

export interface ServerPong extends WireEnvelope {
    v: 1;
    type: 'server.pong';
}

export interface ServerError extends WireEnvelope {
    v: 1;
    type: 'server.error';
    code: string;
}

export type ServerFrame = ServerHello | ServerPong | ServerError;

function record(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function parseServerFrame(data: string): ServerFrame {
    let parsed: unknown;
    try {
        parsed = JSON.parse(data);
    } catch {
        throw new Error('Invalid JSON from server');
    }
    if (!record(parsed) || parsed.v !== PROTOCOL_VERSION || typeof parsed.type !== 'string') {
        throw new Error('Incompatible wire protocol');
    }
    if (parsed.type === 'server.hello') {
        if (!Array.isArray(parsed.capabilities) ||
            !parsed.capabilities.every((x: unknown) => typeof x === 'string')) {
            throw new Error('Invalid server capabilities');
        }
        return parsed as unknown as ServerHello;
    }
    if (parsed.type === 'server.pong') return parsed as unknown as ServerPong;
    if (parsed.type === 'server.error') {
        if (typeof parsed.code !== 'string') throw new Error('Invalid server error');
        return parsed as unknown as ServerError;
    }
    throw new Error('Unknown server frame type');
}

export function encodePing(): string {
    return JSON.stringify({v: PROTOCOL_VERSION, type: 'client.ping'});
}

/* Reserved future state model. Never trust a remote snapshot without schema
 * validation, player scope filtering and room_epoch/seq fencing. */
export interface RoomSnapshot {
    room_id: string;
    room_epoch: number;
    revision: number;
    phase: 'waiting' | 'starting' | 'playing' | 'finished';
    owner_id: string;
    seats: Array<{player_id: string; ready: boolean} | null>;
}
