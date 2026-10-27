import {cardsFromMask, cardMaskFromIds} from '../net/Protocol';
import type {GameAck, GameHand, GameSnapshot} from '../net/Protocol';

function phaseRank(view: GameSnapshot): number {
    return view.phase === 'bidding' ? 1 :
        view.phase === 'redeal_required' ? 2 :
        view.phase === 'playing' ? 3 : 4;
}

function publicTurn(view: GameSnapshot): bigint {
    return BigInt('turn_id' in view ? view.turn_id : view.bid_turn_id);
}

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
        // A newly confirmed game.start ACK can arrive while the displayed
        // snapshot still refers to the FINISHED previous game. Routing a
        // refresh using that old ID would query a stale generation instead
        // of hydrating the newly COMMITTED match.
        const viewId = this.currentView?.game_id;
        const ackId = this.lastAckValue?.game_id;
        if (!viewId) return ackId ?? null;
        if (!ackId) return viewId;
        return BigInt(ackId) > BigInt(viewId) ? ackId : viewId;
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

    /** Server-sourced public state with two separate monotonic fences:
     * owner epoch must never decrease, and *committed gameplay progress*
     * must not regress even if an Owner takes a newer epoch. In particular,
     * a recovered Owner cannot display an old unfinished version of an
     * already-ACKed FINISHED match or resurrect an older game ID. */
    applySnapshot(next: GameSnapshot): boolean {
        if (!this.roomIdValue || next.room_id !== this.roomIdValue) return false;
        const nextEpoch = BigInt(next.room_epoch);
        const nextGame = BigInt(next.game_id);
        const nextPhase = phaseRank(next);
        const nextTurn = publicTurn(next);

        // The most recent durable Game ACK is a progress floor regardless
        // of which Owner epoch delivers the subsequent public snapshot.
        if (this.lastAckValue) {
            const ack = this.lastAckValue;
            if (nextEpoch < BigInt(ack.room_epoch)) return false;
            const ackGame = BigInt(ack.game_id);
            if (nextGame < ackGame) return false;
            if (nextGame === ackGame &&
                (nextPhase < ack.phase ||
                 (nextPhase === ack.phase &&
                  nextTurn < BigInt(ack.turn_id)))) return false;
        }
        const current = this.currentView;
        if (current) {
            const oldEpoch = BigInt(current.room_epoch);
            if (nextEpoch < oldEpoch) return false;
            const oldGame = BigInt(current.game_id);
            if (nextGame < oldGame) return false;
            if (nextGame === oldGame) {
                const oldPhase = phaseRank(current);
                if (nextPhase < oldPhase ||
                    (nextPhase === oldPhase &&
                     (nextTurn < publicTurn(current) ||
                      (nextTurn === publicTurn(current) &&
                       nextEpoch === oldEpoch && this.hydratedValue))))
                    return false;
            }
        }
        // A newer Owner epoch with exactly the same committed game/turn is
        // still accepted to rehydrate a fresh session and private hand.
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
            const current = this.currentView;
            if (BigInt(ack.room_epoch) < BigInt(current.room_epoch)) return false;
            const ackGame = BigInt(ack.game_id);
            const currentGame = BigInt(current.game_id);
            if (ackGame < currentGame) return false;
            if (ackGame === currentGame &&
                (ack.phase < phaseRank(current) ||
                 (ack.phase === phaseRank(current) &&
                  BigInt(ack.turn_id) < publicTurn(current)))) return false;
        }
        // Duplicate Game ACK receipt is NOT a new UI transition. Replayed
        // receipts are legal at the CFlow Owner but must not trigger another
        // client refresh or overwrite a newer local progress floor.
        if (this.lastAckValue &&
            BigInt(ack.seq) <= BigInt(this.lastAckValue.seq)) return false;
        const newGameCommitted = this.currentView !== null &&
            BigInt(ack.game_id) > BigInt(this.currentView.game_id);
        this.lastAckValue = ack;
        if (newGameCommitted) {
            // The old FINISHED score and private hand must disappear as
            // soon as the next game's durable ACK is observed. Public
            // bidding and a fresh member-only hand are fetched afterwards.
            this.currentView = null;
            this.privateHand = null;
            this.awaitingHand = null;
            this.selectedCards.clear();
            this.hydratedValue = false;
        }
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
