// Attach to a tmux-tab via the ttyd websocket and verify round-trip typing.
// usage: node scripts/test-ws-tab.mjs <port> <tabId> <marker>
const [port, tabId, marker = 'WS-TAB-MARK'] = process.argv.slice(2);
const ws = new WebSocket(`ws://127.0.0.1:${port}/ws?tab=${tabId}`, ['tty']);
ws.binaryType = 'arraybuffer';
const decoder = new TextDecoder();
let output = '';
let inputSent = false;

ws.onopen = () => {
    ws.send(new TextEncoder().encode(JSON.stringify({ AuthToken: '', columns: 80, rows: 24 })));
    setTimeout(() => {
        if (!inputSent) {
            ws.send(new TextEncoder().encode('0' + `echo ${marker}\r`));
            inputSent = true;
        }
    }, 800);
};
ws.onmessage = event => {
    const data = new Uint8Array(event.data);
    if (String.fromCharCode(data[0]) !== '0') return;
    output += decoder.decode(data.subarray(1));
    if (output.includes(marker)) {
        console.log('ws-attach ok');
        process.exit(0);
    }
};
ws.onclose = event => {
    console.error(`websocket closed (code ${event.code})`);
    process.exit(1);
};
ws.onerror = () => console.error('websocket error');
setTimeout(() => {
    console.error(`timeout, last output: ${JSON.stringify(output.slice(-200))}`);
    process.exit(1);
}, 15000);
