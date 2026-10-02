  // --- browser-side plugin host (shared by Settings + File Manager) ---------
  // Discovers plugins from the SD card (/api/plugins), loads each plugin.js,
  // and calls its registered render fn with a container + a small api. A plugin
  // is just JS on the SD; adding one needs no firmware change.
  window.CrossPoint = window.CrossPoint || {
    _pending: null,
    registerPlugin(fn) { this._pending = fn; },
    // --- plugin job queue -------------------------------------------------
    // A plugin registers named actions; external systems enqueue jobs with
    // POST /api/plugin-jobs, and any open page hosting the plugin (File
    // Manager, Settings, or the headless /plugins-run page) claims and
    // executes them here, then posts the result back for the caller to poll.
    _actions: {},
    _actionBusy: false,
    _actionTimer: null,
    _pendingCompletion: null,
    registerAction(pluginName, action, fn) {
      (this._actions[pluginName] = this._actions[pluginName] || {})[action] = fn;
      if (!this._actionTimer) this._actionTimer = setInterval(() => this._pollJobs(), 2500);
    },
    async _pollJobs() {
      if (this._actionBusy) return;
      this._actionBusy = true;
      try {
        if (this._pendingCompletion) {
          try {
            await this._post('/api/plugin-jobs/complete', this._pendingCompletion);
            this._pendingCompletion = null;
          } catch (e) { return; }
        }
        for (const plugin of Object.keys(this._actions)) {
          let job;
          try { job = await (await fetch('/api/plugin-jobs/claim?plugin=' + encodeURIComponent(plugin))).json(); }
          catch (e) { continue; }
          if (!job || !job.id) continue;
          const fn = this._actions[plugin][job.action];
          let ok = false;
          let result;
          if (!fn) {
            result = { error: 'unknown action: ' + job.action };
          } else {
            try { result = (await fn(job.args || {})) || {}; ok = true; }
            catch (e) { result = { error: String((e && e.message) || e) }; }
          }
          this._pendingCompletion = { id: job.id, ok, result };
          try {
            await this._post('/api/plugin-jobs/complete', this._pendingCompletion);
            this._pendingCompletion = null;
          } catch (e) { return; }
        }
      } finally {
        this._actionBusy = false;
      }
    },
    async _post(path, body) {
      const r = await fetch(path, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
      });
      if (!r.ok) {
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        throw new Error(path + ' ' + r.status + (detail ? ': ' + detail : ''));
      }
      return r.json();
    },
    // Device capabilities a page can't do itself. relay() resolves to
    // { status, body, headers }, where headers is an ordered list of
    // [name, value] pairs (duplicates preserved, so every Set-Cookie shows up).
    relay(plugin, method, url, headers, body) {
      return this._post('/api/relay', { plugin, method, url, headers: headers || {}, body: body || '' });
    },
    // Collect Set-Cookie values from a relay response into a Cookie header
    // string, so a plugin can carry a session across requests. Generic — this
    // just reads the standard header; it ascribes no meaning to the contents.
    cookiesFrom(resp, existing) {
      const jar = {};
      (existing || '').split(';').forEach((p) => {
        const i = p.indexOf('='); if (i > 0) jar[p.slice(0, i).trim()] = p.slice(i + 1).trim();
      });
      ((resp && resp.headers) || []).forEach(([name, value]) => {
        if (String(name).toLowerCase() !== 'set-cookie') return;
        const first = String(value).split(';')[0];
        const i = first.indexOf('='); if (i > 0) jar[first.slice(0, i).trim()] = first.slice(i + 1).trim();
      });
      return Object.keys(jar).map((k) => k + '=' + jar[k]).join('; ');
    },
    crypto(op, fields) { return this._post('/api/crypto', Object.assign({ op }, fields || {})); },
    async writeFile(plugin, path, dataB64) {
      // Send the base64 as-is (enc=base64); the device decodes it. Base64 is
      // NUL-free ASCII, so a binary payload survives the WebServer's String
      // parsing, which would otherwise truncate a raw body at the first NUL.
      const url = '/api/plugin-fs?plugin=' + encodeURIComponent(plugin) +
        '&path=' + encodeURIComponent(path) + '&enc=base64';
      const r = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: dataB64 || ''
      });
      if (!r.ok) {
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        throw new Error('/api/plugin-fs ' + r.status + (detail ? ': ' + detail : ''));
      }
      return r.json();
    },
    // Same gate the BookFusion browser applies on-device: it knows download_size
    // from its API and asks before spending the transfer. A plugin fetching an
    // arbitrary URL has no such field, so the device HEADs it first (probe) and
    // we ask here. Threshold and the "approx." estimate match
    // BookFusionBrowserActivity (10MB, ~30KB/s measured on hardware) so the two
    // paths warn about the same books in the same words.
    LARGE_FETCH_WARN_BYTES: 10 * 1024 * 1024,
    ESTIMATED_BYTES_PER_SEC: 30 * 1024,
    async confirmLargeFetch(plugin, url, headers) {
      let total = 0;
      try {
        const probe = await this._post('/api/fetch', { plugin, url, headers: headers || {}, probe: true });
        total = Number(probe && probe.total) || 0;
      } catch (e) {
        return true;  // a server that won't answer HEAD is not a reason to block
      }
      if (!total || total < this.LARGE_FETCH_WARN_BYTES) return true;
      const mb = (total / (1024 * 1024)).toFixed(1);
      const sec = Math.ceil(total / this.ESTIMATED_BYTES_PER_SEC);
      const eta = sec > 90 ? Math.ceil(sec / 60) + ' min' : sec + ' sec';
      return window.confirm('This download is ' + mb + ' MB (approx. ' + eta +
        ' over Wi-Fi). Continue?');
    },
    async fetchToSd(plugin, url, dest, headers) {
      if (!(await this.confirmLargeFetch(plugin, url, headers))) {
        throw new Error('download cancelled');
      }
      // Keep each browser-facing request short. Mobile browsers may terminate
      // a request after a few minutes even when response heartbeats are flowing,
      // while the device itself is still downloading successfully.
      const maxBytes = 2 * 1024 * 1024;
      let offset = 0;
      for (;;) {
        const result = await this._post('/api/fetch', {
          plugin, url, dest, headers: headers || {}, offset, maxBytes
        });
        if (result.complete !== false) return result;
        if (!Number.isFinite(result.bytes) || result.bytes <= offset) {
          throw new Error('/api/fetch made no progress');
        }
        offset = result.bytes;
      }
    }
  };

  async function loadPlugins(mount) {
    const host = document.getElementById('plugin-container');
    if (!host) return;
    let list;
    try { list = await (await fetch('/api/plugins')).json(); } catch (e) { return; }
    for (const p of list) {
      // A null mount loads everything: the headless runner page hosts all
      // plugins so any registered action can execute there.
      if (mount && (p.mount || 'settings') !== mount) continue;
      const container = document.createElement('div');
      container.className = 'card';
      container.id = 'plugin-' + p.name;
      host.appendChild(container);
      CrossPoint._pending = null;
      try {
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
          s.onload = resolve; s.onerror = () => reject(new Error('load ' + p.name));
          document.body.appendChild(s);
        });
        if (typeof CrossPoint._pending === 'function') {
          CrossPoint._pending(container, {
            name: p.name,
            // The plugin's own folder on the SD card, for files it keeps beside
            // itself (the store's config.json). Reported by /api/plugins since
            // a plugin may live under any plugin root; older firmware without
            // the field is covered by the historical root.
            dir: p.dir || ('/.crosspoint/plugins/' + p.name),
            relay: (m, u, h, b) => CrossPoint.relay(p.name, m, u, h, b),
            cookiesFrom: (resp, existing) => CrossPoint.cookiesFrom(resp, existing),
            crypto: (op, f) => CrossPoint.crypto(op, f),
            writeFile: (path, d) => CrossPoint.writeFile(p.name, path, d),
            fetchToSd: (u, dest, h) => CrossPoint.fetchToSd(p.name, u, dest, h),
            registerAction: (a, fn) => CrossPoint.registerAction(p.name, a, fn),
            pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) + '&file=' + encodeURIComponent(f)
          });
        }
      } catch (e) {
        const h = document.createElement('h2');
        h.textContent = p.title || p.name;
        const msg = document.createElement('p');
        msg.style.color = '#c0392b';
        msg.textContent = 'Failed to load plugin.';
        container.replaceChildren(h, msg);
      }
    }
  }
