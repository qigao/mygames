import {encodePing, encodeResume, encodeRoomCommand, encodeGameCommand, parseServerFrame} from './Protocol';
import type {RoomCommandType, GameClientCommand, ServerFrame, ServerHello} from './Protocol';

export type ConnectionState =
    'offline' | 'connecting' | 'handshaking' | 'restoring' | 'ready' | 'failed';

/**
 * Cocos browser WS client. The recovery capability is kept in memory only.
 * It is not a login token and must never be placed in a URL or debug log.
 */
export class GameSocket {
    private socket: WebSocket | null = null;
    private generation = 0;
    private requestOrdinal = 0;
    private status: ConnectionState = 'offline';
    private roomsAvailable = false;
    private gamesAvailable = false;
    private gameNonce: string | null = null;
    private readonly pendingGameActions = new Map<string, string>();

    private resumeTicket: string | null = null;
    private playerId: string | null = null;
    private provisional: ServerHello | null = null;

    constructor(
        private readonly onState: (state: ConnectionState, reason?: string) => void,
        private readonly onFrame: (frame: ServerFrame) => void
    ) {}

    get state(): ConnectionState { return this.status; }
    get canResume(): boolean { return this.resumeTicket !== null; }
    get currentPlayerId(): string | null { return this.playerId; }
    get canPlayGames(): boolean { return this.status === 'ready' && this.gamesAvailable; }
    get pendingGameCount(): number { return this.pendingGameActions.size; }

    connect(url: string): void {
        // An accidental Connect click must not replace a healthy connection,
        // which could cause an active-lease rejection during takeover.
        if (this.socket && this.status !== 'offline' && this.status !== 'failed')
            return;
        // Replacing the WebSocket does NOT discard a previously received ticket.
        // Explicit disconnect() is the only local credential-forgetting path.
        this.closeTransport();
        if (!/^wss?:\/\//i.test(url)) {
            this.transition('failed', 'Invalid WebSocket URL');
            return;
        }
        const id = ++this.generation;
        this.transition('connecting');
        let ws: WebSocket;
        try {
            ws = new WebSocket(url);
        } catch {
            this.transition('failed', 'Cannot open WebSocket');
            return;
        }
        this.socket = ws;
        ws.onopen = () => {
            if (id === this.generation) this.transition('handshaking');
        };
        ws.onmessage = (event: MessageEvent) => {
            if (id !== this.generation) return;
            if (typeof event.data !== 'string') {
                this.fail('Binary frames unsupported');
                return;
            }
            try {
                const frame = parseServerFrame(event.data);
                this.handleFrame(frame);
            } catch (error) {
                this.fail(error instanceof Error ? error.message : 'Protocol error');
            }
        };
        ws.onerror = () => {
            if (id === this.generation) this.fail('WebSocket transport error');
        };
        ws.onclose = () => {
            if (id === this.generation && this.status !== 'failed') {
                this.socket = null;
                this.roomsAvailable = false;
                this.gamesAvailable = false;
                this.provisional = null;
                this.transition('offline');
            }
        };
    }

    ping(): void {
        this.ensureReady();
        this.socket!.send(encodePing());
    }
    createRoom(): string { return this.requestRoom('room.create'); }
    joinRoom(roomId: string): string {
        return this.requestRoom('room.join', {room_id: roomId});
    }
    setReady(ready: boolean): string {
        return this.requestRoom('room.ready', {ready});
    }
    leaveRoom(): string { return this.requestRoom('room.leave'); }
    startRoom(): string { return this.requestRoom('room.start'); }
    requestSnapshot(): string { return this.requestRoom('room.snapshot.get'); }

    /** A secure, per-Cocos-session nonce prevents a new browser process
     * from reusing a previously journalled per-player request_id. Existing
     * pending commands are retained across transient disconnect and resent
     * byte-for-byte ONLY after the original session bearer is recovered.
     * Never retry with a newly minted ID or an unbounded command queue. */
    private newGameRequestId(): string {
        if (!this.gameNonce) {
            const entropy = globalThis.crypto?.getRandomValues?.(new Uint8Array(8));
            if (!entropy) throw new Error('Secure request ID generation unavailable');
            this.gameNonce = [...entropy].map(x => x.toString(16).padStart(2, '0')).join('');
        }
        return 'g-' + this.gameNonce + '-' + (++this.requestOrdinal).toString(36);
    }

    private sendGameCommand(type: GameClientCommand,
                            options: {game_id?: string; turn_id?: string;
                                      bid?: number; cards_hex?: string} = {},
                            durable: boolean): string {
        this.ensureReady();
        if (!this.gamesAvailable) throw new Error('Game service unavailable');
        if (durable && this.pendingGameActions.size >= 8)
            throw new Error('Resolve the pending game requests first');
        const id = this.newGameRequestId();
        const json = encodeGameCommand(type, id, options);
        if (durable) this.pendingGameActions.set(id, json);
        try { this.socket!.send(json); }
        catch (error) {
            this.pendingGameActions.delete(id);
            throw error;
        }
        return id;
    }
    startGame(): string { return this.sendGameCommand('game.start', {}, true); }
    bid(gameId: string, turnId: string, score: number): string {
        return this.sendGameCommand('game.bid',
                                    {game_id: gameId, turn_id: turnId, bid: score}, true);
    }
    play(gameId: string, turnId: string, cardMask: string): string {
        return this.sendGameCommand('game.play',
                                    {game_id: gameId, turn_id: turnId,
                                     cards_hex: cardMask}, true);
    }
    pass(gameId: string, turnId: string): string {
        return this.sendGameCommand('game.pass',
                                    {game_id: gameId, turn_id: turnId}, true);
    }
    requestGameSnapshot(gameId: string): string {
        return this.sendGameCommand('game.snapshot.get', {game_id: gameId}, false);
    }
    requestGameHand(gameId: string): string {
        return this.sendGameCommand('game.hand.get', {game_id: gameId}, false);
    }
    retryPendingGameActions(): void {
        this.ensureReady();
        if (!this.gamesAvailable) throw new Error('Game service unavailable');
        for (const original of this.pendingGameActions.values()) this.socket!.send(original);
    }

    /** Simulated transient network disconnect. Retains the in-memory ticket. */
    suspend(): void {
        if (this.socket?.readyState === WebSocket.OPEN) this.socket.close();
    }

    /** User-initiated logout: drops local recovery capability. */
    disconnect(): void {
        this.closeTransport();
        this.resumeTicket = null;
        this.playerId = null;
        this.pendingGameActions.clear();
        this.gameNonce = null;
        this.transition('offline');
    }

    private handleFrame(frame: ServerFrame): void {
        if (frame.type === 'server.hello') {
            if (this.status !== 'handshaking' ||
                !frame.capabilities.includes('ping')) {
                throw new Error('Incompatible server handshake');
            }
            this.roomsAvailable = frame.capabilities.includes('rooms');
            this.gamesAvailable = frame.capabilities.includes('games');
            if (this.resumeTicket && frame.resume_token && frame.player_id &&
                frame.capabilities.includes('resume')) {
                this.provisional = frame;
                this.transition('restoring');
                this.socket!.send(encodeResume(this.resumeTicket));
                return;
            }
            const hadPreviousLease = this.resumeTicket !== null;
            if (hadPreviousLease) this.pendingGameActions.clear();
            this.resumeTicket = frame.resume_token ?? null;
            this.playerId = frame.player_id ?? null;
            this.transition('ready');
            if (hadPreviousLease) {
                // Connected to an older/unsupported server: never keep a room
                // view associated with the previous process/identity.
                this.onFrame({v: 1, type: 'session.error', code: 'resume_unavailable'});
            }
            this.onFrame(frame);
            return;
        }
        if (frame.type === 'session.ready') {
            if (this.status !== 'restoring' || !this.provisional)
                throw new Error('Unexpected session recovery');
            this.resumeTicket = frame.resume_token;
            this.playerId = frame.player_id;
            this.provisional = null;
            this.transition('ready');
            // If the recovered peer temporarily cannot advertise games,
            // retain unresolved original receipt IDs but do NOT dispatch
            // them. A later compatible durable Owner may still reconcile.
            if (this.gamesAvailable) this.retryPendingGameActions();
            this.onFrame(frame);
            return;
        }
        if (frame.type === 'session.error') {
            if (this.status !== 'restoring' || !this.provisional)
                throw new Error('Unexpected recovery rejection');
            // Recovery was rejected. Continue only as a brand-new, limited
            // development session, and make the UI discard its old room state.
            const fresh = this.provisional;
            this.provisional = null;
            this.pendingGameActions.clear();
            this.resumeTicket = fresh.resume_token ?? null;
            this.playerId = fresh.player_id ?? null;
            this.transition('ready');
            this.onFrame(frame);
            this.onFrame(fresh);
            return;
        }
        if (this.status !== 'ready')
            throw new Error('Event received before session handshake');
        if (frame.type === 'game.ack')
            this.pendingGameActions.delete(frame.request_id);
        if (frame.type === 'game.error' &&
            frame.code !== 'storage_unavailable' &&
            frame.code !== 'durable_game_unavailable') {
            // Deterministic gameplay rejection is terminal. A storage error
            // is NOT: COMMIT_UNKNOWN may already have changed the database.
            // Retain that exact request_id/body for trusted recovery, never
            // guess success or replace it with a new request.
            this.pendingGameActions.delete(frame.request_id);
        }
        this.onFrame(frame);
    }

    private requestRoom(type: RoomCommandType,
                        options: {room_id?: string; ready?: boolean} = {}): string {
        this.ensureReady();
        if (!this.roomsAvailable) throw new Error('Server does not support rooms');
        const id = 'req-' + this.generation + '-' + (++this.requestOrdinal);
        this.socket!.send(encodeRoomCommand(type, id, options));
        return id;
    }

    private ensureReady(): void {
        if (this.status !== 'ready' || this.socket?.readyState !== WebSocket.OPEN)
            throw new Error('Not connected');
    }

    private closeTransport(): void {
        ++this.generation;
        this.roomsAvailable = false;
        this.gamesAvailable = false;
        this.provisional = null;
        this.requestOrdinal = 0;
        const previous = this.socket;
        this.socket = null;
        if (previous) {
            previous.onopen = null;
            previous.onmessage = null;
            previous.onerror = null;
            previous.onclose = null;
            previous.close();
        }
    }

    private fail(reason: string): void {
        this.closeTransport();
        this.transition('failed', reason);
    }
    private transition(state: ConnectionState, reason?: string): void {
        this.status = state;
        this.onState(state, reason);
    }
}
