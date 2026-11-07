(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const statusBadge = $("statusBadge");
  const statusGrid = $("statusGrid");
  const camModelLine = $("camModelLine");
  const camError = $("camError");
  const camHint = $("camHint");
  const captureBtn = $("captureBtn");
  const gallery = $("gallery");
  const loadMoreBtn = $("loadMoreBtn");
  const refreshBtn = $("refreshBtn");
  const propsRefreshBtn = $("propsRefreshBtn");
  const propsList = $("props");
  const gallerySort = $("gallerySort");
  const viewer = $("viewer");
  const viewerImg = $("viewerImg");
  const viewerClose = $("viewerClose");
  const viewerDownload = $("viewerDownload");
  const viewerDelete = $("viewerDelete");
  const footerDot = $("footerDot");
  const clock = $("clock");
  const aboutCamera = $("aboutCamera");
  const aboutFw = $("aboutFw");
  const scrollToSettingsBtn = $("scrollToSettingsBtn");
  const settingsCard = $("settingsCard");

  const PAGE_SIZE = 24;
  let galleryOffset = 0;
  let galleryTotal = 0;
  let currentViewerHandle = null;
  let lastProps = [];

  function fmtBytes(n) {
    if (!n || n <= 0) return "–";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return n.toFixed(i === 0 ? 0 : 1) + " " + units[i];
  }

  /* ---------------- status glance tiles ---------------- */

  const ICONS = {
    mode: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="7" width="18" height="13" rx="2"></rect><circle cx="12" cy="13.5" r="3.2"></circle></svg>',
    focus: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="3"></circle><circle cx="12" cy="12" r="8"></circle></svg>',
    iso: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="3" width="18" height="18" rx="3"></rect><path d="M8 8v8M12 8v8M16 8l2 8"></path></svg>',
    aperture: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="9"></circle><path d="M12 3v6M12 15v6M4.2 7.5l5.2 3M14.6 13.5l5.2 3M4.2 16.5l5.2-3M14.6 10.5l5.2-3"></path></svg>',
    shutter: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="9"></circle><path d="M12 7v5l3 2"></path></svg>',
    battery: '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="2" y="7" width="18" height="10" rx="2"></rect><path d="M22 10v4"></path></svg>',
  };

  const QUICK_GLANCE = [
    { match: "Exposure program", label: "Режим съёмки", icon: ICONS.mode },
    { match: "Focus mode", label: "Режим фокусировки", icon: ICONS.focus },
    { match: "ISO", label: "ISO", icon: ICONS.iso },
    { match: "Aperture (F-number)", label: "Диафрагма", icon: ICONS.aperture },
    { match: "Shutter speed", label: "Выдержка", icon: ICONS.shutter },
    { match: "Battery level", label: "Батарея", icon: ICONS.battery, ok: true },
  ];

  function renderStatusGrid(props) {
    statusGrid.innerHTML = "";
    const byName = {};
    (props || []).forEach((p) => { byName[p.name] = p; });
    let any = false;
    QUICK_GLANCE.forEach((q) => {
      const p = byName[q.match];
      if (!p) return;
      any = true;
      const div = document.createElement("div");
      div.innerHTML =
        '<span class="stat-icon' + (q.ok ? " stat-ok" : "") + '">' + q.icon + "</span>" +
        '<span class="stat-body"><span class="label">' + q.label + '</span>' +
        '<span class="value"></span></span>';
      div.querySelector(".value").textContent = String(p.value);
      statusGrid.appendChild(div);
    });
    if (!any) {
      statusGrid.innerHTML = '<p class="hint-text">Камера не сообщает параметры съёмки.</p>';
    }
  }

  /* ---------------- status / capture ---------------- */

  async function refreshStatus() {
    try {
      const r = await fetch("/api/status");
      const s = await r.json();

      statusBadge.className = "badge " + ({
        ready: "badge-ready",
        busy: "badge-busy",
        disconnected: "badge-unknown",
        not_ptp: "badge-bad",
        error: "badge-bad",
      }[s.state] || "badge-unknown");
      statusBadge.textContent = {
        ready: "Камера подключена",
        busy: "Занято",
        disconnected: "Нет камеры",
        not_ptp: "Неподдерживаемый режим USB",
        error: "Ошибка камеры",
      }[s.state] || s.state;

      const modelStr = s.model ? (s.manufacturer + " " + s.model) : "–";
      let line = modelStr;
      if (s.serial) line += " · №" + s.serial;
      if (s.storage_info_valid) {
        line += " · " + fmtBytes(s.storage_free_bytes) + " свободно из " + fmtBytes(s.storage_total_bytes);
      }
      camModelLine.textContent = s.model ? line : "";
      aboutCamera.textContent = s.model ? modelStr : "—";
      aboutFw.textContent = s.version || "—";

      camError.textContent = s.last_error || "";

      if (s.state === "not_ptp") {
        camHint.textContent = "USB-устройство подключено, но не предоставляет интерфейс PTP/Still-Image. " +
          "В меню K200D установите [USB Connection] в [PictBridge], а не [PC].";
      } else if (s.state === "disconnected") {
        camHint.textContent = "Подключите K200D к USB-порту ESP32-S3 и включите камеру.";
      } else if (s.state === "ready" && !s.capture_supported) {
        camHint.textContent = "Камера подключена, но не сообщает о поддержке удалённой съёмки — " +
          "можно просматривать и скачивать уже снятые фото ниже.";
      } else {
        camHint.textContent = "";
      }

      captureBtn.disabled = !(s.state === "ready" && s.capture_supported);
      footerDot.className = "dot";
    } catch (e) {
      statusBadge.className = "badge badge-bad";
      statusBadge.textContent = "нет связи";
      footerDot.className = "dot dot-bad";
    }
  }

  async function doCapture() {
    captureBtn.disabled = true;
    const label = captureBtn.lastChild;
    const prevText = label.textContent;
    label.textContent = "Съёмка…";
    try {
      const r = await fetch("/api/capture", { method: "POST" });
      const j = await r.json();
      if (j.ok) {
        galleryOffset = 0;
        await loadGallery(true, true);
      } else {
        alert("Ошибка съёмки: " + (j.error || "неизвестная ошибка"));
      }
    } catch (e) {
      alert("Ошибка съёмки (сетевая ошибка).");
    }
    label.textContent = prevText;
    await refreshStatus();
  }

  /* ---------------- gallery ---------------- */

  function thumbEl(item) {
    const div = document.createElement("div");
    div.className = "thumb";
    div.dataset.handle = item.handle;
    div.dataset.filename = item.filename;
    div.dataset.captureDate = item.capture_date || "";

    const img = document.createElement("div");
    img.className = "thumb-img";
    if (item.has_thumbnail) {
      const el = document.createElement("img");
      el.loading = "lazy";
      el.src = "/api/object?handle=" + item.handle + "&kind=thumb";
      el.alt = item.filename;
      el.onerror = () => {
        img.innerHTML = '<svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="3" y="4" width="18" height="16" rx="2"></rect><circle cx="8.5" cy="9.5" r="1.5"></circle><path d="M21 16l-5.5-5.5L3 20"></path></svg>';
      };
      img.appendChild(el);
    } else {
      img.innerHTML = '<svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="3" y="4" width="18" height="16" rx="2"></rect><circle cx="8.5" cy="9.5" r="1.5"></circle><path d="M21 16l-5.5-5.5L3 20"></path></svg>';
    }
    div.appendChild(img);

    const meta = document.createElement("div");
    meta.className = "thumb-meta";
    meta.innerHTML = '<div class="fname"></div><div class="fsub"></div>';
    meta.querySelector(".fname").textContent = item.filename;
    meta.querySelector(".fsub").textContent =
      [fmtBytes(item.size), item.capture_date].filter(Boolean).join(" · ");
    div.appendChild(meta);

    div.addEventListener("click", () => openViewer(item));
    return div;
  }

  function sortGallery() {
    const mode = gallerySort.value;
    const items = Array.from(gallery.children);
    items.sort((a, b) => {
      if (mode === "name") {
        return a.dataset.filename.localeCompare(b.dataset.filename);
      }
      return (b.dataset.captureDate || "").localeCompare(a.dataset.captureDate || "");
    });
    items.forEach((el) => gallery.appendChild(el));
  }

  async function loadGallery(reset) {
    try {
      const url = "/api/objects?offset=" + galleryOffset + "&limit=" + PAGE_SIZE + (reset ? "&refresh=1" : "");
      const r = await fetch(url);
      const j = await r.json();
      galleryTotal = j.total || 0;
      if (reset) {
        gallery.innerHTML = "";
      }
      const frag = document.createDocumentFragment();
      (j.items || []).forEach((item) => frag.appendChild(thumbEl(item)));
      gallery.appendChild(frag);
      galleryOffset += (j.items || []).length;
      loadMoreBtn.style.display = galleryOffset < galleryTotal ? "" : "none";
      sortGallery();
    } catch (e) {
      /* camera likely not connected yet; leave gallery as-is */
    }
  }

  function openViewer(item) {
    currentViewerHandle = item.handle;
    viewerImg.src = "/api/object?handle=" + item.handle + "&kind=thumb";
    viewerDownload.href = "/api/object?handle=" + item.handle;
    viewerDownload.setAttribute("download", item.filename || "photo");
    viewer.classList.remove("hidden");
    const full = new Image();
    full.onload = () => { if (currentViewerHandle === item.handle) viewerImg.src = full.src; };
    full.src = "/api/object?handle=" + item.handle;
  }

  function closeViewer() {
    viewer.classList.add("hidden");
    viewerImg.src = "";
    currentViewerHandle = null;
  }

  async function deleteCurrent() {
    if (currentViewerHandle == null) return;
    if (!confirm("Удалить этот снимок с карты памяти камеры?")) return;
    try {
      const r = await fetch("/api/object?handle=" + currentViewerHandle, { method: "DELETE" });
      const j = await r.json();
      if (j.ok) {
        const el = gallery.querySelector('.thumb[data-handle="' + currentViewerHandle + '"]');
        if (el) el.remove();
        closeViewer();
      } else {
        alert("Ошибка удаления: " + (j.error || "неизвестная ошибка"));
      }
    } catch (e) {
      alert("Ошибка удаления (сетевая ошибка).");
    }
  }

  /* ---------------- camera settings ---------------- */

  function settingRow(p) {
    const row = document.createElement("div");
    row.className = "setting-row";
    const label = document.createElement("label");
    label.textContent = p.name;
    row.appendChild(label);

    let input;
    if (p.is_string || p.read_only) {
      input = document.createElement("input");
      input.type = "text";
      input.value = p.value;
      input.disabled = true;
    } else if (p.enum && p.enum.length > 0) {
      input = document.createElement("select");
      p.enum.forEach((v) => {
        const opt = document.createElement("option");
        opt.value = v;
        opt.textContent = v;
        if (v === p.value) opt.selected = true;
        input.appendChild(opt);
      });
    } else if (p.min !== undefined && p.max !== undefined) {
      input = document.createElement("input");
      input.type = "number";
      input.min = p.min;
      input.max = p.max;
      input.step = p.step || 1;
      input.value = p.value;
    } else {
      input = document.createElement("input");
      input.type = "number";
      input.value = p.value;
    }
    row.appendChild(input);

    if (!p.read_only) {
      input.addEventListener("change", async () => {
        try {
          const r = await fetch("/api/props", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ code: p.code, value: Number(input.value) }),
          });
          const j = await r.json();
          if (!j.ok) {
            alert("Не удалось установить «" + p.name + "»: " + (j.error || "неизвестная ошибка"));
          }
        } catch (e) {
          alert("Не удалось установить «" + p.name + "» (сетевая ошибка).");
        }
      });
    }
    return row;
  }

  async function loadProps() {
    propsList.innerHTML = '<span class="hint-text">Загрузка…</span>';
    try {
      const r = await fetch("/api/props");
      const j = await r.json();
      lastProps = j.props || [];
      propsList.innerHTML = "";
      if (lastProps.length === 0) {
        propsList.innerHTML = '<span class="hint-text">Камера не сообщает о настройках.</span>';
      } else {
        lastProps.forEach((p) => propsList.appendChild(settingRow(p)));
      }
      renderStatusGrid(lastProps);
    } catch (e) {
      propsList.innerHTML = '<span class="hint-text">Не удалось загрузить настройки камеры.</span>';
      renderStatusGrid([]);
    }
  }

  /* ---------------- nav / tabs ---------------- */

  const navItems = document.querySelectorAll(".nav-item");
  const panels = document.querySelectorAll(".panel");
  navItems.forEach((t) => t.addEventListener("click", () => {
    navItems.forEach((x) => x.classList.remove("active"));
    panels.forEach((p) => p.classList.remove("active"));
    t.classList.add("active");
    document.querySelector('.panel[data-panel="' + t.dataset.tab + '"]').classList.add("active");
  }));

  const subtabs = document.querySelectorAll(".subtab");
  const subpanels = document.querySelectorAll(".subpanel");
  subtabs.forEach((t) => t.addEventListener("click", () => {
    subtabs.forEach((x) => x.classList.remove("active"));
    subpanels.forEach((p) => p.classList.remove("active"));
    t.classList.add("active");
    document.querySelector('.subpanel[data-subpanel="' + t.dataset.subtab + '"]').classList.add("active");
  }));

  scrollToSettingsBtn.addEventListener("click", () => {
    settingsCard.scrollIntoView({ behavior: "smooth", block: "start" });
  });

  /* ---------------- clock ---------------- */

  function tickClock() {
    clock.textContent = new Date().toLocaleTimeString("ru-RU");
  }

  captureBtn.addEventListener("click", doCapture);
  refreshBtn.addEventListener("click", () => { galleryOffset = 0; loadGallery(true); });
  loadMoreBtn.addEventListener("click", () => loadGallery(false));
  gallerySort.addEventListener("change", sortGallery);
  propsRefreshBtn.addEventListener("click", loadProps);
  viewerClose.addEventListener("click", closeViewer);
  viewerDelete.addEventListener("click", deleteCurrent);
  viewer.addEventListener("click", (e) => { if (e.target === viewer) closeViewer(); });

  refreshStatus();
  loadGallery(true);
  loadProps();
  tickClock();
  setInterval(refreshStatus, 4000);
  setInterval(tickClock, 1000);
})();
