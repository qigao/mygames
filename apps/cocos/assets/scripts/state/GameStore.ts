import {cardsFromMask, cardMaskFromIds} from '../net/Protocol';
import type {GameAck, GameHand, GameSnapshot} from '../net/Protocol';

/**
 * UI-only state, not a game authority. Never persist/log a hand or pass it to
 * MQTT, localStorage, RoomStore or a public snapshot. All plays MUST be
 * checked again by the native CFlow/M5a Game Owner.
 */
export class GameStore {
    private roomIdValue: string | null = null;
    private playerIdValue: string | null = null;
    private currentView: GameSnapshot | null = null;
    private privateHand: GameHand | null = null;
    private readonly selectedCards = new Set<number>();
    private awaitingHand: {requestId: string; gameId: string} | null = null;
    private lastAckValue: GameAck | null = null;
    private hydratedValue = false;

    get roomId(): string | null { return this.roomIdValue; }
    get playerId(): string | null { return this.playerIdValue; }
    get snapshot(): GameSnapshot | null { return this.currentView; }
    get hand(): readonly number[] {
        return this.privateHand ? cardsFromMask(this.privateHand.cards_hex) : [];
    }
    get selected(): readonly number[] { return [...this.selectedCards].sort((a,b)=>b-a); }
    get lastAck(): GameAck | null { return this.lastAckValue; }
    get hydrated(): boolean { return this.hydratedValue; }
    get gameId(): string | null {
        return this.currentView?.game_id ?? this.lastAckValue?.game_id ?? null;
    }
    get isMyTurn(): boolean {
        if (!this.hydratedValue || !this.currentView || !this.privateHand)
            return false;
        const seat = this.localSeat;
        if (seat === null) return false;
        if (this.currentView.phase === 'bidding')
            return this.currentView.bid_turn_seat === seat;
        return this.currentView.phase === 'playing' &&
            this.currentView.turn_seat === seat;
    }
    get localSeat(): number | null { return this.seatValue; }
    private seatValue: number | null = null;

    bindRoom(roomId: string | null, playerId: string | null, seat: number | null): void {
        if (roomId !== this.roomIdValue || playerId !== this.playerIdValue) {
            this.reset();
            this.roomIdValue = roomId;
            this.playerIdValue = playerId;
        }
        this.seatValue = seat !== null && Number.isInteger(seat) &&
                         seat >= 0 && seat <= 2 ? seat : null;
    }

    /** Retain only the PUBLIC game ID for a future reconnect. Temporarily
     * clear the private hand, turn authority and selected physical cards. */
    suspend(): void {
        this.privateHand = null;
        this.awaitingHand = null;
        this.selectedCards.clear();
        this.hydratedValue = false;
    }

    reset(): void {
        this.currentView = null;
        this.privateHand = null;
        this.awaitingHand = null;
        this.lastAckValue = null;
        this.selectedCards.clear();
        this.hydratedValue = false;
        this.roomIdValue = null;
        this.playerIdValue = null;
        this.seatValue = null;
    }

    /** Server-sourced but never directly executable. Monotonic epoch/game/
     * bid-turn/game-turn ensures late or duplicate public fanout cannot roll
     * the UI backwards after a newer confirmed turn or recovered Owner. */
    applySnapshot(next: GameSnapshot): boolean {
        if (!this.roomIdValue || next.room_id !== this.roomIdValue) return false;
        // An already committed Game ACK is also a monotonic public floor.
        // An older unsolicited snapshot may arrive after that ACK but
        // before the fresh snapshot query, and cannot restore old cards.
        if (this.lastAckValue) {
            const ack = this.lastAckValue;
            if (BigInt(next.room_epoch) < BigInt(ack.room_epoch) ||
                (next.room_epoch === ack.room_epoch &&
                 BigInt(next.game_id) < BigInt(ack.game_id))) return false;
        }
        const current = this.currentView;
        if (current) {
            const nextEpoch = BigInt(next.room_epoch);
            const prevEpoch = BigInt(current.room_epoch);
            if (nextEpoch < prevEpoch) return false;
            if (nextEpoch === prevEpoch) {
                const nextGame = BigInt(next.game_id);
                const oldGame = BigInt(current.game_id);
                if (nextGame < oldGame) return false;
                if (nextGame === oldGame && this.hydratedValue) {
                    const rank = (x: GameSnapshot) =>
                        x.phase === 'bidding' ? 1 : x.phase === 'redeal_required' ?
                            2 : x.phase === 'playing' ? 3 : 4;
                    if (rank(next) < rank(current)) return false;
                    if (rank(next) === rank(current)) {
                        const n = 'turn_id' in next ?
                            next.turn_id : next.bid_turn_id;
                        const p = 'turn_id' in current ?
                            current.turn_id : current.bid_turn_id;
                        if (BigInt(n) <= BigInt(p)) return false;
                    }
                }
            }
        }
        // A new valid public state requires a matching new private member
        // hand. No stale selectable hand may leak across a turn or new game.
        this.currentView = next;
        this.privateHand = null;
        this.awaitingHand = null;
        this.selectedCards.clear();
        this.hydratedValue = true;
        return true;
    }

    noteAck(ack: GameAck): boolean {
        if (!this.roomIdValue || ack.room_id !== this.roomIdValue) return false;
        if (this.currentView) {
            const newerEpoch = BigInt(ack.room_epoch);
            const existingEpoch = BigInt(this.currentView.room_epoch);
            if (newerEpoch < existingEpoch) return false;
            if (newerEpoch === existingEpoch &&
                BigInt(ack.game_id) < BigInt(this.currentView.game_id))
                return false;
        }
        if (this.lastAckValue && BigInt(ack.seq) < BigInt(this.lastAckValue.seq))
            return false;
        this.lastAckValue = ack;
        return true;
    }

    expectHand(requestId: string, gameId: string): void {
        this.awaitingHand = {requestId, gameId};
    }

    applyPrivateHand(next: GameHand): boolean {
        if (!this.hydratedValue || !this.roomIdValue ||
            next.room_id !== this.roomIdValue ||
            next.game_id !== this.currentView?.game_id ||
            next.request_id !== this.awaitingHand?.requestId ||
            next.game_id !== this.awaitingHand?.gameId)
            return false;
        this.privateHand = next;
        this.awaitingHand = null;
        this.selectedCards.clear();
        return true;
    }

    toggle(card: number): boolean {
        if (!this.isMyTurn || this.currentView?.phase !== 'playing' ||
            !this.privateHand || !Number.isInteger(card) ||
            !this.hand.includes(card)) return false;
        if (this.selectedCards.has(card)) this.selectedCards.delete(card);
        else this.selectedCards.add(card);
        return true;
    }

    selectedMask(): string {
        return cardMaskFromIds(this.selected);
    }

    clearSelection(): void { this.selectedCards.clear(); }
}
