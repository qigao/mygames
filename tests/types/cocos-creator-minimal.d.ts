/** Test-only compile-shape shim for the public Cocos Creator 3.8
 * Graphics/Button/Label/Node APIs used by the M5e runtime scene.
 *
 * Not a replacement for the REAL Creator Editor type declarations, AssetDB,
 * scene import, font coverage, Web preview or native build qualification.
 * The shipped app never depends on this test file.
 */
declare module 'cc' {
    export const _decorator: {
        ccclass(name: string): ClassDecorator;
        property: any;
    };
    export class Color {
        constructor(r: number, g: number, b: number, a: number);
    }
    export class Vec3 {
        constructor(x: number, y: number, z: number);
    }
    export class Component {
        node: Node;
        protected onLoad(): void;
        protected onDestroy(): void;
    }
    export class Node {
        constructor(name?: string);
        layer: number;
        active: boolean;
        setParent(parent: Node): void;
        setPosition(position: Vec3): void;
        setScale(x: number, y: number, z: number): void;
        addComponent<T>(constructor: new (...args: any[]) => T): T;
        getComponent<T>(constructor: new (...args: any[]) => T): T | null;
        on(event: string, callback: (...args: any[]) => void, context?: object): void;
        destroy(): void;
    }
    export class UITransform {
        width: number;
        height: number;
        setContentSize(width: number, height: number): void;
    }
    export class Graphics {
        fillColor: Color;
        strokeColor: Color;
        lineWidth: number;
        roundRect(x: number, y: number, width: number, height: number, radius: number): void;
        fill(): void;
        stroke(): void;
    }
    export class Label {
        static HorizontalAlign: {CENTER: number};
        static VerticalAlign: {CENTER: number};
        static Overflow: {SHRINK: number};
        string: string;
        fontSize: number;
        lineHeight: number;
        color: Color;
        horizontalAlign: number;
        verticalAlign: number;
        overflow: number;
    }
    export class Button {
        static EventType: {CLICK: string};
        static Transition: {SCALE: number; NONE: number};
        transition: number;
        zoomScale: number;
        interactable: boolean;
    }
    export class EditBox {
        static InputMode: {SINGLE_LINE: number};
        textLabel: Label | null;
        placeholderLabel: Label | null;
        inputMode: number;
        maxLength: number;
        placeholder: string;
        string: string;
    }
    export const Layers: {Enum: {UI_2D: number}};
}
