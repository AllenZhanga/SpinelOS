(() => {
  const params = new URLSearchParams(window.location.search);
  const mode = params.get("mode") || "hub";

  const topBar = document.getElementById("top-bar");
  const hub = document.getElementById("desktop-hub");

  if (mode === "bar") {
    topBar.classList.remove("hidden");
  } else {
    hub.classList.remove("hidden");
  }

  function sendCommand(cmd) {
    if (!cmd) return;
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.sysbridge) {
      window.webkit.messageHandlers.sysbridge.postMessage(cmd);
    } else {
      console.log("[sysbridge mock]", cmd);
    }
  }

  document.addEventListener("click", (ev) => {
    const target = ev.target.closest("[data-cmd]");
    if (!target) return;
    ev.stopPropagation();
    sendCommand(target.getAttribute("data-cmd"));
  });

  function updateClock() {
    const el = document.getElementById("bar-clock");
    if (!el) return;
    const now = new Date();
    const hh = String(now.getHours()).padStart(2, "0");
    const mm = String(now.getMinutes()).padStart(2, "0");
    el.textContent = `${hh}:${mm}`;
  }

  window.__spinelUpdateTelemetry = (data) => {
    const set = (id, val) => {
      const el = document.getElementById(id);
      if (el && val) el.textContent = val;
    };
    set("bar-net", `网络: ${data.net}`);
    set("bar-mem", `内存: ${data.mem}`);
    set("hub-gpu", data.gpu);
    set("hub-mem", data.mem);
    set("hub-net", data.net);
    set("hub-kernel", data.kernel);
  };

  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
