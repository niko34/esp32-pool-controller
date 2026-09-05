## Métadonnées
- **ID** : feature-007
- **Slug** : fraicheur-capteurs
- **Type** : feature
- **Statut** : **clôturée — supersédée** (2026-07-04, sans implémentation dédiée)
- **Priorité** : haute (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-04
- **Doc(s) liée(s)** : [docs/subsystems/sensors.md](../../docs/subsystems/sensors.md), [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md)

## Note de clôture (2026-07-04)

Spec rédigée en avril 2026, **supersédée par des features postérieures** avant d'avoir été lancée. Audit complet contre le code v2.6.0 :

**Cœur sécurité (raison d'être de la priorité « haute ») : entièrement couvert, en plus strict**
- Timestamps par sonde : `_lastPhMs`/`_lastOrpMs` dans `sensors.h` ; `getPh()`/`getOrp()` retournent **NaN si dernière lecture valide > 20 s** (`kSensorStaleTimeoutMs` — la spec proposait 60 s) + log **critical** à la transition (`_checkStaleAndLog`). → feature-021.
- « Le PID ne consomme jamais une valeur stale » : le PID auto consomme `getPhFiltered()` gated par `isPhFilterReady()` (fail-closed : warmup, EZO injoignable, instable), garde `FilterNotReady` dans `evaluateDose()` (logique pure ADR-0017), **testée en natif** (`test_ready_false_when_stale`). → features 025 + 036.
- Alerte : topic MQTT retain `{base}/alerts/sensor_stale` edge-triggered (feature-021) ; UI chips « EZO indisponible » sur pages pH/ORP + calibration (features 025/034).
- L'affirmation « Comportement actuel : la valeur précédente reste en cache, pas de péremption détectée » n'est **plus vraie** depuis feature-021 (`_lastPh = NAN` sur échec EZO).

**Reliquats non couverts (observabilité, pas sécurité) — reportés dans `feature-022-observabilite-dosage.md` (section « Reliquats hérités de feature-007 », R1-R4)** :
- R1 : auto-discovery HA `binary_sensor` `device_class: problem` pour le stale (le topic existe sans discovery) — intérêt élevé, coût trivial ;
- R2 : badge stale explicite sur cartes dashboard ; R3 : champs WS booléens dédiés ; R4 : fraîcheur température (capteur figé).

Aucun code modifié au titre de cette spec. Livrables et critères d'acceptation annotés ci-dessous à titre de traçabilité.

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (une mesure périmée alimente le PID : refus de régulation nécessaire)
- [ ] ux-ui-designer — non (badge réutilise le style existant des bandeaux d'alerte)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Exposer explicitement la fraîcheur des mesures pH et ORP via un timestamp `lastReadMs` par sonde, un champ WebSocket `ph_sensor_fresh` / `orp_sensor_fresh` (bool), et un badge UI « capteur périmé » sur les pages concernées. En cas de péremption (seuil défini), la régulation automatique refuse de démarrer.

## Contexte
Aujourd'hui, si le module EZO pH ou ORP ne répond pas, `sensors.getPh()` peut renvoyer NAN. Le PID tolère partiellement NAN (la plupart des branches retournent simplement `false` sans démarrer), mais **aucune alerte explicite** n'est levée : l'utilisateur voit `--` sur l'UI et peut croire à un simple glitch. Pire, une valeur figée (dernière lecture valide conservée) serait utilisée par le PID sans être détectée.

Voir [docs/subsystems/sensors.md](../../docs/subsystems/sensors.md) (lecture périodique) et [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md) (gardes du PID).

## Comportement attendu

### Côté firmware
- `Sensors` mémorise pour chaque sonde (pH, ORP, temp) un `uint32_t lastReadMs` mis à jour à chaque lecture **valide** (pas NAN, pas hors-bornes physiques).
- Expose `bool isPhFresh()`, `bool isOrpFresh()`, `bool isTempFresh()` qui retournent `(millis() - lastReadMs) < kSensorFreshnessMs`.
- `kSensorFreshnessMs` = 60 000 ms par défaut (1 minute ; à confirmer par `pool-chemistry` selon la fréquence de lecture capteur — à date ~5 s).
- `PumpController.update()` : si `!sensors.isPhFresh()` en mode `automatic` → aucune démarrage de dosage pH. Idem pour ORP. Pas d'arrêt forcé d'une injection en cours (évite les rebonds ; l'injection se terminera naturellement après `minInjectionTimeMs`).
- Champs WS publiés dans `sensor_data` : `ph_sensor_fresh`, `orp_sensor_fresh`, `temperature_sensor_fresh` (bool).
- Entité HA correspondante : `binary_sensor.{name}_ph_sensor_stale` (inverse), avec `device_class: problem`.

### Côté UI
- Badge rouge « Capteur pH périmé » sur la carte dashboard pH et sur `/ph` (bloc Statistiques) si `ph_sensor_fresh = false`.
- Symétrique ORP.
- Comportement du PID (pas de dosage) mentionné dans le badge hover : « Régulation suspendue tant que la mesure n'est pas fraîche ».

### Cas limites
- WebSocket déconnecté : le badge devient « inconnu » (gris), pas « périmé » (on ne sait pas).
- Sonde en cours de calibration : considérée **fraîche** si la dernière lecture < seuil (la calibration n'invalide pas les lectures courantes).
- Seuil dépassé pendant une injection : injection poursuivie jusqu'à son terme naturel, suivante refusée.
- Boot : tant qu'aucune lecture valide n'a eu lieu, `fresh = false` → bien sûr aucune régulation automatique.

## Comportement actuel
- `Sensors` lit pH/ORP toutes les ~5 s (voir `sensors.cpp`).
- En cas d'échec EZO, la valeur précédente reste en cache → pas de NAN mais **pas de péremption détectée**.
- Aucun champ WS dédié à la fraîcheur.
- L'UI affiche `--` uniquement quand la valeur est NAN, pas quand elle est stale.

## Contraintes techniques
- ESP32, offline first
- Le mutex `i2cMutex` protège les lectures — ne pas allonger la section critique
- `kSensorFreshnessMs` ≥ 3 × période de lecture (marge pour tolérer 1-2 échecs ponctuels sans fausse alerte)
- Pas d'appel `millis()` dans la section critique du WS builder (déjà un pattern utilisé)
- **Le PID ne doit jamais utiliser une valeur stale** — règle stricte validée par `pool-chemistry`

## Fichiers potentiellement concernés
- `src/sensors.h` / `src/sensors.cpp` — ajouter `lastReadMs`, `isXxxFresh()`, mettre à jour à chaque lecture OK
- `src/constants.h` — `constexpr unsigned long kSensorFreshnessMs = 60000;`
- `src/pump_controller.cpp` — ajouter les gardes `isPhFresh()` / `isOrpFresh()` dans `update()`
- `src/ws_manager.cpp` — publier les 3 champs `*_sensor_fresh`
- `src/mqtt_manager.cpp` — publier binary_sensor HA stale (+ auto-discovery)
- `data/index.html` — badges rouge « Capteur pH/ORP périmé » sur dashboard + /ph + /orp
- `data/app.js` — consommation WS, affichage badge
- `docs/API.md` — documenter les champs WS
- `docs/MQTT.md` — documenter les nouveaux binary_sensor
- `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/features/page-dashboard.md` — mentionner le badge stale
- `docs/subsystems/sensors.md` — règle de fraîcheur
- `docs/subsystems/pump-controller.md` — ajouter au `canDose()` / règles

## Livrables — check-list (annotée à la clôture, 2026-07-04)
- [x] A1 — `Sensors::lastReadMs[3]` → **SUPERSÉDÉ** (feature-021) : `_lastPhMs`/`_lastOrpMs` pour pH/ORP ; temp non timestampée (→ R4 feature-022)
- [x] A2 — `isPhFresh()` etc. → **SUPERSÉDÉ** : la fraîcheur est intégrée à `getPh()`/`getOrp()` (NaN si stale) et à `isPhFilterReady()`/`isOrpFilterReady()` (feature-025)
- [x] A3 — `kSensorFreshnessMs = 60000` → **SUPERSÉDÉ en plus strict** : `kSensorStaleTimeoutMs = 20000` + `kSensorFilterMaxAgeMs = 20000`
- [x] A4 — Garde anti-stale du dosage auto → **SUPERSÉDÉ** : garde `FilterNotReady` dans `evaluateDose()` (feature-036, testée en natif) ; ne coupe pas une injection en cours ✓
- [ ] A5 — Champs WS `*_sensor_fresh` → **NON FAIT** ; équivalent implicite (valeurs `null` + `phDoseBlockedReason`) ; reporté R3 feature-022
- [ ] A6 — Auto-discovery HA `binary_sensor` problem → **NON FAIT** (topic `alerts/sensor_stale` existe sans discovery, feature-021) ; reporté R1 feature-022
- [ ] B1/B2 — Badge dashboard → **PARTIEL** : pages détail couvertes (chips « EZO indisponible », features 025/034), dashboard `--` sans libellé ; reporté R2 feature-022
- [x] B3 — Raison visible → **SUPERSÉDÉ** : `phDoseBlockedReason` affiché (chips + boutons désactivés avec raison)
- [x] C1-C5, C7 — Docs → couvertes par les docs des features 021/025/036 (sensors.md, pump-controller.md, ws-manager.md, MQTT.md, CHANGELOG)
- [x] C6 — ADR seuil/contrat → **SUPERSÉDÉ** : ADR-0016 (filtre capteurs) + ADR-0017 (logique pure) formalisent le contrat
- [x] D1 — n/a (aucun code modifié au titre de cette spec)

## Nouvelles signatures / API
- `bool Sensors::isPhFresh() const`, `bool Sensors::isOrpFresh() const`, `bool Sensors::isTempFresh() const`
- Champs WS : `ph_sensor_fresh` (bool), `orp_sensor_fresh` (bool), `temperature_sensor_fresh` (bool)
- Topic MQTT : `{base}/ph_sensor_stale` (bool retain), `{base}/orp_sensor_stale`, + auto-discovery `binary_sensor.{name}_ph_sensor_stale` (device_class `problem`)
- Constante : `constexpr unsigned long kSensorFreshnessMs = 60000;`

## Sécurité chimique
- Dosage impliqué : **oui** — refus d'injection si mesure périmée
- Limites journalières / horaires concernées : aucune directement ; on **bloque en amont**
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : **oui** — garde supplémentaire avant `computePID()`
- Risque identifié : sans cette spec, le PID peut régler sur une valeur figée → dérive chimique lente non détectée.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante : refus de régulation imposé en amont du PID), 3 (alternatives : timestamp local vs fraîcheur côté driver EZO ; seuil 60 s vs N × période de lecture)
- **Sujet pressenti** : `docs/adr/NNNN-fraicheur-capteurs.md` — formaliser le contrat « PID ne consomme jamais une valeur stale » et justifier le seuil retenu.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun nouveau

### Champs WebSocket
- `ph_sensor_fresh` (bool), `orp_sensor_fresh` (bool), `temperature_sensor_fresh` (bool)

### Topics MQTT
- `{base}/ph_sensor_stale` (retain, inverse) + auto-discovery `binary_sensor` `device_class: problem`
- Symétrique ORP et temp

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/features/page-dashboard.md` — badges
- `docs/subsystems/sensors.md` — règle fraîcheur + helpers
- `docs/subsystems/pump-controller.md` — garde supplémentaire
- `docs/subsystems/ws-manager.md` — 3 nouveaux champs
- `docs/subsystems/mqtt-manager.md` — 3 nouvelles entités HA

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — champs WS nouveaux
- `docs/MQTT.md` — entités HA
- `CHANGELOG.md` — entrée

#### Docs immuables
- `docs/adr/NNNN-fraicheur-capteurs.md` — ADR contrat fraîcheur + seuil

## Critères d'acceptation (annotés à la clôture, 2026-07-04)
- [x] Nominal : lecture OK en continu → **couvert** (équivalent : `getPh()` non-NaN, filtre ready)
- [x] Sonde débranchée : péremption détectée → **couvert en plus strict** : NaN + log critical au bout de 20 s (vs 60 s spec), chip UI « EZO indisponible », alerte MQTT `alerts/sensor_stale` ; entité HA auto-découverte **manquante** (→ R1 feature-022)
- [x] Aucune injection auto sur mesure périmée → **couvert** : garde `FilterNotReady` fail-closed dans `evaluateDose()`, testée en natif
- [x] Retour de la sonde : reprise automatique → **couvert** (re-sync filtre feature-033 : reprise bornée et sécurisée après ≈60 s de rejets)
- [x] Pas d'interruption forcée d'une injection en cours → **couvert** (la garde s'applique au démarrage, pas en cours)
- [x] Constante de seuil dans `constants.h` → **couvert** : `kSensorStaleTimeoutMs` / `kSensorFilterMaxAgeMs`
- [x] Libellés en français → **couvert** (chips/toasts existants)
- [x] `pio run` / `./build_fs.sh` → n/a (aucun code modifié au titre de cette spec)
- [ ] Badges dashboard dédiés → **non couvert**, reporté R2/R3 feature-022

## Plan de test manuel
1. Nominal : attendre 5 min, vérifier que les 3 champs `*_sensor_fresh` restent à `true`.
2. Débrancher physiquement l'EZO pH : au bout d'environ 60 s, badge rouge UI, entité HA `problem` active. ORP et temp toujours frais.
3. Mode `automatic`, pH à 7.5 (cible 7.2) : injection démarre. Débrancher sonde pendant injection → injection se termine naturellement (≥ 30 s). Suivante refusée (fresh = false).
4. Rebrancher : au prochain cycle de lecture (5 s), fresh repasse à true. Injection reprend normalement au cycle PID suivant.
5. Home Assistant : l'entité `binary_sensor.*_ph_sensor_stale` doit se propager en MQTT retain, même si le device HA redémarre.

## Notes pour agent
- **Fréquence de lecture capteur** : vérifier dans `sensors.cpp` la période exacte avant de fixer `kSensorFreshnessMs`. Si la période passe de 5 à 10 s un jour, la constante doit rester cohérente (3× marge).
- **Ne pas filtrer la fraîcheur en mode `scheduled`** : en programmé, l'injection est aveugle au capteur, donc la fraîcheur n'a aucune influence. Les autres gardes (daily, hourly, cycles) restent actives.
- **Injection manuelle** : hors périmètre ici — peut continuer même avec un capteur stale (l'opérateur décide). L'UI peut quand même afficher le badge.
- **HA** : `device_class: problem` → s'intègre aux dashboards HA d'alerte natif.

## Hors périmètre
- Historique des pannes capteur (log circulaire) — spec séparée si besoin
- Recalibration auto si drift détecté — non, pas fiable sans tampon
- Fraîcheur des sondes auxiliaires (DS18B20 seule) — incluse par symétrie mais sans impact régulation
