// Lock type and pattern. Both are kept by root in /var/lib/gts4lv-lock (the pattern as a salted hash, for
// PAM), so they are saved through pkexec gts4lv-lock-set, which asks for the user's password.
import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const TYPES = [
    ['pattern', 'Pattern', 'Draw a pattern on a 3x3 dot grid. Your password works too.'],
    ['password', 'Password', 'Type your password'],
    ['swipe', 'Swipe', 'Swipe up on the clock, no password. The tablet also logs you in by itself at start.'],
];
const HELPER = '/usr/local/sbin/gts4lv-lock-set';

function currentType() {
    try {
        const [, bytes] = Gio.File.new_for_path(`/var/lib/gts4lv-lock/${GLib.get_user_name()}.type`).load_contents(null);
        return new TextDecoder().decode(bytes).trim();
    } catch {
        return 'password';
    }
}

/** pkexec gts4lv-lock-set args…, with stdin; resolves to [ok, error text]. */
function helper(args, input = null) {
    return new Promise(resolve => {
        const proc = Gio.Subprocess.new(['pkexec', HELPER, ...args],
            Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDERR_PIPE);
        proc.communicate_utf8_async(input, null, (p, res) => {
            try {
                const [, , stderr] = p.communicate_utf8_finish(res);
                resolve([p.get_successful(), (stderr ?? '').trim()]);
            } catch (e) {
                resolve([false, e.message]);
            }
        });
    });
}

/** A 3x3 dot pad: calls onDone('14789') when a drag ends. */
function patternPad(onDone) {
    const area = new Gtk.DrawingArea({content_width: 240, content_height: 240, halign: Gtk.Align.CENTER});
    let seq = [], pos = null, error = false;
    const geo = () => {
        const w = area.get_width(), h = area.get_height(), cell = Math.min(w, h) / 3;
        return [(w - cell * 3) / 2, (h - cell * 3) / 2, cell];
    };
    const center = dot => {
        const [x0, y0, cell] = geo();
        return [x0 + cell * (dot % 3) + cell / 2, y0 + cell * Math.floor(dot / 3) + cell / 2];
    };
    const hit = (x, y) => {
        const cell = geo()[2];
        for (let dot = 0; dot < 9; dot++) {
            const [cx, cy] = center(dot);
            if (Math.hypot(x - cx, y - cy) > cell * 0.3 || seq.includes(dot))
                continue;
            const last = seq.at(-1);
            if (last !== undefined) {
                const dr = Math.floor(dot / 3) - Math.floor(last / 3), dc = dot % 3 - last % 3;
                const mid = last + (dr / 2) * 3 + dc / 2;
                if (dr % 2 === 0 && dc % 2 === 0 && !seq.includes(mid))
                    seq.push(mid);
            }
            seq.push(dot);
            return;
        }
    };
    area.set_draw_func((a, cr) => {
        const cell = geo()[2];
        const [r, g, b] = error ? [0.88, 0.11, 0.14] : [0.21, 0.52, 0.89];
        if (seq.length) {
            cr.setSourceRGBA(r, g, b, 0.9);
            cr.setLineWidth(cell * 0.06);
            cr.setLineCap(1);
            cr.setLineJoin(1);
            seq.forEach((dot, i) => (i ? cr.lineTo(...center(dot)) : cr.moveTo(...center(dot))));
            if (pos)
                cr.lineTo(...pos);
            cr.stroke();
        }
        const fg = a.get_color();
        for (let dot = 0; dot < 9; dot++) {
            const on = seq.includes(dot);
            if (on)
                cr.setSourceRGBA(r, g, b, 1);
            else
                cr.setSourceRGBA(fg.red, fg.green, fg.blue, 0.6);
            cr.arc(...center(dot), cell * (on ? 0.09 : 0.06), 0, 2 * Math.PI);
            cr.fill();
        }
        cr.$dispose();
    });
    const drag = new Gtk.GestureDrag();
    drag.connect('drag-begin', (g, x, y) => {
        seq = [];
        error = false;
        pos = [x, y];
        hit(x, y);
        area.queue_draw();
    });
    drag.connect('drag-update', (g, dx, dy) => {
        const [, sx, sy] = g.get_start_point();
        pos = [sx + dx, sy + dy];
        hit(...pos);
        area.queue_draw();
    });
    drag.connect('drag-end', () => {
        pos = null;
        area.queue_draw();
        if (seq.length)
            onDone(seq.map(d => d + 1).join(''));
    });
    area.add_controller(drag);
    area.reset = (isError = false) => {
        error = isError;
        if (!isError)
            seq = [];
        area.queue_draw();
    };
    return area;
}

export default class LockTypePrefs extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const page = new Adw.PreferencesPage();
        window.add(page);

        const typeGroup = new Adw.PreferencesGroup({title: 'Screen lock'});
        const row = new Adw.ComboRow({title: 'Lock type', model: Gtk.StringList.new(TYPES.map(t => t[1]))});
        const type = currentType();
        row.selected = Math.max(0, TYPES.findIndex(t => type.startsWith(t[0])));
        row.subtitle = TYPES[row.selected][2];
        typeGroup.add(row);
        page.add(typeGroup);

        const patternGroup = new Adw.PreferencesGroup({
            title: 'Pattern',
            description: type === 'pattern' ? 'A pattern is set. Draw a new one to change it.'
                : 'Draw a pattern of at least 4 dots.',
        });
        const status = new Gtk.Label({wrap: true, margin_top: 6, margin_bottom: 6});
        let first = null;
        const pad = patternPad(async seq => {
            if (seq.length < 4) {
                status.label = 'Connect at least 4 dots';
                pad.reset(true);
                return;
            }
            if (first === null) {
                first = seq;
                status.label = 'Draw the same pattern again to confirm';
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 400, () => (pad.reset(), GLib.SOURCE_REMOVE));
                return;
            }
            if (seq !== first) {
                first = null;
                status.label = 'The patterns did not match. Draw a new pattern';
                pad.reset(true);
                return;
            }
            first = null;
            status.label = 'Saving…';
            let [ok, err] = await helper(['pattern'], `${seq}\n`);
            if (ok && TYPES[row.selected][0] !== 'pattern') {
                row.selected = 0;   // saves the type through notify::selected
                return;
            }
            status.label = ok ? 'Pattern saved' : `Not saved${err ? `: ${err}` : ''}`;
            pad.reset(!ok);
        });
        status.label = 'Draw a pattern';
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, margin_top: 12, margin_bottom: 12});
        box.append(pad);
        box.append(status);
        patternGroup.add(box);
        page.add(patternGroup);

        let saving = false;
        row.connect('notify::selected', async () => {
            if (saving)
                return;
            const [name, , subtitle] = TYPES[row.selected];
            row.subtitle = subtitle;
            saving = true;
            const [ok, err] = await helper(['type', name]);
            saving = false;
            if (!ok) {
                row.subtitle = `Not changed${err ? `: ${err}` : ''}`;
                return;
            }
            if (name === 'pattern' && !currentType().startsWith('pattern'))
                status.label = 'Draw a pattern';
            else if (name === 'pattern')
                status.label = currentType() === 'pattern' ? 'Pattern lock is on' : 'Now draw a pattern';
        });
    }
}
