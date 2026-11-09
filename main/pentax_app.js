(function () {
'use strict';

var API = '/api/pentax';
var SD_API = '/api/sd';

/* ---------------------------- small formatting helpers ---------------------------- */

function fmtShutter(nom, denom) {
  if (!denom) return '—';
  var sec = nom / denom;
  if (sec >= 1) return (Math.round(sec * 10) / 10) + 's';
  return '1/' + Math.round(denom / nom);
}
function fmtAperture(nom, denom) {
  if (!denom) return '—';
  return 'F' + (nom / denom).toFixed(1).replace(/\.0$/, '');
}
function fmtEv(nom, denom) {
  if (!denom) return '—';
  var v = nom / denom;
  return (v > 0 ? '+' : '') + v.toFixed(1);
}
function fmtFocal(nom, denom) {
  if (!denom) return '—';
  return Math.round(nom / denom) + 'mm';
}

/* Standard-ish stepping tables, used only to move +/- one notch from the
 * camera's current reported value; the camera itself is the source of
 * truth for the actual value. */
var SHUTTER_TABLE = [
  [1, 4000], [1, 3200], [1, 2500], [1, 2000], [1, 1600], [1, 1250], [1, 1000],
  [1, 800], [1, 640], [1, 500], [1, 400], [1, 320], [1, 250], [1, 200], [1, 160],
  [1, 125], [1, 100], [1, 80], [1, 60], [1, 50], [1, 40], [1, 30], [1, 25], [1, 20],
  [1, 15], [1, 13], [1, 10], [1, 8], [1, 6], [1, 5], [1, 4], [3, 10], [2, 5], [1, 2],
  [3, 5], [4, 5], [1, 1], [13, 10], [16, 10], [2, 1], [25, 10], [3, 1], [4, 1], [5, 1],
  [6, 1], [8, 1], [10, 1], [13, 1], [15, 1], [20, 1], [25, 1], [30, 1]
];
var APERTURE_TABLE = [10, 12, 14, 17, 20, 24, 28, 33, 40, 48, 56, 67, 80, 95, 110, 130, 160, 190, 220, 270, 320];
var ISO_TABLE = [100, 200, 400, 800, 1600];

function nearestIndex(table, valueFn, cur) {
  var best = 0, bestDiff = Infinity;
  for (var i = 0; i < table.length; i++) {
    var d = Math.abs(valueFn(table[i]) - cur);
    if (d < bestDiff) { bestDiff = d; best = i; }
  }
  return best;
}

/* ------------------------------------ networking ----------------------------------- */

function postJSON(path, body) {
  return fetch(API + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {}),
  }).then(function (r) { return r.json().catch(function () { return {}; }); });
}
function postParam(param, extra) {
  var body = Object.assign({ param: param }, extra || {});
  return postJSON('/param', body);
}
function postEmpty(path) {
  return fetch(API + path, { method: 'POST' }).then(function (r) { return r.json().catch(function () { return {}; }); });
}

/* ------------------------------------ state ----------------------------------------- */

var lastStatus = null;
var selectedSlot = 0;
var sdPath = '/';

/* ------------------------------------ rendering ------------------------------------- */

function setSeg(id, activeValue) {
  var el = document.getElementById(id);
  if (!el) return;
  var btns = el.querySelectorAll('button');
  for (var i = 0; i < btns.length; i++) {
    btns[i].classList.toggle('active', Number(btns[i].dataset.value) === Number(activeValue));
  }
}

function render(st) {
  lastStatus = st;

  var dot = document.getElementById('stateDot');
  dot.className = 'dot' + (st.state === 'ready' ? ' ok' : st.state === 'busy' ? ' busy' : '');

  var disconnectedBanner = document.getElementById('disconnectedBanner');
  var wrongModeBanner = document.getElementById('wrongModeBanner');
  var ready = document.getElementById('cameraReady');
  disconnectedBanner.hidden = st.state !== 'disconnected';
  wrongModeBanner.hidden = st.state !== 'wrong_usb_mode';
  ready.hidden = (st.state === 'disconnected' || st.state === 'wrong_usb_mode');

  var errRow = document.getElementById('errRow');
  if (st.last_error) {
    errRow.hidden = false;
    document.getElementById('errText').textContent = st.last_error;
  } else {
    errRow.hidden = true;
  }

  if (st.params_valid && st.params) {
    renderParams(st.params);
  }
  renderWriteOnly(st.write_only || {});
  renderBuffer(st.buffers || []);
  renderSequence(st.sequence || {});

  var sdBadge = document.getElementById('sdBadge');
  var sdAvailable = st.sd && st.sd.mounted;
  sdBadge.textContent = sdAvailable ? 'available' : 'unavailable';
  sdBadge.className = 'badge ' + (sdAvailable ? 'badge-ok' : 'badge-unavailable');

  document.getElementById('rawStatus').textContent = JSON.stringify(st, null, 2);
}

function renderParams(p) {
  document.getElementById('valShutter').textContent = fmtShutter(p.shutter_speed.nom, p.shutter_speed.denom);
  document.getElementById('valAperture').textContent = fmtAperture(p.aperture.nom, p.aperture.denom);
  document.getElementById('valIso').textContent = p.iso;
  document.getElementById('valEv').textContent = fmtEv(p.ev_comp.nom, p.ev_comp.denom);

  document.getElementById('lensFocal').textContent = fmtFocal(p.focal_length.nom, p.focal_length.denom);
  document.getElementById('lensAperture').textContent =
    fmtAperture(p.lens_max_aperture.nom, p.lens_max_aperture.denom) + '–' +
    fmtAperture(p.lens_min_aperture.nom, p.lens_min_aperture.denom);
  document.getElementById('lensSR').textContent = p.shake_reduction ? 'ON' : 'OFF';

  setSeg('segFormat', p.image_format);
  setSeg('segRawType', p.raw_format);
  setSeg('segJpegRes', p.jpeg_resolution_idx);
  setSeg('segStars', p.jpeg_stars);
  setSeg('segCustomImage', p.custom_image_tone);
  setSeg('segAfMode', p.af_mode);
  setSeg('segAfArea', p.af_area_select);
  setSeg('segDrive', p.drive_mode);

  document.getElementById('sldSat').value = p.saturation;
  document.getElementById('sldSharp').value = p.sharpness;
  document.getElementById('sldContrast').value = p.contrast;
  document.getElementById('sldHue').value = p.jpeg_hue;

  renderAfGrid(p.af_point_mask);
}

function renderAfGrid(mask) {
  var grid = document.getElementById('afGrid');
  if (grid.children.length !== 11) {
    grid.innerHTML = '';
    for (var i = 0; i < 11; i++) {
      var d = document.createElement('div');
      d.className = 'af-dot';
      d.dataset.bit = i;
      d.onclick = (function (bit) {
        return function () { postParam('af_point', { value: 1 << bit }); };
      })(i);
      grid.appendChild(d);
    }
  }
  for (var j = 0; j < 11; j++) {
    grid.children[j].classList.toggle('active', (mask & (1 << j)) !== 0);
  }
}

function renderWriteOnly(wo) {
  if (wo.white_balance) {
    setSeg('segWb', wo.white_balance.mode);
    document.getElementById('wbNote').textContent = wo.white_balance.set
      ? 'Записано (без подтверждения): mode ' + wo.white_balance.mode
      : 'Ещё не задано на этой сессии';
  }
  if (wo.ae_metering) {
    setSeg('segMetering', wo.ae_metering.mode);
  }
  if (wo.flash_mode) {
    document.getElementById('flashMode').value = wo.flash_mode.mode;
  }
  if (wo.flash_ev_comp && wo.flash_ev_comp.denom) {
    document.getElementById('flashEv').value = (wo.flash_ev_comp.nom / wo.flash_ev_comp.denom).toFixed(1);
  }
}

function renderBuffer(buffers) {
  var grid = document.getElementById('bufGrid');
  if (grid.children.length !== 10) {
    grid.innerHTML = '';
    for (var i = 0; i < 10; i++) {
      var d = document.createElement('div');
      d.className = 'buf-slot';
      d.textContent = i;
      d.dataset.slot = i;
      d.onclick = (function (slot) { return function () { selectSlot(slot); }; })(i);
      grid.appendChild(d);
    }
  }
  for (var j = 0; j < 10; j++) {
    grid.children[j].classList.toggle('filled', !!buffers[j]);
    grid.children[j].classList.toggle('selected', j === selectedSlot);
  }
}

function selectSlot(slot) {
  selectedSlot = slot;
  document.getElementById('bufSelectedLabel').textContent = slot;
  document.getElementById('linkBufFull').href = API + '/photo?slot=' + slot + '&type=full';
  document.getElementById('linkBufPreview').href = API + '/photo?slot=' + slot + '&type=preview';
  if (lastStatus) renderBuffer(lastStatus.buffers || []);
}

function renderSequence(seq) {
  var el = document.getElementById('seqProgress');
  if (seq.running) {
    el.textContent = 'Выполняется: ' + seq.kind + ' (' + seq.done + ' / ' + seq.total + ')';
  } else {
    el.textContent = 'Серия не выполняется';
  }
}

/* ------------------------------------ status loop ----------------------------------- */

function fetchStatus() {
  fetch(API + '/status').then(function (r) { return r.json(); }).then(render).catch(function () {});
}

function connectWs() {
  var ws;
  try { ws = new WebSocket('ws://' + location.host + '/ws'); } catch (e) { return; }
  ws.onmessage = function (e) {
    try {
      var partial = JSON.parse(e.data);
      if (lastStatus) {
        lastStatus.state = partial.state;
        lastStatus.buffer_mask = partial.buffer_mask;
        lastStatus.bytes_done = partial.bytes_done;
        lastStatus.bytes_total = partial.bytes_total;
        lastStatus.last_error = partial.last_error;
        render(lastStatus);
      }
    } catch (err) { /* ignore */ }
  };
  ws.onclose = function () { setTimeout(connectWs, 3000); };
}

/* ------------------------------------ wiring: tabs ----------------------------------- */

document.querySelectorAll('.tab').forEach(function (tab) {
  tab.onclick = function () {
    document.querySelectorAll('.tab').forEach(function (t) { t.classList.remove('active'); });
    document.querySelectorAll('.panel').forEach(function (p) { p.classList.remove('active'); });
    tab.classList.add('active');
    document.querySelector('.panel[data-panel="' + tab.dataset.tab + '"]').classList.add('active');
    if (tab.dataset.tab === 'buffer') sdList(sdPath);
  };
});

/* ------------------------------------ wiring: segmented controls --------------------- */

function wireSeg(id, param, extra) {
  var el = document.getElementById(id);
  if (!el) return;
  el.querySelectorAll('button').forEach(function (btn) {
    btn.onclick = function () {
      var body = Object.assign({ value: Number(btn.dataset.value) }, extra || {});
      postParam(param, body).then(fetchStatus);
    };
  });
}
wireSeg('segFormat', 'file_format');
wireSeg('segRawType', 'raw_format');
wireSeg('segJpegRes', 'jpeg_resolution');
wireSeg('segStars', 'jpeg_quality');
wireSeg('segCustomImage', 'custom_image_tone');
wireSeg('segAfMode', 'af_mode');
wireSeg('segAfArea', 'af_area');
wireSeg('segDrive', 'drive_mode');
wireSeg('segWb', 'white_balance');
wireSeg('segMetering', 'ae_metering');

/* No wireSlider() for sldSat/sldSharp/sldContrast/sldHue: the camera
 * doesn't have a confirmed set command for them (see the hint text next to
 * those sliders), so they are display-only (disabled in the HTML). */

/* ------------------------------------ wiring: steppers -------------------------------- */

document.querySelectorAll('.stepbtn').forEach(function (btn) {
  btn.onclick = function () {
    if (!lastStatus || !lastStatus.params) return;
    var dir = Number(btn.dataset.dir);
    var p = lastStatus.params;
    var name = btn.dataset.param;
    if (name === 'shutter_speed') {
      var cur = p.shutter_speed.nom / p.shutter_speed.denom;
      var idx = nearestIndex(SHUTTER_TABLE, function (e) { return e[0] / e[1]; }, cur);
      idx = Math.max(0, Math.min(SHUTTER_TABLE.length - 1, idx + dir));
      postParam('shutter_speed', { nom: SHUTTER_TABLE[idx][0], denom: SHUTTER_TABLE[idx][1] }).then(fetchStatus);
    } else if (name === 'aperture') {
      var curF = p.aperture.nom / p.aperture.denom;
      var idx2 = nearestIndex(APERTURE_TABLE, function (e) { return e / 10; }, curF);
      idx2 = Math.max(0, Math.min(APERTURE_TABLE.length - 1, idx2 + dir));
      postParam('aperture', { nom: APERTURE_TABLE[idx2], denom: 10 }).then(fetchStatus);
    } else if (name === 'iso') {
      var idx3 = nearestIndex(ISO_TABLE, function (e) { return e; }, p.iso);
      idx3 = Math.max(0, Math.min(ISO_TABLE.length - 1, idx3 + dir));
      postParam('iso', { value: ISO_TABLE[idx3], auto_min: p.auto_iso_min, auto_max: p.auto_iso_max }).then(fetchStatus);
    } else if (name === 'ev_comp') {
      var curEv = Math.round((p.ev_comp.nom / p.ev_comp.denom) * 10) + dir * 3;
      curEv = Math.max(-30, Math.min(30, curEv));
      postParam('ev_comp', { nom: curEv, denom: 10 }).then(fetchStatus);
    }
  };
});

/* ------------------------------------ wiring: actions --------------------------------- */

document.getElementById('btnAf').onclick = function () { postEmpty('/af').then(fetchStatus); };
document.getElementById('btnRelease').onclick = function () {
  var b = document.getElementById('btnRelease');
  b.disabled = true;
  postEmpty('/capture').then(fetchStatus).then(function () { b.disabled = false; });
};
document.getElementById('btnGreen').onclick = function () { postEmpty('/green-button').then(fetchStatus); };
document.getElementById('btnDust').onclick = function () { postEmpty('/dust-removal').then(fetchStatus); };
document.getElementById('btnBulb').onclick = function () {
  var seconds = Number(document.getElementById('bulbSeconds').value) || 30;
  var b = document.getElementById('btnBulb');
  b.disabled = true;
  b.textContent = 'Экспозиция...';
  postJSON('/bulb', { duration_ms: seconds * 1000 }).then(fetchStatus).then(function () {
    b.disabled = false;
    b.textContent = '⏱ Старт Bulb';
  });
};
document.getElementById('tetherCheck').onchange = function () {
  postJSON('/tether', { connect: this.checked });
};
document.getElementById('btnWbManualApply').onclick = function () {
  var mode = Number(document.getElementById('wbModeManual').value) || 0;
  postParam('white_balance', { value: mode }).then(fetchStatus);
};
document.getElementById('btnFlashApply').onclick = function () {
  var mode = Number(document.getElementById('flashMode').value) || 0;
  var ev = Number(document.getElementById('flashEv').value) || 0;
  postParam('flash_mode', { value: mode }).then(function () {
    return postParam('flash_ev_comp', { nom: Math.round(ev * 10), denom: 10 });
  }).then(fetchStatus);
};

document.getElementById('btnIntervalStart').onclick = function () {
  var count = Number(document.getElementById('intervalCount').value) || 1;
  var seconds = Number(document.getElementById('intervalSeconds').value) || 0;
  postJSON('/intervalometer', { count: count, interval_ms: seconds * 1000 }).then(fetchStatus);
};
document.getElementById('btnBracketStart').onclick = function () {
  var raw = document.getElementById('bracketEvs').value.split(',');
  var ev = [];
  raw.forEach(function (s) {
    var v = parseFloat(s);
    if (!isNaN(v)) ev.push({ nom: Math.round(v * 10), denom: 10 });
  });
  if (ev.length === 0) return;
  postJSON('/bracket', { ev: ev }).then(fetchStatus);
};
document.getElementById('btnSeqStop').onclick = function () { postEmpty('/sequence/stop').then(fetchStatus); };

document.getElementById('btnBufDelete').onclick = function () {
  if (!confirm('Удалить снимок из слота ' + selectedSlot + '?')) return;
  fetch(API + '/photo?slot=' + selectedSlot, { method: 'DELETE' }).then(fetchStatus);
};

/* ------------------------------------ SD card browser ---------------------------------- */

function sdBreadcrumb(path) {
  var el = document.getElementById('sdBreadcrumb');
  el.textContent = path;
}

function sdList(path) {
  sdPath = path;
  sdBreadcrumb(path);
  fetch(SD_API + '/list?path=' + encodeURIComponent(path))
    .then(function (r) { if (!r.ok) throw new Error('sd'); return r.json(); })
    .then(function (data) {
      var list = document.getElementById('sdList');
      list.innerHTML = '';
      if (path !== '/') {
        var up = document.createElement('div');
        up.className = 'sd-row';
        up.textContent = '⬆ ..';
        up.onclick = function () {
          var parent = path.replace(/\/[^/]*$/, '') || '/';
          sdList(parent);
        };
        list.appendChild(up);
      }
      (data.entries || []).forEach(function (e) {
        var row = document.createElement('div');
        row.className = 'sd-row';
        var full = (path === '/' ? '' : path) + '/' + e.name;
        if (e.is_dir) {
          row.innerHTML = '<span>📁 ' + e.name + '</span>';
          row.onclick = function () { sdList(full); };
        } else {
          row.innerHTML = '<span>🖼 ' + e.name + '</span><span class="size">' +
            Math.round(e.size / 1024) + ' KB</span>';
          row.onclick = function () { window.open(SD_API + '/file?path=' + encodeURIComponent(full), '_blank'); };
        }
        list.appendChild(row);
      });
    })
    .catch(function () {
      document.getElementById('sdList').innerHTML = '<div class="hint">SD-карта недоступна</div>';
    });
}

/* ------------------------------------ OTA ----------------------------------------------- */

document.getElementById('btnOtaUpload').onclick = function () {
  var input = document.getElementById('otaFile');
  var status = document.getElementById('otaStatus');
  if (!input.files.length) {
    status.textContent = 'Выберите файл .bin';
    return;
  }
  status.textContent = 'Загрузка...';
  fetch('/api/ota', { method: 'POST', body: input.files[0] })
    .then(function (r) { return r.json(); })
    .then(function (j) {
      status.textContent = j.ok ? 'Прошивка принята, устройство перезагружается...' : ('Ошибка: ' + j.error);
    })
    .catch(function () {
      status.textContent = 'Соединение прервано — вероятно, устройство перезагружается.';
    });
};

/* ------------------------------------ start --------------------------------------------- */

selectSlot(0);
fetchStatus();
connectWs();
setInterval(fetchStatus, 4000);
})();
