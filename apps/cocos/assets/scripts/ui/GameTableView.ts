import {
    Button, Color, Graphics, Label, Layers, Node, UITransform, Vec3,
} from 'cc';
import {cardsFromMask} from '../net/Protocol';
import type {GameStore} from '../state/GameStore';

const C = {
    felt: new Color(13, 64, 51, 255),
    feltEdge: new Color(63, 119, 96, 255),
    card: new Color(241, 244, 230, 255),
    selected: new Color(253, 209, 115, 255),
    cardText: new Color(27, 39, 41, 255),
    red: new Color(175, 39, 54, 255),
    text: new Color(244, 246, 238, 255),
    muted: new Color(166, 194, 179, 255),
    gold: new Color(244, 183, 77, 255),
    button: new Color(45, 112, 94, 255),
    disabled: new Color(55, 78, 70, 255),
};

export interface GameTableActions {
    startGame(): void;
    bid(score: number): void;
    play(): void;
    pass(): void;
    refresh(): void;
    retryPending(): void;
    toggleCard(card: number): void;
}

interface TableButton {
    node: Node;
    component: Button;
    title: Label;
}

/** Cocos Creator 3.8 runtime scene, mounted in the existing Lobby Canvas.
 * No child scene file is generated; importing Lobby.scene in Creator creates
 * the actual playable hand/button nodes under the serialized bootstrap.
 *
 * This is PRESENTATION ONLY; every action still goes through GameSocket and
 * the authoritative C11 game legality + TurboDB receipt transaction.
 */
export class GameTableView {
    public readonly root: Node;
    private readonly header: Label;
    private readonly phase: Label;
    private readonly seats: Label;
    private readonly trick: Label;
    private readonly hint: Label;
    private readonly handCards: Array<{node: Node; face: Label; id: number}> = [];
    private readonly buttons: Record<string, TableButton> = {};
    private readonly actions: GameTableActions;

    constructor(parent: Node, actions: GameTableActions) {
        this.actions = actions;
        this.root = this.node(parent, 'DouDizhuGameTable', 238, -30, 776, 515);
        this.decorate(this.root, 776, 515, C.felt, C.feltEdge, 23);
        this.header = this.text(this.root, 'game-header', '斗地主 · 等待房主开始', 0, 210, 726, 43, 29, C.gold);
        this.phase = this.text(this.root, 'game-phase', '三人准备完毕后由房主开始游戏', 0, 157, 706, 35, 19, C.text);
        this.seats = this.text(this.root, 'game-seats', '等待完整的公共牌局快照', 0, 109, 706, 37, 17, C.text);
        this.trick = this.text(this.root, 'game-trick', '本轮尚无出牌', 0, 51, 720, 54, 18, C.muted);
        this.hint = this.text(this.root, 'game-hint', '只显示本玩家的物理手牌；服务端校验所有动作', 0, -146, 730, 31, 15, C.muted);

        const row1: Array<[string, string, number, () => void]> = [
            ['start','开始对局',-300,()=>actions.startGame()],
            ['bid0','不叫',-180,()=>actions.bid(0)],
            ['bid1','1 分',-60,()=>actions.bid(1)],
            ['bid2','2 分',60,()=>actions.bid(2)],
            ['bid3','3 分',180,()=>actions.bid(3)],
            ['refresh','刷新状态',300,()=>actions.refresh()],
        ];
        for (const [key,label,x,callback] of row1)
            this.buttons[key] = this.button(this.root, key, label, x, -185, 107, 38, callback);
        const row2: Array<[string,string,number,()=>void]> = [
            ['play','出牌',-225,()=>actions.play()],
            ['pass','过牌',-75,()=>actions.pass()],
            ['retry','重试未确认',75,()=>actions.retryPending()],
            ['hand','刷新手牌',225,()=>actions.refresh()],
        ];
        for (const [key,label,x,callback] of row2)
            this.buttons[key] = this.button(this.root, key, label, x, -230, 137, 38, callback);
        this.root.active = false;
    }

    setVisible(visible: boolean): void { this.root.active = visible; }

    /** Completely destroy the previous graphical selection before creating
     * a new private hand. Old opponent hands never exist in this client. */
    private rebuildCards(store: GameStore): void {
        for (const item of this.handCards) item.node.destroy();
        this.handCards.length = 0;
        const cards = store.hand;
        const width = Math.min(38, Math.floor(684 / Math.max(1, cards.length)));
        const step = Math.min(37, width);
        const offset = (cards.length - 1) * step / 2;
        for (let i = 0; i < cards.length; i++) {
            const id = cards[i];
            const card = this.node(this.root, 'self-card-' + id,
                                   i * step - offset, -63, width - 1, 89);
            const selected = store.selected.includes(id);
            this.decorate(card, width - 1, 89, selected ? C.selected : C.card,
                          selected ? C.gold : C.card, 6);
            const face = this.text(card, 'self-card-face-' + id, this.face(id),
                                   0, 0, width - 3, 80, 13, this.cardColor(id));
            const behavior = card.addComponent(Button);
            behavior.transition = Button.Transition.NONE;
            card.on(Button.EventType.CLICK, () => this.actions.toggleCard(id));
            if (selected) card.setPosition(new Vec3(i * step - offset, -39, 0));
            this.handCards.push({node: card, face, id});
        }
    }

    render(store: GameStore, canPlay: boolean, ownerSeat: boolean,
           pendingActions: number): void {
        const view = store.snapshot;
        const phase = view?.phase;
        // Explicit tagged-union narrowing is required under Creator's
        // strict TS compiler: bidding snapshots have NO private/playing
        // remaining-card fields and may not be cast into a playing view.
        const bidView = view?.phase === 'bidding' ? view : null;
        const playView = view &&
            (view.phase === 'playing' || view.phase === 'finished') ? view : null;
        const mySeat = store.localSeat;
        const hasGame = view !== null;
        const finished = playView?.phase === 'finished' ? playView : null;
        const landlordWon = finished ?
            finished.winner_seat === finished.landlord_seat : false;
        const teamWon = finished && mySeat !== null ?
            (mySeat === finished.landlord_seat ? landlordWon : !landlordWon) : false;
        this.header.string = !view ? '斗地主 · 等待发牌' :
            '斗地主 · Game ' + view.game_id +
            (finished ? ' · ' + (landlordWon ? '地主获胜' : '农民获胜') : '');
        this.phase.string = !canPlay ? '本地网关未开放持久化游戏能力' :
            !view ? '房主可开始新局' :
            phase === 'bidding' ?
                '叫分轮次 ' + view.bid_turn_id +
                ' · 轮到座位 ' + (view.bid_turn_seat + 1) +
                ' · 当前 ' + view.highest_bid + ' 分' :
            phase === 'redeal_required' ? '全部不叫 · 房主重新开局' :
            phase === 'playing' ?
                '出牌轮次 ' + view.turn_id + ' · 轮到座位 ' + (view.turn_seat + 1) :
                finished ? '对局结束 · ' + (teamWon ? '你的队伍获胜' : '本队惜败') +
                           ' · 座位 ' + (finished.winner_seat + 1) +
                           ' 率先出完（尚未计分）' :
                           '等待开始新局';
        this.seats.string = !view ? '请等待其他玩家加入并准备' :
            !playView ?
                '座位 ' + (mySeat === null ? '?' : mySeat + 1) +
                ' · 叫分由服务端依次准入' :
                '剩余牌：座位 1 · ' + playView.remaining[0] +
                '     座位 2 · ' + playView.remaining[1] +
                '     座位 3 · ' + playView.remaining[2];
        if (view && (phase === 'playing' || phase === 'finished')) {
            const revealed = cardsFromMask(view.last_cards_hex);
            const kitty = cardsFromMask(view.revealed_kitty_hex);
            this.trick.string =
                '地主座位 ' + (view.landlord_seat + 1) +
                ' · 底牌 ' + kitty.map(c=>this.face(c)).join(' ') +
                '\n当前牌面 ' + (revealed.length ?
                    revealed.map(c=>this.face(c)).join(' ') : '等待领出');
        } else this.trick.string = '地主确定前不能看底牌、牌序或其他玩家手牌';

        this.rebuildCards(store);
        // Do not permit a second mutable game action while a previous
        // durable COMMIT/ACK is unresolved. Retry uses the ORIGINAL ID.
        const mutable = canPlay && pendingActions === 0;
        const bidding = phase === 'bidding' && store.isMyTurn;
        const playing = phase === 'playing' && store.isMyTurn;
        this.hint.string = !store.hydrated ? '重新连接后需同步公共状态与本人手牌' :
            finished ? '本局已结束 · 房主可以开始下一局（积分尚未实现）' :
            !hasGame ? '只有房主可以开始游戏，服务器决定牌局 ID' :
            store.hand.length === 0 ?
                '正在获取本玩家手牌 / 等待私有同步' :
            store.selected.length > 0 ?
                '已选择 ' + store.selected.length + ' 张牌 · 服务端会再次校验' :
                '点击本人手牌选中，再按出牌；不显示其他玩家的卡';
        this.enable('start', mutable && ownerSeat &&
            (!view || phase === 'redeal_required' || phase === 'finished'));
        this.enable('bid0', mutable && bidding);
        this.enable('bid1', mutable && bidding &&
            bidView !== null && bidView.highest_bid < 1);
        this.enable('bid2', mutable && bidding &&
            bidView !== null && bidView.highest_bid < 2);
        this.enable('bid3', mutable && bidding &&
            bidView !== null && bidView.highest_bid < 3);
        this.enable('play', mutable && playing && store.selected.length > 0);
        this.enable('pass', mutable && playing && playView !== null &&
            playView.phase === 'playing' &&
            playView.last_cards_hex !== '00000000000000');
        this.enable('retry', canPlay && pendingActions > 0);
        this.enable('refresh', canPlay && !!store.gameId);
        this.enable('hand', canPlay && !!store.gameId);
    }

    destroy(): void { this.root.destroy(); }

    private face(id: number): string {
        if (id === 52) return '小王';
        if (id === 53) return '大王';
        const ranks = ['3','4','5','6','7','8','9','10','J','Q','K','A','2'];
        const suits = ['♣','♦','♥','♠'];
        return ranks[Math.floor(id/4)] + suits[id%4];
    }
    private cardColor(id: number): Color {
        return id >= 52 || id % 4 === 1 || id % 4 === 2 ? C.red : C.cardText;
    }
    private node(parent: Node, name: string, x: number, y: number,
                 width: number, height: number): Node {
        const value = new Node(name);
        value.layer = Layers.Enum.UI_2D;
        value.setParent(parent);
        value.setPosition(new Vec3(x, y, 0));
        value.addComponent(UITransform).setContentSize(width, height);
        return value;
    }
    private decorate(node: Node, width: number, height: number,
                     fill: Color, border: Color, radius: number): void {
        const g = node.addComponent(Graphics);
        g.fillColor = fill;
        g.roundRect(-width/2, -height/2, width, height, radius);
        g.fill();
        g.lineWidth = 1.2;
        g.strokeColor = border;
        g.roundRect(-width/2, -height/2, width, height, radius);
        g.stroke();
    }
    private text(parent: Node, name: string, content: string,
                 x: number, y: number, width: number, height: number,
                 fontSize: number, color: Color): Label {
        const node = this.node(parent, name, x, y, width, height);
        const label = node.addComponent(Label);
        label.string = content;
        label.fontSize = fontSize;
        label.lineHeight = fontSize + 5;
        label.color = color;
        label.horizontalAlign = Label.HorizontalAlign.CENTER;
        label.verticalAlign = Label.VerticalAlign.CENTER;
        label.overflow = Label.Overflow.SHRINK;
        return label;
    }
    private button(parent: Node, name: string, title: string,
                   x: number, y: number, width: number, height: number,
                   callback: () => void): TableButton {
        const node = this.node(parent, 'game-' + name, x, y, width, height);
        this.decorate(node, width, height, C.button, C.feltEdge, 8);
        const component = node.addComponent(Button);
        component.transition = Button.Transition.SCALE;
        component.zoomScale = 0.97;
        node.on(Button.EventType.CLICK, callback);
        const label = this.text(node, 'game-' + name + '-label',
                                title, 0, 0, width - 7, height - 5, 16, C.text);
        return {node, component, title: label};
    }
    private enable(name: string, available: boolean): void {
        const button = this.buttons[name];
        button.component.interactable = available;
        button.title.color = available ? C.text : C.muted;
    }
}
