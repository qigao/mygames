import {_decorator, Component, EditBox, Label} from 'cc';
import {GameSocket} from '../net/GameSocket';
import {RoomStore} from '../state/RoomStore';

const {ccclass, property} = _decorator;

/** Developer room console: bind fields and Buttons in Cocos Creator Editor. */
@ccclass('ConnectionPanel')
export class ConnectionPanel extends Component {
    @property({type: Label})
    public statusLabel: Label | null = null;

    @property({type: Label})
    public roomLabel: Label | null = null;

    @property({type: EditBox})
    public joinRoomInput: EditBox | null = null;

    @property
    public serverUrl = 'ws://127.0.0.1:1780/ws';

    private readonly rooms = new RoomStore();
    private connection: GameSocket | null = null;

    onLoad(): void {
        this.connection = new GameSocket(
            (state, reason) => {
                if (state === 'offline' || state === 'failed') {
                    this.rooms.reset(); // no session resume implemented in dev gateway
                    this.renderRoom();
                }
                if (this.statusLabel)
                    this.statusLabel.string = reason ? state + ': ' + reason : state;
            },
            (frame) => {
                switch (frame.type) {
                case 'server.pong':
                    if (this.statusLabel) this.statusLabel.string = 'ready (pong received)';
                    break;
                case 'server.error':
                case 'room.error':
                    if (this.statusLabel) this.statusLabel.string = 'server: ' + frame.code;
                    break;
                case 'room.ack':
                    this.rooms.onAck(frame);
                    this.renderRoom();
                    break;
                case 'room.snapshot':
                    if (this.rooms.applySnapshot(frame)) this.renderRoom();
                    break;
                default:
                    break;
                }
            }
        );
    }

    public onConnectClick(): void {
        this.connection?.connect(this.serverUrl);
    }
    public onPingClick(): void { this.run(() => this.connection?.ping()); }
    public onCreateRoomClick(): void { this.run(() => this.connection?.createRoom()); }
    public onJoinRoomClick(): void {
        this.run(() => this.connection?.joinRoom(this.joinRoomInput?.string.trim() ?? ''));
    }
    public onReadyClick(): void { this.run(() => this.connection?.setReady(true)); }
    public onUnreadyClick(): void { this.run(() => this.connection?.setReady(false)); }
    public onLeaveClick(): void { this.run(() => this.connection?.leaveRoom()); }
    public onRefreshClick(): void { this.run(() => this.connection?.requestSnapshot()); }

    private renderRoom(): void {
        if (!this.roomLabel) return;
        const room = this.rooms.current;
        if (!room) {
            this.roomLabel.string = this.rooms.roomId ?? 'No room';
            return;
        }
        const players = room.seats.filter((seat) => seat !== null).length;
        this.roomLabel.string =
            room.room_id + ' (' + players + '/3) rev=' + room.revision +
            ' owner=' + room.owner_id + ' ' + room.phase;
    }
    private run(action: () => unknown): void {
        try { action(); }
        catch (error) {
            if (this.statusLabel)
                this.statusLabel.string = error instanceof Error ? error.message : 'Command failed';
        }
    }

    onDestroy(): void {
        this.connection?.disconnect();
        this.connection = null;
    }
}
