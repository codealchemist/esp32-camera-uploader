/**
 * yicam-bridge receiver: saves base64 JPEG snapshots POSTed by the ESP32
 * into Google Drive, one subfolder per day.
 *
 * Access is limited to the drive.file scope (see appsscript.json): the script
 * can only see and change files and folders it created itself. It creates its
 * own "YiCam" folder on first use and keeps everything inside it. The rest of
 * your Drive is invisible to it.
 *
 * Uses the Advanced Drive service (Drive API v3), enabled by appsscript.json.
 * DriveApp is deliberately not used: it would force the full Drive scope.
 *
 * Script properties (Project Settings -> Script properties):
 *   TOKEN           required. Long random string; must match the ESP32 setting.
 *   RETENTION_DAYS  optional. Days of images kept by cleanup(); default 30.
 *   NOTIFY_EMAIL    optional. Address that gets one email per motion event
 *                   with that event's photos attached. Unset = no emails.
 *   NOTIFY_COOLDOWN_MIN  optional. Minutes without emails after one is sent
 *                   (photos still go to Drive); default 60, 0 = no cooldown.
 *   ROOT_FOLDER_ID  managed by the script. Do not edit.
 *   OWNER_EMAIL     set by setup(): the only account the control page opens for.
 *   NOTIFY_STATE, NOTIFY_LAST_SENT, NOTIFY_SUPPRESSED
 *                   managed by the script (pending email, cooldown bookkeeping).
 *
 * Email notifications: photos arrive one at a time, so they are collected
 * and sent together once no new photo has arrived for a minute (or at the
 * latest 5 minutes after the first one), using a one-time trigger. After an
 * email, a cooldown (NOTIFY_COOLDOWN_MIN) keeps later events from emailing;
 * they are only counted and mentioned in the next email. To read
 * the photos back the script calls the Drive API with its own drive.file
 * token, which still only reaches files it created.
 *
 * First time: run setup() once from the editor, then
 * Deploy -> New deployment -> Web app (Execute as: Me, Who has access: Anyone)
 * and copy the /exec URL into the ESP32 setup portal.
 */

const ROOT_FOLDER_NAME = 'YiCam';
const FOLDER_MIME = 'application/vnd.google-apps.folder';
const DAY_RE = /^(\d{4})-(\d{2})-(\d{2})$/;

const NOTIFY_QUIET_MS = 60 * 1000;          // send once no photo arrived for this long
const NOTIFY_MAX_WAIT_MS = 5 * 60 * 1000;   // ...or at most this long after the first one
const NOTIFY_MAX_PHOTOS = 10;               // attachments per email
const NOTIFY_MAX_BYTES = 20 * 1024 * 1024;  // Gmail allows 25 MB per message
const NOTIFY_COOLDOWN_DEFAULT_MIN = 60;     // no emails for this long after one is sent

function cooldownMs_(props) {
  const v = props.getProperty('NOTIFY_COOLDOWN_MIN');
  const min = v === null || v === '' ? NOTIFY_COOLDOWN_DEFAULT_MIN : Number(v);
  return isFinite(min) && min > 0 ? min * 60 * 1000 : 0;
}

function doPost(e) {
  try {
    const token = PropertiesService.getScriptProperties().getProperty('TOKEN');
    if (!token || !e || !e.parameter || e.parameter.token !== token) {
      return json_({ ok: false, error: 'unauthorized' });
    }
    switch (e.parameter.action) {
      case 'token': return json_(handleToken_());
      case 'folder': return json_(handleFolder_(e.parameter.day));
      case 'saved': return json_(handleSaved_(e.postData && e.postData.contents));
      case 'poll': return json_(handlePoll_(e.postData && e.postData.contents));
    }
    // No action: legacy upload of one base64 JPEG in the body.
    if (!e.postData || !e.postData.contents) {
      return json_({ ok: false, error: 'empty body' });
    }

    const tz = Session.getScriptTimeZone();
    const now = new Date();
    const name = sanitizeName_(e.parameter.name) ||
      'yicam_' + Utilities.formatDate(now, tz, 'yyyyMMdd-HHmmss') + '.jpg';
    const bytes = Utilities.base64Decode(e.postData.contents.replace(/\s/g, ''));
    if (bytes.length < 2 || (bytes[0] & 0xff) !== 0xff || (bytes[1] & 0xff) !== 0xd8) {
      return json_({ ok: false, error: 'not a JPEG' });
    }

    const dayId = dayFolderId_(Utilities.formatDate(now, tz, 'yyyy-MM-dd'));
    const file = Drive.Files.create(
      { name: name, parents: [dayId], mimeType: 'image/jpeg' },
      Utilities.newBlob(bytes, 'image/jpeg', name),
      { fields: 'id' });
    try {
      queueNotification_(file.id, name, dayId);
    } catch (err) {
      console.error('notification not queued: ' + err);  // never fail the upload over it
    }
    rememberLatest_(file.id, name);
    return json_({ ok: true, id: file.id, name: name, size: bytes.length });
  } catch (err) {
    return json_({ ok: false, error: String(err) });
  }
}

// --- Requests from the bridge for direct uploads ------------------------------
// The bridge uploads photos and video clips straight to the Drive API with a
// short-lived access token from this script. The token carries this script's
// permissions; for Drive that is drive.file, so it can only reach files this
// script's app created (the YiCam folder), and it expires within an hour.

/** A fresh access token for the bridge (~1 hour). */
function handleToken_() {
  return { ok: true, access_token: ScriptApp.getOAuthToken(), expires_in: 3000 };
}

/** ID of the day folder (created if missing); day is yyyy-MM-dd in the bridge's timezone. */
function handleFolder_(day) {
  if (!DAY_RE.test(String(day || ''))) return { ok: false, error: 'bad day' };
  return { ok: true, id: dayFolderId_(day) };
}

/**
 * The bridge reports files it uploaded directly, in batches:
 * body = JSON [{"id":..., "name":..., "folder":..., "kind":"photo"|"video"}, ...]
 */
function handleSaved_(body) {
  let files;
  try {
    files = JSON.parse(body || '[]');
  } catch (err) {
    return { ok: false, error: 'bad json' };
  }
  if (!Array.isArray(files)) return { ok: false, error: 'bad json' };
  let n = 0;
  let remotePhotos = 0;
  let newest = null;
  let liveFolder = null;
  for (const f of files.slice(0, 50)) {
    if (!f || typeof f.id !== 'string' || typeof f.folder !== 'string') continue;
    const name = sanitizeName_(f.name) || f.id;
    const live = f.kind !== 'video' && /-l\.jpe?g$/i.test(name);
    if (f.kind !== 'video' && /-r\.jpe?g$/i.test(name)) remotePhotos++;
    if (f.kind !== 'video' && (!newest || name > newest.name)) newest = { id: f.id, name: name };
    if (live) liveFolder = f.folder;
    try {
      if (f.kind === 'video') noteVideo_(f.id, name);
      else if (!live) queueNotification_(f.id, name, f.folder);  // live-view frames never email
      n++;
    } catch (err) {
      console.error('notification not queued: ' + err);
    }
  }
  if (remotePhotos) creditRemotePhotos_(remotePhotos);
  if (newest) rememberLatest_(newest.id, newest.name);
  if (liveFolder) pruneLiveFrames_(liveFolder);
  return { ok: true, noted: n };
}

/**
 * Opened by you (deployment "Only myself", signed in): the control page.
 * Anyone else, e.g. the bridge's own deployment opened anonymously: a health check.
 */
const CODE_VERSION = '0.7.1';

function doGet(e) {
  // ?diag=1: why the control page does or doesn't open. Only yes/no answers,
  // never the addresses themselves, so it's safe on the public deployment too.
  if (e && e.parameter && e.parameter.diag) {
    let active = '', error = '';
    try { active = Session.getActiveUser().getEmail(); } catch (err) { error = String(err).substring(0, 200); }
    const owner = PropertiesService.getScriptProperties().getProperty('OWNER_EMAIL') || '';
    return json_({
      code_version: CODE_VERSION,
      control_page_file: (() => { try { HtmlService.createHtmlOutputFromFile('Control'); return true; } catch (err) { return false; } })(),
      owner_recorded: !!owner,          // false: run setup() once in the editor
      your_email_readable: !!active,
      you_are_owner: !!active && !!owner && active.toLowerCase() === owner.toLowerCase(),
      error: error,
    });
  }
  if (isOwner_()) {
    return HtmlService.createHtmlOutputFromFile('Control')
      .setTitle('YiCam')
      .addMetaTag('viewport', 'width=device-width, initial-scale=1, viewport-fit=cover');
  }
  return json_({ ok: true, service: 'yicam-bridge receiver' });
}

// --- Remote control: check-ins from the bridge, requests from the page --------
// The bridge can't be reached from outside, so it checks in every ~30 s
// (action=poll on the bridge's deployment, with TOKEN). It reports a short
// status, kept in the script cache, and receives queued capture requests.
// Requests have ids; the bridge acknowledges them on its next check-in.
// Unacknowledged requests expire after 2 minutes.

const CMD_TTL_MS = 2 * 60 * 1000;    // a request not picked up by then is dropped
const CMD_KEEP_MS = 30 * 60 * 1000;  // finished requests stay visible this long
const CMD_MAX_QUEUED = 5;

function handlePoll_(body) {
  let st;
  try {
    st = JSON.parse(body || '{}');
  } catch (err) {
    return { ok: false, error: 'bad json' };
  }
  const now = Date.now();
  const ack = Array.isArray(st.ack) ? st.ack.map(String) : [];
  delete st.ack;
  CacheService.getScriptCache().put('BRIDGE', JSON.stringify({ status: st, seen: now }), 21600);
  return withLock_(() => {
    const props = PropertiesService.getScriptProperties();
    let cmds = JSON.parse(props.getProperty('COMMANDS') || '[]');
    let changed = false;
    for (const c of cmds) {
      if (c.state === 'queued' && ack.indexOf(c.id) >= 0) {
        c.state = 'taken';
        c.taken = now;
        changed = true;
      } else if (c.state === 'queued' && now - c.created > CMD_TTL_MS) {
        c.state = 'expired';
        changed = true;
      }
    }
    const n = cmds.length;
    cmds = cmds.filter(c => now - c.created < CMD_KEEP_MS);
    if (changed || cmds.length !== n) props.setProperty('COMMANDS', JSON.stringify(cmds));
    return {
      ok: true,
      commands: cmds.filter(c => c.state === 'queued').map(c => ({ id: c.id, type: c.type, secs: c.secs || 0, every: c.every || 0 })),
    };
  });
}

/** Remote photos (named …-r.jpg) count toward the latest picked-up request. */
function creditRemotePhotos_(n) {
  withLock_(() => {
    const props = PropertiesService.getScriptProperties();
    const cmds = JSON.parse(props.getProperty('COMMANDS') || '[]');
    const c = cmds.filter(x => x.state === 'taken').pop();
    if (!c) return;
    c.photos = (c.photos || 0) + n;
    props.setProperty('COMMANDS', JSON.stringify(cmds));
  });
}

// --- Control page (deployment "Only myself") -------------------------------
// Every function below checks that the signed-in user is the script's owner,
// so they can't be used from the bridge's public deployment.

// The owner's address is recorded by setup(), run in the editor by the owner.
// (Session.getEffectiveUser() would need another permission.)
function isOwner_() {
  try {
    const owner = PropertiesService.getScriptProperties().getProperty('OWNER_EMAIL');
    const who = Session.getActiveUser().getEmail();
    return !!owner && !!who && who.toLowerCase() === owner.toLowerCase();
  } catch (err) {
    return false;
  }
}

function requireOwner_() {
  if (!isOwner_()) throw new Error('Not allowed. Open this page signed in as the script owner.');
}

function ui_state() {
  requireOwner_();
  const bridge = JSON.parse(CacheService.getScriptCache().get('BRIDGE') || 'null');
  const cmds = JSON.parse(PropertiesService.getScriptProperties().getProperty('COMMANDS') || '[]');
  const shown = cmds.filter(c => c.type !== 'live' && c.type !== 'stop');
  return { now: Date.now(), bridge: bridge, commands: shown.slice(-4).reverse(), cmdTtlMs: CMD_TTL_MS,
           latest: latestPhoto_() };
}

function ui_capture(kind) {
  requireOwner_();
  const kinds = { snap: { type: 'snap', secs: 0 }, b30: { type: 'burst', secs: 30 }, b60: { type: 'burst', secs: 60 } };
  const k = kinds[kind];
  if (!k) throw new Error('Unknown request');
  withLock_(() => {
    const props = PropertiesService.getScriptProperties();
    const now = Date.now();
    let cmds = JSON.parse(props.getProperty('COMMANDS') || '[]').filter(c => now - c.created < CMD_KEEP_MS);
    if (cmds.filter(c => c.state === 'queued').length >= CMD_MAX_QUEUED) {
      throw new Error('Too many requests waiting for the bridge.');
    }
    const id = now.toString(36) + Math.floor(Math.random() * 1e6).toString(36);
    cmds.push({ id: id, type: k.type, secs: k.secs, created: now, state: 'queued', photos: 0 });
    props.setProperty('COMMANDS', JSON.stringify(cmds.slice(-20)));
  });
  return ui_state();
}

/** Day folders, newest first: [{day, id}]. */
function ui_days() {
  requireOwner_();
  const rootId = rootFolderId_();
  const days = [];
  let pageToken;
  do {
    const res = Drive.Files.list({
      q: `'${rootId}' in parents and mimeType = '${FOLDER_MIME}' and trashed = false`,
      fields: 'nextPageToken, files(id, name)',
      pageSize: 100,
      pageToken: pageToken,
    });
    (res.files || []).forEach(f => { if (DAY_RE.test(f.name)) days.push({ day: f.name, id: f.id }); });
    pageToken = res.nextPageToken;
  } while (pageToken);
  days.sort((a, b) => (a.day < b.day ? 1 : -1));
  return days;
}

/**
 * One day's photos and clips grouped into events (files less than 2 minutes
 * apart), newest first. Times come from the file names the bridge gives:
 * yyyyMMdd-HHmmss[-NN][-r].jpg and yyyyMMdd-HHmmss.mp4.
 */
function ui_timeline(dayId) {
  requireOwner_();
  const rootId = rootFolderId_();
  const folder = Drive.Files.get(String(dayId), { fields: 'id, name, mimeType, parents' });
  if (folder.mimeType !== FOLDER_MIME || !DAY_RE.test(folder.name) || (folder.parents || []).indexOf(rootId) < 0) {
    throw new Error('Not a YiCam day folder');
  }
  const items = [];
  const thumbs = {};
  let pageToken;
  do {
    const res = Drive.Files.list({
      q: `'${folder.id}' in parents and trashed = false`,
      fields: 'nextPageToken, files(id, name, mimeType, webViewLink, thumbnailLink, createdTime)',
      pageSize: 500,
      pageToken: pageToken,
    });
    (res.files || []).forEach(f => {
      const isVideo = /^video\//.test(f.mimeType);
      if (!isVideo && !/^image\//.test(f.mimeType)) return;
      if (/-l\.jpe?g$/i.test(f.name)) return;  // live-view frames aren't events
      const m = /^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/.exec(f.name);
      const t = m ? Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6])  // local time, as UTC fields
                  : new Date(f.createdTime).getTime();
      items.push({ id: f.id, name: f.name, url: f.webViewLink, video: isVideo, t: t, named: !!m,
                   remote: /-r\.jpe?g$/i.test(f.name) });
      if (f.thumbnailLink) thumbs['th_' + f.id] = f.thumbnailLink;
    });
    pageToken = res.nextPageToken;
  } while (pageToken);
  if (Object.keys(thumbs).length) CacheService.getScriptCache().putAll(thumbs, 21600);

  items.sort((a, b) => a.t - b.t);
  const events = [];
  let ev = null;
  for (const it of items) {
    if (!ev || it.t - ev.end > 2 * 60 * 1000) {
      ev = { start: it.t, end: it.t, photos: [], videos: [] };
      events.push(ev);
    }
    ev.end = Math.max(ev.end, it.t);
    (it.video ? ev.videos : ev.photos).push({ id: it.id, name: it.name, url: it.url, remote: it.remote, t: it.t });
  }
  const hhmm = t => new Date(t).toISOString().substr(11, 5);  // t holds local time in UTC fields
  return {
    day: folder.name,
    folderUrl: 'https://drive.google.com/drive/folders/' + folder.id,
    events: events.reverse().map(e => ({
      start: hhmm(e.start), end: hhmm(e.end),
      remote: e.photos.length > 0 && e.photos.every(p => p.remote),
      photos: e.photos, videos: e.videos.map(v => ({ id: v.id, url: v.url, name: v.name, time: hhmm(v.t) })),
    })),
  };
}

/**
 * Live view from the control page: the bridge takes a frame every `every`
 * seconds for the next 2 minutes; the open page renews it every minute and
 * stops it when paused or hidden. Only one live request waits at a time.
 */
function ui_live(action, every) {
  requireOwner_();
  every = Math.max(3, Math.min(60, Number(every) || 5));
  withLock_(() => {
    const props = PropertiesService.getScriptProperties();
    const now = Date.now();
    let cmds = JSON.parse(props.getProperty('COMMANDS') || '[]')
      .filter(c => now - c.created < CMD_KEEP_MS)
      .filter(c => !(c.state === 'queued' && (c.type === 'live' || c.type === 'stop')));  // superseded
    const id = now.toString(36) + Math.floor(Math.random() * 1e6).toString(36);
    if (action === 'start') cmds.push({ id: id, type: 'live', secs: 120, every: every, created: now, state: 'queued' });
    else cmds.push({ id: id, type: 'stop', secs: 0, created: now, state: 'queued' });
    props.setProperty('COMMANDS', JSON.stringify(cmds.slice(-20)));
  });
  return ui_state();
}

/** Deletes live-view frames older than 15 minutes in a day folder (at most every 5 minutes). */
function pruneLiveFrames_(folderId) {
  const cache = CacheService.getScriptCache();
  if (cache.get('LIVE_PRUNED_' + folderId)) return;
  cache.put('LIVE_PRUNED_' + folderId, '1', 300);
  try {
    const cutoff = new Date(Date.now() - 15 * 60 * 1000).toISOString();
    const res = Drive.Files.list({
      q: `'${folderId}' in parents and name contains '-l.jp' and createdTime < '${cutoff}' and trashed = false`,
      fields: 'files(id, name)', pageSize: 200,
    });
    (res.files || []).forEach(f => {
      if (/-l\.jpe?g$/i.test(f.name)) Drive.Files.update({ trashed: true }, f.id);
    });
  } catch (err) {
    console.error('live frame cleanup: ' + err);
  }
}

/** The newest photo, for the page's "latest" panel. Kept in the script cache. */
function rememberLatest_(id, name) {
  try {  // best effort: never fails an upload report
    const cache = CacheService.getScriptCache();
    const cur = JSON.parse(cache.get('LATEST') || 'null');
    if (cur && cur.name > name) return;  // an older photo reported late
    cache.put('LATEST', JSON.stringify({ id: id, name: name }), 21600);
  } catch (err) {
    console.error('latest photo not recorded: ' + err);
  }
}

function latestPhoto_() {
  const cache = CacheService.getScriptCache();
  const cur = JSON.parse(cache.get('LATEST') || 'null');
  if (cur) return cur;
  try {  // cache expired: look it up once (drive.file: only the script's own files)
    const res = Drive.Files.list({
      q: "mimeType = 'image/jpeg' and trashed = false",
      orderBy: 'createdTime desc', fields: 'files(id, name)', pageSize: 1,
    });
    const f = res.files && res.files[0];
    if (!f) return null;
    cache.put('LATEST', JSON.stringify({ id: f.id, name: f.name }), 21600);
    return { id: f.id, name: f.name };
  } catch (err) {
    return null;
  }
}

/**
 * A photo or clip itself, for the page's viewer: base64 plus its Drive link.
 * Photos are ~150 KB and clips ~5 MB; anything over 25 MB is only linked.
 */
function ui_media(id) {
  requireOwner_();
  id = String(id);
  if (!/^[A-Za-z0-9_-]{10,100}$/.test(id)) throw new Error('Bad id');
  const f = Drive.Files.get(id, { fields: 'id, name, mimeType, size, webViewLink' });  // own files only
  if (!/^(image|video)\//.test(f.mimeType)) throw new Error('Not a photo or clip');
  const out = { id: f.id, name: f.name, mime: f.mimeType, url: f.webViewLink };
  if (Number(f.size || 0) > 25 * 1024 * 1024) {
    out.tooBig = true;
    return out;
  }
  const r = UrlFetchApp.fetch('https://www.googleapis.com/drive/v3/files/' + encodeURIComponent(f.id) + '?alt=media',
    { headers: { Authorization: 'Bearer ' + ScriptApp.getOAuthToken() }, muteHttpExceptions: true });
  if (r.getResponseCode() !== 200) throw new Error('Drive answered HTTP ' + r.getResponseCode());
  out.data = Utilities.base64Encode(r.getBlob().getBytes());
  return out;
}

/** Small thumbnails as data URLs, for up to 12 of the script's own files. */
function ui_thumbs(ids) {
  requireOwner_();
  ids = (Array.isArray(ids) ? ids : []).map(String).filter(id => /^[A-Za-z0-9_-]{10,100}$/.test(id)).slice(0, 12);
  const cache = CacheService.getScriptCache();
  const out = {};
  const have = cache.getAll(ids.map(id => 'td_' + id));
  const links = cache.getAll(ids.map(id => 'th_' + id));
  const todo = [];
  for (const id of ids) {
    if (have['td_' + id]) { out[id] = have['td_' + id]; continue; }
    let link = links['th_' + id];
    if (!link) {
      try {  // drive.file: only the script's own files are reachable
        link = Drive.Files.get(id, { fields: 'thumbnailLink' }).thumbnailLink;
      } catch (err) {
        link = '';
      }
    }
    // Only ever fetch Google's own thumbnail hosts with the token.
    if (link && /^https:\/\/[a-z0-9.-]+\.(googleusercontent|google)\.com\//.test(link)) {
      todo.push({ id: id, url: link.replace(/=s\d+$/, '=s320') });
    }
  }
  if (todo.length) {
    const token = ScriptApp.getOAuthToken();
    const res = UrlFetchApp.fetchAll(todo.map(t => ({
      url: t.url, headers: { Authorization: 'Bearer ' + token }, muteHttpExceptions: true, followRedirects: true,
    })));
    const put = {};
    res.forEach((r, i) => {
      if (r.getResponseCode() !== 200) return;
      const b = r.getBlob();
      const data = 'data:' + (b.getContentType() || 'image/jpeg') + ';base64,' + Utilities.base64Encode(b.getBytes());
      out[todo[i].id] = data;
      if (data.length < 90000) put['td_' + todo[i].id] = data;
    });
    if (Object.keys(put).length) cache.putAll(put, 21600);
  }
  return out;
}

/**
 * Run once from the editor: authorizes the script, creates the YiCam folder
 * and installs the daily cleanup trigger. Safe to run again.
 */
function setup() {
  const me = Session.getActiveUser().getEmail();
  if (me) {
    PropertiesService.getScriptProperties().setProperty('OWNER_EMAIL', me);
    Logger.log('Control page owner recorded (the account running setup).');
  } else {
    Logger.log('Could not read your email: approve the email permission and run setup again.');
  }
  const rootId = rootFolderId_();
  ScriptApp.getProjectTriggers()
    .filter(t => t.getHandlerFunction() === 'cleanup')
    .forEach(t => ScriptApp.deleteTrigger(t));
  ScriptApp.newTrigger('cleanup').timeBased().everyDays(1).atHour(4).create();
  Logger.log('Images folder: https://drive.google.com/drive/folders/' + rootId);
  Logger.log('Daily cleanup trigger installed (around 04:00, ' + Session.getScriptTimeZone() + ').');
}

/** Moves day folders older than RETENTION_DAYS to the Drive bin. */
function cleanup() {
  const days = Number(PropertiesService.getScriptProperties().getProperty('RETENTION_DAYS') || 30);
  const today = new Date();
  const cutoff = new Date(today.getFullYear(), today.getMonth(), today.getDate() - days);
  const rootId = rootFolderId_();
  let pageToken;
  do {
    const res = Drive.Files.list({
      q: `'${rootId}' in parents and mimeType = '${FOLDER_MIME}' and trashed = false`,
      fields: 'nextPageToken, files(id, name)',
      pageSize: 100,
      pageToken: pageToken,
    });
    (res.files || []).forEach(f => {
      const m = DAY_RE.exec(f.name);
      if (m && new Date(+m[1], +m[2] - 1, +m[3]) < cutoff) {
        Drive.Files.update({ trashed: true }, f.id);
        Logger.log('Trashed ' + f.name);
      }
    });
    pageToken = res.nextPageToken;
  } while (pageToken);
}

/**
 * Run from the editor after setting NOTIFY_EMAIL: asks for the email
 * permissions (first time only) and sends a test email with the most recent
 * photo attached.
 */
function testNotification() {
  if (!PropertiesService.getScriptProperties().getProperty('NOTIFY_EMAIL')) {
    throw new Error('Set the NOTIFY_EMAIL script property first.');
  }
  const res = Drive.Files.list({
    q: "mimeType = 'image/jpeg' and trashed = false",
    orderBy: 'createdTime desc',
    fields: 'files(id, name, parents)',
    pageSize: 1,
  });
  const f = res.files && res.files[0];
  if (!f) throw new Error('No photos yet. Take a snapshot from the bridge dashboard first.');
  const now = Date.now();
  sendNotificationEmail_({ photos: [{ id: f.id, name: f.name }], total: 1, first: now, last: now,
                           dayId: f.parents[0] }, true);
  Logger.log('Test email sent to ' + PropertiesService.getScriptProperties().getProperty('NOTIFY_EMAIL'));
}

/** Trigger handler: emails the collected photos once motion has gone quiet. */
function sendNotification() {
  const props = PropertiesService.getScriptProperties();
  const st = withLock_(() => {
    deleteTriggers_('sendNotification');  // one-time triggers linger after firing
    const s = JSON.parse(props.getProperty('NOTIFY_STATE') || 'null');
    if (!s || !s.photos.length) {
      props.deleteProperty('NOTIFY_STATE');
      return null;
    }
    const now = Date.now();
    if (now - s.last < NOTIFY_QUIET_MS && now - s.first < NOTIFY_MAX_WAIT_MS) {
      scheduleNotification_(NOTIFY_QUIET_MS - (now - s.last) + 5000);  // still active: wait more
      return null;
    }
    props.deleteProperty('NOTIFY_STATE');
    return s;
  });
  if (st) sendNotificationEmail_(st, false);
}

// ---------------------------------------------------------------------------

/** Adds a saved photo to the pending email and makes sure one is scheduled. */
function queueNotification_(fileId, name, dayId) {
  const props = PropertiesService.getScriptProperties();
  if (!props.getProperty('NOTIFY_EMAIL')) return;
  withLock_(() => {
    const now = Date.now();
    let st = JSON.parse(props.getProperty('NOTIFY_STATE') || 'null');
    if (!st) {
      // New event. Inside the cooldown: no email, just count it for the next one.
      const lastSent = Number(props.getProperty('NOTIFY_LAST_SENT') || 0);
      if (lastSent && now - lastSent < cooldownMs_(props)) {
        props.setProperty('NOTIFY_SUPPRESSED', String(Number(props.getProperty('NOTIFY_SUPPRESSED') || 0) + 1));
        return;
      }
      st = { photos: [], total: 0, first: now, scheduled: false };
    }
    if (st.photos.length < NOTIFY_MAX_PHOTOS) st.photos.push({ id: fileId, name: name });
    st.total++;
    st.last = now;
    st.dayId = dayId;
    if (!st.scheduled) {
      scheduleNotification_(NOTIFY_QUIET_MS + 5000);
      st.scheduled = true;
    }
    props.setProperty('NOTIFY_STATE', JSON.stringify(st));
  });
}

/**
 * A video clip was uploaded. If its event's email hasn't gone out yet, it is
 * mentioned there; otherwise it just appears in the day folder (clips arrive
 * a minute or two after motion stops, often after the email).
 */
function noteVideo_(fileId, name) {
  const props = PropertiesService.getScriptProperties();
  if (!props.getProperty('NOTIFY_EMAIL')) return;
  withLock_(() => {
    const st = JSON.parse(props.getProperty('NOTIFY_STATE') || 'null');
    if (!st) return;
    st.videos = (st.videos || 0) + 1;
    props.setProperty('NOTIFY_STATE', JSON.stringify(st));
  });
}

function scheduleNotification_(ms) {
  deleteTriggers_('sendNotification');
  ScriptApp.newTrigger('sendNotification').timeBased().after(Math.max(ms, 10000)).create();
}

function deleteTriggers_(handler) {
  ScriptApp.getProjectTriggers()
    .filter(t => t.getHandlerFunction() === handler)
    .forEach(t => ScriptApp.deleteTrigger(t));
}

function sendNotificationEmail_(st, isTest) {
  const props = PropertiesService.getScriptProperties();
  const to = props.getProperty('NOTIFY_EMAIL');
  if (!to) return;
  if (MailApp.getRemainingDailyQuota() < 1) {
    console.warn('Daily email quota used up; notification skipped.');
    return;
  }

  // Read the photos back through the Drive API with this script's own token
  // (drive.file: it can only reach files this script created).
  const token = ScriptApp.getOAuthToken();
  const blobs = [];
  let bytes = 0;
  for (const p of st.photos) {
    const r = UrlFetchApp.fetch(
      'https://www.googleapis.com/drive/v3/files/' + encodeURIComponent(p.id) + '?alt=media',
      { headers: { Authorization: 'Bearer ' + token }, muteHttpExceptions: true });
    if (r.getResponseCode() !== 200) continue;  // deleted meanwhile, etc.
    const b = r.getBlob().setName(p.name).setContentType('image/jpeg');
    const n = b.getBytes().length;
    if (bytes + n > NOTIFY_MAX_BYTES) break;
    bytes += n;
    blobs.push(b);
  }

  const tz = Session.getScriptTimeZone();
  const start = Utilities.formatDate(new Date(st.first), tz, 'HH:mm');
  const end = Utilities.formatDate(new Date(st.last), tz, 'HH:mm');
  const when = start === end ? start : start + '–' + end;
  const day = Utilities.formatDate(new Date(st.first), tz, 'EEE d MMM');
  const plural = st.total === 1 ? 'photo' : 'photos';
  const subject = (isTest ? '[Test] ' : '') + 'YiCam: motion at ' + when + ' (' + st.total + ' ' + plural + ')';
  const folderUrl = 'https://drive.google.com/drive/folders/' + encodeURIComponent(st.dayId);
  const missing = st.total - blobs.length;
  const suppressed = isTest ? 0 : Number(props.getProperty('NOTIFY_SUPPRESSED') || 0);
  const cooldownMin = Math.round(cooldownMs_(props) / 60000);
  const html =
    '<p>Motion on <b>' + day + '</b>, ' + when + ': ' + st.total + ' ' + plural + ' saved to Drive.</p>' +
    (blobs.length ? '<p>' + blobs.length + ' attached.' +
      (missing > 0 ? ' ' + missing + ' more in the folder.' : '') + '</p>' : '<p>The photos could not be attached.</p>') +
    (st.videos ? '<p>' + st.videos + ' video ' + (st.videos === 1 ? 'clip' : 'clips') +
      ' uploaded to the folder; more may follow.</p>' : '') +
    (suppressed ? '<p>Since the previous email, ' + suppressed + ' more ' + (suppressed === 1 ? 'photo was' : 'photos were') +
      ' saved without an email.</p>' : '') +
    '<p><a href="' + folderUrl + '">Open today\'s folder in Drive</a></p>' +
    '<p style="color:#888;font-size:12px">' +
    (cooldownMin ? 'No more emails for ' + cooldownMin + ' minutes; new photos keep going to the folder. ' : '') +
    'Sent by your yicam-bridge Apps Script. To stop these emails, delete the NOTIFY_EMAIL script property.</p>';
  MailApp.sendEmail({ to: to, subject: subject, htmlBody: html, attachments: blobs, name: 'YiCam' });
  if (!isTest) {
    props.setProperty('NOTIFY_LAST_SENT', String(Date.now()));  // starts the cooldown
    props.deleteProperty('NOTIFY_SUPPRESSED');
  }
}

// ---------------------------------------------------------------------------

/** ID of the script's own YiCam folder, created (once) if missing or deleted. */
function rootFolderId_() {
  const props = PropertiesService.getScriptProperties();
  const id = props.getProperty('ROOT_FOLDER_ID');
  if (id && isLiveFolder_(id)) return id;

  return withLock_(() => {
    const again = props.getProperty('ROOT_FOLDER_ID');  // another request may have won
    if (again && again !== id && isLiveFolder_(again)) return again;
    const folder = Drive.Files.create(
      { name: ROOT_FOLDER_NAME, mimeType: FOLDER_MIME }, null, { fields: 'id' });
    props.setProperty('ROOT_FOLDER_ID', folder.id);
    return folder.id;
  });
}

/** ID of the yyyy-MM-dd subfolder, created if missing. */
function dayFolderId_(day) {
  const rootId = rootFolderId_();
  const find = () => {
    const res = Drive.Files.list({
      q: `'${rootId}' in parents and name = '${day}' and mimeType = '${FOLDER_MIME}' and trashed = false`,
      fields: 'files(id)',
      pageSize: 1,
    });
    return res.files && res.files.length ? res.files[0].id : null;
  };
  return find() || withLock_(() => find() || Drive.Files.create(
    { name: day, parents: [rootId], mimeType: FOLDER_MIME }, null, { fields: 'id' }).id);
}

function isLiveFolder_(id) {
  try {
    const f = Drive.Files.get(id, { fields: 'id, mimeType, trashed' });
    return f.mimeType === FOLDER_MIME && !f.trashed;
  } catch (err) {
    return false;  // deleted, or not created by this script
  }
}

/** Serializes folder creation so two simultaneous uploads don't create duplicates. */
function withLock_(fn) {
  const lock = LockService.getScriptLock();
  lock.waitLock(20000);
  try {
    return fn();
  } finally {
    lock.releaseLock();
  }
}

function sanitizeName_(s) {
  if (!s) return '';
  const clean = String(s).replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 80);
  return /\.(jpe?g|mp4)$/i.test(clean) ? clean : clean + '.jpg';
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
