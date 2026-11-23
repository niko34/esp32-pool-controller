// Vecteurs du troisième état de capture : « Connexion… » (feature-213).
//
// ⭐ CE QUE CE HARNAIS PROUVE, ET POURQUOI IL EXISTE.
// `tools/ui_shot.sh -m` doit FIGER l'application dans l'état intermédiaire —
// celui que voit tout le monde à chaque ouverture de page, et celui où un faux
// a vécu sans que personne ne le voie (bug-196 pour l'en-tête, bug-212 pour les
// pastilles). ⛔ « Figer » n'est pas « tomber souvent sur le bon écran » : la
// fiche interdit explicitement la temporisation au jugé. Il faut donc montrer
// qu'AUCUN chemin du code livré ne peut réécrire l'état une fois la pose faite.
//
// ⚠️ Deux pièges déjà payés dans ce dépôt, évités ici par construction :
//   1. ⛔ Ne pas RECOPIER le script injecté. Il est demandé à `ui_shot_server.py`
//      lui-même (`tools/check_mid_stub_vectors.sh` l'écrit dans un fichier et
//      passe son chemin en argument) : on exécute donc ce qui est SERVI.
//      Esprit d'ADR-0099.
//   2. ⛔ Ne rien vérifier LEXICALEMENT. Une sentinelle qui lit du texte
//      (« la regex contient-elle /data ? ») ne tient pas : l'URL réelle est
//      `/data?t=<horodatage>`, construite par l'application, et une variante
//      d'écriture la contournerait en silence. C'est exactement le défaut
//      corrigé dans bug-212. ⇒ Le vecteur central EXÉCUTE le `loadSensorData`
//      LIVRÉ, extrait par ancre, et regarde si `setNetStatus` est appelé.
//
// Les quatre sorties de « mid » dans le code livré :
//   - `_ws.onopen`                        -> setNetStatus('ok')
//   - `_onDisconnect` (onerror / onclose) -> setNetStatus('bad')
//   - le battement de cœur WS             -> setNetStatus('bad')  [armé par onopen seul]
//   - l'issue de `/data`, succès OU échec -> setNetStatus('ok' | 'mid' | 'bad')
// Les trois premières sont fermées par un socket qui n'émet jamais rien ; la
// quatrième par une promesse qui ne se résout jamais.
//
// Exécution : ./tools/check_mid_stub_vectors.sh
//             (⛔ pas `node test/mid_stub_vectors.js` seul : il lui faut le
//              script injecté, que seul le serveur sait produire)

"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const stubPath = process.argv[2];
assert.ok(stubPath, "usage : node mid_stub_vectors.js <fichier-du-script-injecté>\n" +
  "⚠️ Ce harnais se lance par ./tools/check_mid_stub_vectors.sh, qui demande le\n" +
  "   script à tools/ui_shot_server.py au lieu d'en recopier le texte.");

const brut = fs.readFileSync(stubPath, "utf8");
const m = brut.match(/^<script>([\s\S]*)<\/script>$/);
assert.ok(m, "le serveur n'a pas servi un <script>…</script> : " + brut.slice(0, 80));
const STUB = m[1];
assert.ok(STUB.length > 50, "script injecté suspicieusement court");

// ⛔ GARDE ANTI-SILENCE. Mesuree le 2026-11-23 : un mutant qui detournait AUSSI
// `/get-config` a SURVECU, non parce qu'un vecteur le ratait, mais parce qu'un
// `await` sur une promesse qui ne se resout jamais laisse node sortir en 0 --
// la file d'evenements se vide, le harnais s'arrete AU MILIEU, et l'absence de
// sortie passe pour un succes. C'est la meme famille de defaut que la sentinelle
// de bug-212 : un controle qui ne PEUT PAS echouer.
// ⇒ Tant que la derniere ligne n'a pas tourne, le code de sortie est 1.
let fini = false;
process.on("exit", (code) => {
  if (!fini && code === 0) {
    console.error("⛔ mid_stub_vectors s'est ARRETE AVANT LA FIN sans lever d'erreur.\n" +
      "   Cause typique : un `await` sur une promesse qui ne se resout jamais.\n" +
      "   ⚠️ Un harnais interrompu n'est pas un harnais vert.");
    process.exitCode = 1;
  }
});

let n = 0;
const ok = (c, msg) => { assert.ok(c, msg); n++; };
const eq = (a, b, msg) => { assert.deepStrictEqual(a, b, msg); n++; };

// Vide la file des micro-tâches ET quelques tours de boucle : si une promesse
// devait se résoudre, elle l'aurait fait.
const vider = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };

// Pose le stub dans un faux `window`, avec un `fetch` d'origine ESPION.
function poser() {
  const passages = [];
  const win = {
    fetch(entree, options) { passages.push({ entree, options }); return Promise.resolve({ origine: true }); },
  };
  win.window = win;
  // `window` est passé en PARAMÈTRE : il masque tout global du même nom, donc
  // le stub ne peut pas toucher l'environnement du harnais par accident.
  new Function("window", STUB)(win);
  return { win, passages };
}

// ---------------------------------------------------------------------------
// 1. Le faux WebSocket reste EN COURS D'OUVERTURE et n'émet jamais rien.
// ---------------------------------------------------------------------------
(async () => {
  const { win } = poser();
  ok(typeof win.WebSocket === "function", "window.WebSocket est remplacé");
  eq(win.WebSocket.CONNECTING, 0, "CONNECTING exposé (lu par la garde de initWebSocket)");
  eq(win.WebSocket.OPEN, 1, "OPEN exposé (lu par la garde de initWebSocket)");
  eq(win.WebSocket.CLOSING, 2, "CLOSING exposé");
  eq(win.WebSocket.CLOSED, 3, "CLOSED exposé");

  const sock = new win.WebSocket("ws://127.0.0.1/ws");
  eq(sock.readyState, win.WebSocket.CONNECTING,
    "le socket reste EN COURS D'OUVERTURE — ⛔ pas OPEN, sinon l'en-tête dirait « En ligne »");

  // L'application pose ses gestionnaires APRÈS construction : on fait pareil.
  const emis = [];
  sock.onopen = () => emis.push("open");
  sock.onerror = () => emis.push("error");
  sock.onclose = () => emis.push("close");
  sock.onmessage = () => emis.push("message");
  sock.addEventListener("open", () => emis.push("open:listener"));
  sock.addEventListener("close", () => emis.push("close:listener"));
  sock.addEventListener("error", () => emis.push("error:listener"));

  await vider();
  eq(emis, [], "⛔ AUCUN événement WebSocket — onopen ferait 'ok', onerror/onclose feraient 'bad'");
  eq(sock.readyState, 0, "readyState inchangé après le tour de boucle");
  ok(typeof sock.send === "function", "send() existe (l'application envoie son jeton d'authentification)");

  // ---------------------------------------------------------------------------
  // 2. `/data` ne se termine JAMAIS — ⛔ même abandonnée.
  // ---------------------------------------------------------------------------
  {
    const { win: w2, passages } = poser();
    const issues = [];
    const p = w2.fetch("/data?t=1764000000000", { cache: "no-store" });
    p.then(() => issues.push("resolue"), () => issues.push("rejetee"));
    await vider();
    eq(issues, [], "/data : promesse NI résolue NI rejetée — un échec basculerait l'écran");
    eq(passages, [], "/data n'atteint JAMAIS le fetch d'origine");

    // L'application arme un abandon à 10 s (AbortController). Il annule une vraie
    // requête réseau ; il ne doit pas réveiller celle-ci — c'est ce qui rend
    // l'état insensible à la durée de la capture.
    const ctrl = new AbortController();
    const issues2 = [];
    w2.fetch("/data?t=2", { signal: ctrl.signal }).then(
      () => issues2.push("resolue"), () => issues2.push("rejetee"));
    ctrl.abort();
    await vider();
    eq(issues2, [], "⛔ l'abandon à 10 s ne résout pas la promesse : l'écran ne bascule pas avec le temps");
  }

  // ---------------------------------------------------------------------------
  // 3. Tout le reste passe NORMALEMENT — `/get-config` en tête : c'est lui qui
  //    prouve que l'appareil est joignable, et c'est tout l'intérêt de cet état.
  // ---------------------------------------------------------------------------
  {
    const { win: w3, passages } = poser();
    const urls = ["/get-config", "/get-system-info", "/get-logs", "/get-history?range=day",
      "/index.html", "/app.css", "/auth/login"];
    // ⛔ On n'ATTEND pas chaque promesse l'une après l'autre : si l'une d'elles ne
    // se résolvait jamais (c'est précisément le mutant « /get-config détourné lui
    // aussi »), l'attente ne rendrait jamais la main et le harnais s'arrêterait là,
    // en silence. On les lance toutes, on vide la file, puis on COMPTE.
    const abouties = [];
    for (const u of urls) w3.fetch(u, { marque: u }).then((r) => abouties.push([u, r && r.origine]));
    await vider();
    eq(abouties.map((x) => x[0]), urls,
      "⛔ TOUTES ces URL doivent aboutir — `/get-config` en tête : c'est lui qui prouve " +
      "que l'appareil est joignable, et sans lui l'écran ne serait plus « Connexion… »");
    ok(abouties.every((x) => x[1] === true), "chacune atteint bien le fetch d'origine");
    eq(passages.map((p) => p.entree), urls, "les URL traversent inchangées, dans l'ordre");
    eq(passages[0].options.marque, "/get-config", "les options traversent inchangées");
  }

  // ---------------------------------------------------------------------------
  // 4. ⭐ VECTEUR CENTRAL — l'URL interceptée est celle que l'application CONSTRUIT.
  //    On n'inspecte aucun texte : on exécute le `loadSensorData` LIVRÉ, avec le
  //    `fetch` du stub, et on regarde si `setNetStatus` est appelé.
  //    ⚠️ Toutes les variables des DEUX branches (succès et échec) sont fournies :
  //    sans cela, un mutant tomberait sur une ReferenceError AVANT setNetStatus et
  //    ce vecteur passerait au vert sans rien prouver.
  // ---------------------------------------------------------------------------
  {
    const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
    const grab = (anchor) => extractBalancedFromAnchor(src, anchor, "{", "}").text;
    const authFetch = grab("function authFetch(url, options = {}) {");
    const loadSensorData = grab("async function loadSensorData(options = {}) {");
    ok(/fetch\(/.test(authFetch), "authFetch extrait (il porte l'appel réseau)");
    ok(/authFetch\(/.test(loadSensorData), "loadSensorData extrait (il porte l'URL réelle de /data)");

    const { win: w4, passages } = poser();
    const poses = [];
    const executer = new Function("window", "fetch", "noteNetStatus", "noteFetch", `
      "use strict";
      const sessionStorage = { getItem: () => "mock", removeItem: () => {} };
      const location = { href: "" };
      const document = { querySelectorAll: () => [] };
      const SENSOR_REFRESH_MS = 30000, SENSOR_RETRY_MS = 2000;
      let latestSensorData = null, dosingAnchor = null, consecutiveFailures = 0;
      let lastSensorDataLoadTime = 0, sensorDataLoadInFlight = null, sensorDataRetryTimer = null;
      let filtrationRunningOverride = null;
      const debugStart = () => null;
      const setNetStatus = (state, text) => { noteNetStatus(state, text); };
      const updateFiltrationControls = () => {};
      const updateTodayOnCharts = () => {}, updateDashboardMetrics = () => {};
      const updateClockBadge = () => {}, updateDoseBlockBanner = () => {};
      const updateSensorBadges = () => {}, updateStatusCards = () => {};
      const updatePumpCharacterizationBadges = () => {}, updateProductUI = () => {};
      const _updateSondesChip = () => {}, updateDetailSections = () => {};
      const measuresAreValid = () => false, _fmtPh = (v) => String(v), fmtFr = (v) => String(v);
      const updatePhProbeChip = () => {}, updateFilterSub = () => {};
      const $ = () => null;
      ${authFetch}
      ${loadSensorData}
      return loadSensorData({ force: true, source: "vecteur" });
    `);
    executer(w4, w4.fetch, (state, text) => poses.push([state, text]),
      (u) => passages.push(u));

    await vider();
    eq(poses, [],
      "⛔ AUCUNE pose d'état : l'URL que l'application construit (/data?t=<horodatage>) " +
      "est bien celle que le stub retient, et son issue ne revient jamais.");
    eq(passages, [],
      "le fetch d'origine n'a rien vu passer — ⛔ si /data le traversait, le serveur " +
      "local répondrait et l'écran passerait à « En ligne »");
  }

  // ---------------------------------------------------------------------------
  // LIMITES CONNUES — écrites parce qu'elles ne se lisent nulle part ailleurs.
  //  - Ce harnais prouve que l'état ne PEUT PLUS être réécrit. Il ne prouve pas
  //    que l'état POSÉ est le bon : c'est `setNetStatus("mid", "Connexion…")`
  //    juste après un `loadConfig()` réussi (app.js), et c'est la CAPTURE qui le
  //    constate. ⇒ Les deux se complètent, aucun ne remplace l'autre.
  //  - ⛔ Il ne dit rien du déterminisme PIXEL : la pastille d'état porte une
  //    animation `pulse` infinie (app.css) dans les TROIS états. Deux captures
  //    diffèrent donc sur ses 8×8 px, et c'est vrai de `-n` aussi — mesuré.
  //  - Un cinquième chemin d'écriture de l'état ajouté un jour à app.js ne serait
  //    pas vu ici : seul `loadSensorData` est exécuté.
  // ---------------------------------------------------------------------------

  fini = true;
  console.log(`mid_stub_vectors : ${n} assertions OK`);
})().catch((e) => { console.error(e); process.exit(1); });
