// Project AURA — telemetry front end.
// Data sources, tried in order:
//   1. /api/telemetry — the PHP backend (backend-php/), which also stores what the watch sends
//   2. data.json      — static file, used when the page is served without the backend
// Both return the same JSON shape: { meta, astronauts, vitals, readings }.
const DATA_SOURCES = ['/api/telemetry', 'data.json'];
const LIVE_POLL_MS = 5000;    // how often the backend is asked for new watch readings
const LIVE_FRESH_MS = 2 * 60 * 1000;  // watch data older than this is ignored: back to the demo baseline
const ALERTS_URL = '/api/alerts';
const SIMULATOR_URL = '/api/simulator';
const ACK_OPERATOR = 'Mission Control';  // name stored with each acknowledgment
const ALERT_LOG_ROWS = 6;     // alerts shown in the archive panel

// How each card on the page maps to the vitals in the data file.
// The card ids (blood_pressure, sleep_mood) are the ones used in index.html.
const CARD_MAP = {
  heart_rate:     { vitals: ['heart_rate'],                 hotspotId: 'hotspot-cardiovascular' },
  blood_pressure: { vitals: ['systolic_bp', 'diastolic_bp'], hotspotId: 'hotspot-cardiovascular', name: 'Blood Pressure' },
  blood_glucose:  { vitals: ['blood_glucose'],              hotspotId: 'hotspot-immune' },
  body_temp:      { vitals: ['body_temp'],                  hotspotId: 'hotspot-immune' },
  bone_load:      { vitals: ['bone_load'],                  hotspotId: 'hotspot-bone_gravity' },
  sleep_mood:     { vitals: ['sleep_hours'],                hotspotId: 'hotspot-behavioral' },
  radiation_dose: { vitals: ['radiation_dose'],             hotspotId: 'hotspot-immune' }
};

// Vitals without a card that still count toward the readiness ring and body hotspots.
// Format: { vital_id: { hotspotId: '...' } }. Empty now that radiation has its own card.
const EXTRA_VITALS = {};

const ZONE_LABELS = {
  cardiovascular: 'Cardiovascular',
  nutrition: 'Metabolism',
  immune_radiation: 'Radiation & Immuno',
  behavioral: 'Isolation & Confinement',
  bone_gravity: 'Gravity & Musculoskeletal'
};

const UNIT_LABELS = { C: '°C' };
const DECIMALS = { body_temp: 1, sleep_hours: 1, radiation_dose: 2 };  // other vitals: whole numbers
const STATUS_RANK = { nominal: 0, warning: 1, critical: 2 };
const STATUS_SCORE = { nominal: 100, warning: 65, critical: 30 };
const REPLAY_STEP_MS = 700;   // one simulated hour per step
const SPARK_POINTS = 12;      // hours of history drawn in each sparkline

let DATA = null;              // { astronauts, vitals, readings }
let VITALS = {};              // vital_id -> vital row
// Readings are split in two sets:
//   scenario → the demo data (day 1 baseline + last day solar-event replay)
//   live     → readings sent by the watch (source === 'watch'), only when the backend is used
let SCENARIO_SERIES = {};     // astronaut_id -> vital_id -> readings sorted by time
let LIVE_SERIES = {};
let LIVE_LATEST_TIME = null;  // time of the newest watch reading, or null
let SERVER_NOW = null;        // server clock (meta.generated_at), so a wrong PC clock does not matter
let dataSource = null;        // which entry of DATA_SOURCES answered
let mode = 'scenario';        // 'scenario' | 'live'
let ALERTS = [];              // from GET /api/alerts (empty without the backend)
let activeModalAlertId = null;
let simState = null;          // from GET /api/simulator
let ASTRO_BY_PROFILE = {};    // 'male' | 'female' -> astronaut row
let BASELINE_TIME = null;     // last hour of day 1
let REPLAY_TIMES = [];        // every hour of the last day

let currentGender = 'male';
let currentFilter = 'all';
let currentTime = null;
let replayTimer = null;
let isCrisisActive = false;
let activeModalMetricId = null;
const inspectedNodes = new Set();

document.addEventListener('DOMContentLoaded', async () => {
  bindFilterEvents();
  bindGenderEvents();
  setupCardEvents();
  setupInteractiveBodyNodes();
  setupModalEvents();
  setupAlertLogEvents();
  setupWatchSimulator();

  try {
    await loadData();
  } catch (err) {
    console.error(err);
    updateTerminalFeed('TELEMETRY LINK DOWN: no data source answered. Serve the site over HTTP (not file://).', true);
    return;
  }

  showDefaultView();
  applyFilter(currentFilter);
  const origin = dataSource === 'data.json' ? 'static data.json' : 'AURA backend';
  updateTerminalFeed(
    mode === 'live'
      ? `Live downlink (${origin}) — ${DATA.astronauts.length} crew nodes. Last watch packet ${formatTime(currentTime)}.`
      : `Telemetry synchronized (${origin}) — ${DATA.astronauts.length} crew nodes online. Last sync ${formatTime(currentTime)}.`
  );

  if (dataSource !== 'data.json') {
    setInterval(pollLive, LIVE_POLL_MS);
    refreshSimulatorStatus();
  }
});

// Live watch data when there is some, otherwise the day 1 baseline of the scenario.
// True when the watch sent something recently. Old test readings left in the database
// must not replace the demo baseline forever.
function liveIsFresh() {
  if (!LIVE_LATEST_TIME) return false;
  const now = Date.parse(SERVER_NOW || new Date().toISOString());
  return now - Date.parse(LIVE_LATEST_TIME) <= LIVE_FRESH_MS;
}

// Live watch data when the watch is currently sending, otherwise the day 1 baseline of the scenario.
function showDefaultView() {
  mode = liveIsFresh() ? 'live' : 'scenario';
  currentTime = mode === 'live' ? LIVE_LATEST_TIME : BASELINE_TIME;
  updateModePill(mode);
  renderVitals();
}

function updateModePill(state) {
  const pill = document.getElementById('mode-pill');
  if (!pill) return;
  const labels = { live: 'Live Watch Downlink', scenario: 'Mission Baseline · Demo Data', replay: 'Solar Event Replay · Demo Data' };
  pill.textContent = labels[state];
  pill.classList.toggle('mode-live', state === 'live');
  pill.classList.toggle('mode-replay', state === 'replay');
}

// Asks the backend for new watch readings. Paused while the solar-event replay is on screen.
async function pollLive() {
  refreshSimulatorStatus();
  if (isCrisisActive) return;
  const previousLatest = LIVE_LATEST_TIME;
  const previousMode = mode;
  try {
    await loadData();
  } catch (err) {
    console.warn('Live poll failed', err);
    return;
  }
  if (isCrisisActive) return;

  const newPacket = LIVE_LATEST_TIME !== previousLatest;
  const nextMode = liveIsFresh() ? 'live' : 'scenario';
  if (!newPacket && nextMode === previousMode) return;

  showDefaultView();
  if (mode === 'live') {
    updateTerminalFeed(`${LIVE_LATEST_TIME.slice(11, 19)} UTC — watch packet received from the crew.`);
  } else if (previousMode === 'live') {
    updateTerminalFeed(`Watch downlink silent for over ${LIVE_FRESH_MS / 60000} min — back to mission baseline ${formatTime(currentTime)}.`);
  }
}

// ---------- Data ----------

async function fetchTelemetry() {
  const sources = dataSource ? [dataSource] : DATA_SOURCES;
  let lastError = null;
  for (const url of sources) {
    try {
      const res = await fetch(url, { cache: 'no-store' });
      if (!res.ok) throw new Error(`HTTP ${res.status} while loading ${url}`);
      const json = await res.json();
      if (!json.readings || !json.readings.length) throw new Error(`${url} returned no readings`);
      dataSource = url;
      return json;
    } catch (err) {
      lastError = err;
    }
  }
  throw lastError;
}

function groupSeries(readings) {
  const series = {};
  readings.forEach(r => {
    const byVital = series[r.astronaut_id] || (series[r.astronaut_id] = {});
    (byVital[r.vital_id] || (byVital[r.vital_id] = [])).push(r);
  });
  Object.values(series).forEach(byVital =>
    Object.values(byVital).forEach(list => list.sort((a, b) => a.recorded_at.localeCompare(b.recorded_at)))
  );
  return series;
}

function activeSeries() {
  return mode === 'live' ? LIVE_SERIES : SCENARIO_SERIES;
}

async function loadData() {
  DATA = await fetchTelemetry();

  VITALS = {};
  DATA.vitals.forEach(v => { VITALS[v.vital_id] = v; });

  const scenario = DATA.readings.filter(r => r.source !== 'watch');
  const live = DATA.readings.filter(r => r.source === 'watch');
  SCENARIO_SERIES = groupSeries(scenario);
  LIVE_SERIES = groupSeries(live);
  LIVE_LATEST_TIME = live.length ? live.map(r => r.recorded_at).sort().pop() : null;
  SERVER_NOW = (DATA.meta && DATA.meta.generated_at) || null;

  ALERTS = hasBackend() ? await fetchAlerts() : [];

  ASTRO_BY_PROFILE = {
    male: DATA.astronauts.find(a => a.sex === 'M'),
    female: DATA.astronauts.find(a => a.sex === 'F')
  };

  const times = [...new Set(scenario.map(r => r.recorded_at))].sort();
  const firstDay = times[0].slice(0, 10);
  const lastDay = times[times.length - 1].slice(0, 10);
  BASELINE_TIME = times.filter(t => t.startsWith(firstDay)).pop();
  REPLAY_TIMES = times.filter(t => t.startsWith(lastDay));
}

// Status from the thresholds — same rule as the SQL view v_readings_status.
function computeStatus(vitalId, value) {
  const v = VITALS[vitalId];
  if (!v) return 'nominal';
  if ((v.critical_low != null && value < v.critical_low) || (v.critical_high != null && value > v.critical_high)) return 'critical';
  if ((v.warning_low != null && value < v.warning_low) || (v.warning_high != null && value > v.warning_high)) return 'warning';
  return 'nominal';
}

function worstStatus(statuses) {
  return statuses.reduce((worst, s) => (STATUS_RANK[s] > STATUS_RANK[worst] ? s : worst), 'nominal');
}

function currentAstronaut() {
  return ASTRO_BY_PROFILE[currentGender];
}

// Latest reading of a vital at or before the given time.
function latestReading(astronautId, vitalId, time) {
  const list = (activeSeries()[astronautId] || {})[vitalId] || [];
  let found = null;
  for (const r of list) {
    if (r.recorded_at > time) break;
    found = r;
  }
  return found;
}

function history(astronautId, vitalId, time, count) {
  const list = (activeSeries()[astronautId] || {})[vitalId] || [];
  return list.filter(r => r.recorded_at <= time).slice(-count);
}

function formatValue(vitalId, value) {
  return vitalId in DECIMALS ? Number(value).toFixed(DECIMALS[vitalId]) : String(Math.round(value));
}

function formatTime(iso) {
  return iso.replace('T', ' ').slice(0, 16) + ' UTC';
}

function unitLabel(vitalId) {
  const unit = VITALS[vitalId].unit;
  return UNIT_LABELS[unit] || unit;
}

function refText(vitalId) {
  const v = VITALS[vitalId];
  const u = unitLabel(vitalId);
  if (v.warning_low != null && v.warning_high != null) return `Ref: ${v.warning_low}-${v.warning_high} ${u}`;
  if (v.warning_low != null) return `Target: ≥ ${v.warning_low} ${u}`;
  if (v.warning_high != null) return `Limit: ≤ ${v.warning_high} ${u}`;
  return '';
}

// Builds what a card displays for the current astronaut and time.
function buildMetric(cardId, astronautId, time) {
  const cfg = CARD_MAP[cardId];
  const readings = cfg.vitals.map(id => latestReading(astronautId, id, time));
  if (readings.some(r => !r)) return null;

  const statuses = readings.map(r => computeStatus(r.vital_id, r.value));
  const status = worstStatus(statuses);
  const main = VITALS[cfg.vitals[0]];
  const worstIdx = Math.max(0, statuses.indexOf(status));
  const driver = VITALS[cfg.vitals[worstIdx]];

  let val, ref;
  if (cfg.vitals.length === 2) {
    const [a, b] = cfg.vitals;
    val = `${formatValue(a, readings[0].value)}/${formatValue(b, readings[1].value)}`;
    ref = `Ref: ≤ ${VITALS[a].warning_high}/${VITALS[b].warning_high} ${unitLabel(a)}`;
  } else {
    val = formatValue(main.vital_id, readings[0].value);
    ref = refText(main.vital_id);
  }

  return {
    id: cardId,
    name: cfg.name || main.label,
    categoryLabel: ZONE_LABELS[main.zone] || main.zone,
    val,
    unit: unitLabel(main.vital_id),
    ref,
    status,
    countermeasure: status === 'nominal' ? main.nominal_message : driver.countermeasure,
    recordedAt: readings[0].recorded_at,
    hotspotId: cfg.hotspotId,
    ...sparkline(cfg, astronautId, main.vital_id, time)
  };
}

// Values of the card's main vital over the last hours, plus the card's status at each of those
// times (worst of all its vitals: blood pressure uses systolic AND diastolic).
function sparkline(cfg, astronautId, mainVitalId, time) {
  const hist = history(astronautId, mainVitalId, time, SPARK_POINTS);
  return {
    spark: hist.map(r => r.value),
    sparkStatus: hist.map(r => worstStatus(cfg.vitals.map(id => {
      const at = latestReading(astronautId, id, r.recorded_at);
      return at ? computeStatus(id, at.value) : 'nominal';
    })))
  };
}

function currentMetrics() {
  const astro = currentAstronaut();
  const metrics = {};
  Object.keys(CARD_MAP).forEach(cardId => {
    const m = buildMetric(cardId, astro.astronaut_id, currentTime);
    if (m) metrics[cardId] = m;
  });
  return metrics;
}

function extraStatuses() {
  const astro = currentAstronaut();
  return Object.keys(EXTRA_VITALS).map(vitalId => {
    const r = latestReading(astro.astronaut_id, vitalId, currentTime);
    return { vitalId, hotspotId: EXTRA_VITALS[vitalId].hotspotId, status: r ? computeStatus(vitalId, r.value) : 'nominal' };
  });
}

// ---------- Rendering ----------

function renderVitals() {
  if (!DATA) return;
  const metrics = currentMetrics();
  const hotspotStatus = {};

  // Cards with no reading yet (e.g. the watch has not sent that vital): show "--".
  Object.keys(CARD_MAP).filter(id => !metrics[id]).forEach(id => {
    const valEl = document.getElementById(`val-${id}`);
    if (valEl) valEl.textContent = '--';
    const badgeEl = document.getElementById(`status-${id}`);
    if (badgeEl) {
      badgeEl.className = 'status-badge';
      badgeEl.textContent = 'NO DATA';
    }
  });

  Object.values(metrics).forEach(metric => {
    const valEl = document.getElementById(`val-${metric.id}`);
    if (valEl) valEl.textContent = metric.val;

    const refEl = document.getElementById(`ref-${metric.id}`);
    if (refEl) refEl.textContent = metric.ref;

    const badgeEl = document.getElementById(`status-${metric.id}`);
    if (badgeEl) {
      badgeEl.className = `status-badge ${metric.status}`;
      badgeEl.textContent = metric.status.toUpperCase();
    }

    const cardEl = document.querySelector(`.vital-card[data-metric-id="${metric.id}"]`);
    if (cardEl) {
      const track = cardEl.querySelector('.sparkline-track');
      if (track) drawSparkline(track, metric.spark, metric.sparkStatus);
    }

    hotspotStatus[metric.hotspotId] = worstStatus([hotspotStatus[metric.hotspotId] || 'nominal', metric.status]);
  });

  extraStatuses().forEach(e => {
    hotspotStatus[e.hotspotId] = worstStatus([hotspotStatus[e.hotspotId] || 'nominal', e.status]);
  });

  // Several cards share one body hotspot: it shows the worst of them.
  Object.entries(hotspotStatus).forEach(([id, status]) => {
    const hotspot = document.getElementById(id);
    if (hotspot) hotspot.setAttribute('data-status', status);
  });

  updateReadinessRing(metrics);
  updateProfileDots();
  renderAlertLog();

  if (activeModalMetricId && !document.getElementById('medical-modal').classList.contains('hidden')) {
    fillModal(activeModalMetricId, metrics);
  }
}

function sparklinePoints(values) {
  const min = Math.min(...values);
  const max = Math.max(...values);
  const span = max - min || 1;
  const step = 100 / (values.length - 1);
  return values.map((v, i) => [(i * step).toFixed(1), (21 - ((v - min) / span) * 18).toFixed(1)]);
}

// Gray line for the whole history, then every segment touching an out-of-range reading is
// redrawn in orange (warning) or red (critical). So a spike stays colored as long as it is
// visible in the graph, even after the current value is back to nominal.
function drawSparkline(track, values, statuses) {
  if (!values || values.length < 2) return;
  const pts = sparklinePoints(values);
  track.setAttribute('d', pts.map((p, i) => `${i === 0 ? 'M' : 'L'}${p[0]},${p[1]}`).join(' '));
  track.style.stroke = '';

  const svg = track.ownerSVGElement;
  let layer = svg.querySelector('.sparkline-alerts');
  if (!layer) {
    layer = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    layer.setAttribute('class', 'sparkline-alerts');
    svg.appendChild(layer);
  }
  layer.replaceChildren();
  for (let i = 1; i < pts.length; i++) {
    const status = worstStatus([statuses[i - 1], statuses[i]]);
    if (status === 'nominal') continue;
    const seg = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    seg.setAttribute('class', `spark-seg ${status}`);
    seg.setAttribute('d', `M${pts[i - 1][0]},${pts[i - 1][1]} L${pts[i][0]},${pts[i][1]}`);
    layer.appendChild(seg);
  }
}

// Status dot on the MALE / FEMALE buttons: worst status of ALL vitals of that astronaut
// (radiation included), so a problem on the other astronaut is visible without switching.
function updateProfileDots() {
  Object.entries(ASTRO_BY_PROFILE).forEach(([profile, astro]) => {
    const dot = document.getElementById(`dot-${profile}`);
    if (!dot || !astro) return;
    const problems = [];
    let status = 'nominal';
    Object.keys(VITALS).forEach(vitalId => {
      const r = latestReading(astro.astronaut_id, vitalId, currentTime);
      if (!r) return;
      const s = computeStatus(vitalId, r.value);
      if (s !== 'nominal') problems.push(`${VITALS[vitalId].label} ${s}`);
      status = worstStatus([status, s]);
    });
    dot.setAttribute('data-status', status);
    dot.parentElement.title = `${astro.display_name}: ${problems.length ? problems.join(', ') : 'all nominal'}`;
  });
}

function updateReadinessRing(metrics) {
  if (!DATA) return;
  const statuses = Object.values(metrics || currentMetrics()).map(m => m.status)
    .concat(extraStatuses().map(e => e.status));
  if (!statuses.length) return;

  const avg = Math.round(statuses.reduce((sum, s) => sum + STATUS_SCORE[s], 0) / statuses.length);
  const pctEl = document.getElementById('readiness-pct');
  const ringFg = document.getElementById('ring-progress');

  if (pctEl && ringFg) {
    pctEl.textContent = `${avg}%`;
    const circumference = 150.8;
    ringFg.style.strokeDashoffset = circumference - (avg / 100) * circumference;

    let color = 'var(--state-critical)';
    if (avg >= 85) color = 'var(--state-nominal)';
    else if (avg >= 60) color = 'var(--state-warning)';
    ringFg.style.stroke = color;
    pctEl.style.color = color;
  }
}

// ---------- Controls ----------

function bindGenderEvents() {
  const maleBtn = document.getElementById('gender-male');
  const femaleBtn = document.getElementById('gender-female');
  if (!maleBtn || !femaleBtn) return;

  maleBtn.addEventListener('click', () => switchProfile('male'));
  femaleBtn.addEventListener('click', () => switchProfile('female'));
}

function switchProfile(gender) {
  if (currentGender === gender) return;
  currentGender = gender;

  const isMale = gender === 'male';
  document.getElementById('gender-male').classList.toggle('active', isMale);
  document.getElementById('gender-female').classList.toggle('active', !isMale);
  const maleSil = document.getElementById('silhouette-male');
  const femaleSil = document.getElementById('silhouette-female');
  if (maleSil) maleSil.classList.toggle('active', isMale);
  if (femaleSil) femaleSil.classList.toggle('active', !isMale);

  inspectedNodes.clear();
  setupInteractiveBodyNodes();
  renderVitals();
  applyFilter(currentFilter);

  const astro = currentAstronaut();
  if (astro) {
    updateTerminalFeed(`Crew profile switched: ${astro.astronaut_id} ${astro.display_name.toUpperCase()} (${astro.role}) — telemetry synchronized.`);
  }
}

function bindFilterEvents() {
  const chips = document.querySelectorAll('.filter-chip');
  chips.forEach(chip => {
    chip.addEventListener('click', () => {
      chips.forEach(c => c.classList.remove('active'));
      chip.classList.add('active');
      currentFilter = chip.getAttribute('data-filter');
      applyFilter(currentFilter);
    });
  });
}

// data-category may hold several categories separated by spaces (e.g. "immuno metabolic").
function matchesFilter(el, filter) {
  return filter === 'all' || (el.getAttribute('data-category') || '').split(/\s+/).includes(filter);
}

function applyFilter(filter) {
  document.querySelectorAll('.vital-card').forEach(card => {
    card.classList.toggle('hidden', !matchesFilter(card, filter));
  });

  document.querySelectorAll('.interactive-node').forEach(node => {
    node.classList.toggle('dimmed', !matchesFilter(node, filter));
  });
}

function setupCardEvents() {
  document.querySelectorAll('.vital-card').forEach(card => {
    card.addEventListener('click', () => openModal(card.getAttribute('data-metric-id')));
  });
}

function setupInteractiveBodyNodes() {
  document.querySelectorAll('.interactive-node').forEach(node => {
    node.removeEventListener('click', handleSingleNodeClick);
    const id = node.getAttribute('data-metric-id');
    if (!inspectedNodes.has(id)) {
      node.classList.remove('inspected');
      node.addEventListener('click', handleSingleNodeClick);
    }
  });
}

function handleSingleNodeClick(e) {
  const node = e.currentTarget;
  const id = node.getAttribute('data-metric-id');
  if (!id) return;

  openModal(id);
  inspectedNodes.add(id);
  node.classList.add('inspected');
  node.removeEventListener('click', handleSingleNodeClick);
}

// ---------- Modal ----------

function setupModalEvents() {
  const modal = document.getElementById('medical-modal');
  const dismissBtn = document.getElementById('btn-modal-dismiss');

  if (dismissBtn) dismissBtn.addEventListener('click', acknowledgeAndClose);
  if (modal) {
    modal.addEventListener('click', (e) => {
      if (e.target === modal) acknowledgeAndClose();
    });
  }
}

function acknowledgeAndClose() {
  const modal = document.getElementById('medical-modal');
  if (modal) modal.classList.add('hidden');
  const alertId = activeModalAlertId;
  activeModalMetricId = null;
  activeModalAlertId = null;

  if (alertId) {
    acknowledgeAlert(alertId);
  } else if (hasBackend()) {
    updateTerminalFeed('Protocol reviewed — no open alert for this vital.');
  } else {
    updateTerminalFeed('Protocol acknowledged by crew — committed to the flight archive.');
  }
}

function openModal(metricId) {
  if (!DATA || !CARD_MAP[metricId]) return;
  activeModalMetricId = metricId;
  fillModal(metricId, currentMetrics());
  document.getElementById('medical-modal').classList.remove('hidden');
}

function fillModal(metricId, metrics) {
  const data = metrics[metricId];
  if (!data) return;
  const astro = currentAstronaut();

  document.getElementById('modal-metric-name').textContent = `${data.name} (${data.categoryLabel})`;
  document.getElementById('modal-current-val').textContent = `${data.val} ${data.unit}`;
  document.getElementById('modal-countermeasure').textContent =
    `${data.countermeasure} — ${astro.display_name}, reading of ${formatTime(data.recordedAt)}.`;

  const badge = document.getElementById('modal-status');
  badge.className = `status-badge ${data.status}`;
  badge.textContent = data.status.toUpperCase();

  // Link the dialog to the open alert of this card, if there is one.
  const alert = openAlertForCard(metricId);
  activeModalAlertId = alert ? alert.id : null;
  const stamp = document.getElementById('modal-id-stamp');
  const button = document.getElementById('btn-modal-dismiss');
  if (hasBackend()) {
    if (stamp) {
      stamp.textContent = alert
        ? `ALERT #${alert.id} // OPEN SINCE ${formatTime(alert.recorded_at)}`
        : 'DECISION-SUPPORT // NO OPEN ALERT';
    }
    if (button) button.textContent = alert ? 'Acknowledge & Close' : 'Close';
  }
}

// ---------- Alerts (backend only) ----------
// The backend creates an alert when a reading leaves its thresholds; mission control
// acknowledges it from the dialog or from the archive panel (POST /api/alerts/{id}/ack).

function hasBackend() {
  return !!dataSource && dataSource !== 'data.json';
}

async function fetchAlerts() {
  try {
    const res = await fetch(`${ALERTS_URL}?state=all&limit=200`, { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return (await res.json()).alerts || [];
  } catch (err) {
    console.warn('Alerts could not be loaded', err);
    return ALERTS;
  }
}

// Alerts matching what is on screen: watch alerts in live mode,
// scenario alerts already "happened" at the displayed time otherwise.
function visibleAlerts() {
  const source = mode === 'live' ? 'watch' : 'scenario';
  return ALERTS
    .filter(a => a.source === source && (mode === 'live' || a.recorded_at <= currentTime))
    .map(alertView)
    .sort((a, b) => b.recorded_at.localeCompare(a.recorded_at) || b.id - a.id);
}

function openAlertForCard(cardId) {
  const astro = currentAstronaut();
  const vitals = (CARD_MAP[cardId] || {}).vitals || [];
  const candidates = visibleAlerts().filter(a =>
    !a.acknowledged && a.astronaut_id === astro.astronaut_id && vitals.includes(a.vital_id));
  candidates.sort((a, b) => STATUS_RANK[b.status] - STATUS_RANK[a.status]);
  return candidates[0] || null;
}

// What an alert looks like at the displayed time.
// Live mode: the server's values. Scenario mode: recomputed from the scenario readings up to
// currentTime, so the replay never shows values that have not "happened" yet.
function alertView(alert) {
  if (mode === 'live') return alert;
  const list = ((SCENARIO_SERIES[alert.astronaut_id] || {})[alert.vital_id] || [])
    .filter(r => r.recorded_at >= alert.recorded_at && r.recorded_at <= currentTime);
  if (!list.length) return alert;

  let status = 'nominal';
  let peak = list[0].value;
  let count = 0;
  list.forEach(r => {
    const s = computeStatus(r.vital_id, r.value);
    if (s !== 'nominal') count += 1;
    if (STATUS_RANK[s] >= STATUS_RANK[status]) { status = s; peak = r.value; }
  });
  const last = list[list.length - 1];
  return {
    ...alert,
    status,
    peak_value: peak,
    last_value: last.value,
    last_status: computeStatus(last.vital_id, last.value),
    readings_count: count
  };
}

function alertValueText(a) {
  const unit = UNIT_LABELS[a.unit] || a.unit;
  const fmt = v => formatValue(a.vital_id, v);
  const parts = [`${fmt(a.first_value)} ${unit}`];
  if (a.peak_value !== a.first_value) parts.push(`peak ${fmt(a.peak_value)}`);
  if (a.last_value !== a.peak_value) parts.push(`now ${fmt(a.last_value)}`);
  return parts.join(' · ');
}

function renderAlertLog() {
  const panel = document.getElementById('alert-log');
  const list = document.getElementById('alert-log-list');
  const count = document.getElementById('alert-log-count');
  if (!panel || !list) return;

  panel.classList.toggle('hidden', !hasBackend());
  if (!hasBackend()) return;

  // Only the astronaut on screen, so the list matches the cards. The other one is summarized.
  const astro = currentAstronaut();
  const all = visibleAlerts();
  const alerts = all.filter(a => a.astronaut_id === astro.astronaut_id);
  const open = alerts.filter(a => !a.acknowledged).length;
  const otherOpen = all.filter(a => a.astronaut_id !== astro.astronaut_id && !a.acknowledged);
  if (count) {
    let text = `${astro.display_name}: ${open} open · ${alerts.length} total`;
    if (otherOpen.length) text += ` — ${otherOpen[0].display_name}: ${otherOpen.length} open (switch profile)`;
    count.textContent = text;
    count.classList.toggle('has-open', open + otherOpen.length > 0);
  }

  list.replaceChildren();
  if (!alerts.length) {
    const li = document.createElement('li');
    li.className = 'alert-log-empty';
    li.textContent = `No alert for ${astro.display_name} in this window.`;
    list.appendChild(li);
    return;
  }

  alerts.slice(0, ALERT_LOG_ROWS).forEach(a => {
    const li = document.createElement('li');
    li.className = `alert-row${a.acknowledged ? ' acknowledged' : ''}`;

    const badge = document.createElement('span');
    badge.className = `status-badge ${a.acknowledged ? 'nominal' : a.status}`;
    badge.textContent = a.status.toUpperCase();

    const main = document.createElement('div');
    main.className = 'alert-main';
    const title = document.createElement('span');
    title.className = 'alert-title';
    title.textContent = `${a.display_name} — ${a.vital_label}: ${alertValueText(a)}`;
    const meta = document.createElement('span');
    meta.className = 'alert-meta';
    meta.textContent = `#${a.id} · ${formatTime(a.recorded_at)} · ${a.readings_count} reading${a.readings_count > 1 ? 's' : ''}`
      + (a.last_status === 'nominal' ? ' · back to nominal' : '');
    main.append(title, meta);

    let action;
    if (a.acknowledged) {
      action = document.createElement('span');
      action.className = 'alert-ack-state';
      action.textContent = `ACK ${a.acknowledged_at.slice(11, 16)} · ${a.acknowledged_by}`;
    } else {
      action = document.createElement('button');
      action.className = 'alert-ack-btn';
      action.dataset.alertId = a.id;
      action.textContent = 'ACKNOWLEDGE';
    }

    li.append(badge, main, action);
    list.appendChild(li);
  });
}

function setupAlertLogEvents() {
  const list = document.getElementById('alert-log-list');
  if (!list) return;
  list.addEventListener('click', (e) => {
    const btn = e.target.closest('.alert-ack-btn');
    if (!btn) return;
    btn.disabled = true;
    acknowledgeAlert(Number(btn.dataset.alertId));
  });
}

async function acknowledgeAlert(id) {
  try {
    const res = await fetch(`${ALERTS_URL}/${id}/ack`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ acknowledged_by: ACK_OPERATOR })
    });
    const json = await res.json();
    if (json.alert) {
      ALERTS = ALERTS.map(a => (a.id === id ? json.alert : a));
    }
    if (!res.ok && res.status !== 409) throw new Error(json.error || `HTTP ${res.status}`);

    const a = json.alert;
    updateTerminalFeed(
      `Alert #${id} acknowledged by ${a.acknowledged_by} — ${a.display_name}, ${a.vital_label}. Committed to the flight archive.`
    );
  } catch (err) {
    console.error(err);
    updateTerminalFeed(`ACK FAILED for alert #${id}: ${err.message}`, true);
  }
  renderAlertLog();
}

// ---------- Live watch simulator (backend only) ----------
// The button asks the PHP server to run sensor-cpp/simulator.exe (or the PHP simulator if the
// .exe has not been built). The simulator then sends readings to the API like a real watch,
// and pollLive() switches the dashboard to live mode.

async function refreshSimulatorStatus() {
  if (!hasBackend()) return;
  try {
    const res = await fetch(SIMULATOR_URL, { cache: 'no-store' });
    if (res.ok) simState = await res.json();
  } catch (err) {
    console.warn('Simulator status unavailable', err);
  }
  renderWatchSimulator();
}

function renderWatchSimulator() {
  const panel = document.getElementById('watch-sim');
  const btn = document.getElementById('btn-watch-sim');
  const select = document.getElementById('watch-sim-crisis');
  const status = document.getElementById('watch-sim-status');
  if (!panel || !btn) return;

  panel.classList.toggle('hidden', !hasBackend() || !simState);
  if (!simState) return;

  const running = !!simState.running;
  btn.classList.toggle('running', running);
  btn.innerHTML = running ? '&#9632; Stop' : '&#9654; Run';
  if (select) {
    select.disabled = running;
    if (running && simState.crisis) select.value = simState.crisis;
  }
  if (status) {
    if (running) {
      const engine = simState.engine === 'cpp' ? 'C++ simulator.exe' : 'PHP simulator (simulator.exe not built)';
      status.textContent = `RUNNING · ${engine} · crisis: ${simState.crisis} · since ${simState.started_at.slice(11, 19)} UTC`;
    } else {
      status.textContent = simState.cpp_available
        ? 'Ready · engine: C++ simulator.exe'
        : 'Ready · simulator.exe not built (sensor-cpp\\build.bat): the PHP simulator will be used';
    }
  }
}

function setupWatchSimulator() {
  const btn = document.getElementById('btn-watch-sim');
  if (!btn) return;
  btn.addEventListener('click', async () => {
    const running = simState && simState.running;
    btn.disabled = true;
    try {
      if (!running && isCrisisActive) window.simulateSolarFlareCrisis();  // leave the replay first

      const crisis = document.getElementById('watch-sim-crisis').value;
      const res = await fetch(`${SIMULATOR_URL}/${running ? 'stop' : 'start'}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ crisis })
      });
      const json = await res.json();
      if (!res.ok) throw new Error(json.error || `HTTP ${res.status}`);
      simState = json;

      if (running) {
        updateTerminalFeed('Watch simulator stopped — dashboard returns to the mission baseline after 2 min of silence.');
      } else {
        const engine = json.engine === 'cpp' ? 'C++ simulator.exe' : 'PHP simulator';
        updateTerminalFeed(`Watch simulator started (${engine}, crisis: ${crisis}) — waiting for the first packets...`);
        setTimeout(pollLive, 2500);
      }
    } catch (err) {
      console.error(err);
      updateTerminalFeed(`WATCH SIMULATOR ERROR: ${err.message}`, true);
    }
    btn.disabled = false;
    renderWatchSimulator();
  });
}

// ---------- Terminal ----------

function updateTerminalFeed(message, isAlert) {
  const terminalFeed = document.getElementById('terminal-feed');
  if (!terminalFeed) return;
  terminalFeed.textContent = message;
  terminalFeed.style.color = isAlert ? 'var(--state-critical)' : 'var(--accent-cyan)';
  clearTimeout(updateTerminalFeed.timer);
  updateTerminalFeed.timer = setTimeout(() => { terminalFeed.style.color = ''; }, 4500);
}

// ---------- Day 2 replay (solar event) ----------
// Replays the last day of the data file hour by hour. Every status change for
// any crew member is written to the decision log.

function setFlareButton(state) {
  const btn = document.querySelector('.btn-primary-flare');
  if (!btn) return;
  if (state === 'replaying') {
    btn.innerHTML = '<span class="btn-flare-icon">■</span><span>Stop Replay &amp; Restore Baseline</span>';
    btn.style.background = 'linear-gradient(135deg, #ff3b5c 0%, #b91c1c 100%)';
  } else if (state === 'done') {
    btn.innerHTML = '<span class="btn-flare-icon">✓</span><span>Restore Nominal Telemetry</span>';
    btn.style.background = 'linear-gradient(135deg, #00e5a3 0%, #059669 100%)';
  } else {
    btn.innerHTML = '<span class="btn-flare-icon">⚡</span><span>Simulate Solar Flare (SPE)</span>';
    btn.style.background = '';
  }
}

function crewStatuses(time) {
  const out = {};
  DATA.astronauts.forEach(a => {
    Object.keys(VITALS).forEach(vitalId => {
      const r = latestReading(a.astronaut_id, vitalId, time);
      if (r) out[`${a.astronaut_id}|${vitalId}`] = { astro: a, vitalId, value: r.value, status: computeStatus(vitalId, r.value) };
    });
  });
  return out;
}

function stopReplay() {
  clearInterval(replayTimer);
  replayTimer = null;
}

window.simulateSolarFlareCrisis = function () {
  if (!DATA || !REPLAY_TIMES.length) return;

  if (isCrisisActive) {
    stopReplay();
    isCrisisActive = false;
    inspectedNodes.clear();
    setupInteractiveBodyNodes();
    showDefaultView();
    setFlareButton('idle');
    updateTerminalFeed(
      mode === 'live'
        ? `Live downlink restored — last watch packet ${formatTime(currentTime)}.`
        : `Baseline restored — telemetry of ${formatTime(currentTime)}. All crew nodes polling.`
    );
    return;
  }

  // The replay always uses the scenario data, starting from the day 1 baseline.
  isCrisisActive = true;
  mode = 'scenario';
  currentTime = BASELINE_TIME;
  updateModePill('replay');
  inspectedNodes.clear();
  setupInteractiveBodyNodes();
  setFlareButton('replaying');
  updateTerminalFeed(`Replaying mission day ${REPLAY_TIMES[0].slice(0, 10)} — solar particle event window.`);

  let previous = crewStatuses(currentTime);
  let i = 0;

  replayTimer = setInterval(() => {
    currentTime = REPLAY_TIMES[i];
    renderVitals();

    const now = crewStatuses(currentTime);
    // Changes of the astronaut on screen first, then the most severe.
    const shownId = currentAstronaut().astronaut_id;
    const changes = Object.keys(now)
      .filter(k => !previous[k] || previous[k].status !== now[k].status)
      .map(k => now[k])
      .sort((a, b) => (b.astro.astronaut_id === shownId) - (a.astro.astronaut_id === shownId)
        || STATUS_RANK[b.status] - STATUS_RANK[a.status]);
    previous = now;

    const clock = currentTime.slice(11, 16);
    if (changes.length) {
      const c = changes[0];
      const more = changes.length > 1 ? ` (+${changes.length - 1} more)` : '';
      updateTerminalFeed(
        `${clock} UTC — ${c.astro.display_name}: ${VITALS[c.vitalId].label} ${formatValue(c.vitalId, c.value)} ${unitLabel(c.vitalId)} → ${c.status.toUpperCase()}${more}`,
        c.status === 'critical'
      );
    } else {
      const terminalFeed = document.getElementById('terminal-feed');
      if (terminalFeed && !terminalFeed.style.color) {
        terminalFeed.textContent = `${clock} UTC — downlink received, no status change.`;
      }
    }

    i += 1;
    if (i >= REPLAY_TIMES.length) {
      stopReplay();
      setFlareButton('done');
    }
  }, REPLAY_STEP_MS);
};