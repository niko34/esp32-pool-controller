// Vecteurs de l'attente du retour de l'appareil après mise à jour (bug-177).
// EXTRAIT le texte vivant de data/app.js (ADR-0099), jamais une recopie :
// OTA_WAIT_CFG, otaProbeOutcome, otaRebootWaitStep, otaRecoveryTarget,
// otaFetchNoStore (fetch simulé : couvre le ?t= anti-cache et le fetch nu).
// Exécution : node test/ota_wait_vectors.js
"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
const fn = (a) => extractBalancedFromAnchor(src, a, "{", "}").text;
const cfgMatch = src.match(/const OTA_WAIT_CFG = \{[^}]*\};/);
if (!cfgMatch) { console.error("OTA_WAIT_CFG introuvable"); process.exit(1); }

const calls = [];
const live = new Function("fetch", "AbortController", `
  "use strict";
  ${cfgMatch[0]}
  ${fn("function otaProbeOutcome(res) {")}
  ${fn("function otaRebootWaitStep(st, cfg) {")}
  ${fn("function otaRecoveryTarget(o) {")}
  ${fn("async function otaFetchNoStore(path, timeoutMs) {")}
  return { OTA_WAIT_CFG, otaProbeOutcome, otaRebootWaitStep, otaRecoveryTarget, otaFetchNoStore };
`)(
  async (url, opts) => { calls.push({ url, opts }); return { status: 200 }; },
  class { constructor() { this.signal = {}; } abort() {} }
);
const { OTA_WAIT_CFG: cfg, otaProbeOutcome, otaRebootWaitStep, otaRecoveryTarget, otaFetchNoStore } = live;

let failures = 0;
function check(label, got, want) {
  try { assert.deepStrictEqual(got, want); console.log("OK   " + label); }
  catch (_) { failures++; console.log(`FAIL ${label}\n  obtenu  : ${JSON.stringify(got)}\n  attendu : ${JSON.stringify(want)}`); }
}
const step = (elapsedS, outcome, attempts = 0) =>
  otaRebootWaitStep({ startedMs: 1000, nowMs: 1000 + elapsedS * 1000, attempts, outcome }, cfg);
const act = (s, o, a) => step(s, o, a).action;

// otaProbeOutcome
check("probe 200 => servable", otaProbeOutcome({ status: 200 }), "servable");
check("probe 204 => servable", otaProbeOutcome({ status: 204 }), "servable");
check("probe 429 => alive (preuve de vie)", otaProbeOutcome({ status: 429 }), "alive");
check("probe 503 => alive", otaProbeOutcome({ status: 503 }), "alive");
check("probe 500 => alive", otaProbeOutcome({ status: 500 }), "alive");
check("probe 404 => alive", otaProbeOutcome({ status: 404 }), "alive");
check("probe 302 => alive", otaProbeOutcome({ status: 302 }), "alive");
check("probe null => absent", otaProbeOutcome(null), "absent");

// GRÂCE : jamais reload/confirm avant 8 s
for (const t of [0, 1, 3, 7.9]) {
  check(`grace ${t}s + null => wait`, act(t, null), "wait");
  check(`grace ${t}s + alive => wait`, act(t, "alive", 1), "wait");
  check(`grace ${t}s + absent => wait`, act(t, "absent", 1), "wait");
  check(`grace ${t}s + servable => wait (JAMAIS reload/confirm)`, act(t, "servable"), "wait");
  check(`grace ${t}s + confirmed => wait (JAMAIS reload)`, act(t, "confirmed"), "wait");
}
check("grace raison", step(3, null).reason, "grace");
check("grace : delayMs = reste de grace", step(3, null).delayMs, 5000);

// Après la grâce
check("8 s + null => probe", act(8, null, 0), "probe");
check("14 s + servable => confirm", act(14, "servable"), "confirm");
check("14 s + confirmed => reload", act(14, "confirmed"), "reload");
check("confirm non-2xx (alive) => probe, pas reload", act(16, "alive", 2), "probe");
check("confirm non-2xx (absent) => probe", act(16, "absent", 2), "probe");
check("429 (alive) à 20 s => probe, ni reload ni giveup", act(20, otaProbeOutcome({ status: 429 }), 3), "probe");
check("503 (alive) à 20 s => probe", act(20, otaProbeOutcome({ status: 503 }), 3), "probe");
check("alive => raison alive", step(20, "alive", 3).reason, "alive");
check("absent => raison absent", step(20, "absent", 3).reason, "absent");

// Borne du budget (>=)
check("absent à 89 s => pas giveup", act(89, "absent", 40), "probe");
check("absent à 89,999 s => pas giveup", act(89.999, "absent", 40), "probe");
check("absent à 90 s => giveup (borne exacte)", act(90, "absent", 40), "giveup");
check("giveup raison timeout", step(90, "absent", 40).reason, "timeout");
check("alive à 95 s => giveup", act(95, "alive", 40), "giveup");

// delayMs / remainingS
check("delayMs 0 à la 1re tentative", step(10, null, 0).delayMs, 0);
check("delayMs 2000 ensuite", step(10, "absent", 1).delayMs, 2000);
check("remainingS 47 s écoulées => 43 réels => 45", step(47, "absent", 5).remainingS, 45);
check("remainingS exact multiple (40 s restants) => 40", step(50, "absent", 5).remainingS, 40);
check("remainingS 41 s restants => 45", step(49, "absent", 5).remainingS, 45);
check("remainingS 1 s restante => 5", step(89, "absent", 5).remainingS, 5);
check("remainingS début => 90", step(0, null).remainingS, 90);
check("remainingS jamais négatif", step(200, "absent", 5).remainingS, 0);
check("horloge qui recule => pas d'exception, wait", otaRebootWaitStep({ startedMs: 5000, nowMs: 1000, attempts: 0, outcome: null }, cfg).action, "wait");

// otaRecoveryTarget
check("host === ip => null", otaRecoveryTarget({ currentHost: "192.168.1.50", ip: "192.168.1.50" }), null);
check("ip vide => null", otaRecoveryTarget({ currentHost: "pool.local", ip: "" }), null);
check("ip absente => null", otaRecoveryTarget({ currentHost: "pool.local" }), null);
check("o absent => null", otaRecoveryTarget(undefined), null);
check("ip 'abc' => null", otaRecoveryTarget({ currentHost: "pool.local", ip: "abc" }), null);
check("ip '1.2.3' => null", otaRecoveryTarget({ currentHost: "pool.local", ip: "1.2.3" }), null);
check("ip '999.1.1.1' => null", otaRecoveryTarget({ currentHost: "pool.local", ip: "999.1.1.1" }), null);
check("ip '1.2.3.4.5' => null", otaRecoveryTarget({ currentHost: "pool.local", ip: "1.2.3.4.5" }), null);
const rt = (ip) => otaRecoveryTarget({ currentHost: "pool.local", ip });
const tgt = (ip) => ({ url: `http://${ip}/`, label: `http://${ip}/` });
check("ip '255.255.255.255' => accepté (borne haute valide)", rt("255.255.255.255"), tgt("255.255.255.255"));
check("ip '256.1.1.1' => null (off-by-one, 1er octet)", rt("256.1.1.1"), null);
check("ip '1.256.1.1' => null (2e octet)", rt("1.256.1.1"), null);
check("ip '1.1.256.1' => null (3e octet)", rt("1.1.256.1"), null);
check("ip '1.1.1.256' => null (dernier octet)", rt("1.1.1.256"), null);
check("ip '01.1.1.1' => null (zéro de tête refusé)", rt("01.1.1.1"), null);
// 0.0.0.0 refusé : adresse inutilisable comme cible ; le repli « cherchez-le
// dans votre box » vaut mieux qu'un lien mort.
check("ip '0.0.0.0' => null (inutilisable comme cible)", rt("0.0.0.0"), null);
check("nominal", otaRecoveryTarget({ currentHost: "pool.local", ip: "192.168.1.50" }),
  { url: "http://192.168.1.50/", label: "http://192.168.1.50/" });

// otaFetchNoStore : fetch nu, ?t= anti-cache, no-store
(async () => {
  await otaFetchNoStore("/index.html", 6000);
  await otaFetchNoStore("/auth/status", 3000);
  check("fetch : URL = chemin + ?t=<horodatage>", /^\/index\.html\?t=\d+$/.test(calls[0].url), true);
  check("fetch : cache no-store", calls[0].opts.cache, "no-store");
  check("fetch : 2e sonde cible /auth/status?t=", /^\/auth\/status\?t=\d+$/.test(calls[1].url), true);
  check("fetch : pas d'en-tête d'authentification", calls[0].opts.headers, undefined);
  if (failures > 0) { console.error(`\n${failures} vecteur(s) en échec.`); process.exit(1); }
  console.log("\nTous les vecteurs ota_wait sont conformes.");
})();
