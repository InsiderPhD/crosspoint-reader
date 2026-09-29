  // --- install a plugin from a .zip ----------------------------------------
  // The firmware has no archive support: the zip is expanded here in the
  // browser (JSZip is already served for the EPUB converter) and each file is
  // written with /api/plugin-fs, the same endpoint plugins use for their own
  // storage. That keeps the device side to what it already does — write one
  // small file at an absolute path, creating parent directories.
  //
  // Without this, installing the Plugin Store itself means hand-copying its
  // files into /plugins/<name>/ through the File Manager, or pulling the SD
  // card — a bootstrap problem, since the store is what installs everything
  // else.
  const PLUGIN_ROOT = '/.crosspoint/plugins';
  // /api/plugin-fs caps a request body at 256KB of base64, i.e. ~192KB of file.
  const MAX_PLUGIN_FILE_BYTES = 192 * 1024;
  // SD roots a zip may already be rooted at (see PluginLocations::kRoots). An
  // archive laid out for "extract at the card root" carries one of these as its
  // first path segment and is written verbatim; anything else is treated as the
  // plugin's own folder and lands under PLUGIN_ROOT.
  const SD_ROOT_DIRS = ['.crosspoint', 'plugins', '.plugins'];

  function pluginInstallStatus(text, isError) {
    const el = document.getElementById('pluginInstallStatus');
    if (!el) return;
    el.textContent = text || '';
    el.style.color = isError ? '#e74c3c' : '';  // '' falls back to .install-status
  }

  // Bytes -> base64 without blowing the stack on a large file: String.fromCharCode
  // applied to a whole 190KB array exceeds the argument limit in some browsers.
  function bytesToBase64(bytes) {
    let binary = '';
    const CHUNK = 0x8000;
    for (let i = 0; i < bytes.length; i += CHUNK) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
    }
    return btoa(binary);
  }

  // Drops archive noise and rejects anything that could escape the plugin root.
  function isInstallableEntry(path) {
    if (!path || path.endsWith('/')) return false;
    const parts = path.split('/');
    if (parts.some((p) => p === '..' || p === '')) return false;
    if (parts[0] === '__MACOSX') return false;
    return !parts.some((p) => p === '.DS_Store' || p.startsWith('._'));
  }

  // Where each entry lands, and which plugin name owns the write. Three shapes:
  // rooted at an SD root ("<root>/<name>/..."), a single top-level folder
  // ("<name>/..."), or bare files at the zip root (named after the zip).
  function planPluginInstall(paths, zipFileName) {
    const first = paths.map((p) => p.split('/')[0]);
    const rooted = SD_ROOT_DIRS.includes(first[0]) && first.every((f) => f === first[0]);
    if (rooted) {
      return paths.map((p) => {
        const segments = p.split('/');
        // <root>/<name>/... — the name is the folder under the root, and for
        // /.crosspoint that root is two segments deep (/.crosspoint/plugins).
        const nameIndex = segments[0] === '.crosspoint' ? 2 : 1;
        return { path: '/' + p, plugin: segments[nameIndex] || segments[nameIndex - 1] || 'plugin', zipPath: p };
      });
    }
    const singleFolder = first.every((f) => f === first[0]) && paths.every((p) => p.includes('/'));
    const name = singleFolder ? first[0] : zipFileName.replace(/\.zip$/i, '');
    const safeName = name.replace(/[^A-Za-z0-9._-]/g, '_');
    return paths.map((p) => ({
      path: PLUGIN_ROOT + '/' + safeName + '/' + (singleFolder ? p.split('/').slice(1).join('/') : p),
      plugin: safeName,
      zipPath: p,
    }));
  }

  async function installPluginZip() {
    const input = document.getElementById('pluginZip');
    const button = document.getElementById('pluginInstallBtn');
    const file = input && input.files && input.files[0];
    if (!file) {
      pluginInstallStatus('Choose a .zip first.', true);
      return;
    }
    if (typeof JSZip === 'undefined') {
      pluginInstallStatus('JSZip failed to load; reload the page and try again.', true);
      return;
    }

    button.disabled = true;
    input.disabled = true;
    try {
      pluginInstallStatus('Reading archive...');
      const zip = await JSZip.loadAsync(file);
      const paths = Object.keys(zip.files).filter((p) => !zip.files[p].dir && isInstallableEntry(p));
      if (!paths.length) {
        pluginInstallStatus('That archive has no installable files.', true);
        return;
      }

      const plan = planPluginInstall(paths, file.name);
      const installed = new Set();
      // One at a time: the device serves a single connection, and concurrent
      // writes would queue behind each other anyway while holding SD locks.
      for (let i = 0; i < plan.length; i++) {
        const entry = plan[i];
        pluginInstallStatus('Installing ' + (i + 1) + '/' + plan.length + ': ' + entry.zipPath);
        const bytes = await zip.files[entry.zipPath].async('uint8array');
        if (bytes.length > MAX_PLUGIN_FILE_BYTES) {
          throw new Error(entry.zipPath + ' is too large to install over the web (' +
            Math.round(bytes.length / 1024) + 'KB; limit ' + Math.round(MAX_PLUGIN_FILE_BYTES / 1024) + 'KB)');
        }
        await CrossPoint.writeFile(entry.plugin, entry.path, bytesToBase64(bytes));
        installed.add(entry.plugin);
      }

      pluginInstallStatus('Installed ' + Array.from(installed).join(', ') + ' (' + plan.length + ' files). Reloading...');
      // Discovery is per-request, so a reload is all it takes for the new
      // plugin's own card to appear below.
      setTimeout(() => window.location.reload(), 1200);
    } catch (e) {
      pluginInstallStatus('Install failed: ' + ((e && e.message) || e), true);
    } finally {
      button.disabled = false;
      input.disabled = false;
    }
  }
