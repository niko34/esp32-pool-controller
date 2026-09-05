# Bug — requête Slope de boot perdue en silence + champs pente absents de /data

- **Type** : bug (diagnostic sonde pH — feature-024)
- **Priorité** : basse (cosmétique/diagnostic — la mesure et la régulation ne sont PAS affectées)
- **Statut** : done (v2.21.2, 2026-08-23) — tests natifs 640/640, revue Approved, visa pool-chemistry GO ; vérification on-target post-OTA restante (3 premiers critères)
- **Créée** : 2026-08-23, diagnostic on-target en v2.21.1

- **Domaine** : sensors (file EZO), web_routes_data, UI puce sonde pH

## Symptôme rapporté

Après le reboot OTA v2.21.1, la carte pH affiche « EZO indisponible » (premières
secondes, cache calibration = -1) puis « Pente non disponible » — pendant ~20 min,
y compris après refresh navigateur. La carte ORP n'affiche rien d'anormal.

## Diagnostic on-target (2026-08-23)

1. **La sonde et le module vont bien** : EZO pH détecté (fw 2.17), 2 points de
   calibration, lectures à 5 s nominales. Pente mesurée une fois la requête
   passée : **acide 101,0 % / base 100,0 % / zéro −1,03 mV** (sonde excellente).
2. **La requête `Slope,?` enfilée au boot (`sensors.cpp:158-160`) ne produit RIEN** :
   ni ligne de succès (`EZO pH slope : …`) ni warning d'échec au journal, et les
   caches `_phSlope*` restent NaN pendant ~20 min (constaté jusqu'à uptime ~1150 s).
   Le premier POST `/debug/ph_slope_refresh` manuel n'a pas non plus produit de log
   immédiat ; les suivants ont réussi normalement (3 lignes de succès).
   Hypothèses à départager en debug : (a) requête de boot consommée alors que
   l'EZO est occupé par les lectures d'init → échec streak=1 SILENCIEUX
   (`kEzoBusFailMaxConsecutive = 2` : un échec isolé ne se journalise jamais) ;
   (b) file recréée/purgée après l'enqueue de boot (`vQueueDelete` sensors.cpp:80-82) ;
   (c) timing I²C au boot.
3. **Incohérence de transport** : les champs `phSlopeAcid/Base/Zero/AgeMs` sont
   dans la trame WS (`ws_manager.cpp:400-406`) mais **absents de GET /data**
   (`web_routes_data.cpp` — grep vide). Un client sans WS (poll seul) ne peut
   jamais afficher la santé de sonde. C'est aussi ce qui a compliqué CE diagnostic
   (mes lectures /data montraient null alors que la trame WS était complète).
4. L'asymétrie pH/ORP est normale : l'EZO ORP n'a pas de diagnostic de pente,
   la puce ORP n'a donc pas cet état.

## Correctif attendu

1. La requête Slope de boot doit aboutir ou se journaliser : soit retenter
   automatiquement quelques secondes plus tard tant que `_phSlopeQueriedMs == 0`
   (sans attendre le cycle 24 h), soit journaliser dès le PREMIER échec quand
   aucune pente n'a jamais été lue.
2. Ajouter `phSlopeAcid/Base/Zero/AgeMs` à GET /data (mêmes règles de null que
   la trame WS) — parité de transport.
3. (Option UI) Distinguer « Pente pas encore lue depuis le démarrage » de
   « Pente non disponible » (le premier est un état transitoire attendu).

## Critères d'acceptation

- [ ] Après un reboot, la puce sonde pH atteint son état nominal (« Calibré · sonde X % ») en < 2 min sans action manuelle — *code livré (retry auto), **à vérifier on-target post-OTA v2.21.2***
- [ ] Un échec de `Slope,?` sans pente jamais lue se voit au journal dès la première occurrence — *code livré (warning one-shot), **à vérifier on-target post-OTA v2.21.2***
- [ ] `curl /data | jq .phSlopeAcid` renvoie la même valeur que la trame WS — *code livré (parité /data), **à vérifier on-target post-OTA v2.21.2***
- [x] `pio run` + `pio test -e native` sans régression — **640/640 tests natifs verts** (dont 15 nouveaux `test_native_slope_retry`)

## Notes

- Contournement immédiat : `POST /debug/ph_slope_refresh` (existant, feature-024).
- La confusion initiale « EZO indisponible » (calPoints -1 les toutes premières
  secondes) est un second libellé transitoire du même reboot — voir
  `classifyPhProbe` (data/app.js:3564).

---

## Notes d'implémentation (v2.21.2, 2026-08-23)

### Cause racine (debugger)

Un **unique échec MUET** de la `Slope,?` de boot : statut EZO 255/NoData ou
parasite pendant l'init du module — les **seuls chemins de `atlas_ezo.cpp`
sans aucun log** — et échec isolé (streak = 1 < `kEzoBusFailMaxConsecutive = 2`)
donc jamais journalisé. La requête n'était ensuite **jamais retentée** : le
re-enqueue du cycle 24 h exige `_phSlopeQueriedMs != 0`, or il vaut 0 tant
qu'aucune pente n'a réussi. Les hypothèses (b) **file purgée** (`vQueueDelete`
avant l'enqueue de boot, ordre vérifié) et (c) **timing I²C** ont été
**réfutées** au debug.

### Correctifs livrés

1. **Retry borné au boot** : fonction pure `shouldRetryBootSlope()`
   (`src/slope_retry_logic.h`, pattern ADR-0017, sans Arduino.h/FreeRTOS),
   consommée en fin de `SensorManager::update()`. Constantes :
   `kPhSlopeBootRetryIntervalMs = 30 s`, `kPhSlopeBootRetryMaxAttempts = 5`.
   Garde `_phEzoDetected` (readInfo pH OK au boot). Compteur
   `_phSlopeBootAttempts` incrémenté AVANT l'enqueue, **même si la file est
   pleine**, **jamais réarmé**. 15 tests natifs (640/640 au total).
2. **Logs** : warning **dès le PREMIER échec** quand aucune pente jamais lue
   (one-shot, `_phSlopeBootFirstFailLogged`) ; info par retentative (`n/5`) ;
   warning unique à l'épuisement (`_phSlopeBootExhaustedLogged`) ; warning
   `atlas_ezo.cpp` sur statut hors protocole `default:` avec la valeur
   (255/NoData reste muet — chemin nominal).
3. **Parité GET /data** : `phSlopeAcid/Base/Zero/AgeMs` avec les mêmes règles
   null/arrondi que la trame WS (pentes 1 décimale, zéro 2 décimales, ageMs
   null si UINT32_MAX), lecture **cache uniquement** (zéro I²C en contexte
   async_tcp), buffer StaticJson 1792 → 1920 o.
4. **UI `classifyPhProbe`, 3 états** : « Sonde pH : lecture en cours… »
   (calPoints inconnu, transitoire boot — remplace le trompeur « EZO
   indisponible »), « Pente en cours de lecture… » (ageMs null, se résout
   < 2 min grâce au retry), « Pente non disponible » (slope* null + ageMs
   non-null = pente invalidée, classe warn — erreur réelle). États sonde
   usée/à remplacer/calibration inchangés.

### Décisions

- **C0 (UI, choix architect)** : aucun champ de /data ou du WS ne distingue le
  transitoire de boot (`calPoints -1` les premières secondes) d'un vrai mutisme
  EZO → **un seul libellé** neutre « Sonde pH : lecture en cours… » plutôt
  qu'un nouveau champ firmware. Un vrai mutisme finit en log `EZO pH` au
  journal + valeurs pH figées à « -- ».
- **D2 (garde anti-underflow)** : si `nowMs < lastAttemptMs`, refus de la
  retentative (attente du cycle suivant) — choix **conservateur** documenté :
  un wrap de `millis()` légitime (~49 j, hors fenêtre de boot) est aussi refusé.

### Gate pool-chemistry : GO (3 conditions vérifiées)

- **C1** : borne DURE de 5 transactions (chaque `Slope,?` bloque `loopTask`
  ~900 ms), compteur incrémenté à chaque tentative même si l'enqueue échoue,
  jamais remis à zéro.
- **C2** : jamais de retry sans module EZO pH détecté (bus muet → 0 tentative).
- Aucun chemin de dosage touché ; `/data` en lecture cache seule.

### Fichiers modifiés

`src/slope_retry_logic.h` (nouveau), `src/sensors.{h,cpp}`, `src/atlas_ezo.cpp`,
`src/constants.h`, `src/web_routes_data.cpp`, `data/app.js`,
`test/test_native_slope_retry/` (nouveau, 15 tests), `src/version.h` (2.21.2).

### Documentation

`docs/subsystems/sensors.md` (mécanisme de retry, logs, cause racine),
`docs/API.md` (4 champs /data), `docs/features/page-ph.md` (3 états de la puce),
`CHANGELOG.md` [2.21.2]. **Aucun ADR** : fix + patterns existants (ADR-0017),
aucun des 4 critères de déclenchement rempli.
