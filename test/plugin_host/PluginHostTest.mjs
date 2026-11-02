// Node test for the shared browser plugin host (src/network/html/js/plugin-host.js),
// adapted from CrossInk's test/plugin_host. Run test/run_plugin_host_test.sh.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const context = vm.createContext({ setInterval: () => 1, confirm: () => true });
context.window = context;
vm.runInContext(
  fs.readFileSync(new URL('../../src/network/html/js/plugin-host.js', import.meta.url), 'utf8'),
  context
);
const host = context.CrossPoint;
let calls = [];
let replies = [];
context.fetch = async (url, init) => {
  calls.push({ url, body: init?.body && init.headers?.['Content-Type'] === 'application/json' ? JSON.parse(init.body) : init?.body });
  const reply = replies.shift();
  return { ok: !(reply && reply.httpError), status: (reply && reply.httpError) || 200, json: async () => reply };
};

// Staged GET fetch: a HEAD probe (no total), then 2MB segments resumed at the
// device's byte count until complete.
replies = [{ total: 0 }, { status: 200, bytes: 5, complete: false }, { status: 206, bytes: 8, complete: true }];
assert.equal((await host.fetchToSd('fixture', 'https://example.test/book', '/book')).bytes, 8);
assert.equal(calls[0].body.probe, true);
assert.equal(calls[1].body.offset, 0);
assert.equal(calls[2].body.offset, 5);

// A segment that makes no progress fails instead of looping.
replies = [{ total: 0 }, { status: 200, bytes: 0, complete: false }];
await assert.rejects(() => host.fetchToSd('fixture', 'https://example.test/book', '/book'));

// A device error status rejects with the device's message.
replies = [{ httpError: 403, error: 'plugin system disabled' }];
await assert.rejects(() => host.relay('fixture', 'GET', 'https://example.test/', {}, ''), /403: plugin system disabled/);

// A POST fetch is sent once, and a failure carries the device's result.
calls = [];
replies = [{ status: 500, complete: false, error: 'http', bytes: 0 }];
await assert.rejects(
  () => host.fetchToSd('fixture', 'https://example.test/api', '/out.json', {}, { method: 'post', body: '{}' }),
  (err) => err.result && err.result.status === 500
);
assert.equal(calls.length, 1);
assert.equal(calls[0].body.method, 'POST');

// Plugin and action names never resolve inherited Object.prototype members.
host.registerAction('__proto__', 'refresh', () => ({ updated: true }));
assert.equal(Object.getPrototypeOf(host._actions), null);
assert.equal(Object.getPrototypeOf(host._actions.__proto__), null);
replies = [{ id: 3, action: 'toString', args: {} }, { ok: true }];
calls = [];
await host._pollJobs();
assert.equal(calls.at(-1).url, '/api/plugin-jobs/complete');
assert.equal(calls.at(-1).body.ok, false);
assert.match(calls.at(-1).body.result.error, /unknown action/);

// Cookies from a relay response merge into an existing Cookie header.
assert.equal(
  host.cookiesFrom({ headers: [['Set-Cookie', 'a=1; Path=/'], ['set-cookie', 'b=2']] }, 'a=0; c=3'),
  'a=1; c=3; b=2'
);

// An async renderer failure shows the error card, not an unhandled rejection.
const container = { appendChild() {}, replaceChildren(...children) { this.children = children; } };
context.document = {
  getElementById: () => container,
  createElement: (kind) => (kind === 'script' ? {} : { style: {}, replaceChildren: container.replaceChildren }),
  body: {
    appendChild(script) {
      host.registerPlugin(async () => {
        throw new Error('fixture rejection');
      });
      script.onload();
    }
  }
};
let rendered;
container.appendChild = (value) => {
  rendered = value;
};
replies = [[{ name: 'fixture', title: 'Fixture' }]];
await context.loadPlugins(null);
assert.equal(rendered.children[1].textContent, 'Failed to load plugin.');

console.log('Plugin host tests passed: staged fetch, POST fetch, errors, action isolation, cookies, async render failure.');
