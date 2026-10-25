import {encodePing, parseServerFrame} from './Protocol';
import type {ServerFrame} from './Protocol';

export type ConnectionState =
    'offline' | 'connecting' | 'handshaking' | 'ready' | 'failed';

export class GameSocket {
    private socket: WebSocket | null = null;
    private generation = 0;
    private status: ConnectionState = 'offline';

    constructor(
        private readonly onState: (state: ConnectionState, reason?: string) => void,
        private readonly onFrame: (frame: ServerFrame) => void
    ) {}

    get state(): ConnectionState {
        return this.status;
    }

    connect(url: string): void {
        this.disconnect();
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
                if (frame.type === 'server.hello') {
                    if (this.status !== 'handshaking' ||
                        !frame.capabilities.includes('ping')) {
                        this.fail('Incompatible server handshake');
                        return;
                    }
                    this.transition('ready');
                } else if (this.status !== 'ready') {
                    this.fail('Event received before handshake');
                    return;
                }
                this.onFrame(frame);
            } catch (error) {
                this.fail(error instanceof Error ? error.message : 'Protocol error');
            }
        };
        ws.onerror = () => {
            if (id === this.generation) this.fail('WebSocket transport error');
        };
        ws.onclose = () => {
            if (id === this.generation && this.status !== 'failed') {
                this.transition('offline');
            }
        };
    }

    ping(): void {
        if (this.status !== 'ready' || this.socket?.readyState !== WebSocket.OPEN) {
            throw new Error('Not connected');
        }
        this.socket.send(encodePing());
    }

    disconnect(): void {
        ++this.generation;
        const previous = this.socket;
        this.socket = null;
        if (previous !== null) {
            previous.onopen = null;
            previous.onmessage = null;
            previous.onerror = null;
            previous.onclose = null;
            previous.close();
        }
        this.transition('offline');
    }

    private fail(reason: string): void {
        this.transition('failed', reason);
        const previous = this.socket;
        this.socket = null;
        ++this.generation;
        if (previous) {
            previous.onopen = null;
            previous.onmessage = null;
            previous.onerror = null;
            previous.onclose = null;
            previous.close();
        }
    }

    private transition(state: ConnectionState, reason?: string): void {
        this.status = state;
        this.onState(state, reason);
    }
}
