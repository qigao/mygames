import {
    _decorator, Button, Color, Component, EditBox, Graphics, Label,
    Layers, Node, UITransform, Vec3,
} from 'cc';
import {GameSocket} from '../net/GameSocket';
import {RoomStore} from '../state/RoomStore';
import {GameStore} from '../state/GameStore';
import {GameTableView} from './GameTableView';
import type {ConnectionState} from '../net/GameSocket';
import type {RoomSnapshot, ServerFrame} from '../net/Protocol';

const {ccclass, property} = _decorator;
const DESIGN_WIDTH = 1280;
const DESIGN_HEIGHT = 720;

const COLORS = {
    background: new Color(12, 26, 31, 255),
    card: new Color(22, 42, 47, 255),
    border: new Color(59, 88, 89, 255),
    felt: new Color(22, 77, 63, 255),
    seat: new Color(16, 63, 54, 255),
    input: new Color(16, 33, 39, 255),
    primary: new Color(243, 188, 91, 255),
    secondary: new Color(46, 109, 98, 255),
    muted: new Color(126, 160, 156, 255),
    white: new Color(245, 249, 244, 255),
    dark: new Color(25, 33, 32, 255),
};

@ccclass('LobbyBootstrap')
export class LobbyBootstrap extends Component {
    @property
    public serverUrl = 'ws://127.0.0.1:1780/ws';

    @property
    public autoConnect = true;

    private readonly rooms = new RoomStore();
    private readonly games = new GameStore();
    private gameTable: GameTableView | null = null;
    private socket: GameSocket | null = null;
    private playerId: string | null = null;

    private statusText: Label | null = null;
    private identityText: Label | null = null;
    private roomTitle: Label | null = null;
    private roomSubtitle: Label | null = null;
    private roomSeats: Label[] = [];
    private joinBox: EditBox | null = null;

    protected onLoad(): void {
        this.createLobbyUI();
        this.socket = new GameSocket(
            (state, reason) => this.onConnectionState(state, reason),
            (frame) => this.onServerFrame(frame)
        );
        if (this.autoConnect) this.socket.connect(this.serverUrl);
    }

    protected onDestroy(): void {
        this.socket?.disconnect();
        this.socket = null;
        this.games.reset();
        this.gameTable?.destroy();
        this.gameTable = null;
    }

    private onConnectionState(state: ConnectionState, reason?: string): void {
        if (state === 'offline' || state === 'failed') {
            this.games.suspend(); // clear private cards, not the durable game ID
            if (!this.socket?.canResume) {
                this.rooms.reset();
                this.games.reset();
                this.playerId = null;
            }
            this.renderRoom();
        }
        if (this.statusText) {
            const title = state === 'ready' ? '已连接 · 可创建房间' :
                          state === 'restoring' ? '正在恢复原有房间身份…' :
                          state === 'handshaking' ? '连接建立，等待协议握手…' :
                          state === 'connecting' ? '正在连接本地服务器…' :
                          state === 'failed' ? '连接失败' : '尚未连接';
            this.statusText.string = reason ? title + '：' + reason : title;
        }
    }

    private onServerFrame(frame: ServerFrame): void {
        switch (frame.type) {
        case 'server.hello':
        case 'session.ready':
            this.playerId = frame.player_id ?? null;
            if (this.identityText)
                this.identityText.string = '开发会话 ID：' + (this.playerId ?? '未知');
            this.bindGameContext();
            this.renderRoom();
            if (frame.type === 'session.ready' && this.games.gameId)
                this.requestGameSnapshot();
            break;
        case 'session.error':
            this.rooms.reset();
            this.games.reset();
            this.renderRoom();
            this.report('原房间恢复失败，已创建新的临时会话。');
            break;
        case 'room.ack':
            this.rooms.onAck(frame);
            if (frame.seat !== 3 && this.joinBox)
                this.joinBox.string = frame.room_id;
            this.bindGameContext();
            this.renderRoom();
            break;
        case 'room.snapshot':
            if (this.rooms.applySnapshot(frame)) {
                this.bindGameContext();
                this.renderRoom();
            }
            break;
        case 'game.ack':
            if (this.games.noteAck(frame)) {
                // The ACK is confirmed durable, but it contains no private
                // hand; refresh the actual public state before requesting it.
                this.games.clearSelection();
                this.report('已提交 ' + frame.action + ' · 序号 ' + frame.seq);
                this.requestGameSnapshot();
            }
            break;
        case 'game.snapshot':
            this.bindGameContext();
            if (this.games.applySnapshot(frame)) {
                this.renderRoom();
                // Request only this authenticated session's private cards.
                if (frame.phase === 'finished')
                    this.requestGameHistory();
                if (frame.phase === 'bidding' ||
                    frame.phase === 'playing' || frame.phase === 'finished') {
                    this.execute((socket) => {
                        const id = socket.requestGameHand(frame.game_id);
                        this.games.expectHand(id, frame.game_id);
                    });
                }
            }
            break;
        case 'game.hand':
            if (this.games.applyPrivateHand(frame)) this.renderRoom();
            break;
        case 'game.result':
            if (this.games.applyHistoricalResult(frame)) this.renderRoom();
            break;
        case 'game.standings':
            if (this.games.applyStandings(frame)) this.renderRoom();
            break;
        case 'game.error':
            this.report('游戏操作拒绝：' + frame.code);
            this.renderRoom();
            break;
        case 'room.error':
        case 'server.error':
            this.report('服务器拒绝：' + frame.code);
            break;
        case 'server.pong':
            this.report('连接正常 · Pong');
            break;
        }
    }

    private report(message: string): void {
        if (this.statusText) this.statusText.string = message;
    }

    private execute(action: (socket: GameSocket) => unknown): void {
        try {
            if (!this.socket) throw new Error('连接尚未建立');
            action(this.socket);
        } catch (error) {
            this.report(error instanceof Error ? error.message : '操作失败');
        }
    }

    private createNode(parent: Node, name: string, x: number, y: number,
                       width: number, height: number): Node {
        const node = new Node(name);
        node.layer = Layers.Enum.UI_2D;
        node.setParent(parent);
        node.setPosition(new Vec3(x, y, 0));
        const transform = node.addComponent(UITransform);
        transform.setContentSize(width, height);
        return node;
    }

    private panel(parent: Node, name: string, x: number, y: number,
                  width: number, height: number,
                  fill: Color, border: Color, radius = 16): Node {
        const node = this.createNode(parent, name, x, y, width, height);
        const graphic = node.addComponent(Graphics);
        graphic.fillColor = fill;
        graphic.roundRect(-width / 2, -height / 2, width, height, radius);
        graphic.fill();
        graphic.lineWidth = 2;
        graphic.strokeColor = border;
        graphic.roundRect(-width / 2, -height / 2, width, height, radius);
        graphic.stroke();
        return node;
    }

    private text(parent: Node, name: string, content: string,
                 x: number, y: number, width: number, height: number,
                 size: number, color = COLORS.white): Label {
        const node = this.createNode(parent, name, x, y, width, height);
        const label = node.addComponent(Label);
        label.string = content;
        label.fontSize = size;
        label.lineHeight = size + 5;
        label.color = color;
        label.horizontalAlign = Label.HorizontalAlign.CENTER;
        label.verticalAlign = Label.VerticalAlign.CENTER;
        label.overflow = Label.Overflow.SHRINK;
        return label;
    }

    private button(parent: Node, name: string, title: string,
                   x: number, y: number, width: number, height: number,
                   action: () => void, primary = false): void {
        const bg = primary ? COLORS.primary : COLORS.secondary;
        const foreground = primary ? COLORS.dark : COLORS.white;
        const node = this.panel(parent, name, x, y, width, height, bg, bg, 10);
        const behavior = node.addComponent(Button);
        behavior.transition = Button.Transition.SCALE;
        behavior.zoomScale = 0.96;
        this.text(node, name + '-label', title, 0, 0, width - 12, height - 8, 21, foreground);
        node.on(Button.EventType.CLICK, action, this);
    }

    private createInput(parent: Node, x: number, y: number): EditBox {
        const node = this.panel(parent, 'room-code', x, y, 324, 46,
                                COLORS.input, COLORS.border, 10);
        const value = this.text(node, 'input-value', '', 0, 0, 300, 39, 20);
        const placeholder = this.text(node, 'input-placeholder', '输入房间号，例如 room-1',
                                      0, 0, 300, 39, 17, COLORS.muted);
        const box = node.addComponent(EditBox);
        box.textLabel = value;
        box.placeholderLabel = placeholder;
        box.inputMode = EditBox.InputMode.SINGLE_LINE;
        box.maxLength = 39;
        box.placeholder = '输入房间号，例如 room-1';
        return box;
    }

    private createLobbyUI(): void {
        const canvas = this.node.getComponent(UITransform);
        const width = canvas?.width ?? DESIGN_WIDTH;
        const height = canvas?.height ?? DESIGN_HEIGHT;
        const scale = Math.min(width / DESIGN_WIDTH, height / DESIGN_HEIGHT);
        const root = this.createNode(this.node, 'LobbyUI', 0, 0, DESIGN_WIDTH, DESIGN_HEIGHT);
        root.setScale(scale, scale, 1);

        this.panel(root, 'background', 0, 0, DESIGN_WIDTH, DESIGN_HEIGHT,
                   COLORS.background, COLORS.background, 0);
        this.text(root, 'brand', 'MYGAMES  /  斗地主', -409, 302, 425, 59,
                  42, COLORS.primary);
        this.text(root, 'subtitle', '原生 C11 房间 · CHttp/CNet 实时连接', -386, 255,
                  455, 32, 18, COLORS.muted);

        this.panel(root, 'lobby-card', -406, -30, 410, 515,
                   COLORS.card, COLORS.border);
        this.text(root, 'section', '大厅 · 创建或加入', -406, 186, 355, 43,
                  29, COLORS.white);
        this.identityText = this.text(root, 'identity', '开发会话 ID：未连接',
                                      -406, 143, 354, 27, 18, COLORS.muted);
        this.button(root, 'connect', '连接服务器', -406, 96, 325, 46,
                    () => this.socket?.connect(this.serverUrl), true);
        this.button(root, 'create', '创建房间', -406, 38, 325, 46,
                    () => this.execute((s) => s.createRoom()));
        this.joinBox = this.createInput(root, -406, -19);
        this.button(root, 'join', '加入房间', -406, -77, 325, 46,
                    () => this.execute((s) => s.joinRoom(this.joinBox?.string.trim() ?? '')));
        this.button(root, 'ready', '准备', -494, -135, 148, 43,
                    () => this.execute((s) => s.setReady(true)), true);
        this.button(root, 'unready', '取消准备', -318, -135, 148, 43,
                    () => this.execute((s) => s.setReady(false)));
        this.button(root, 'refresh', '刷新', -494, -193, 148, 43,
                    () => this.execute((s) => s.requestSnapshot()));
        this.button(root, 'leave', '退出房间', -318, -193, 148, 43,
                    () => this.execute((s) => s.leaveRoom()));
        this.button(root, 'suspend', '模拟断线 · 保留身份', -406, -246, 325, 41,
                    () => this.socket?.suspend());
        this.button(root, 'room-start', '三人已准备 · 房主启动', -406, -290, 325, 34,
                    () => this.execute(s => s.startRoom()));

        this.panel(root, 'table', 238, -31, 780, 515, COLORS.felt, COLORS.border, 24);
        this.roomTitle = this.text(root, 'room-title', '尚未进入房间', 238, 179, 690,
                                   52, 37, COLORS.primary);
        this.roomSubtitle = this.text(root, 'room-subtitle', '创建一个房间，或输入房间号加入',
                                      238, 133, 698, 36, 20, COLORS.white);

        const positions = [
            {x: 27, y: 22}, {x: 444, y: 22}, {x: 235, y: -143},
        ];
        for (let i = 0; i < positions.length; i++) {
            const position = positions[i];
            this.panel(root, 'seat-' + i, position.x, position.y, 205, 117,
                       COLORS.seat, COLORS.border);
            this.roomSeats.push(this.text(root, 'seat-label-' + i,
                '座位 ' + (i + 1) + '\n等待玩家加入',
                position.x, position.y, 186, 103, 20, COLORS.white));
        }
        this.text(root, 'phase-caption', '房主开始房间后，切换到对局牌桌。',
                  238, -248, 700, 29, 17, COLORS.muted);
        this.gameTable = new GameTableView(root, {
            startGame: () => this.execute(s => s.startGame()),
            bid: score => this.placeBid(score),
            play: () => this.playCards(),
            pass: () => this.passTurn(),
            refresh: () => this.requestGameSnapshot(),
            history: () => this.requestGameHistory(),
            retryPending: () => this.execute(s => s.retryPendingGameActions()),
            toggleCard: card => {
                if (this.games.toggle(card)) this.renderGame();
            },
        });
        this.statusText = this.text(root, 'status', '尚未连接', 0, -328, 1160,
                                    38, 19, COLORS.muted);
        this.renderRoom();
    }

    /** A room identity change invalidates every private card. Never infer
     * a seat from a client claimed value; use the latest server room view. */
    private bindGameContext(): void {
        const snapshot = this.rooms.current;
        const seat = snapshot && this.playerId ?
            snapshot.seats.findIndex(s => s?.player_id === this.playerId) : -1;
        this.games.bindRoom(this.rooms.roomId, this.playerId,
                            seat >= 0 ? seat : null);
    }

    private requestGameSnapshot(): void {
        const gameId = this.games.gameId;
        if (!gameId) { this.renderGame(); return; }
        this.execute(socket => {
            this.games.suspend();
            socket.requestGameSnapshot(gameId);
            this.renderGame();
        });
    }

    /** Results are fetched only after current CFlow session identity is
     * reattached to this exact Room; no direct ORM or stale hand read.
     * A previous finished game's immutable score is still accessible after
     * starting a different rematch game ID. */
    private requestGameHistory(): void {
        if (!this.rooms.roomId || !this.playerId || !this.socket?.canPlayGames)
            return;
        this.execute(socket => {
            const id = socket.requestGameStandings();
            this.games.expectStandings(id);
            const finished = this.games.lastFinishedGameId;
            if (finished) {
                const resultId = socket.requestGameResult(finished);
                this.games.expectHistory(resultId, finished);
            }
        });
    }

    private placeBid(score: number): void {
        const view = this.games.snapshot;
        if (!view || view.phase !== 'bidding' || !this.games.isMyTurn)
            return;
        this.execute(socket => socket.bid(view.game_id, view.bid_turn_id, score));
    }
    private playCards(): void {
        const view = this.games.snapshot;
        if (!view || view.phase !== 'playing' || !this.games.isMyTurn)
            return;
        this.execute(socket => socket.play(view.game_id, view.turn_id,
                                            this.games.selectedMask()));
    }
    private passTurn(): void {
        const view = this.games.snapshot;
        if (!view || view.phase !== 'playing' || !this.games.isMyTurn ||
            view.last_cards_hex === '00000000000000')
            return;
        this.execute(socket => socket.pass(view.game_id, view.turn_id));
    }

    private renderGame(): void {
        if (!this.gameTable) return;
        const room = this.rooms.current;
        const playable = room?.phase === 'starting';
        this.gameTable.setVisible(playable === true);
        if (playable) this.gameTable.render(this.games, !!this.socket?.canPlayGames,
                             room.owner_id === this.playerId,
                             this.socket?.pendingGameCount ?? 0);
    }

    private renderRoom(): void {
        const room: RoomSnapshot | null = this.rooms.current;
        if (this.roomTitle)
            this.roomTitle.string = room?.room_id ?? this.rooms.roomId ?? '尚未进入房间';
        if (this.roomSubtitle) {
            if (!room) {
                this.roomSubtitle.string = this.rooms.roomId
                    ? '正在同步房间状态…'
                    : '创建一个房间，或输入房间号加入';
            } else {
                const count = room.seats.filter((seat) => seat !== null).length;
                this.roomSubtitle.string = count + '/3 玩家 · 修订 #' + room.revision +
                    ' · ' + (room.phase === 'waiting' ? '等待准备' : '正在启动');
            }
        }
        for (let i = 0; i < this.roomSeats.length; i++) {
            const seat = room?.seats[i] ?? null;
            this.roomSeats[i].string = seat
                ? '座位 ' + (i + 1) + '\n' + (seat.player_id === this.playerId ? '自己' : '玩家 ' + seat.player_id) +
                  (seat.player_id === room?.owner_id ? ' · 房主' : '') +
                  '\n' + (seat.ready ? '● 已准备' : '○ 未准备')
                : '座位 ' + (i + 1) + '\n等待玩家加入';
        }
        this.renderGame();
    }
}
