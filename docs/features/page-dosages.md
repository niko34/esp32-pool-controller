# Page Produits — `/dosages`

- **Fichier UI** : [`data/index.html:977`](../../data/index.html:977) (section `#view-dosages`)
- **URL** : `http://poolcontroller.local/#/dosages`

## Rôle

Suivre le volume des bidons de produits chimiques (pH et chlore), alerter quand un bidon approche la fin. Le cumul injecté depuis le dernier « Nouveau bidon » est comparé au volume nominal du bidon pour afficher un volume restant et une barre de progression.

## Structure

Deux cartes symétriques, une par produit :

1. **Carte pH** (`#product-ph-card`)
   - Toggle `ph_tracking_enabled` — active / désactive le suivi de ce bidon.
   - Barre de volume (`#product-ph-bar`) + labels « restant / total » (L).
   - **Relevé permanent** (v2.45.0, feature-110) : **Volume injecté** (`#product-ph-injected`, mL) et **Démarrages pompe** (`#product-ph-starts`) — affichés **en continu**, pour que le relevé **existe avant** le jour où il est détruit.
   - Champ **Volume du bidon (L)** (`#ph_container_l`) — par défaut 20 L.
   - Champ **Seuil d'alerte (L)** (`#ph_alert_threshold_l`) — par défaut 2 L.
   - Bouton **Enregistrer** (met à jour volume / seuil sans toucher au cumul).
   - Bouton **Nouveau bidon** — **demande confirmation** (v2.45.0) et **présente le relevé** avant de le détruire, puis réinitialise le cumul **et** les compteurs à 0.
2. **Carte Chlore / ORP** (`#product-orp-card`) — strictement symétrique, toggle `orp_tracking_enabled`.

Le titre de la carte pH s'adapte à `ph_correction_type` (affiche « pH+ (base) » si mode pH+ — [`app.js:1770`](../../data/app.js:1770)).

## Données consommées (WebSocket `/ws` + `GET /get-config`)

**pH**
- `ph_tracking_enabled` (bool)
- `ph_container_ml` (float, mL)
- `ph_remaining_ml` (float, mL) — calculé : `container_ml − total_injected_ml` (borné à 0)
- `ph_alert_threshold_ml` (float, mL)
- `ph_total_injected_ml` (float, mL) — le **`V`** du relevé
- `ph_starts` (`uint32`) — le **`N`** du relevé (v2.45.0)
- `ph_anom` (`uint32`) et `ph_anom_ml` (float, mL) — hygiène du relevé, **`GET /get-config` seulement** (absents de la trame WS)

**ORP / chlore**
- `orp_tracking_enabled`, `orp_container_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml`, `orp_total_injected_ml`, `orp_starts`, `orp_anom`, `orp_anom_ml`

⚠️ **Le rendu du relevé ne touche au DOM que si la clé est présente** (`app.js:3257`). `ph_starts` et `ph_total_injected_ml` **ne sont pas** dans la trame `sensor_data` : sans cette garde, chaque trame capteurs effacerait le relevé une seconde après son affichage (récidive du motif « parité `/data` ↔ WebSocket »).

Voir [`config.h:194`](../../src/config.h:194) struct `ProductConfig` et [`ws_manager.cpp:900`](../../src/ws_manager.cpp:900) (trame `config`) / [`ws_manager.cpp:609`](../../src/ws_manager.cpp:609) (trame `sensor_data`) pour la diffusion.

## Actions

| Action | Endpoint | Auth | Payload |
|--------|----------|------|---------|
| Activer / désactiver suivi | `POST /save-config` | CRITICAL | `{ "ph_tracking_enabled": bool }` ou idem `orp_` |
| Sauvegarder volume & seuil | `POST /save-config` | CRITICAL | `{ "ph_container_ml": N, "ph_alert_threshold_ml": N }` (UI convertit L → mL) |
| Nouveau bidon (reset cumul) | `POST /save-config` | CRITICAL | `{ "ph_container_ml": N, "ph_reset_container": true }` — reset `phTotalInjectedMl`, `phStartCount`, `phAcctAnomalyCount` et `phAcctAnomalyMl` à 0, **dans le même bloc** ([`web_routes_config.cpp:1110`](../../src/web_routes_config.cpp:1110)) |

Remarques :
- Les volumes sont **stockés côté firmware en mL** ; l'UI les saisit et les affiche en L.
- Le cumul `ph_total_injected_ml` / `orp_total_injected_ml` est incrémenté par [`pump_controller.cpp`](../../src/pump_controller.cpp) à chaque injection effectivement réalisée. **v2.21.0** ([ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) : le volume est calculé sur les **débits mesurés** (pesée) — pouls auto/scheduled : kick 400 ms au débit-255 pesé + plateau au débit d'amplitude pesé (`pulseDeltaVolumeMl`) ; marche manuelle `?duty=N` : **majorant** débit-255, sauf duty == duty calibré. Une pompe **non caractérisée** (aucune pesée saisie) ne dose pas en auto/programmé, donc n'incrémente rien — le badge « Débit non caractérisé — pesée requise » l'explique côté Réglages. Fini le « débit nominal `kPumpMaxFlowMlPerMin = 90` » (constante supprimée, elle créditait de l'acide qui ne sortait pas du bidon).

## Relevé du biais de comptage — mode d'emploi (feature-110, v2.45.0)

L'appareil **sur-compte** le volume injecté. La concordance jerricane du **06/09/2026** l'a chiffré pour la première fois : **+4,76 %**, soit **~240 mL sur ~5,04 L** réellement injectés. Le sens est **conservateur** (quotas resserrés, alerte de bidon vide en avance), mais **la nature du biais reste inconnue** — et les deux correctifs possibles sont **incompatibles** :

| Modèle | Équation | Correctif |
|---|---|---|
| **Multiplicatif** | `V_compté = k · V_réel` | diviser par `k` |
| **Déficit fixe par démarrage** | `V_compté = V_réel + N·δ` | soustraire `N·δ` |

Avec le **volume seul**, une inconnue pour une équation : les deux modèles sont **indiscernables**. Avec le **volume `V` et les démarrages `N`**, ils se départagent. Sous le modèle du déficit fixe, le 06/09 donne `δ = 240 / N`, soit **δ ≈ 0,21 mL/démarrage** aux ordres de grandeur observés.

**Cette version mesure, elle ne corrige rien.** Le correctif appartient à [feature-078](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md).

### Quoi noter, et quand

**Quand** : au **changement de bidon**, juste avant de cliquer « Nouveau bidon » — le geste est déjà fait, il n'y a **aucune tâche à mémoriser**. La confirmation du bouton **présente le relevé** à l'instant exact où il va être détruit.

**Quoi** — les cinq nombres, **ensemble** :

| À noter | Où le lire |
|---|---|
| `V` — volume injecté compté | carte produit (« Volume injecté ») ou `*_total_injected_ml` |
| `N` — démarrages pompe | carte produit (« Démarrages pompe ») ou `*_starts` |
| `anom_count` — ticks anormaux | `*_anom` — **`GET /get-config`** ou MQTT `{base}/diagnostic` |
| `anom_ml` — volume crédité par ces ticks | `*_anom_ml` — même sources |
| `V_bidon` — volume **physiquement** mesuré (doseur, pesée) | mesure manuelle |

Les quatre premiers sont aussi dans le topic MQTT **retenu** `{base}/diagnostic` ([MQTT.md](../MQTT.md#relevé-du-biais-de-comptage-feature-110-v2450)) : ils se relisent **plusieurs jours plus tard**, à froid, et survivent à un redémarrage de l'appareil.

> ⚠️ **Un `N` sans son `V`, ou l'un des deux sans son hygiène, ne conclut sur rien.** Les compteurs sont remis à zéro **ensemble** : un relevé partiel ne peut pas être complété après coup.

### Comment reconnaître un relevé qui ne conclut rien

Verdict `pool-chemistry` du **2026-09-07**. Le polluant se juge contre le **signal** (**4,76 %**), pas contre le volume du bidon :

| `anom_ml / V_bidon` | Verdict |
|---|---|
| **< 0,5 %** | relevé **concluant** |
| **0,5 – 1,5 %** | concluant **seulement** après soustraction explicite de `anom_ml` de `V`, écart **recalculé**, et **incertitude publiée** |
| **> 1,5 %** | **NON CONCLUANT sur δ** — aucune calibration `feature-078` ne doit s'appuyer dessus |

**Co-critère sur le dénominateur** : `anom_count / N ≤ 2 %`. Chaque tick anormal vaut approximativement **un front perdu** — au-delà, c'est `N` lui-même qui est douteux, indépendamment du volume.

### « Déficit de démarrage », jamais « amorçage »

`δ ≈ 0,21 mL` vaut **0,19 s de plateau** à 68 mL/min. C'est la signature d'un déficit de **rampe** — montée LEDC, inertie du rotor, relaxation du tube — qui se paie **à chaque front**. Un vrai **désamorçage** coûterait bien davantage et ne survient qu'après un **arrêt long** : un péristaltique ne se désamorce pas entre deux pouls d'une même séance.

**Réserve** : si le mécanisme réel était pourtant le désamorçage, `N` **surcompterait** les fronts et **`δ` ajusté serait un minorant**.

### D'où vient `anom_ml`, et pourquoi il n'est pas décoratif

Défaut **pré-existant**, **révélé** par cette feature et **non introduit** par elle. Les sorties anticipées de `PumpController::update()` — **eau absente**, **stabilisation** post-calibration, **assistant de mesure du débit** (`flowAssistSuspend`), **capteurs non initialisés** — coupent les pompes mais **`return` avant la comptabilité**. L'état de pompe reste **rémanent** : à la reprise, le firmware crédite **toute la durée de la coupure** au débit plateau, pendant que le redémarrage physique **n'est pas compté**.

| Durée de coupure | Volume fantôme crédité |
|---|---:|
| 5 min | ~5,7 mL |
| 1 h | ~68 mL |
| 6 h | ~408 mL |

Les **240 mL** du 06/09 valent **3,5 min** de crédit fantôme : **quelques reprises suffiraient à expliquer *tout* le biais** sans qu'aucun déficit de démarrage n'existe. Les compteurs d'anomalie captent précisément ce cas — le tick de reprise dépasse les **5 000 ms** (`kAcctDeltaAnomalyMs`) de plusieurs ordres de grandeur.

**Ces deux biais poussent `δ` dans le même sens** : `V` surcompte **et** `N` sous-compte — un déficit fixe est donc **systématiquement favorisé par l'asymétrie**. C'est pourquoi l'hygiène du relevé n'est pas un ornement : sans elle, on conclurait au déficit fixe pour de mauvaises raisons.

### Une seule campagne ne peut pas conclure

Un point unique ajuste les **deux** modèles **exactement** — un paramètre, une équation : ils restent **indiscernables**. Il faut **au moins deux campagnes dont le mL/démarrage moyen diffère d'un facteur ≥ 2**.

Et le relevé du **06/09 n'est pas requalifiable après coup** : les compteurs **n'existaient pas**, donc `anom_ml = 0` **n'y prouve rien**. Cette réserve est dirigée vers **`feature-078`** (qui calera le correctif), pas vers cette feature (qui n'en applique aucun).

### Le chemin rapide : le protocole de banc

Le relevé de bidon est le chemin **sans effort humain** — il se déclenche tout seul, mais il attend le prochain changement de bidon. Le protocole de banc écrit dans [`constants.h:2088`](../../src/constants.h:2088) est le chemin **rapide** : **24 pouls de 5 s contre 12 pouls de 10 s**, à **temps de pompe égal**.

- volumes **égaux** ⇒ biais **multiplicatif** ;
- le premier (24 pouls) **plus faible** ⇒ **déficit fixe par démarrage**.

Les deux chemins restent valides et se **recoupent** : deux verdicts concordants valent mieux qu'un seul.

### La confirmation du bouton « Nouveau bidon »

Le bouton partait au **premier clic**, sans confirmation, sur une action **irréversible**. Depuis la v2.45.0 (`resetProduct()`, [`app.js`](../../data/app.js)) :

1. **`GET /get-config` est rejoué d'abord** — obligatoire : `window._config` peut provenir d'une trame WS `config` **partielle**, où `*_anom` / `*_anom_ml` **ne sont jamais** présents. Sans ce rechargement, le relevé serait **incomplet à l'instant même où il est détruit**. Échec de lecture ⇒ toast d'erreur et **aucune remise à zéro**.
2. Une **confirmation** (`confirmDialog`, variante `danger`) présente **Volume injecté** et **Démarrages pompe**, avec les mêmes libellés que la carte.
3. L'**hygiène n'est affichée que si elle est non nulle** — « Relevé peu fiable : *n* démarrages (*x* mL) ont eu lieu hors comptage normal. » Un « 0 » permanent apprendrait à ignorer la zone, et l'avertissement passerait inaperçu le jour où il invalide vraiment le relevé.
4. Seul un **« oui » explicite** (`ok === true`) déclenche le `POST /save-config`.

Le texte de repli (`window.confirm()` des navigateurs sans `<dialog>`) porte **les deux valeurs en clair** : c'est le seul contenu affiché dans ce cas.

Une valeur de bidon **saisie mais non enregistrée** reste prise en compte par la remise à zéro (lue **avant** le rechargement) — comportement inchangé.

## Règles firmware

- **Persistance** : `saveProductConfig()` / `loadProductConfig()` ([`config.cpp:693`](../../src/config.cpp:693) / [`:735`](../../src/config.cpp:735), namespace `pool_prod`). `productConfigDirty` est armé par [`pump_controller.cpp`](../../src/pump_controller.cpp) à chaque injection **et à chaque démarrage compté** ; flush différé **de 60 s** ([`main.cpp:501`](../../src/main.cpp:501)) pour limiter l'écriture NVS — feature-110 n'ajoute **aucune** transaction.
- **Alerte** : quand `remaining_ml ≤ alert_threshold_ml`, la barre passe en rouge côté UI (seuil appliqué au rendu, pas au firmware — aucune action automatique, juste un signal visuel).
- **Tracking désactivé** : le cumul produit **N'AVANCE PLUS**. `creditInjectedVolume()` ([`pump_controller.cpp:2588`](../../src/pump_controller.cpp:2588)) n'incrémente `*TotalInjectedMl` **que si** `productTrackingEnabled()`, et depuis la v2.45.0 les **compteurs de démarrages sont sous la même garde** — `V` et `N` couvrent donc **exactement** le même périmètre, par construction. Seuls les registres de **sécurité** journaliers (`dailyPhInjectedMl` / `dailyOrpInjectedMl`) continuent d'avancer, tracking ou non.
  ⚠️ **Conséquence pour le relevé** : une campagne pendant laquelle le suivi a été désactivé, même brièvement, est **inexploitable** — ni `V` ni `N` ne couvrent la période, et aucun compteur d'anomalie ne le signale.

Voir aussi [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat de comptabilisation du volume injecté.

## Cas limites

- **Bidon vide** (`remaining_ml ≤ 0`) : la barre affiche 0 %, l'injection **n'est pas bloquée** par le firmware (la sécurité vient de `max_ph_ml_per_day` / `max_chlorine_ml_per_day`, pas du suivi produit).
- **Toggle désactivé** : la carte et son toggle restent visibles, mais **tout le corps de la carte est masqué** (`display: none` sur `#product-<prefix>-body`) — relevé compris. Voir `applyProductTracking()` dans [`app.js:3278`](../../data/app.js:3278).
- **Aucun capteur pH/ORP connecté** : le suivi fonctionne quand même tant qu'une injection a lieu (mode manuel ou programmée).

## Interaction MQTT / Home Assistant

Aucune entité HA dédiée aux bidons produits à date. Les cumuls journaliers (`ph_daily_ml`, `orp_daily_ml`) sont exposés séparément via les entités pH/ORP — voir [page-ph.md](page-ph.md), [page-orp.md](page-orp.md).

## Fichiers

- [`data/index.html:977`](../../data/index.html:977) — structure HTML ; relevé permanent à [`data/index.html:1452`](../../data/index.html:1452) (pH) et [`:1507`](../../data/index.html:1507) (chlore)
- [`data/app.js:1783`](../../data/app.js:1783) — `setupProductScreen()`, `saveProduct()`, `resetProduct()`, `updateProductUI()`
- [`data/app.js:3347`](../../data/app.js:3347) — `_buildProductResetSummary()` : contenu de la confirmation « Nouveau bidon »
- [`src/config.h:194`](../../src/config.h:194) — struct `ProductConfig` (dont les 6 compteurs de feature-110)
- [`src/web_routes_config.cpp:301`](../../src/web_routes_config.cpp:301) — les 6 clés servies par `/get-config` ; reset à [`:1110`](../../src/web_routes_config.cpp:1110) / [`:1131`](../../src/web_routes_config.cpp:1131)
- [`src/web_routes_data.cpp:87`](../../src/web_routes_data.cpp:87) — champs exposés dans `/data`
- [`src/ws_manager.cpp:917`](../../src/ws_manager.cpp:917) — `ph_starts` / `orp_starts` dans la trame `config`
- [`src/mqtt_manager.cpp:1195`](../../src/mqtt_manager.cpp:1195) — les 8 clés du topic retenu `{base}/diagnostic`
- [`src/pump_controller.cpp:2664`](../../src/pump_controller.cpp:2664) — comptage des démarrages (front OFF→ON) ; volume à [`:2588`](../../src/pump_controller.cpp:2588)
- [`src/dosing_logic.cpp:229`](../../src/dosing_logic.cpp:229) — `evaluatePumpStartAccounting()` / `saturatingInc()` (logique pure)

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — contrat de comptabilisation du volume injecté et [comptage des démarrages de pompe](../subsystems/pump-controller.md#comptage-des-démarrages-de-pompe-feature-110-v2450).
- [docs/API.md](../API.md#suivi-produit--relevé-du-biais-de-comptage-feature-110-v2450) — les 6 clés de `/get-config`.
- [docs/MQTT.md](../MQTT.md#relevé-du-biais-de-comptage-feature-110-v2450) — les 8 clés du topic retenu `{base}/diagnostic`.
- [docs/subsystems/ws-manager.md](../subsystems/ws-manager.md#feature-110-v2450--compteurs-de-démarrages-de-pompe-ph_starts--orp_starts) — parité WS et les 4 clés HTTP-seules.
- [feature-078](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md) — la spec qui **corrigera** le biais, une fois sa nature établie.
