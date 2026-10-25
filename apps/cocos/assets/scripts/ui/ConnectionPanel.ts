import {_decorator, Component, Label} from 'cc';
import {GameSocket} from '../net/GameSocket';

const {ccclass, property} = _decorator;

/** Attach this script to a Canvas node and assign statusLabel in Cocos Editor. */
@ccclass('ConnectionPanel')
export class ConnectionPanel extends Component {
    @property({type: Label})
    public statusLabel: Label | null = null;

    @property
    public serverUrl = 'ws://127.0.0.1:1780/ws';

    private connection: GameSocket | null = null;

    onLoad(): void {
        this.connection = new GameSocket(
            (state, reason) => {
                if (this.statusLabel) {
                    this.statusLabel.string = reason ? state + ': ' + reason : state;
                }
            },
            (frame) => {
                if (frame.type === 'server.pong' && this.statusLabel)
                    this.statusLabel.string = 'ready (pong received)';
                if (frame.type === 'server.error' && this.statusLabel)
                    this.statusLabel.string = 'server error: ' + frame.code;
            }
        );
    }

    /** Bind to a Cocos Button click event. No room commands are implemented yet. */
    public onConnectClick(): void {
        this.connection?.connect(this.serverUrl);
    }

    /** Bind to the second Cocos Button event. */
    public onPingClick(): void {
        try {
            this.connection?.ping();
        } catch {
            if (this.statusLabel) this.statusLabel.string = 'connect first';
        }
    }

    onDestroy(): void {
        this.connection?.disconnect();
        this.connection = null;
    }
}
