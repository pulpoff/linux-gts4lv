// The lock type of the lock screen and the login screen: a pattern, the password, or a swipe.
//
// Pattern, as on Android (and as Neo's Phosh patch 0106 does it): a 3x3 dot grid stands in for the password
// entry whenever the prompt asks for a secret, and the dots joined in order ("14789") go to PAM as the
// answer. The pattern is its own secret, not the password: gts4lv-pattern-auth (pam_exec, ahead of pam_unix
// in gdm-password) checks it against a salted hash, and the password still works after it. A dot passed over
// on the way joins too (1 to 3 passes 2). Fewer than 4 dots, or a wrong pattern, turn red for a second.
// Typing on a keyboard, or "Use password", brings the entry back.
// Swipe: swiping up on the lock screen's clock unlocks without a password (logging in is GDM's automatic
// login then, set by gts4lv-lock-set).
// The type is /var/lib/gts4lv-lock/<user>.type, written by gts4lv-lock-set (pkexec, from the preferences),
// readable by both the user's shell and GDM's. One hook on AuthPrompt covers both screens; GDM builds its
// login prompt before extensions load, so a prompt that already exists is fitted too.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as AuthPrompt from 'resource:///org/gnome/shell/gdm/authPrompt.js';
import * as UnlockDialog from 'resource:///org/gnome/shell/ui/unlockDialog.js';

const BLUE = [0.21, 0.52, 0.89];
const RED = [0.88, 0.11, 0.14];
const SIZE = 230;           // the grid's side, in logical pixels
const MIN_DOTS = 4;

/** pattern | password | swipe, for a user; "pattern-unset" (no pattern drawn yet) counts as password. */
function lockType(user) {
    if (!user)
        return 'password';
    try {
        const [, bytes] = Gio.File.new_for_path(`/var/lib/gts4lv-lock/${user}.type`).load_contents(null);
        const type = new TextDecoder().decode(bytes).trim();
        return ['pattern', 'swipe'].includes(type) ? type : 'password';
    } catch {
        return 'password';
    }
}

// The row the grid stands in for. GNOME 51 keeps the password entry and its
// submit button together in _entryArea; hiding only the entry left the button.
// The whole row goes (_mainBox, with GDM's back button): the back button moves
// up to the name line, and the row's height goes to the grid.
function inputRow(prompt) {
    return prompt._mainBox ?? prompt._entryArea ?? prompt._passwordEntry;
}

const PatternGrid = GObject.registerClass(
class PatternGrid extends St.BoxLayout {
    _init(prompt) {
        super._init({orientation: Clutter.Orientation.VERTICAL, x_align: Clutter.ActorAlign.CENTER});
        this._prompt = prompt;
        this._unlock = prompt._mode === AuthPrompt.AuthPromptMode.UNLOCK_ONLY;
        // The lock screen is the session's own user; the login screen learns it from begin().
        this.user = this._unlock ? GLib.get_user_name() : null;
        const scale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
        this._area = new St.DrawingArea({reactive: true, can_focus: true, width: SIZE * scale, height: SIZE * scale});
        this._area.connect('repaint', () => this._repaint());
        this._label = new St.Label({text: 'Draw your pattern', x_align: Clutter.ActorAlign.CENTER,
            style: 'font-size: 11pt; padding-top: 12px;'});
        this._passwordButton = new St.Button({label: 'Use password', style_class: 'button',
            x_align: Clutter.ActorAlign.CENTER, style: 'margin-top: 12px; margin-bottom: 24px; padding: 4px 16px;'});
        this._passwordButton.connect('clicked', () => this.entryMode(true));
        this.add_child(this._area);
        this.add_child(this._label);
        this.add_child(this._passwordButton);
        this._addBackButton();

        this._seq = [];
        this._active = false;
        this._error = false;
        this._pos = [0, 0];
        this._clearId = 0;

        for (const sig of ['button-press-event', 'motion-event', 'button-release-event', 'touch-event'])
            this._area.connect(sig, (a, ev) => this._event(ev));
        this._area.connect('key-press-event', (a, ev) => this._key(ev));

        prompt._userVerifier.connectObject('verification-failed', () => {
            if (this.visible && this._seq.length)
                this._fail('Wrong pattern');
        }, this);
        // The lock screen only ever asks for the password: the grid is up from the start. The login screen
        // asks for a user first, so the grid waits for a secret question.
        prompt.connectObject(
            'reset', () => (this._unlock ? this.patternMode() : this.entryMode(false)),
            'prompted', () => {
                if (prompt._entry === prompt._passwordEntry)
                    this.patternMode();
                else
                    this.entryMode(false);
            }, this);
        this.connect('destroy', () => {
            if (this._clearId)
                GLib.source_remove(this._clearId);
        });
    }

    // GDM's back button ("<"), on the name line at the left while the grid is up: it shows and works
    // whenever the prompt's own one does. _userWell is wrapped in an overlay for it, not given it: the
    // user widget inside is replaced on every begin().
    _addBackButton() {
        const prompt = this._prompt, well = prompt._userWell, cancel = prompt.cancelButton;
        if (!well || !cancel)
            return;
        this._back = new St.Button({style_class: 'login-dialog-button cancel-button', accessible_name: 'Back',
            icon_name: 'go-previous-symbolic', can_focus: true,
            x_expand: true, y_expand: true, x_align: Clutter.ActorAlign.START, y_align: Clutter.ActorAlign.END});
        this._back.connect('clicked', () => prompt._handleCancel());
        cancel.bind_property('opacity', this._back, 'opacity', GObject.BindingFlags.SYNC_CREATE);
        cancel.bind_property('reactive', this._back, 'reactive', GObject.BindingFlags.SYNC_CREATE);
        this._top = new St.Widget({layout_manager: new Clutter.BinLayout(), x_expand: true,
            y_expand: well.y_expand});
        prompt.replace_child(well, this._top);
        this._top.add_child(well);
        this._top.add_child(this._back);
        this._back.hide();
    }

    _unwrap() {
        if (!this._top)
            return;
        const well = this._prompt._userWell;
        this._top.remove_child(well);
        this._prompt.replace_child(this._top, well);
        this._top.destroy();
        this._top = null;
    }

    patternMode() {
        if (lockType(this.user) !== 'pattern') {
            this.entryMode(false);
            return;
        }
        this.show();
        this._back?.show();
        inputRow(this._prompt).hide();
        this._reset();
        this._label.text = 'Draw your pattern';
        this._focus();
    }

    entryMode(focus) {
        this.hide();
        this._back?.hide();
        inputRow(this._prompt).show();
        if (focus)
            this._prompt._entry.grab_key_focus();
    }

    // Keyboard focus on the grid, not the entry: a focused entry opens the on-screen keyboard over the grid.
    _focus() {
        if (this.visible)
            GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                if (this.visible && this._area.mapped)
                    this._area.grab_key_focus();
                return GLib.SOURCE_REMOVE;
            });
    }

    // A typed character switches to the password entry, with the character in it.
    _key(ev) {
        const ch = ev.get_key_unicode();
        if (!ch || ch < ' ' || ch === '\x7f')
            return Clutter.EVENT_PROPAGATE;
        this.entryMode(true);
        const entry = this._prompt._entry;
        entry.text += ch;
        entry.clutter_text.set_cursor_position(-1);
        return Clutter.EVENT_STOP;
    }

    // The allocation, not get_surface_size(): GNOME 51 answers that only inside a repaint, and a touch
    // measured against nothing hit no dot.
    _geometry() {
        const [w, h] = this._area.get_size();
        const cell = Math.min(w, h) / 3;
        return [(w - cell * 3) / 2, (h - cell * 3) / 2, cell];
    }

    _center(dot) {
        const [x0, y0, cell] = this._geometry();
        return [x0 + cell * (dot % 3) + cell / 2, y0 + cell * Math.floor(dot / 3) + cell / 2];
    }

    _hit(x, y) {
        const cell = this._geometry()[2];
        for (let dot = 0; dot < 9; dot++) {
            const [cx, cy] = this._center(dot);
            if (Math.hypot(x - cx, y - cy) > cell * 0.3 || this._seq.includes(dot))
                continue;
            const last = this._seq.at(-1);
            if (last !== undefined) {
                const dr = Math.floor(dot / 3) - Math.floor(last / 3), dc = dot % 3 - last % 3;
                const mid = last + (dr / 2) * 3 + dc / 2;
                if (dr % 2 === 0 && dc % 2 === 0 && !this._seq.includes(mid))
                    this._seq.push(mid);
            }
            this._seq.push(dot);
            return;
        }
    }

    _event(ev) {
        const type = ev.type();
        const begin = type === Clutter.EventType.BUTTON_PRESS || type === Clutter.EventType.TOUCH_BEGIN;
        const move = type === Clutter.EventType.MOTION || type === Clutter.EventType.TOUCH_UPDATE;
        const end = type === Clutter.EventType.BUTTON_RELEASE || type === Clutter.EventType.TOUCH_END ||
            type === Clutter.EventType.TOUCH_CANCEL;
        if (!begin && !move && !end)
            return Clutter.EVENT_PROPAGATE;
        if (!begin && !this._active)
            return move ? Clutter.EVENT_PROPAGATE : Clutter.EVENT_STOP;

        const [sx, sy] = ev.get_coords();
        const [, x, y] = this._area.transform_stage_point(sx, sy);
        this._pos = [x, y];
        if (begin) {
            this._reset();
            this._active = true;
            this._label.text = 'Release when done';
        }
        this._hit(x, y);
        if (end) {
            this._active = false;
            this._finish();
        }
        this._area.queue_repaint();
        return Clutter.EVENT_STOP;
    }

    _finish() {
        if (this._seq.length === 0) {
            this._label.text = 'Draw your pattern';
            return;
        }
        if (this._seq.length < MIN_DOTS) {
            this._fail(`Connect at least ${MIN_DOTS} dots`);
            return;
        }
        this._label.text = 'Checking…';
        this._prompt._entry.text = this._seq.map(d => d + 1).join('');
        this._prompt._activateNext(true);
    }

    _fail(text) {
        this._error = true;
        this._active = false;
        this._label.text = text;
        this._area.queue_repaint();
        if (this._clearId)
            GLib.source_remove(this._clearId);
        this._clearId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._clearId = 0;
            this._reset();
            this._label.text = 'Draw your pattern';
            return GLib.SOURCE_REMOVE;
        });
    }

    _reset() {
        if (this._clearId) {
            GLib.source_remove(this._clearId);
            this._clearId = 0;
        }
        this._seq = [];
        this._active = false;
        this._error = false;
        this._area.queue_repaint();
    }

    _repaint() {
        const cr = this._area.get_context();
        const cell = this._geometry()[2];
        const [r, g, b] = this._error ? RED : BLUE;
        // the line: blue while drawing, red after a wrong pattern
        if (this._seq.length) {
            cr.setSourceRGBA(r, g, b, 0.9);
            cr.setLineWidth(cell * 0.06);
            cr.setLineCap(1);       // round
            cr.setLineJoin(1);
            this._seq.forEach((dot, i) => {
                const [cx, cy] = this._center(dot);
                if (i === 0)
                    cr.moveTo(cx, cy);
                else
                    cr.lineTo(cx, cy);
            });
            if (this._active)
                cr.lineTo(...this._pos);
            cr.stroke();
        }
        // the dots: bigger once in the pattern
        for (let dot = 0; dot < 9; dot++) {
            const on = this._seq.includes(dot);
            const [cx, cy] = this._center(dot);
            if (on)
                cr.setSourceRGBA(r, g, b, 1);
            else
                cr.setSourceRGBA(1, 1, 1, 0.85);
            cr.arc(cx, cy, cell * (on ? 0.09 : 0.06), 0, 2 * Math.PI);
            cr.fill();
        }
        cr.$dispose();
    }
});

function fit(prompt, grids) {
    if (!prompt || prompt._patternGrid)
        return;
    const grid = new PatternGrid(prompt);
    prompt._patternGrid = grid;
    // Next to the input row, in whatever holds it: GNOME 51 moved the row into
    // _inputWell, and inserting next to it on the prompt itself added nothing.
    (prompt._mainBox.get_parent() ?? prompt).insert_child_above(grid, prompt._mainBox);
    grids.add(grid);
    grid.connect('destroy', () => grids.delete(grid));
    // The login screen's user: picked in the user list (or typed), handed to begin().
    const begin = prompt.begin;
    prompt.begin = function (params) {
        grid.user = params?.userName ?? grid.user;
        return begin.call(this, params);
    };
    if (grid._unlock || prompt._entry === prompt._passwordEntry)
        grid.patternMode();
    else
        grid.entryMode(false);
}

export default class LockTypeExtension extends Extension {
    enable() {
        const grids = this._grids = new Set();

        const authProto = AuthPrompt.AuthPrompt.prototype;
        const origInit = this._origInit = authProto._init;
        authProto._init = function (...args) {
            origInit.apply(this, args);
            fit(this, grids);
        };
        fit(Main.screenShield?._dialog?._authPrompt, grids);

        // Swipe: reaching the prompt page (a swipe, a tap or a key on the clock) unlocks instead.
        const unlockProto = UnlockDialog.UnlockDialog.prototype;
        const origSwipeEnd = this._origSwipeEnd = unlockProto._swipeEnd;
        const origShowPrompt = this._origShowPrompt = unlockProto._showPrompt;
        const swipe = () => lockType(GLib.get_user_name()) === 'swipe';
        unlockProto._swipeEnd = function (tracker, duration, endProgress) {
            if (endProgress && swipe()) {
                Main.screenShield.deactivate(true);
                return;
            }
            origSwipeEnd.call(this, tracker, duration, endProgress);
        };
        unlockProto._showPrompt = function (...args) {
            if (swipe()) {
                Main.screenShield.deactivate(true);
                return;
            }
            origShowPrompt.apply(this, args);
        };
    }

    disable() {
        AuthPrompt.AuthPrompt.prototype._init = this._origInit;
        UnlockDialog.UnlockDialog.prototype._swipeEnd = this._origSwipeEnd;
        UnlockDialog.UnlockDialog.prototype._showPrompt = this._origShowPrompt;
        for (const grid of this._grids) {
            inputRow(grid._prompt).show();
            delete grid._prompt.begin;
            delete grid._prompt._patternGrid;
            grid._unwrap();
            grid.destroy();
        }
        this._grids = null;
    }
}
