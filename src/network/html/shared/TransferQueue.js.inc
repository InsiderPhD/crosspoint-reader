// ---- TransferQueue: one reader job at a time, with progress ------------------------------------
//
// Shared by the File Manager's "Optimize on the card" and the BookFusion page
// (pulled in with the build's include directive, styles in TransferQueue.css.inc;
// never write the directive itself in this file — the build expands it here too). The reader serves one connection at a time and a long
// job (a download, an optimize) holds it for minutes, so jobs run strictly one
// after another, and any other request the page makes goes through withReader()
// so it waits for the job in flight instead of timing out behind it.
//
// A job is { key, label, run(ctl) }. run is async and returns a short summary
// for its row, or throws an Error whose message becomes the row's failure. ctl:
//   ctl.step(text)               the row's status text ("Downloading…")
//   ctl.progress(done, total)    fill the bar; total 0/undefined = indeterminate
//   ctl.cancelled                true once stop() was called (check between steps)
// run already holds the reader (withReader), so it must use plain fetch / the
// tq* helpers below, never withReader() itself: that would wait on its own lock.

// Serialises every request to the reader, queue jobs included.
let readerChain = Promise.resolve();
function withReader(fn) {
  const run = readerChain.then(fn, fn);
  readerChain = run.catch(() => {});
  return run;
}

function tqFormatBytes(bytes) {
  if (!bytes) return '0 KB';
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  return Math.max(1, Math.round(bytes / 1024)) + ' KB';
}

// GET a file, reporting bytes as they arrive (total from Content-Length).
async function tqFetchBlob(url, onProgress) {
  const res = await fetch(url);
  if (!res.ok) throw new Error('could not download it (' + res.status + ')');
  const total = parseInt(res.headers.get('Content-Length') || '0', 10) || 0;
  if (!res.body || !res.body.getReader) {
    const blob = await res.blob();
    onProgress(blob.size, blob.size);
    return blob;
  }
  const reader = res.body.getReader();
  const chunks = [];
  let done = 0;
  onProgress(0, total);
  for (;;) {
    const { done: finished, value } = await reader.read();
    if (finished) break;
    chunks.push(value);
    done += value.length;
    onProgress(done, total);
  }
  return new Blob(chunks);
}

// POST a body, reporting upload progress (fetch() cannot). Resolves with
// { status, text } once the reader has answered.
function tqUpload(url, body, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.upload.onprogress = (e) => onProgress(e.loaded, e.lengthComputable ? e.total : 0);
    xhr.onload = () => resolve({ status: xhr.status, text: xhr.responseText });
    xhr.onerror = () => reject(new Error('the connection to the reader dropped'));
    xhr.send(body);
  });
}

// Read a newline-delimited JSON response line by line.
async function tqReadNdjson(res, onLine) {
  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffered = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (value) buffered += decoder.decode(value, { stream: !done });
    let nl;
    while ((nl = buffered.indexOf('\n')) >= 0) {
      const line = buffered.slice(0, nl).trim();
      buffered = buffered.slice(nl + 1);
      if (line) onLine(JSON.parse(line));
    }
    if (done) break;
  }
  if (buffered.trim()) onLine(JSON.parse(buffered));
}

class TransferQueue {
  // listEl: the element the rows render into.
  // options.autoStart: run jobs as soon as they are added (default true);
  //   false holds them until start().
  // options.onChange(job): a job was added, removed, or changed state (not on
  //   every progress tick).
  // options.onIdle(): the queue ran dry.
  constructor(listEl, options = {}) {
    this.listEl = listEl;
    this.listEl.classList.add('tq-list');
    this.autoStart = options.autoStart !== false;
    this.onChange = options.onChange || (() => {});
    this.onIdle = options.onIdle || (() => {});
    this.jobs = [];
    this.running = false;
    this.cancelled = false;
  }

  get pending() { return this.jobs.filter(j => j.state === 'queued' || j.state === 'active').length; }
  get busy() { return this.running; }
  find(key) { return this.jobs.find(j => j.key === key); }

  // Returns the job, or the existing one when that key is already queued or running.
  add(spec) {
    const existing = this.find(spec.key);
    if (existing && (existing.state === 'queued' || existing.state === 'active')) return existing;
    if (existing) this.jobs.splice(this.jobs.indexOf(existing), 1);
    const job = { ...spec, state: 'queued', status: 'Waiting', done: 0, total: 0, summary: '' };
    job.row = this._buildRow(job);
    this.jobs.push(job);
    this.listEl.appendChild(job.row.el);
    this.onChange(job);
    if (this.autoStart) this.start();
    return job;
  }

  // Waiting jobs can be taken out; finished ones dismissed. A running one cannot.
  remove(key) {
    const job = this.find(key);
    if (!job || job.state === 'active') return;
    this.jobs.splice(this.jobs.indexOf(job), 1);
    job.row.el.remove();
    this.onChange(job);
  }

  clearFinished() {
    this.jobs.filter(j => j.state !== 'queued' && j.state !== 'active').forEach(j => this.remove(j.key));
  }

  // Stop after the current step: waiting jobs are marked stopped, the running
  // one sees ctl.cancelled.
  stop() {
    this.cancelled = true;
    this.jobs.filter(j => j.state === 'queued').forEach(j => this._finish(j, 'stopped', 'Not started'));
  }

  async start() {
    if (this.running) return;
    this.running = true;
    this.cancelled = false;
    for (;;) {
      const job = this.jobs.find(j => j.state === 'queued');
      if (!job || this.cancelled) break;
      job.state = 'active';
      this._render(job);
      this.onChange(job);
      const queue = this;
      const ctl = {
        step: (text) => { job.status = text; job.done = 0; job.total = 0; queue._render(job); },
        progress: (done, total) => { job.done = done; job.total = total || 0; queue._render(job); },
        get cancelled() { return queue.cancelled; },
      };
      try {
        const summary = await withReader(() => job.run(ctl));
        this._finish(job, 'done', summary || 'Done');
      } catch (e) {
        const stopped = this.cancelled && e && e.message === 'Cancelled by user';
        this._finish(job, stopped ? 'stopped' : 'failed', stopped ? 'Stopped' : 'Failed: ' + (e && e.message));
      }
    }
    this.running = false;
    this.onIdle();
  }

  _finish(job, state, status) {
    job.state = state;
    job.status = status;
    this._render(job);
    this.onChange(job);
  }

  _buildRow(job) {
    const el = document.createElement('div');
    el.className = 'tq-row';
    const head = document.createElement('div');
    head.className = 'tq-head';
    const label = document.createElement('span');
    label.className = 'tq-label';
    label.textContent = job.label;
    const side = document.createElement('span');
    side.className = 'tq-side';
    const status = document.createElement('span');
    status.className = 'tq-status';
    const remove = document.createElement('button');
    remove.className = 'tq-remove';
    remove.type = 'button';
    remove.title = 'Remove';
    remove.setAttribute('aria-label', 'Remove ' + job.label);
    remove.textContent = '×';
    remove.addEventListener('click', () => this.remove(job.key));
    side.append(status, remove);
    head.append(label, side);
    const bar = document.createElement('div');
    bar.className = 'tq-bar';
    bar.setAttribute('role', 'progressbar');
    const fill = document.createElement('div');
    fill.className = 'tq-fill';
    bar.appendChild(fill);
    el.append(head, bar);
    const row = { el, status, remove, bar, fill };
    job.row = row;
    this._render(job);
    return row;
  }

  _render(job) {
    const r = job.row;
    if (!r) return;
    r.el.className = 'tq-row tq-' + job.state;
    r.remove.hidden = job.state === 'active';
    const active = job.state === 'active';
    r.bar.hidden = !active && job.state !== 'done';
    let text = job.status;
    if (active && job.total > 0) {
      const pct = Math.min(100, Math.floor((job.done / job.total) * 100));
      text += ' ' + pct + '%';
      r.bar.classList.remove('tq-indeterminate');
      r.fill.style.width = pct + '%';
      r.bar.setAttribute('aria-valuenow', String(pct));
    } else if (active) {
      if (job.done > 0) text += ' ' + tqFormatBytes(job.done);
      r.bar.classList.add('tq-indeterminate');
      r.fill.style.width = '';
      r.bar.removeAttribute('aria-valuenow');
    } else if (job.state === 'done') {
      r.bar.classList.remove('tq-indeterminate');
      r.fill.style.width = '100%';
    }
    r.status.textContent = text;
  }
}
