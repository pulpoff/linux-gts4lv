// Mutter raises the input panel only when a text-input-v3 client sends
// enable while its field is already focused (GTK re-enables on every tap);
// Chromium enables once per focus, so its fields never get the keyboard.
// Open it ourselves when a Chromium field reports its caret: it does on the
// enabling commit and on every tap that moves the caret.
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const BROWSER = /chrom/i;

export default class OskChromium extends Extension {
    enable() {
        this._seat = global.stage.context.get_backend().get_default_seat();
        this._idleId = 0;
        Main.inputMethod.connectObject(
            'cursor-location-changed', () => this._queue(), this);
    }

    disable() {
        Main.inputMethod.disconnectObject(this);
        if (this._idleId)
            GLib.source_remove(this._idleId);
        this._idleId = 0;
        this._seat = null;
    }

    // Deferred until the commit that carried the caret is fully applied.
    _queue() {
        if (this._idleId)
            return;
        this._idleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._idleId = 0;
            this._maybeOpen();
            return GLib.SOURCE_REMOVE;
        });
    }

    _maybeOpen() {
        if (!this._seat.touch_mode || !Main.inputMethod.currentFocus || Main.keyboard.visible)
            return;
        const win = global.display.focus_window;
        if (!win || !BROWSER.test(win.get_wm_class() ?? ''))
            return;
        Main.keyboard.open(Main.layoutManager.focusIndex);
    }
}
