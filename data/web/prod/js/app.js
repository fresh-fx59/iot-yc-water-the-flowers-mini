const $ = (s) => document.querySelector(s);
const fmtTs = (u) => u ? new Date(u * 1000).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : 'never';

async function refresh() {
  try {
    const r = await fetch('/api/status');
    const j = await r.json();
    $('#version').textContent = 'v' + j.version;
    $('#state').textContent = j.state;
    $('#motor').textContent = j.pump ? 'ON' : 'off';
    const pct = (j.soil.pct === -1) ? 'uncalibrated' : (j.soil.pct + '%');
    $('#soil').textContent = `raw=${j.soil.raw} (${pct}, threshold=${j.soil.threshold})`;
    const ov = j.overflow;
    $('#overflow').textContent = ov.detected
      ? `LATCHED (raw=${ov.raw_value}, streak=${ov.trigger_streak})`
      : `clear (streak=${ov.trigger_streak})`;
    $('#overflow').className = ov.detected ? 'alert' : 'ok';
    $('#halted').textContent = j.halted ? 'HALTED' : 'no';
    $('#last-run').textContent = fmtTs(j.schedule.last_run_unix);
    $('#next-run').textContent = fmtTs(j.schedule.next_run_unix);
    $('#skips').textContent = j.schedule.consecutive_skips_wet;
    $('#cal-current').textContent = j.soil.raw;
  } catch (e) {
    console.error(e);
  }
}

document.querySelectorAll('#controls button').forEach((b) => {
  b.addEventListener('click', async () => {
    const r = await fetch('/api/' + b.dataset.act, { method: 'POST' });
    if (!r.ok) {
      try {
        const err = await r.json();
        alert('error: ' + (err.error || r.status));
      } catch {
        alert('error: ' + r.status);
      }
    }
    refresh();
  });
});

document.querySelectorAll('#calibration button').forEach((b) => {
  b.addEventListener('click', async () => {
    const r = await fetch('/api/calibrate?ref=' + b.dataset.cal, { method: 'POST' });
    if (!r.ok) alert('calibration error: ' + r.status);
    refresh();
  });
});

$('#settings-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const fd = new FormData(e.target);
  const partial = {};
  for (const [k, v] of fd.entries()) partial[k] = parseInt(v, 10);
  // Fetch full current settings so the POST body contains all 7 required fields.
  const cur = await (await fetch('/api/settings')).json();
  const r = await fetch('/api/settings', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ ...cur, ...partial }),
  });
  if (!r.ok) {
    try {
      const err = await r.json();
      alert('settings error: ' + (err.error || r.status));
    } catch {
      alert('settings error: ' + r.status);
    }
  }
  refresh();
});

(async () => {
  try {
    const cur = await (await fetch('/api/settings')).json();
    for (const [k, v] of Object.entries(cur)) {
      const el = document.querySelector(`[name=${k}]`);
      if (el) el.value = v;
    }
  } catch (e) {
    console.error('settings hydrate failed', e);
  }
})();

setInterval(refresh, 1000);
refresh();
