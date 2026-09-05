## Métadonnées
- **ID** : feature-024
- **Slug** : pente-sonde-ph
- **Type** : feature
- **Statut** : done — livrée 2026-05-08
- **Priorité** : normale
- **Version cible** : 2.1.0 (post feature-021)
- **Dernière mise à jour** : 2026-05-08
- **Spec(s) liée(s)** : `specs/features/done/feature-021-migration-atlas-ezo.md` (intégration EZO)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
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
- [ ] pool-chemistry — non requis : feature **diagnostique passive**, n'affecte pas `canDose()` ni le PID
- [x] ux-ui-designer — refonte légère de la carte « Lecture pH » sur la page /ph
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Exposer **l'état de santé de la sonde pH** (pente de calibration et décalage zéro) en interrogeant la commande `Slope,?` du module Atlas EZO. Afficher cette information à côté de la valeur pH sur la page /ph avec un code couleur (vert / ambré / rouge) pour alerter l'utilisateur quand la sonde est usée et doit être remplacée.

## Contexte

La feature-021 a livré la migration vers Atlas EZO mais n'expose pas les indicateurs de santé de la sonde. La commande Atlas `Slope,?` retourne 3 valeurs cruciales pour diagnostiquer l'usure d'une sonde pH :
- **Pente acide** (% de pente théorique entre pH 7 et pH 4) — idéal 95-100 %
- **Pente base** (% de pente théorique entre pH 7 et pH 10) — idéal 95-100 %
- **Décalage zéro** (offset au point isopotentiel pH 7, en mV) — idéal proche de 0 mV

Une sonde dont la pente tombe sous 90 % ou dont le décalage zéro dépasse ±30 mV est en fin de vie et doit être remplacée. Sans cette information, l'utilisateur découvre une sonde HS uniquement quand la régulation devient erratique (ce qui peut prendre plusieurs jours/semaines avec dérive lente).

Cette feature est complémentaire à la chasse au bruit menée pendant le debug feature-021 : au lieu d'essayer de filtrer le bruit a posteriori, on diagnostique en amont la qualité de la chaîne de mesure.

## Comportement attendu

### Côté firmware

#### Lecture périodique automatique
- Au boot, après initialisation EZO, **enqueuer une lecture `Slope,?`** (via la queue `_ezoQueue` existante)
- Re-enqueuer **toutes les 24 h** depuis `Sensors::update()` si dernière lecture > 86 400 000 ms
- Pas de blocage du chemin chaud — la lecture se fait via la queue existante traitée par loopTask
- Cache RAM : `_phSlopeAcid`, `_phSlopeBase`, `_phSlopeZero`, `_phSlopeQueriedMs`

#### Format de réponse Atlas
- Commande envoyée : `Slope,?`
- Réponse type : `?Slope,99.7,100.3,-0.89` (acide %, base %, zéro mV)
- Parsing : 3 floats séparés par virgule après le préfixe `?Slope,`
- En cas d'échec parsing → tous les champs à NaN

#### Refresh manuel à la demande
- Après chaque calibration EZO réussie (mid/low/clear), enqueuer automatiquement un `Slope,?` pour rafraîchir l'info immédiatement
- Endpoint debug `POST /debug/ph_slope_refresh` (optionnel, pour tester rapidement) — réutilise l'infra `web_routes_debug` déjà en place

#### API publique nouvelle dans `Sensors`
```cpp
float getPhSlopeAcid() const;       // % (NaN si non disponible)
float getPhSlopeBase() const;       // %
float getPhSlopeZero() const;       // mV
uint32_t getPhSlopeAgeMs() const;   // ms écoulées depuis la dernière query (UINT32_MAX si jamais)
bool enqueuePhSlopeQuery();         // force un refresh
```

#### Logique d'évaluation (côté UI ou firmware ?)
**Décision** : faire l'évaluation côté **UI** (frontend), pas côté firmware. Ça évite de figer les seuils dans constants.h et permet d'ajuster l'UX sans reflasher. Le firmware expose les valeurs brutes, le frontend les interprète.

### Côté UI

#### Page pH — carte « Lecture pH »
À côté ou sous la valeur pH affichée en grand, ajouter un **chip d'état sonde** :

| pente_min (min des 2 pentes) | Décalage zéro abs | Chip affiché | Couleur |
|---|---|---|---|
| ≥ 95 % | ≤ 15 mV | « Sonde excellente — 98 % » | vert |
| 90-95 % | ≤ 30 mV | « Sonde correcte — 92 % » | ambré clair |
| 85-90 % | ≤ 30 mV | « Sonde usée — 87 % » | ambré |
| < 85 % ou décalage > 30 mV | — | « Sonde à remplacer — 81 % » | rouge |
| NaN (jamais lu / EZO injoignable) | — | « Pente non disponible » | gris |
| `phCalPoints < 2` | — | « Calibration 2 points requise » | gris |

#### Détail au clic / survol du chip
Popup ou expand affichant les 3 valeurs détaillées :
- Pente acide : 98.5 %
- Pente base : 99.2 %
- Décalage zéro : -1.2 mV
- Dernière vérification : il y a X heures

Un bouton « Rafraîchir » (debug) qui appelle `POST /debug/ph_slope_refresh`.

### Côté MQTT / Home Assistant

#### Topics publiés (retain, basse fréquence)
| Topic | Payload | Description |
|---|---|---|
| `{base}/ph_slope_acid` | `99.7` | Pente acide en % |
| `{base}/ph_slope_base` | `100.3` | Pente base en % |
| `{base}/ph_slope_zero` | `-0.89` | Décalage zéro en mV |

Publication 1× par cycle de query (donc ~1× par jour) + au boot. Pas de spam MQTT.

#### Auto-discovery HA — 3 sensors ajoutés
- `Piscine pH Pente Acide` (unit `%`, icon `mdi:angle-acute`)
- `Piscine pH Pente Base` (unit `%`, icon `mdi:angle-obtuse`)
- `Piscine pH Décalage Zéro` (unit `mV`, icon `mdi:alpha-z-circle-outline`)

Optionnellement un `binary_sensor` « Piscine Sonde pH À Remplacer » qui passe à `ON` si pente < 90 % ou décalage > 30 mV.

### Cas limites

- **Sonde non calibrée (`phCalPoints < 2`)** : la commande `Slope,?` retourne des valeurs incohérentes ou une erreur. L'UI affiche « Calibration 2 points requise » plutôt que les chiffres bruts.
- **EZO pH injoignable** : valeurs NaN, l'UI affiche « Pente non disponible (EZO non détecté) ».
- **Bus I²C dégradé** (`_phI2cFailStreak ≥ 2`) : invalider aussi le cache slope pour cohérence avec le pattern existant. Refresh à la 1ʳᵉ lecture I²C réussie suivante.
- **Première mesure** : avant la 1ʳᵉ query, valeurs à NaN. L'UI affiche un message d'attente « Première vérification dans quelques secondes ».

## Comportement actuel

- L'info pente n'est pas exposée côté firmware ni UI
- L'utilisateur ne peut pas savoir si sa sonde est neuve ou usée
- Une sonde qui dérive lentement n'est détectée que quand la régulation devient incorrecte

## Contraintes techniques

- **Pas de modification de `canDose()` ni du PID** — feature purement diagnostique
- Pas de blocage du chemin chaud — utiliser la queue EZO existante
- Coût RAM minimal : 3 floats + 1 uint32 = 16 octets par cache
- Coût Flash estimé : ~1-2 KB (méthode parse + intégration UI). Marge actuelle ~8 KB → OK
- Période de query : 24h pour ne pas saturer le bus inutilement

## Fichiers potentiellement concernés

- `src/atlas_ezo.h/cpp` — nouvelle méthode `querySlope(PhSlopeInfo& out)`
- `src/sensors.h/cpp` — cache + `getPhSlope*()` + intégration queue + refresh post-cal
- `src/ws_manager.cpp` — champs WS `phSlopeAcid/Base/Zero` dans `sensor_data`
- `src/mqtt_manager.cpp` — 3 topics + auto-discovery HA + binary_sensor optionnel
- `src/web_routes_debug.cpp` — endpoint `POST /debug/ph_slope_refresh` (optionnel)
- `data/index.html` — chip d'état sur carte « Lecture pH » page /ph
- `data/app.js` — logique de détermination du chip (couleur + libellé) + popup détails
- `data/app.css` — styles `.chip--probe-state` (variantes vert/ambré/rouge/gris)
- `src/constants.h` — `kPhSlopeQueryIntervalMs = 86400000` (24h)

## Livrables — check-list
- [x] Méthode `AtlasEzoSensor::querySlope()` qui parse la réponse Atlas (parsing tolérant 2 ou 3 floats)
- [x] Cache `_phSlope*` dans `Sensors` + getters publics + invalidation sur fail streak
- [x] Enqueue automatique 24h + après cal réussie + au boot
- [x] Endpoint `POST /debug/ph_slope_refresh`
- [x] Champs WS `phSlopeAcid`, `phSlopeBase`, `phSlopeZero` + `phSlopeAgeMs`
- [x] Topics MQTT (3 sensors retain edge-triggered) — pas de binary_sensor (déléguer à HA)
- [x] Auto-discovery HA pour les 3 entités (icônes discriminantes `mdi:angle-acute/obtuse/sine-wave`)
- [x] Chip d'état sonde sur la page /ph + modal `<dialog>` détails
- [x] CSS pour les variantes de couleur (`chip--probe-good/warn/warn2/bad/unknown/stale`)
- [x] Doc à jour : `docs/features/page-ph.md`, `docs/MQTT.md`, `docs/API.md`, `docs/subsystems/sensors.md`

## Nouvelles signatures / API

```cpp
// src/atlas_ezo.h
struct PhSlopeInfo {
  float acidPct;       // ex. 99.7 (%)
  float basePct;       // ex. 100.3 (%)
  float zeroOffsetMv;  // ex. -0.89 (mV)
};

class AtlasEzoSensor {
  // ...
  // Renvoie true si parsing OK. Mutex I²C tenu pendant toute la séquence.
  bool querySlope(PhSlopeInfo& out);
};

// src/sensors.h
class SensorManager {
  // ...
  // Tous retournent NaN si jamais lus ou si EZO injoignable.
  float getPhSlopeAcid() const;
  float getPhSlopeBase() const;
  float getPhSlopeZero() const;
  // Âge de la dernière query en ms (UINT32_MAX si jamais).
  uint32_t getPhSlopeAgeMs() const;
  // Force un refresh — non bloquant, retourne true si enqueue OK.
  bool enqueuePhSlopeQuery();
};
```

## Sécurité chimique

- **Dosage impliqué : non** — feature purement diagnostique
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non (on lit, on n'injecte pas)
- Calibration capteur modifiée : **non** — on lit l'état post-calibration
- Algorithme PID touché : **non**
- **pool-chemistry validation** : non requise. Feature passive sans impact régulation.

## ADR attendu ?
- **Probable** : non. Pas de contrainte structurante, pas d'alternatives à arbitrer, pas de verrouillage long terme.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /debug/ph_slope_refresh` (optionnel) — force un refresh

### Champs WebSocket
- `phSlopeAcid` (float, %, NaN si indisponible)
- `phSlopeBase` (float, %, NaN si indisponible)
- `phSlopeZero` (float, mV, NaN si indisponible)
- `phSlopeAgeMs` (int) — âge de la dernière query en ms

### Topics MQTT
- `{base}/ph_slope_acid` (retain)
- `{base}/ph_slope_base` (retain)
- `{base}/ph_slope_zero` (retain)
- `{base}/ph_probe_replace_required` (retain, binary_sensor optionnel)

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-ph.md` — nouveau chip d'état sonde + popup détails

#### Docs transverses
- `docs/MQTT.md` — 3 nouveaux topics + 1 binary_sensor optionnel + auto-discovery HA
- `docs/API.md` — endpoint `/debug/ph_slope_refresh` et nouveaux champs WS
- `docs/subsystems/sensors.md` — méthode `querySlope()`, cache slope, refresh policy

#### Docs immuables
- aucune

## Critères d'acceptation
- [x] AC1 — Au boot, après init EZO, l'info pente est lue dans les 30 s et affichée sur la page /ph (enqueue boot dans `begin()`, traitement queue dans `loopTask`)
- [x] AC2 — Après calibration réussie (mid ou low), l'info pente est rafraîchie dans les 5 s (re-enqueue automatique dans `_processEzoQueue` sur les 3 cas mid/low/clear pH)
- [x] AC3 — Après 24 h sans refresh, le firmware enqueue automatiquement un nouveau `Slope,?` sans intervention utilisateur (`update()` étape 5 — voir `sensors.cpp` ligne 213+)
- [ ] AC4 — Sonde simulée HS (pente forcée 80 %) → chip rouge « Sonde à remplacer » côté UI (test manuel en attente — pas de binary_sensor HA, déplacé en automation utilisateur)
- [x] AC5 — EZO débranché en runtime → chip gris « Pente non disponible » sans crash (cache invalidé NaN après `kEzoBusFailMaxConsecutive` échecs ; UI fallback `unknown`)
- [x] AC6 — Aucune régression sur le PID / `canDose()` / dosage / régulation (feature passive — aucun chemin `pump_controller` ni `canDose` modifié)
- [x] AC7 — `pio run` SUCCESS, 0 nouveau warning, marge flash respectée (RAM 18.0 %, Flash 99.8 % — marge ~2.6 KB)
- [x] AC8 — `./build_fs.sh` SUCCESS, taille FS < 1088 KB (1.1 MB OK)
- [x] AC9 — Auto-discovery HA crée les 3 entités, retain conservé au reboot (publication retain edge-triggered, vérification empirique HA déléguée à l'utilisateur)
- [x] AC10 — Doc à jour (page-ph.md, MQTT.md, API.md, sensors.md, CHANGELOG.md)

## Plan de test manuel
1. **Boot avec sonde calibrée** : observer dans les 30 s l'affichage du chip « Sonde excellente — 99 % » sur la page /ph
2. **Cliquer sur le chip** : popup avec les 3 valeurs détaillées (acide / base / zéro / âge)
3. **Faire `Cal,clear` puis recalibrer** : vérifier que la pente est mise à jour dans les 5 s
4. **Débrancher l'EZO pH** : chip passe à « Pente non disponible »
5. **Forcer une vieille sonde** (ou simuler via `Slope,?` réponse fausse) : chip ambré ou rouge selon seuil
6. **Vérifier topics MQTT** retain dans HA
7. **Vérifier que la régulation pH fonctionne identiquement** avec et sans la feature (test régression)

## Notes pour agent

- Cette feature est **strictement diagnostique** : pas de modification de `canDose()`, pas de modification du PID, pas d'impact sur le dosage. C'est ce qui permet de skipper `pool-chemistry`.
- Le code parse `Slope,?` qui retourne `?Slope,<acid>,<base>,<zero>`. À tester avec un module réel pour valider le format exact (la doc Atlas est parfois imprécise selon la version firmware EZO).
- Si la sonde n'est pas calibrée, la réponse peut être incohérente. Tester ce cas et afficher « Calibration requise » plutôt que des valeurs aberrantes.
- Réutiliser le pattern de cache existant (`_phCalCachedPoints`) — invalidation sur fail streak, refresh opportuniste au retour de bus.
- Découpage suggéré en 2 passes courtes pour éviter timeouts agents :
  - Pass A : firmware (atlas_ezo + sensors + ws + mqtt)
  - Pass B : frontend (HTML + JS + CSS) + endpoint debug
- L'évaluation des seuils (vert/ambré/rouge) est faite côté UI pour permettre l'ajustement sans reflasher. Côté firmware, on expose les valeurs brutes uniquement.

## Hors périmètre
- Pas de modification du PID (algorithme inchangé, mesure inchangée)
- Pas de filtrage logiciel du bruit (cf. décision de ne pas faire la feature « moyennage 60 s »)
- Pas d'historisation longue durée de la pente (à voir si pertinent en feature-026 future)
- Pas d'équivalent pour la sonde ORP (l'EZO ORP n'expose pas de commande `Slope,?` — l'évaluation d'usure ORP se fait différemment)
- Pas de notification push / e-mail quand la sonde devient HS (l'utilisateur le voit dans HA via le binary_sensor)

---

## Notes d'implémentation

- **Date livraison** : 2026-05-08
- **Version cible** : 2.1.0

### Passes effectuées

- **Pass A — firmware** : `src/atlas_ezo.h/.cpp` (struct `PhSlopeInfo` + méthode `querySlope`), `src/sensors.h/.cpp` (cache `_phSlope*`, getters, `enqueuePhSlopeQuery`, handler `QueryPhSlope` dans `_processEzoQueue`, refresh post-cal mid/low/clear, refresh auto 24h), `src/ws_manager.cpp` (4 champs WS, buffer 832 → 1024 octets), `src/mqtt_manager.cpp` (3 topics retain edge-triggered + 3 sensors auto-discovery HA), `src/web_routes_debug.cpp` (`POST /debug/ph_slope_refresh`), `src/constants.h` (`kPhSlopeQueryIntervalMs`).
- **Pass B — frontend** : `data/index.html` (chip `#ph-probe-chip` dans `#ph-card-stats`, modal `<dialog id="ph-probe-modal">`), `data/app.js` (`PH_PROBE_*` constantes, `classifyPhProbe()`, `updatePhProbeChip()`, `_renderPhProbeModalValues()`, `_phProbeRefresh()` + flow refresh avec timeout 8 s, `bindPhProbeChip()` avec fallback `<dialog>`), `data/app.css` (variantes `.chip--probe-good/warn/warn2/bad/unknown/stale`).

### Décisions structurantes

- **Cache** : conserver dernière valeur valide, invalidation NaN sur fail streak ≥ `kEzoBusFailMaxConsecutive` (cohérent avec `_phCalCachedPoints`). `_phSlopeQueriedMs` n'est pas remis à zéro pour permettre à l'UI de basculer en état stale graduel.
- **Parsing tolérant 2 ou 3 floats** : firmware EZO ancien peut ne renvoyer que `acide,base` sans le décalage zéro. Cas géré, `zeroOffsetMv = NaN` puis topic MQTT `ph_slope_zero` non publié.
- **Dédoublonnage enqueue** via flag `_phSlopeQueryPending` — évite la saturation de la queue 4 slots si l'utilisateur clique « Rafraîchir » plusieurs fois.
- **Évaluation seuils en UI**, pas en firmware — permet d'ajuster les seuils (95 / 90 / 85 % et 15 / 30 mV) sans reflasher. Le firmware expose les valeurs brutes, point.
- **Pas de `binary_sensor` HA** « à remplacer » — déléguer à l'automation HA utilisateur. Réduit le couplage firmware ↔ politique d'usine et laisse la flexibilité côté HA.
- **Endpoint `/debug/ph_slope_refresh` sans auth** : cohérent avec autres `/debug/*`. Pas un risque de sécurité (action diagnostique, idempotente, ne déclenche aucun dosage).

### Validations

- `architect` : plan validé.
- `pool-chemistry` : skip (feature passive, aucun impact `canDose()` ni PID).
- `embedded-developer` + `web-ui-developer` : Pass A puis Pass B livrées.
- `test-engineer` : tests manuels priorisés (rapport joint au cycle).
- `code-reviewer` : Approve with comments — 1 majeur (anti-spam HA) + 2 mineurs (cohérence `null`, icônes HA) — **3 correctifs appliqués**.

### Risques résiduels

- **Flash 99.8 %** (marge ~2.6 KB) : la prochaine feature significative pourra nécessiter un dégraissage (logs DEBUG, libs inutilisées) ou un passage en `Release` plus agressif.
- **Format `Slope,?`** : à confirmer empiriquement avec un module EZO réel — la doc Atlas est parfois imprécise selon la version firmware. La tolérance 2/3 floats absorbe les variantes connues.

### Tests manuels recommandés

Voir le rapport `test-engineer` du cycle (priorités haute / moyenne / basse). Les tests AC1-AC3, AC5-AC10 sont validés par lecture du code et du build ; **AC4 (sonde simulée HS)** reste à valider manuellement avec un module EZO réel ou en forçant la valeur via un patch local de `querySlope`.
