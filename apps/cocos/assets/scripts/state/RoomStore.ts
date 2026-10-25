import type {RoomAck, RoomSnapshot} from '../net/Protocol';

/** Presentation-only room state. Never authorizes or mutates the server room. */
export class RoomStore {
    private joinedId: string | null = null;
    private snapshot: RoomSnapshot | null = null;

    get roomId(): string | null { return this.joinedId; }
    get current(): RoomSnapshot | null { return this.snapshot; }

    reset(): void {
        this.joinedId = null;
        this.snapshot = null;
    }

    onAck(ack: RoomAck): void {
        if (ack.seat === 3) { // server confirms our leave
            if (ack.room_id === this.joinedId) this.reset();
            return;
        }
        if (ack.room_id !== this.joinedId) {
            this.joinedId = ack.room_id;
            this.snapshot = null;
        }
    }

    /** Ignore revisions from older owners/epochs or unexpected rooms. */
    applySnapshot(next: RoomSnapshot): boolean {
        if (next.room_id !== this.joinedId) return false;
        const current = this.snapshot;
        if (current && (next.room_epoch < current.room_epoch ||
            (next.room_epoch === current.room_epoch && next.revision <= current.revision))) {
            return false;
        }
        this.snapshot = next;
        return true;
    }
}
