## Métadonnées
- **ID** : feature-025
- **Slug** : lissage-mesures-ph-orp-pid
- **Type** : feature
- **Statut** : draft
- **Priorité** : haute
- **Version cible** : 2.2.0 (post feature-024)
- **Dernière mise à jour** : 2026-06-11
- **Spec(s) liée(s)** : `specs/features/done/feature-021-migration-atlas-ezo.md` (intégration EZO), `specs/features/done/feature-024-pente-sonde-ph.md` (diagnostic sonde pH)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID)
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
- [x] pool-chemistry — requis : impact direct sur les décisions de dosage et la dynamique de régulation
- [x] ux-ui-designer — affichage mesure brute / filtrée + état de stabilité
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Mettre en place un **lissage logiciel robuste des mesures pH et ORP** afin de réduire l'impact des fluctuations ponctuelles de lecture sur l'affichage, MQTT/Home Assistant et l'algorithme de régulation.

La feature introduit une chaîne de filtrage explicite :

```text
mesure brute Atlas EZO
→ rejet des valeurs aberrantes
→ filtre médian court
→ moyenne exponentielle lente
→ mesure filtrée utilisée par l'UI / MQTT / PID
```

Elle adapte également l'algorithme PID pour éviter les effets indésirables liés à une mesure filtrée : intégrale trop agressive, réaction à une mesure non stabilisée, surdosage pendant le temps de mélange hydraulique.

## Contexte

Les mesures Atlas EZO pH/ORP sont stables dans un environnement flottant propre, mais peuvent fluctuer fortement en présence de bruit secteur, alimentation 12 V, couplage capacitif ou EMI. Des fluctuations observées de l'ordre de 0,1 à 0,5 pH ne doivent pas être directement transmises à la régulation, car elles peuvent provoquer :

- une activation injustifiée des pompes doseuses ;
- une variation rapide de la sortie PID ;
- une accumulation excessive du terme intégral ;
- un surdosage chimique avant homogénéisation du bassin ;
- une instabilité visuelle sur l'interface et dans Home Assistant.

Cette feature ne remplace pas le travail matériel de réduction du bruit. Elle constitue une **protection logicielle complémentaire** : les valeurs brutes restent visibles pour diagnostic, tandis que les valeurs filtrées deviennent la référence pour l'affichage principal et la régulation.

## Comportement attendu

### Côté firmware

#### Chaîne de filtrage pH / ORP

Pour chaque nouvelle lecture valide Atlas EZO :

1. Stocker la valeur brute (`raw`).
2. Vérifier que la valeur est dans une plage plausible.
3. Rejeter les sauts instantanés incompatibles avec la dynamique piscine.
4. Ajouter la valeur dans un petit buffer circulaire.
5. Calculer une médiane sur les dernières valeurs.
6. Mettre à jour une moyenne exponentielle (`EMA`).
7. Exposer la valeur filtrée (`filtered`) et l'état de stabilité.

#### Paramètres initiaux recommandés

| Paramètre | pH | ORP |
|---|---:|---:|
| Période lecture EZO | existante | existante |
| Buffer médian | 7 mesures | 7 mesures |
| EMA alpha | 0.10 | 0.08 |
| Rejet saut instantané | > 0.15 pH / lecture | > 50 mV / lecture |
| Plage plausible | 0.0 à 14.0 | -1000 à +1500 mV |
| Stabilisation initiale | 5 valeurs valides | 5 valeurs valides |

Ces valeurs doivent être centralisées dans `constants.h` pour ajustement facile.

#### Rejet des valeurs aberrantes

Une mesure doit être ignorée pour le filtre si :

- elle vaut `NaN` ;
- elle sort de la plage plausible ;
- elle saute de plus que le seuil autorisé par rapport à la dernière valeur filtrée, sauf pendant l'initialisation du filtre ;
- le bus I²C est en fail streak au-dessus du seuil existant.

La mesure brute reste disponible pour debug, mais le filtre n'est pas mis à jour.

#### Initialisation du filtre

Au boot ou après reconnexion EZO :

- ne pas déclencher de dosage tant que le filtre n'a pas reçu au moins `kSensorFilterWarmupSamples` valeurs valides ;
- afficher un état `warming_up` côté UI ;
- publier MQTT avec un indicateur de stabilité si disponible.

#### API publique nouvelle dans `Sensors`

```cpp
float getPhRaw() const;               // dernière valeur brute Atlas, NaN si indisponible
float getOrpRaw() const;              // dernière valeur brute Atlas, NaN si indisponible
float getPhFiltered() const;          // valeur filtrée utilisée par la régulation
float getOrpFiltered() const;         // valeur filtrée utilisée par la régulation
bool isPhFilterReady() const;         // true après warmup et dernière mesure valide récente
bool isOrpFilterReady() const;
uint8_t getPhRejectedCount() const;   // compteur glissant / depuis boot, selon pattern projet
uint8_t getOrpRejectedCount() const;
void resetPhFilter();                 // reset manuel / debug / calibration
void resetOrpFilter();
```

#### Impact sur le chemin de mesure existant

- Les getters historiques `getPhValue()` / `getOrpValue()` doivent être analysés avant modification.
- Décision recommandée : conserver leur comportement public actuel si beaucoup de code en dépend, et ajouter explicitement `getPhFiltered()` / `getOrpFiltered()` pour le PID.
- Si les getters existants sont basculés vers la valeur filtrée, documenter clairement le changement et exposer les valeurs brutes sous de nouveaux noms.

### Adaptations PID / régulation

#### Valeur d'entrée PID

Le PID ne doit plus utiliser la mesure brute. Il doit utiliser :

```cpp
phInput = sensors.getPhFiltered();
orpInput = sensors.getOrpFiltered();
```

uniquement si :

```cpp
sensors.isPhFilterReady()
sensors.isOrpFilterReady()
```

Sinon, le dosage est bloqué temporairement avec une raison explicite.

#### Pause obligatoire après dosage

Après chaque activation de pompe pH ou ORP :

- bloquer toute nouvelle décision de dosage pendant une période de mélange ;
- durée recommandée initiale : `kPhMixingDelayMs = 10 à 20 min`, `kOrpMixingDelayMs = 10 à 20 min` ;
- continuer à lire et filtrer les mesures pendant cette période ;
- ne pas accumuler l'intégrale PID pendant la pause.

#### Anti-windup PID

Le terme intégral doit être limité ou désactivé dans les cas suivants :

- filtre non prêt ;
- dosage bloqué par `canDose()` ;
- période de mélange active ;
- sortie PID saturée ;
- mesure rejetée ou capteur instable.

Décision recommandée pour la première version :

- `Kd = 0` ;
- `Ki = 0` ou très faible ;
- privilégier une régulation proportionnelle temporisée ;
- ne réactiver l'intégrale qu'après validation terrain.

#### Sortie PID bornée

La sortie PID doit rester une **durée maximale d'injection dans une fenêtre de temps**, et non une commande continue.

Exemple :

```text
fenêtre de régulation : 60 min
sortie PID : 0 à 120 s max d'injection par fenêtre
pause mélange : 15 min minimum après injection
```

#### Hystérésis / zone morte

Ajouter ou vérifier l'existence d'une zone morte autour des consignes :

| Paramètre | Valeur initiale recommandée |
|---|---:|
| pH deadband | ±0.03 à ±0.05 pH |
| ORP deadband | ±10 à ±20 mV |

Si la valeur filtrée est dans la zone morte, la sortie PID est forcée à 0 et l'intégrale ne doit pas accumuler.

### Côté UI

#### Affichage mesure brute / filtrée

Sur les pages pH et ORP :

- afficher en grand la valeur filtrée ;
- afficher la valeur brute en petit ou dans un panneau debug ;
- afficher un chip d'état du filtre :
  - `Stabilisation...`
  - `Mesure stable`
  - `Pics rejetés`
  - `Capteur instable`
  - `EZO indisponible`

#### Détail au clic / survol

Popup ou expand affichant :

- valeur brute ;
- valeur médiane ;
- valeur filtrée ;
- nombre de valeurs rejetées ;
- âge de la dernière mesure valide ;
- état `filterReady` ;
- raison de blocage dosage si applicable.

#### Paramètres avancés

Optionnel pour cette feature : exposer les paramètres de filtre en lecture seule dans l'UI. La modification runtime des paramètres est hors périmètre sauf si une infrastructure de configuration existe déjà.

### Côté MQTT / Home Assistant

#### Topics publiés

| Topic | Payload | Description |
|---|---|---|
| `{base}/ph_raw` | `7.46` | Dernière mesure pH brute Atlas |
| `{base}/ph_filtered` | `7.43` | Mesure pH filtrée utilisée par régulation |
| `{base}/ph_filter_ready` | `ON/OFF` | Filtre pH prêt |
| `{base}/ph_rejected_count` | `3` | Nombre de mesures pH rejetées |
| `{base}/orp_raw` | `690` | Dernière mesure ORP brute Atlas |
| `{base}/orp_filtered` | `682` | Mesure ORP filtrée utilisée par régulation |
| `{base}/orp_filter_ready` | `ON/OFF` | Filtre ORP prêt |
| `{base}/orp_rejected_count` | `2` | Nombre de mesures ORP rejetées |

#### Auto-discovery HA

Ajouter les sensors / binary_sensors correspondants :

- `Piscine pH brut` (`pH`)
- `Piscine pH filtré` (`pH`)
- `Piscine Filtre pH prêt` (`binary_sensor`)
- `Piscine ORP brut` (`mV`)
- `Piscine ORP filtré` (`mV`)
- `Piscine Filtre ORP prêt` (`binary_sensor`)

Publication à chaque cycle de mesure ou selon le rythme MQTT existant. Éviter le spam si les valeurs n'évoluent pas.

### Cas limites

- **Boot / warmup** : pas de dosage tant que le filtre n'est pas prêt.
- **Calibration pH** : reset du filtre pH après calibration réussie, puis warmup obligatoire.
- **Calibration ORP** : reset du filtre ORP après calibration réussie si calibration ORP supportée.
- **EZO injoignable** : filtre marqué non prêt, dosage bloqué.
- **Valeur brute aberrante isolée** : valeur rejetée, valeur filtrée conservée, dosage non déclenché sur le pic.
- **Longue dérive réelle** : la médiane + EMA doivent suivre progressivement la dérive ; ne pas figer indéfiniment la valeur filtrée.
- **Changement réel rapide après injection** : la période de mélange empêche de réagir trop vite ; le filtre suit lentement la stabilisation.
- **Trop de rejets consécutifs** : capteur déclaré instable, dosage bloqué avec raison explicite.

## Comportement actuel

- Les mesures pH/ORP brutes peuvent être utilisées directement par l'affichage, MQTT et/ou régulation.
- Les fluctuations ponctuelles peuvent être visibles dans l'UI et dans Home Assistant.
- Le PID risque de réagir à des pics non représentatifs de l'état réel du bassin.
- Le terme intégral peut potentiellement accumuler pendant une période où le dosage est bloqué ou pendant le temps de mélange.

## Contraintes techniques

- Ne pas bloquer le chemin chaud de lecture capteurs.
- Pas de délai actif (`delay`) dans la boucle principale pour attendre la stabilisation.
- Coût RAM maîtrisé : buffers circulaires courts uniquement.
- Les valeurs brutes doivent rester disponibles pour diagnostic EMI / sonde.
- Les valeurs filtrées doivent être clairement identifiées comme celles utilisées par la régulation.
- Le PID ne doit jamais doser sur une valeur `NaN`, non prête, ou instable.
- Les limites de sécurité chimique existantes doivent rester prioritaires sur toute sortie PID.

## Fichiers potentiellement concernés

- `src/sensors.h/cpp` — ajout filtre pH/ORP, buffers, getters raw/filtered, état ready/rejected
- `src/sensor_filter.h/cpp` — option recommandé : classe dédiée réutilisable `SensorFilter`
- `src/pump_controller.h/cpp` — entrée PID filtrée, anti-windup, pause mélange, zone morte
- `src/constants.h` — paramètres filtre, deadband, mixing delay, seuils de rejet
- `src/ws_manager.cpp` — champs WS raw/filtered/filterReady/rejected
- `src/mqtt_manager.cpp` — topics raw/filtered/filterReady/rejected + auto-discovery HA
- `src/web_routes_debug.cpp` — endpoint reset filtre / debug état filtre optionnel
- `data/index.html` — affichage valeurs filtrées + détail debug
- `data/app.js` — rendu état filtre, popup détails, reason dosage blocked
- `data/app.css` — styles chips filtre / stabilité
- `docs/subsystems/sensors.md` — documentation chaîne de filtrage
- `docs/subsystems/pump_controller.md` — documentation adaptation PID
- `docs/MQTT.md` — nouveaux topics
- `docs/API.md` — nouveaux champs WebSocket / endpoints debug
- `docs/features/page-ph.md` — affichage brut/filtré et état filtre

## Livrables — check-list

- [x] Classe ou module de filtrage pH/ORP avec médiane + EMA + rejet de pics (`SensorFilter`)
- [x] Paramètres centralisés dans `constants.h`
- [x] Valeurs brutes et filtrées exposées côté `Sensors`
- [x] État `filterReady` et compteur de rejets exposés
- [x] Reset filtre après calibration pH / ORP
- [x] PID/pump_controller basculé sur les valeurs filtrées
- [x] Blocage dosage si filtre non prêt ou capteur instable
- [x] Anti-windup PID pendant blocages, pause mélange et saturation sortie
- [x] Zone morte pH / ORP autour des consignes (= seuil de démarrage)
- [x] Pause mélange obligatoire après injection
- [x] Champs WebSocket ajoutés
- [x] Topics MQTT + auto-discovery HA ajoutés
- [x] UI : valeur filtrée principale + valeur brute debug + chip stabilité
- [x] Endpoint debug optionnel `POST /debug/sensor_filter_reset` (+ `GET /debug/sensor_filter_state`)
- [x] Docs à jour : `sensors.md`, `pump-controller.md`, `MQTT.md`, `API.md`, `page-ph.md`, `page-orp.md`, `CHANGELOG.md`, ADR-0016

## Nouvelles signatures / API

```cpp
// src/sensor_filter.h
class SensorFilter {
public:
  struct Config {
    float minValue;
    float maxValue;
    float maxStep;
    float emaAlpha;
    uint8_t medianWindow;
    uint8_t warmupSamples;
    uint8_t maxConsecutiveRejects;
  };

  explicit SensorFilter(const Config& config);

  bool addSample(float raw, uint32_t nowMs);
  void reset();

  float raw() const;
  float median() const;
  float filtered() const;
  bool ready() const;
  bool unstable() const;
  uint8_t rejectedCount() const;
  uint8_t consecutiveRejects() const;
  uint32_t ageMs(uint32_t nowMs) const;
};

// src/sensors.h
class SensorManager {
public:
  float getPhRaw() const;
  float getPhMedian() const;
  float getPhFiltered() const;
  bool isPhFilterReady() const;
  bool isPhFilterUnstable() const;
  uint8_t getPhRejectedCount() const;

  float getOrpRaw() const;
  float getOrpMedian() const;
  float getOrpFiltered() const;
  bool isOrpFilterReady() const;
  bool isOrpFilterUnstable() const;
  uint8_t getOrpRejectedCount() const;

  void resetPhFilter();
  void resetOrpFilter();
};

// src/pump_controller.h
class PumpController {
public:
  // À appeler quand une injection démarre ou se termine selon architecture existante.
  void notifyPhDose(uint32_t nowMs);
  void notifyOrpDose(uint32_t nowMs);

  bool isPhMixingDelayActive(uint32_t nowMs) const;
  bool isOrpMixingDelayActive(uint32_t nowMs) const;
};
```

## Sécurité chimique

- **Dosage impliqué : oui** — la valeur d'entrée de la régulation change.
- Limites journalières / horaires concernées : oui, les limites existantes doivent rester prioritaires.
- Filtration requise avant action : inchangé si déjà géré ; à vérifier par `pool-chemistry`.
- Calibration capteur modifiée : non, mais le filtre est réinitialisé après calibration.
- Algorithme PID touché : **oui** — entrée filtrée, anti-windup, zone morte, pause mélange.
- **pool-chemistry validation** : requise avant merge.

## ADR attendu ?

- **Oui, recommandé** : choix de régulation « PID complet » vs « proportionnel temporisé ».
- L'ADR doit acter la stratégie retenue pour éviter un PID trop agressif sur un système hydraulique lent.
- Décision proposée : conserver l'infrastructure PID si elle existe, mais configurer par défaut une régulation de type **P temporisée** (`Ki = 0`, `Kd = 0`) avec pause mélange, deadband et limites de dosage.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés

- `POST /debug/sensor_filter_reset` — reset filtres pH/ORP (optionnel)
- `GET /debug/sensor_filter_state` — état brut du filtre (optionnel si infra debug légère)

### Champs WebSocket

- `phRaw` (float, pH, NaN si indisponible)
- `phMedian` (float, pH, NaN si indisponible)
- `phFiltered` (float, pH, NaN si indisponible)
- `phFilterReady` (bool)
- `phFilterUnstable` (bool)
- `phRejectedCount` (int)
- `orpRaw` (float, mV, NaN si indisponible)
- `orpMedian` (float, mV, NaN si indisponible)
- `orpFiltered` (float, mV, NaN si indisponible)
- `orpFilterReady` (bool)
- `orpFilterUnstable` (bool)
- `orpRejectedCount` (int)
- `phMixingDelayActive` (bool)
- `orpMixingDelayActive` (bool)
- `phDoseBlockedReason` (string, optionnel)
- `orpDoseBlockedReason` (string, optionnel)

### Topics MQTT

- `{base}/ph_raw` (retain)
- `{base}/ph_median` (retain)
- `{base}/ph_filtered` (retain)
- `{base}/ph_filter_ready` (retain)
- `{base}/ph_filter_unstable` (retain)
- `{base}/ph_rejected_count` (retain)
- `{base}/orp_raw` (retain)
- `{base}/orp_median` (retain)
- `{base}/orp_filtered` (retain)
- `{base}/orp_filter_ready` (retain)
- `{base}/orp_filter_unstable` (retain)
- `{base}/orp_rejected_count` (retain)
- `{base}/ph_mixing_delay_active` (retain)
- `{base}/orp_mixing_delay_active` (retain)

### Documentation à produire / mettre à jour

#### Docs vivantes

- `docs/features/page-ph.md` — valeur filtrée, valeur brute, chip stabilité
- `docs/features/page-orp.md` — valeur filtrée, valeur brute, chip stabilité si page dédiée

#### Docs transverses

- `docs/subsystems/sensors.md` — architecture du filtre, paramètres, reset après calibration
- `docs/subsystems/pump_controller.md` — entrée filtrée, anti-windup, pause mélange, deadband
- `docs/MQTT.md` — nouveaux topics + auto-discovery HA
- `docs/API.md` — nouveaux champs WS et endpoints debug
- `CHANGELOG.md` — entrée feature-025

#### Docs immuables

- ADR régulation PID vs P temporisée, si le projet maintient un répertoire `docs/adr/`

## Critères d'acceptation

- [x] AC1 — Au boot, les filtres pH/ORP restent `not ready` tant que moins de 5 mesures valides ont été reçues.
- [x] AC2 — Tant que `phFilterReady=false`, aucune injection pH ne peut être déclenchée ; une raison de blocage explicite est exposée.
- [x] AC3 — Tant que `orpFilterReady=false`, aucune injection ORP/chlore ne peut être déclenchée ; une raison de blocage explicite est exposée.
- [x] AC4 — Un pic pH isolé > 0.15 pH par rapport à la valeur filtrée est rejeté et ne modifie pas la sortie PID.
- [x] AC5 — Un pic ORP isolé > 50 mV par rapport à la valeur filtrée est rejeté et ne modifie pas la sortie PID.
- [x] AC6 — Après calibration pH réussie, le filtre pH est réinitialisé et repasse en warmup.
- [x] AC7 — Après injection pH, aucune nouvelle injection pH n'est possible pendant `kPhMixingDelayMs`.
- [x] AC8 — Pendant la pause mélange, l'intégrale PID n'accumule pas d'erreur.
- [x] AC9 — Si la mesure filtrée est dans la zone morte autour de la consigne, la sortie PID est à 0.
- [x] AC10 — Les valeurs brutes restent visibles dans l'UI et publiées en MQTT pour diagnostic.
- [x] AC11 — La valeur affichée principale pH/ORP correspond à la valeur filtrée.
- [x] AC12 — Les topics MQTT raw/filtered/filterReady sont créés via auto-discovery HA et conservent leur état au reboot.
- [x] AC13 — Capteur EZO débranché en runtime : filtre non prêt, dosage bloqué, UI sans crash.
- [x] AC14 — `pio run` SUCCESS, 0 nouveau warning critique, marge flash documentée.
- [x] AC15 — `./build_fs.sh` SUCCESS, taille FS sous la limite projet.
- [x] AC16 — Validation `pool-chemistry` : pas de surdosage possible en cas de mesure instable ou filtrage en warmup (GO).

## Plan de test manuel

1. **Boot normal** : vérifier que les valeurs brutes apparaissent rapidement, mais que `filterReady` devient vrai seulement après le warmup.
2. **Mesure stable** : vérifier que pH/ORP filtrés suivent les valeurs brutes sans oscillation visible excessive.
3. **Injection de pic simulé pH** : forcer ou mocker une valeur brute à +0.3 pH ; vérifier rejet et absence de dosage.
4. **Injection de pic simulé ORP** : forcer ou mocker une valeur brute à +100 mV ; vérifier rejet et absence de dosage.
5. **Calibration pH** : vérifier reset filtre + warmup + blocage temporaire du dosage.
6. **Déconnexion EZO pH** : vérifier `phFilterReady=false`, UI état erreur, dosage pH bloqué.
7. **Déconnexion EZO ORP** : vérifier `orpFilterReady=false`, UI état erreur, dosage ORP bloqué.
8. **Sortie PID saturée** : vérifier que l'intégrale ne continue pas à augmenter pendant saturation.
9. **Pause mélange** : déclencher une injection courte et vérifier qu'une seconde injection est impossible avant expiration du délai.
10. **Home Assistant** : vérifier présence et retain des nouveaux sensors MQTT.
11. **Comparaison terrain** : logger pendant 24 h `raw` vs `filtered` pour valider que le filtre réduit les pics sans masquer une dérive lente réelle.

## Notes pour agent

- Ne pas utiliser le filtrage pour masquer un défaut matériel majeur. Si `raw` reste très instable longtemps, exposer `filterUnstable=true` et bloquer le dosage.
- Le PID doit consommer uniquement les valeurs filtrées et prêtes.
- Éviter tout `delay()` : la pause mélange doit être gérée par timestamps.
- Le filtre doit être déterministe, testable hors matériel Atlas.
- Prévoir des tests unitaires simples sur `SensorFilter` si l'infra de test existe : warmup, médiane, EMA, rejet de pic, reset.
- Attention à la RAM : préférer un buffer fixe de 7 floats par capteur plutôt qu'une structure dynamique.
- Garder les valeurs brutes dans les logs DEBUG pour investiguer les EMI.
- Les seuils proposés sont des valeurs initiales ; ils peuvent être ajustés après mesure terrain.
- Ne pas publier tous les détails du filtre en MQTT si cela sature le broker ; suivre le rythme MQTT existant et dédupliquer les publications.
- Si le projet utilise déjà une notion de `canDose()`, ajouter les nouvelles raisons de blocage sans contourner les sécurités existantes.
- Si la calibration utilise déjà une queue EZO, déclencher le reset filtre uniquement après confirmation de succès.
- Pour le pH, privilégier une régulation lente avec petits pulses et pause mélange. Pour l'ORP, être encore plus conservateur car l'ORP dépend du pH, de la température et du stabilisant.

## Hors périmètre

- Pas de correction matérielle EMI / PCB.
- Pas de changement des consignes chimiques par défaut.
- Pas de calcul automatique de quantité de produit en fonction du volume bassin.
- Pas d'historisation longue durée raw vs filtered au-delà des publications existantes.
- Pas de tuning automatique des coefficients PID.
- Pas de modification du protocole Atlas EZO.
- Pas de filtrage de la température, sauf si une classe générique permet de l'ajouter sans impact.

---

## Notes d'implémentation

- **Date livraison** : 2026-06-16
- **Version cible** : 2.2.0
- **Statut** : livré — tests verts (test-engineer 11/11), pool-chemistry GO, code-reviewer Approved. ADR-0016 créé.

### Fichiers modifiés / créés

- **Créés** : `src/sensor_filter.h`, `src/sensor_filter.cpp` (classe `SensorFilter`), `test/` (tests unitaires filtre), `docs/adr/0016-regulation-p-temporisee-vs-pid.md`.
- **Firmware** : `src/sensors.{h,cpp}` (intégration filtres, getters raw/median/filtered/ready/unstable/rejected, reset après calibration), `src/pump_controller.{h,cpp}` (entrée filtrée, régulation P temporisée Kp pH=8/ORP=0.3 Ki=Kd=0, anti-windup, zone morte = seuil démarrage, pause mélange `notify*Dose`/`is*MixingDelayActive`, `getPhDoseBlockedReason`/`getOrpDoseBlockedReason`, gardes `canDose` #4/#4b/#4c/#6b), `src/constants.h` (`kSensorFilter*`, `kPhEmaAlpha`/`kOrpEmaAlpha`, plages, `kPhMixingDelayMs`/`kOrpMixingDelayMs`), `src/config.h` (`orpStartThreshold` 10 → 15).
- **Web / WS / MQTT** : `src/ws_manager.cpp`, `src/web_routes_data.cpp` (champs raw/median/filtered/ready/unstable/rejected/mixing/blockedReason), `src/web_routes_debug.cpp` (`/debug/sensor_filter_reset`, `/debug/sensor_filter_state`), `src/mqtt_manager.{h,cpp}` (topics + auto-discovery HA).
- **Frontend** : `data/index.html`, `data/app.js` (valeur filtrée principale, ligne brute, chip 5 états, modal détail), `data/app.css`.
- **Docs** : `docs/subsystems/sensors.md`, `docs/subsystems/pump-controller.md`, `docs/MQTT.md`, `docs/API.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/adr/README.md`, `docs/README.md`, `CHANGELOG.md`.

### Décisions structurantes actées

- **ADR-0016** : régulation **P temporisée** par défaut (Kp seul, Ki=0, Kd=0 impératif) sur mesure filtrée, plutôt qu'un PID complet retuné — système hydraulique lent + mesure lissée. Infra PID conservée pour réactivation future de Ki.
- `ph`/`orp` (WS, `/data`, MQTT) = valeur **filtrée** (fallback brut si non amorcé) ; brut/médiane exposés séparément.
- Zone morte PID = seuil de démarrage existant (un seul deadband, pas de second seuil). `orpStartThreshold` relevé 10 → 15 mV.
- Pause mélange en gates `canDose()` **indépendantes** (OR) du timer post-calibration. Gérée par timestamps.

### Passes suggérées

- **Pass A — architecture + filtre capteurs** : créer `SensorFilter`, intégrer dans `Sensors`, exposer raw/median/filtered/ready/rejected, reset après calibration.
- **Pass B — régulation** : basculer `pump_controller` / PID sur filtered, ajouter deadband, anti-windup, pause mélange, raisons de blocage.
- **Pass C — UI / WS / MQTT** : exposer les nouveaux champs, affichage principal filtré, debug brut, auto-discovery HA.
- **Pass D — tests / documentation** : tests unitaires filtre, tests manuels régulation, docs, changelog, ADR si retenu.

### Décisions structurantes proposées

- **Valeur principale affichée** : valeur filtrée.
- **Valeur utilisée par PID** : valeur filtrée uniquement si filtre prêt.
- **Valeur brute** : conservée pour debug UI/MQTT/logs.
- **Régulation par défaut** : P temporisée plutôt que PID complet agressif.
- **Terme dérivé** : désactivé (`Kd = 0`) pour éviter l'amplification du bruit résiduel.
- **Terme intégral** : désactivé ou très faible au départ, avec anti-windup strict.
- **Sécurité dosage** : toute instabilité capteur bloque le dosage plutôt que de lisser agressivement.

### Validations attendues

- `architect` : valider architecture `SensorFilter` et intégration sans duplication.
- `pool-chemistry` : valider deadband, pause mélange, bornes de dosage, anti-surdosage.
- `embedded-developer` : implémenter sans blocage loop et sans allocation dynamique.
- `web-ui-developer` : afficher clairement brut vs filtré pour éviter toute confusion utilisateur.
- `test-engineer` : couvrir les cas pic isolé, warmup, reset, EZO débranché, pause mélange.
- `code-reviewer` : vérifier que tous les chemins de dosage passent bien par les valeurs filtrées prêtes.

### Risques résiduels

- Un filtrage trop lent peut retarder la détection d'une dérive réelle.
- Un filtrage trop agressif peut masquer un problème EMI qui devrait bloquer le dosage.
- La marge Flash peut être faible si l'UI ajoute trop de code.
- Le changement d'entrée PID peut modifier la dynamique de régulation existante et nécessiter un retuning.
- Les seuils initiaux doivent être validés sur bassin réel.

### Tests manuels recommandés

Voir le plan de test manuel ci-dessus. Priorité haute aux tests AC2, AC3, AC4, AC5, AC7, AC8 et AC16 avant activation du dosage automatique.
