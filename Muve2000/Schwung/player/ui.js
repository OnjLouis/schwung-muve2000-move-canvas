import * as os from 'os';
import { decodeDelta, decodeAcceleratedDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import { buildFilepathBrowserState, refreshFilepathBrowser, moveFilepathBrowserSelection, activateFilepathBrowserItem } from '/data/UserData/schwung/shared/filepath_browser.mjs';

const root = '/data/UserData/UserLibrary';
const cc = { click: 3, jog: 14, shift: 49, menu: 50, back: 51, loop: 58, play: 85, firstKnob: 71 };
const firstPad = 68, partsPerPort = 16, ports = 4;
const fs = {
    stat: path => os.stat(path),
    readdir(path) {
        const result = os.readdir(path) || [];
        return Array.isArray(result[0]) ? result[0] : Array.isArray(result) ? result : [];
    }
};
let screen, browser, selected, port, part, shift, editing, redraw, poll, prior, lastFolder, lastKnob, lastTurn, parked, summary, file;
let awaitingStartup, startupPrompt, startupError;
const get = key => String(host_module_get_param(key) ?? '');
const number = key => Number(get(key)) || 0;
const on = key => get(key) === '1';
const set = (key, value) => host_module_set_param(key, String(value));
const setAndRead = (key, value) => typeof host_module_set_param_blocking === 'function'
    ? host_module_set_param_blocking(key, String(value)) : set(key, value);
const toggle = key => set(key, on(key) ? 0 : 1);
const songName = name => name.replace(/\.(mid|midi|kar)$/i, '');
const label = item => {
    if (!item) return 'Empty folder';
    const text = item.label || item.name || item.filename || item.kind;
    return item.kind === 'file' ? songName(text) : text;
};
const renderFirst = () => get('playback_mode') !== 'live';
const retryRender = () => on('render_retry');
const limit = (value, min, max) => Math.max(min, Math.min(max, value));
const partId = n => port * partsPerPort + n;
const partName = n => 'Part ' + String.fromCharCode(65 + port) + (n + 1);
const time = ms => Math.floor(ms / 60000) + ':' + String(Math.floor(ms / 1000) % 60).padStart(2, '0');
const spokenTime = ms => {
    const seconds = Math.floor(ms / 1000), minutes = Math.floor(seconds / 60);
    return (minutes ? minutes + (minutes === 1 ? ' minute, ' : ' minutes, ') : '') + seconds % 60 + ' seconds';
};
const states = ['Stopped', 'Playing', 'Finished', 'Seeking', 'Overload', 'Rendering'];
const state = () => states[Number(get('transport_info').split(',')[0])] || 'Stopped';
const cpu = () => get('cpu').split(',').map(value => Number(value) || 0);
const row = (text, run, value = () => '', edit = '') => ({ text, run, value, edit });
const flag = key => on(key) ? 'On' : 'Off';
const renderPercent = (elapsed, duration) => duration > 0 ? limit(Math.floor(100 * elapsed / duration), 0, 99) : 0;

function refreshFiles() {
    refreshFilepathBrowser(browser, fs);
    const parent = browser.items.find(item => item.kind === 'up');
    if (parent) parent.label = 'Back';
    else browser.items.push({kind:'exit', label:'Back'});
}
function activateFile() {
    const current = browser.items[browser.selectedIndex];
    if (current?.kind === 'exit') { back(); return {action:'exit'}; }
    const previous = browser.currentDir;
    const result = activateFilepathBrowserItem(browser);
    if (current?.kind === 'up') browser.selectedPath = previous;
    return result;
}
function browse() {
    browser = buildFilepathBrowserState({ root, filter: ['.mid', '.midi', '.kar'], name: 'MIDI Files', start_path: lastFolder }, '');
    refreshFiles();
    screen = 'files';
    announce('Open MIDI File. ' + label(browser.items[browser.selectedIndex]));
}
function speakStatus() {
    const [average = 0, peak = 0, voices = 0] = cpu();
    const name = songName(get('loaded')) || 'No file';
    if (get('rendered') === 'preparing') {
        announce(name + '. Rendering, ' + renderPercent(number('position_ms'), number('duration_ms')) + ' percent.');
        return;
    }
    const lead = number('live_buffer_ms');
    const delay = lead > 0 ? ' Live edit delay ' + (lead / 1000).toFixed(1) + ' seconds.' : '';
    announce(name + '. ' + state() + '. ' + spokenTime(number('position_ms')) + ' of ' +
        spokenTime(number('duration_ms')) + '. Render CPU ' + average + ' percent, peak ' + peak +
        ' percent. ' + voices + ' voices. ' + number('underruns') + ' audio gaps. ' + number('midi_drops') + ' dropped MIDI messages.' + delay);
}
function play() {
    if (on('playing')) {
        const preparing = get('rendered') === 'preparing';
        set('stop', 1);
        announce(preparing ? 'Rendering cancelled' : 'Stopped');
    }
    else if (get('loaded')) {
        const retry = retryRender();
        const prepare = (renderFirst() || retry) && get('rendered') === 'off';
        setAndRead(prepare ? 'render_first' : 'play', 1);
        const status = get('status');
        if (status && status !== 'Ready.') { announce(status); return; }
        if (prepare) {
            show('home'); poll = 0; prior = 5;
            announce(retry ? 'Rendering from the start. Press Play to cancel.' : 'Rendering MIDI file. Press Play to cancel.');
        } else announce('Playing');
    }
    else browse();
}
function seek(seconds) {
    if (!get('loaded')) return;
    if (get('rendered') === 'preparing') { announce('Wait for rendering or press Play to cancel.'); return; }
    const end = Math.max(0, Math.ceil(number('duration_ms') / 1000));
    const to = limit(seconds, 0, end);
    set('seek', to);
    announce(to === 0 ? 'Start of file' : spokenTime(to * 1000));
}
function show(next) { screen = next; selected = 0; editing = false; }
const backRow = row('Back', () => { back(); return false; });
function onScreenRows() {
    if (screen === 'parts') return Array.from({ length: partsPerPort }, (_, n) =>
        row(partName(n), () => { part = n; show('part'); }, () => on('mute:' + partId(n)) ? 'Muted' :
            on('solo:' + partId(n)) ? 'Solo' : on('used:' + partId(n)) ? 'Used' : '')).concat(backRow);
    if (screen === 'part') {
        const i = partId(part);
        return [
            row('Mute', () => toggle('mute:' + i), () => flag('mute:' + i)),
            row('Solo', () => toggle('solo:' + i), () => flag('solo:' + i)),
            row('Level trim', () => { editing = !editing; }, () => number('trim:' + i) + ' percent', 'trim:' + i), backRow
        ];
    }
    return [
        row('Open MIDI File', browse),
        row(on('playing') ? 'Stop' : 'Play', play),
        row('Back to Start', () => seek(0)),
        row('Loop', () => toggle('loop'), () => flag('loop')),
        row('Parts', () => { show('parts'); announce('Port ' + String.fromCharCode(65 + port) + ', ' + partName(0)); },
            () => 'Port ' + String.fromCharCode(65 + port)),
        row('USB MIDI Input', () => toggle('usb'), () => flag('usb')),
        row('Master Volume', () => { editing = !editing; }, () => number('master') + ' percent', 'master'),
        row('File Info', () => announce((songName(get('loaded')) || 'No file') + '. ' + spokenTime(number('duration_ms')) + '. ' +
            (Array.from({ length: ports * partsPerPort }, (_, i) => on('used:' + i)).filter(Boolean).length) + ' parts used.')),
        row('Status', speakStatus),
        row('All Notes Off', () => { set('panic', 1); announce('All notes off'); }),
        row('Playback Mode', () => {
            if (on('playing')) { announce('Stop playback before changing mode.'); return false; }
            const mode = renderFirst() ? 'live' : 'render';
            setAndRead('playback_mode', mode);
            if (get('playback_mode') !== mode) { announce(get('status')); return false; }
            if (mode === 'live') setAndRead('render_live', 1);
        }, () => renderFirst() ? 'Render First' : 'Live'), backRow
    ];
}
function sayRow() {
    const item = onScreenRows()[selected], value = item.value();
    announce(item.text + (value ? ', ' + value : ''));
}
function back() {
    if (editing) { editing = false; sayRow(); }
    else if (screen === 'part') { show('parts'); selected = part; sayRow(); }
    else if (screen === 'menu') { show('home'); announce(state()); }
    else {
        const fromParts = screen === 'parts';
        show('menu');
        if (fromParts) selected = onScreenRows().findIndex(item => item.text === 'Parts');
        sayRow();
    }
}
function draw() {
    clear_screen();
    drawMenuHeader(screen === 'files' ? 'Open MIDI File' : screen === 'part' ? partName(part) :
        screen === 'parts' ? 'Parts, Port ' + String.fromCharCode(65 + port) : 'Muve2000 MIDI Player',
        editing ? 'EDIT' : '');
    if (screen === 'home') {
        const [mode = 0, elapsed = 0, duration = 0, average = 0, peak = 0] = summary;
        if (awaitingStartup || startupError) {
            print(2, 16, startupError ? 'Muve2000 not ready' : 'Starting Muve2000...', 1);
        } else {
            print(2, 16, (songName(file) || 'No MIDI file selected').slice(0, 21), 1);
            const progress = mode === 5 ? 'Rendering ' + renderPercent(elapsed, duration) + '%' :
                (states[mode] || 'Stopped') + ' ' + time(elapsed) + '/' + time(duration);
            print(2, 28, progress.slice(0, 21), 1);
            print(2, 39, 'CPU ' + average + '%  Peak ' + peak + '%', 1);
        }
        drawMenuFooter('Click:menu Back:hide');
    } else {
        const files = screen === 'files';
        drawMenuList({
            items: files ? browser.items : onScreenRows(),
            selectedIndex: files ? browser.selectedIndex : selected,
            listArea: { topY: menuLayoutDefaults.listTopY, bottomY: menuLayoutDefaults.listBottomWithFooter },
            getLabel: files ? label : item => item.text,
            getValue: files ? () => '' : item => item.value(),
            valueAlignRight: true, announce: false
        });
        drawMenuFooter(editing ? 'Jog:change Menu:done' : 'Jog:move Click:open');
    }
    if (typeof host_flush_display === 'function') host_flush_display();
}
function init() {
    file = get('loaded');
    show(file ? 'home' : 'menu');
    browser = null; port = 0; part = 0; shift = false; redraw = true;
    parked = false; summary = [];
    awaitingStartup = true; startupPrompt = !file; startupError = false;
    poll = 0; prior = ''; lastFolder = root; lastKnob = -1; lastTurn = 0;
}
function onResume() { parked = false; redraw = true; shift = false; poll = 0; }
function tick() {
    if (parked) return;
    const visible = typeof shadow_get_display_mode !== 'function' || shadow_get_display_mode() === 1;
    const currentPoll = ++poll;
    if (visible && awaitingStartup && currentPoll % 8 === 1) {
        const ready = get('ready');
        if (ready === '1' || ready === '-1') {
            awaitingStartup = false;
            startupError = ready === '-1';
            if (startupError) {
                startupPrompt = false;
                announce(get('status') || 'Muve2000 could not start.');
            }
            redraw = true;
        }
    }
    if (!awaitingStartup && !startupError && currentPoll % 24 === 1 && screen === 'home') {
        summary = get('transport_info').split(',').map(Number);
        file = get('loaded');
        const mode = summary[0];
        if (prior !== mode) {
            if (mode === 4) announce(retryRender() ? 'Live playback overloaded. Press Play to render this file from the start.' :
                get('status') || 'Playback stopped.');
            else if (mode === 2) announce('End of file');
            else if (prior === 5 && mode === 1) announce('Playing rendered file');
            else if (prior === '') {
                const status = get('status');
                if (status && status !== 'Ready.') announce(status);
            }
            prior = mode;
        }
        redraw = true;
    }
    if (redraw) { redraw = false; draw(); }
    // Shared speech drops messages while the host is still showing the previous screen.
    if (visible && startupPrompt) {
        startupPrompt = false;
        announce('Open MIDI File');
    }
}
function onMidiMessageInternal(data) {
    if (!data || data.length < 3) return;
    const type = data[0] & 240, key = data[1], v = data[2];
    if (v && (type === 144 || type === 176) && !isCapacitiveTouchMessage(data)) startupPrompt = false;
    if (type === 176 && key === cc.shift) { shift = v > 0; return; }
    if (type === 176 && key === cc.back && v && typeof host_suspend_overtake === 'function') {
        parked = true;
        host_suspend_overtake();
        return;
    }
    if (screen !== 'files' && type === 144 && v && key >= firstPad && key < firstPad + partsPerPort) {
        if (get('rendered') !== 'off' && get('rendered')) { announce('Switch Playback Mode to Live to change parts.'); return; }
        const n = key - firstPad, control = (shift ? 'solo:' : 'mute:') + partId(n);
        toggle(control);
        announce(partName(n) + (shift ? ', solo ' : ', mute ') + flag(control));
    } else if (screen !== 'files' && ((type === 144 && v && key < 8) ||
        (type === 176 && key >= cc.firstKnob && key < cc.firstKnob + 8))) {
        const turn = type === 176, n = (turn ? key - cc.firstKnob : key) + (shift ? 8 : 0), control = 'trim:' + partId(n);
        const d = turn ? decodeAcceleratedDelta(v, key) : 0;
        if (turn && !d) return;
        if (turn && get('rendered') !== 'off' && get('rendered')) { announce('Switch Playback Mode to Live to change parts.'); return; }
        const value = limit(number(control) + d, 0, 100);
        if (turn) set(control, value);
        const now = Date.now(), named = !turn || n !== lastKnob || now - lastTurn > 1500;
        announce((named ? partName(n) + ', level trim, ' : '') + value + ' percent');
        lastKnob = n; lastTurn = now;
    } else if (isCapacitiveTouchMessage(data) || type !== 176) return;
    else if (screen === 'files') {
        if (key === cc.jog) {
            moveFilepathBrowserSelection(browser, decodeDelta(v));
            announce(label(browser.items[browser.selectedIndex]));
        } else if (key === cc.menu && v) back();
        else if (key === cc.click && v) {
            const result = activateFile();
            if (result.action === 'open') {
                refreshFiles();
                announce(label(browser.items[browser.selectedIndex]));
            } else if (result.action === 'select') {
                lastFolder = result.value.slice(0, result.value.lastIndexOf('/')) || root;
                setAndRead('file_path', result.value);
                show('home');
                prior = ''; poll = 0;
                announce('Opening ' + songName(result.filename));
            }
        } else return;
    } else if (key === cc.play && v) { if (shift) seek(0); else play(); }
    else if (key === cc.loop && v) { toggle('loop'); announce('Loop, ' + flag('loop')); }
    else if (screen === 'home') {
        if (key === cc.click && v && shift) speakStatus();
        else if ((key === cc.click || key === cc.menu) && v) { show('menu'); sayRow(); }
        else if (key === cc.jog) {
            const d = decodeDelta(v);
            if (d) seek(Math.floor(number('position_ms') / 1000) + d * (shift ? 30 : 5));
        } else return;
    } else if (key === cc.menu && v) back();
    else if (key === cc.click && v) {
        if (screen === 'parts' && shift) {
            port = (port + 1) % ports; selected = 0;
            announce('Port ' + String.fromCharCode(65 + port));
        } else {
            const item = onScreenRows()[selected];
            if (item.run() !== false && item.value() && (screen === 'menu' || screen === 'part')) sayRow();
        }
    } else if (key === cc.jog) {
        const d = decodeDelta(v), items = onScreenRows();
        if (!d) return;
        if (editing) {
            const control = items[selected].edit;
            const value = limit(number(control) + d, 0, 100);
            set(control, value);
            announce(value + ' percent');
        } else if (screen === 'parts' && shift) {
            port = limit(port + d, 0, ports - 1); selected = 0;
            announce('Port ' + String.fromCharCode(65 + port));
        } else { selected = limit(selected + d, 0, items.length - 1); sayRow(); }
    } else return;
    redraw = true;
}
function onMidiMessageExternal() {}
Object.assign(globalThis, { init, tick, onResume, onMidiMessageInternal, onMidiMessageExternal });
