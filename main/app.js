(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const statusBadge = $("statusBadge");
  const camModel = $("camModel");
  const camSerial = $("camSerial");
  const camStorage = $("camStorage");
  const camError = $("camError");
  const camHint = $("camHint");
  const captureBtn = $("captureBtn");
  const captureStatus = $("captureStatus");
  const gallery = $("gallery");
  const loadMoreBtn = $("loadMoreBtn");
  const refreshBtn = $("refreshBtn");
  const propsRefreshBtn = $("propsRefreshBtn");
  const propsList = $("props");
  const viewer = $("viewer");
  const viewerImg = $("viewerImg");
  const viewerClose = $("viewerClose");
  const viewerDownload = $("viewerDownload");
  const viewerDelete = $("viewerDelete");

  const PAGE_SIZE = 24;
  let galleryOffset = 0;
  let galleryTotal = 0;
  let lastState = null;
  let currentViewerHandle = null;

  function fmtBytes(n) {
    if (!n || n <= 0) return "–";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return n.toFixed(i === 0 ? 0 : 1) + " " + units[i];
  }

  async function refreshStatus() {
    try {
      const r = await fetch("/api/status");
      const s = await r.json();
      lastState = s.state;

      statusBadge.className = "badge " + ({
        ready: "badge-ready",
        busy: "badge-busy",
        disconnected: "badge-unknown",
        not_ptp: "badge-bad",
        error: "badge-bad",
      }[s.state] || "badge-unknown");
      statusBadge.textContent = {
        ready: "Camera ready",
        busy: "Busy",
        disconnected: "No camera",
        not_ptp: "Unsupported USB mode",
        error: "Camera error",
      }[s.state] || s.state;

      camModel.textContent = s.model ? (s.manufacturer + " " + s.model) : "–";
      camSerial.textContent = s.serial || "–";
      if (s.storage_info_valid) {
        camStorage.textContent = fmtBytes(s.storage_free_bytes) + " free of " + fmtBytes(s.storage_total_bytes);
      } else {
        camStorage.textContent = "–";
      }
      camError.textContent = s.last_error || "";

      if (s.state === "not_ptp") {
        camHint.textContent = "A USB device is attached but it isn't exposing a PTP/Still-Image interface. " +
          "On the K200D set Menu > [USB Connection] to [PictBridge], not [PC].";
      } else if (s.state === "disconnected") {
        camHint.textContent = "Plug the K200D into the ESP32-S3's USB port and power it on.";
      } else if (s.state === "ready" && !s.capture_supported) {
        camHint.textContent = "Camera connected, but it did not advertise remote capture support - " +
          "you may still be able to browse/download existing photos below.";
      } else {
        camHint.textContent = "";
      }

      captureBtn.disabled = !(s.state === "ready" && s.capture_supported);
    } catch (e) {
      statusBadge.className = "badge badge-bad";
      statusBadge.textContent = "offline";
    }
  }

  async function doCapture() {
    captureBtn.disabled = true;
    captureStatus.textContent = "Capturing…";
    try {
      const r = await fetch("/api/capture", { method: "POST" });
      const j = await r.json();
      if (j.ok) {
        captureStatus.textContent = j.warning ? j.warning : "Captured.";
        galleryOffset = 0;
        await loadGallery(true, true);
      } else {
        captureStatus.textContent = "Capture failed: " + (j.error || "unknown error");
      }
    } catch (e) {
      captureStatus.textContent = "Capture failed (network error).";
    }
    await refreshStatus();
    setTimeout(() => { captureStatus.textContent = ""; }, 6000);
  }

  function thumbEl(item) {
    const div = document.createElement("div");
    div.className = "thumb";
    div.dataset.handle = item.handle;
    div.dataset.filename = item.filename;
    if (item.has_thumbnail) {
      const img = document.createElement("img");
      img.loading = "lazy";
      img.src = "/api/object?handle=" + item.handle + "&kind=thumb";
      img.alt = item.filename;
      img.onerror = () => { div.innerHTML = "<div class=\"thumb-fallback\">" + item.filename + "</div>"; };
      div.appendChild(img);
    } else {
      const f = document.createElement("div");
      f.className = "thumb-fallback";
      f.textContent = item.filename;
      div.appendChild(f);
    }
    div.addEventListener("click", () => openViewer(item));
    return div;
  }

  async function loadGallery(reset, prepend) {
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
      if (prepend) {
        gallery.insertBefore(frag, gallery.firstChild);
      } else {
        gallery.appendChild(frag);
      }
      galleryOffset += (j.items || []).length;
      loadMoreBtn.style.display = galleryOffset < galleryTotal ? "" : "none";
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
    // swap in the full resolution image once it's loaded
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
    if (!confirm("Delete this photo from the camera's memory card?")) return;
    try {
      const r = await fetch("/api/object?handle=" + currentViewerHandle, { method: "DELETE" });
      const j = await r.json();
      if (j.ok) {
        const el = gallery.querySelector('.thumb[data-handle="' + currentViewerHandle + '"]');
        if (el) el.remove();
        closeViewer();
      } else {
        alert("Delete failed: " + (j.error || "unknown error"));
      }
    } catch (e) {
      alert("Delete failed (network error).");
    }
  }

  function propRow(p) {
    const row = document.createElement("div");
    row.className = "prop-row";
    const name = document.createElement("span");
    name.className = "prop-name";
    name.textContent = p.name;
    row.appendChild(name);

    let input;
    if (p.is_string || p.read_only) {
      input = document.createElement("input");
      input.type = "text";
      input.value = p.is_string ? p.value : p.value;
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
            alert("Could not set " + p.name + ": " + (j.error || "unknown error"));
          }
        } catch (e) {
          alert("Could not set " + p.name + " (network error).");
        }
      });
    }
    return row;
  }

  async function loadProps() {
    propsList.innerHTML = "<span class=\"hint-text\">Loading…</span>";
    try {
      const r = await fetch("/api/props");
      const j = await r.json();
      propsList.innerHTML = "";
      if (!j.props || j.props.length === 0) {
        propsList.innerHTML = "<span class=\"hint-text\">No camera properties reported.</span>";
        return;
      }
      j.props.forEach((p) => propsList.appendChild(propRow(p)));
    } catch (e) {
      propsList.innerHTML = "<span class=\"hint-text\">Could not load camera settings.</span>";
    }
  }

  captureBtn.addEventListener("click", doCapture);
  refreshBtn.addEventListener("click", () => { galleryOffset = 0; loadGallery(true, false); });
  loadMoreBtn.addEventListener("click", () => loadGallery(false, false));
  propsRefreshBtn.addEventListener("click", loadProps);
  viewerClose.addEventListener("click", closeViewer);
  viewerDelete.addEventListener("click", deleteCurrent);
  viewer.addEventListener("click", (e) => { if (e.target === viewer) closeViewer(); });

  refreshStatus();
  loadGallery(true, false);
  loadProps();
  setInterval(refreshStatus, 4000);
})();
