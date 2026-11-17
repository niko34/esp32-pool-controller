// =============================================================================
// FICHIER GÉNÉRÉ — NE PAS ÉDITER À LA MAIN (feature-151 D3-b1, ADR-0124)
// Source : src/ota_recovery_page.html.in + extraits de data/app.js
// Régénérer : node tools/gen_recovery_page.js
// Contrôle  : tools/check_recovery_page_generated.sh (rouge à la moindre dérive)
// =============================================================================
#include "ota_recovery_page.h"
#include "version.h"

const char kOtaRecoveryPageHtml[] = R"RECOVERYPAGE(<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PoolController • Mode secours</title>
<style>
:root{--bg:#e8ecf4;--panel:#fff;--text:#111827;--muted:#4b5563;--accent:#2563eb;--danger:#b91c1c;--ok:#15803d;--line:#6b7280}
*{box-sizing:border-box}
body{margin:0;font:16px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
main{max-width:560px;margin:0 auto;padding:0 16px 32px}
h1{font-size:22px;margin:8px 0 12px}
.card{background:var(--panel);border:1px solid rgba(0,0,0,.1);border-radius:16px;padding:16px;margin:12px 0}
.callout{padding:12px;border-radius:14px;border:1px solid rgba(37,99,235,.3);background:rgba(37,99,235,.08)}
.callout p{margin:0}.callout p+p{margin-top:6px}
a{color:var(--accent);font-weight:600}
label{display:block;font-weight:600;margin:12px 0 4px}
input:not(.vh){display:block;width:100%;min-height:44px;font:inherit;padding:8px 10px;border:1px solid var(--line);border-radius:10px;background:#fff;color:var(--text)}
button{display:block;width:100%;min-height:44px;margin-top:16px;font:inherit;font-weight:600;border:0;border-radius:10px;background:var(--accent);color:#fff}
button:disabled{opacity:.5}
progress{width:100%;height:12px;margin-top:12px}
.hint{color:var(--muted);font-size:14px;margin:4px 0 0}
#msg{margin-top:12px;font-weight:600}
#msg.err{color:var(--danger);border:1px solid var(--danger);border-radius:10px;padding:10px}
#msg.ok{color:var(--ok)}
.vh{position:absolute;width:1px;height:1px;opacity:0;overflow:hidden}
.pick{display:block;min-height:56px;margin:12px 0 0;padding:8px 10px;border:2px dashed var(--line);border-radius:10px;font-weight:400}
.pick b{display:block}
.pick span{display:block;color:var(--muted);font-size:14px;overflow-wrap:anywhere}
.pick.has{border-style:solid;border-color:var(--accent)}
.pick.has span{color:var(--text)}
.vh:focus-visible+.pick{outline:3px solid var(--accent);outline-offset:2px}
.vh:disabled+.pick{opacity:.5}
.ver{margin:0 0 4px;color:var(--muted)}.ver b{color:var(--text)}
[hidden]{display:none!important}
</style>
</head>
<body>
<main>
<h1>Interface à restaurer</h1>
<div class="callout">
<p><b>L'interface web est absente ou endommagée.</b></p>
<p>La régulation continue : pH, ORP, pompes et filtration fonctionnent.</p>
</div>
<p><a href="/ui">Ouvrir l'interface quand même</a></p>

<section class="card" id="s-wait"><p id="t-wait">Vérification de l'appareil…</p></section>

<section class="card" id="s-login" hidden>
<label for="pw">Mot de passe administrateur</label>
<input id="pw" type="password" autocomplete="current-password">
<button id="b-login" type="button">Se connecter</button>
</section>

<section class="card" id="s-first" hidden>
<p class="hint">Choisissez un mot de passe administrateur (8 caractères minimum).</p>
<label for="pw1">Nouveau mot de passe</label>
<input id="pw1" type="password" autocomplete="new-password">
<label for="pw2">Confirmation</label>
<input id="pw2" type="password" autocomplete="new-password">
<button id="b-first" type="button">Enregistrer et continuer</button>
</section>

<section class="card" id="s-up" hidden>
<p class="ver">Version du firmware : <b>)RECOVERYPAGE" FIRMWARE_VERSION R"RECOVERYPAGE(</b></p>
<p class="hint">Les deux fichiers sont nécessaires.</p>
<input id="f-bin" class="vh" type="file" accept=".bin">
<label class="pick" for="f-bin"><b>Image (.bin)</b><span id="n-bin">Appuyer pour choisir un fichier</span></label>
<input id="f-sig" class="vh" type="file" accept=".sig">
<label class="pick" for="f-sig"><b>Signature (.sig)</b><span id="n-sig">Appuyer pour choisir un fichier</span></label>
<button id="b-send" type="button" disabled>Vérifier et envoyer</button>
<progress id="pg" max="100" value="0" hidden></progress>
<p id="t-pg" class="hint" hidden></p>
</section>

<p id="msg" role="status" aria-live="polite" hidden></p>
<p id="home" hidden><a href="/">Ouvrir l'interface</a></p>
</main>
<script>
"use strict";
const SHA256_K = new Uint32Array([
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
]);
function createSha256() {
  const h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const w = new Uint32Array(64);
  const block = new Uint8Array(64);
  let blockLen = 0;
  let total = 0;

  function compress(buf, off) {
    for (let i = 0; i < 16; i++) {
      w[i] = (buf[off + i * 4] << 24) | (buf[off + i * 4 + 1] << 16) | (buf[off + i * 4 + 2] << 8) | buf[off + i * 4 + 3];
    }
    for (let i = 16; i < 64; i++) {
      const x = w[i - 15], y = w[i - 2];
      const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + SHA256_K[i] + w[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) | 0;
      hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
    h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
  }

  return {
    update(bytes) {
      total += bytes.length;
      let i = 0;
      if (blockLen) {
        const need = Math.min(64 - blockLen, bytes.length);
        block.set(bytes.subarray(0, need), blockLen);
        blockLen += need;
        i = need;
        if (blockLen === 64) { compress(block, 0); blockLen = 0; }
      }
      for (; i + 64 <= bytes.length; i += 64) compress(bytes, i);
      if (i < bytes.length) {
        block.set(bytes.subarray(i), 0);
        blockLen = bytes.length - i;
      }
    },
    hex() {
      const bitLenHi = Math.floor(total / 0x20000000);
      const bitLenLo = (total * 8) >>> 0;
      const pad = new Uint8Array(blockLen < 56 ? 64 - blockLen : 128 - blockLen);
      pad[0] = 0x80;
      pad[pad.length - 8] = (bitLenHi >>> 24) & 0xff;
      pad[pad.length - 7] = (bitLenHi >>> 16) & 0xff;
      pad[pad.length - 6] = (bitLenHi >>> 8) & 0xff;
      pad[pad.length - 5] = bitLenHi & 0xff;
      pad[pad.length - 4] = (bitLenLo >>> 24) & 0xff;
      pad[pad.length - 3] = (bitLenLo >>> 16) & 0xff;
      pad[pad.length - 2] = (bitLenLo >>> 8) & 0xff;
      pad[pad.length - 1] = bitLenLo & 0xff;
      this.update(pad);
      let out = "";
      for (let i = 0; i < 8; i++) out += (h[i] >>> 0).toString(16).padStart(8, "0");
      return out;
    },
  };
}
async function sha256OfFile(file, onProgress) {
  const hasher = createSha256();
  const CHUNK = 262144;
  for (let off = 0; off < file.size; off += CHUNK) {
    const slice = file.slice(off, Math.min(off + CHUNK, file.size));
    const buf = await slice.arrayBuffer();
    hasher.update(new Uint8Array(buf));
    if (onProgress) onProgress(Math.min(off + CHUNK, file.size) / file.size);
  }
  return hasher.hex();
}
function parseOtaManifestLine(line) {
  const tokens = line.split(" ");
  if (tokens.length !== 4) return null;
  const [magic, kindTok, versionTok, hexTok] = tokens;
  if (magic !== "pool-ota/1") return null;
  let genre;
  if (kindTok === "firmware") genre = "firmware";
  else if (kindTok === "filesystem") genre = "filesystem";
  else return null;
  const versionMatch = /^(\d{1,5})\.(\d{1,5})\.(\d{1,5})$/.exec(versionTok);
  if (!versionMatch) return null;
  for (let i = 1; i <= 3; i++) {
    const comp = versionMatch[i];
    if (comp.length > 1 && comp[0] === "0") return null;  // zéro de tête interdit
  }
  if (!hexTok.startsWith("sha256:")) return null;
  const hex = hexTok.slice(7);
  if (!/^[0-9a-f]{64}$/.test(hex)) return null;  // 64 hex STRICTEMENT minuscules
  return { genre, version: versionTok, digest: hex };
}
function parseSigFile(text) {
  if (typeof text !== "string" || text.length === 0) {
    return { ok: false, code: "vide" };
  }
  let normalized = text.replace(/\r\n/g, "\n");
  if (normalized.endsWith("\n")) {
    normalized = normalized.slice(0, -1);
  }
  const lines = normalized.split("\n");
  if (lines.length !== 2 || lines[0].length === 0 || lines[1].length === 0) {
    return { ok: false, code: "lignes" };
  }
  const manifestLine = lines[0];
  const parsed = parseOtaManifestLine(manifestLine);
  if (!parsed) {
    return { ok: false, code: "manifeste" };
  }
  const signatureLine = lines[1];
  if (signatureLine.length !== 512 || !/^[A-Za-z0-9+/]{512}$/.test(signatureLine)) {
    return { ok: false, code: "signature" };
  }
  return {
    ok: true,
    manifest: manifestLine,
    signature: signatureLine,
    genre: parsed.genre,
    version: parsed.version,
    digest: parsed.digest,
  };
}

const $ = (id) => document.getElementById(id);
const show = (id, on) => { $(id).hidden = !on; };
const TOKEN_KEY = "authToken";

// Message : textContent UNIQUEMENT, texte fixe, jamais le texte du serveur ni du .sig.
function say(kind, text) {
  const m = $("msg");
  m.hidden = !text;
  m.className = kind;
  m.textContent = text || "";
}
const fail = (cause) => say("err", "Échec : " + cause);

// Rend la main au navigateur entre deux tranches : sans cela la barre ne se
// repaint pas sur téléphone pendant le calcul de l'empreinte.
function yielding(file) {
  return {
    size: file.size,
    slice: (a, b) => {
      const s = file.slice(a, b);
      return { arrayBuffer: () => new Promise((r) => setTimeout(r, 20)).then(() => s.arrayBuffer()) };
    },
  };
}

function readText(file) {
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload = () => resolve(String(r.result));
    r.onerror = () => reject(new Error("lecture"));
    r.readAsText(file);
  });
}

async function postJson(path, body) {
  const res = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  let data = null;
  try { data = await res.json(); } catch (e) { data = null; }
  return { ok: res.ok, status: res.status, data };
}

// Jeton périmé ou refusé : on l'OUBLIE et on remet l'utilisateur devant le
// formulaire. Un simple message bouclerait : le rechargement relirait le même jeton.
function backToLogin() {
  sessionStorage.removeItem(TOKEN_KEY);
  show("s-up", false); show("s-first", false); show("s-wait", false);
  show("s-login", true);
}

function openUpload() {
  show("s-wait", false); show("s-login", false); show("s-first", false);
  show("s-up", true);
  say("", "");
}

function gotToken(data) {
  if (!data || typeof data.token !== "string") { fail("réponse inattendue de l'appareil."); return; }
  sessionStorage.setItem(TOKEN_KEY, data.token);
  openUpload();
}

async function start() {
  try {
    const res = await fetch("/auth/status");
    const st = await res.json();
    show("s-wait", false);
    if (st.firstBoot) { show("s-first", true); return; }
    if (st.authEnabled === false || sessionStorage.getItem(TOKEN_KEY)) { openUpload(); return; }
    show("s-login", true);
  } catch (e) {
    show("s-wait", false);
    show("s-login", true);
    fail("appareil injoignable.");
  }
}

$("b-login").addEventListener("click", async () => {
  say("", "");
  try {
    const r = await postJson("/auth/login", { username: "admin", password: $("pw").value });
    if (r.ok) gotToken(r.data);
    else fail(r.status === 401 ? "mot de passe incorrect." : "connexion refusée (HTTP " + r.status + ").");
  } catch (e) { fail("appareil injoignable."); }
});

$("b-first").addEventListener("click", async () => {
  say("", "");
  const a = $("pw1").value;
  if (a.length < 8) { fail("8 caractères minimum."); return; }
  if (a !== $("pw2").value) { fail("les deux mots de passe diffèrent."); return; }
  try {
    // "admin" : mot de passe d'usine du premier démarrage, seul littéral admis.
    const r = await postJson("/auth/change-password", { currentPassword: "admin", newPassword: a });
    if (r.ok) gotToken(r.data);
    else fail("enregistrement refusé (HTTP " + r.status + ").");
  } catch (e) { fail("appareil injoignable."); }
});

function refreshSend() {
  for (const k of ["bin", "sig"]) {
    const f = $("f-" + k).files[0];
    $("n-" + k).textContent = f ? f.name : "Appuyer pour choisir un fichier";
    $("f-" + k).nextElementSibling.classList.toggle("has", !!f);
  }
  $("b-send").disabled = !($("f-bin").files.length && $("f-sig").files.length);
}
$("f-bin").addEventListener("change", refreshSend);
$("f-sig").addEventListener("change", refreshSend);

function progress(label, pct) {
  show("pg", true); show("t-pg", true);
  $("pg").value = pct;
  $("t-pg").textContent = label + " : " + pct + " %";
}

function waitForDevice() {
  say("ok", "Envoi terminé. L'appareil redémarre, patientez…");
  let tries = 0;
  const tick = async () => {
    tries++;
    try {
      const res = await fetch("/auth/status", { cache: "no-store" });
      if (res.ok) {
        say("ok", "Terminé : l'appareil est de retour.");
        show("home", true);
        return;
      }
    } catch (e) { /* appareil encore en redémarrage */ }
    if (tries > 45) { fail("l'appareil ne répond pas, rechargez la page."); return; }
    setTimeout(tick, 2000);
  };
  setTimeout(tick, 8000);
}

$("b-send").addEventListener("click", async () => {
  const bin = $("f-bin").files[0];
  const sigFile = $("f-sig").files[0];
  if (!bin || !sigFile) return;
  say("", "");
  $("b-send").disabled = true; $("f-bin").disabled = true; $("f-sig").disabled = true;
  const rearm = () => { $("f-bin").disabled = false; $("f-sig").disabled = false; refreshSend(); };
  try {
    let sig;
    try { sig = parseSigFile(await readText(sigFile)); } catch (e) { sig = { ok: false }; }
    if (!sig.ok) { fail("fichier de signature invalide."); rearm(); return; }

    progress("Vérification du fichier", 0);
    say("", "Cela peut durer quelques secondes. Ne fermez pas cette page.");
    const hex = await sha256OfFile(yielding(bin), (ratio) => {
      progress("Vérification du fichier", Math.round(ratio * 100));
    });
    if (hex !== sig.digest) { fail("le fichier ne correspond pas à la signature."); rearm(); return; }
    say("", "");

    // ORDRE IMPOSÉ : l'appareil lit les champs PENDANT l'analyse de la partie
    // fichier — un champ placé après update_file se lit comme absent.
    const fd = new FormData();
    fd.append("sha256", hex);
    fd.append("update_type", sig.genre);
    fd.append("manifest", sig.manifest);
    fd.append("signature", sig.signature);
    fd.append("update_file", bin);

    const xhr = new XMLHttpRequest();
    xhr.upload.addEventListener("progress", (e) => {
      if (e.lengthComputable) progress("Envoi", Math.round((e.loaded / e.total) * 100));
    });
    xhr.addEventListener("load", () => {
      if (xhr.status === 200 && xhr.responseText.trim() === "OK") { waitForDevice(); return; }
      if (xhr.status === 401) {
        backToLogin();
        fail("connexion expirée, reconnectez-vous.");
      } else {
        // La PHRASE de l'appareil (corps « FAIL: <phrase> »), telle quelle, en textContent.
        let c = String(xhr.responseText || "").replace(/^\s*FAIL\s*:?\s*/i, "").trim().slice(0, 200);
        fail(c || "Mise à jour refusée. Consultez le journal (onglet Avancé).");
      }
      rearm();
    });
    xhr.addEventListener("error", () => { fail("erreur réseau."); rearm(); });
    xhr.open("POST", "/update");
    const t = sessionStorage.getItem(TOKEN_KEY);
    if (t) xhr.setRequestHeader("X-Auth-Token", t);
    xhr.send(fd);
  } catch (e) {
    fail("calcul de l'empreinte impossible.");
    rearm();
  }
});

start();
</script>
</body>
</html>
)RECOVERYPAGE";
const size_t kOtaRecoveryPageHtmlLen = sizeof(kOtaRecoveryPageHtml) - 1;
