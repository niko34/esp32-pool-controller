# Fix — Cause de refus de dosage devinée par analyse de texte (panne affichée comme état normal)

## Type
Bug fix d'observabilité de la chaîne de refus de dosage — modèle bug. **Deux défauts, une seule cause racine.**

- **Version cible** : 2.25.1 (correctif immédiat de la 2.25.0 déployée le matin du 27/08)
- **Créée** : 2026-08-27
- **Statut** : **livré** — `pio run` SUCCESS 0 warning, revue OK (2 passes), gate `pool-chemistry` GO ; suite native **778/778**
- **Domaines** : régulation pH/ORP (observabilité seule), API HTTP, WebSocket, MQTT, frontend
- **Specs liées** : `refonte-seuils-ph-fixes-derives` (v2.25.0), feature-022 (sonde figée), feature-058 (filtre), ADR-0041 (porte de validité), **ADR-0049** (créé)

## Symptômes

**Défaut n°1 — panne annoncée sur une sonde saine** (rapporté par l'utilisateur).
Au démarrage, la porte de validité des mesures reste fermée pendant `stabilizationDelayMin` (5 min par défaut), puis le filtre exige 13 échantillons à 5 s d'intervalle. Pendant ~6 min, la cause de refus servie était « lecture pH/ORP filtrée indisponible (NaN) » ; le fragment **« indispo »** correspondait à la règle de l'UI, qui affichait **« EZO indisponible »** — une panne matérielle annoncée sur une sonde parfaitement saine, à chaque redémarrage.

**Défaut n°2 — panne présentée comme un état normal** (symétrique, **plus grave**, non rapporté car invisible).
La cause `FilterNotReady` était libellée « filtre capteur non prêt (**warmup / EZO injoignable**) » : **une seule phrase pour deux causes physiques opposées**. « warm » étant testé avant « ezo » côté UI, une **vraie panne du module EZO** s'affichait **« Stabilisation du filtre »**, côté pH comme côté ORP. La régulation était à l'arrêt et l'appareil annonçait une attente banale.

## Cause racine

L'interface web **devinait** la catégorie du refus en cherchant des **fragments de texte** dans la phrase française servie par le firmware (`_doseBlockReasonFr`, `String.includes()`).

Le défaut n°2 montre que le problème n'est pas une simple erreur d'ordre des tests : **la phrase elle-même mélangeait deux causes**, donc aucun analyseur de texte, même correct, n'aurait pu les distinguer. Deux problèmes superposés :

1. **Transport** — déduire une catégorie machine d'une chaîne rédigée pour un humain, que toute reformulation, traduction ou troncature (96 o) casse silencieusement.
2. **Source** — le firmware ne savait pas lui-même laquelle des **quatre** raisons de `SensorFilter::ready() == false` s'appliquait : fenêtre incomplète (`warmingUp`), plus aucune mesure acceptée / trop ancienne (`stale`), capteur figé (`frozen`), porte d'alimentation fermée (`gateClosed`). Deux états d'attente normaux, deux **pannes**.

## Conception retenue

### Firmware — un code machine à côté du texte

- `phDoseBlockedCode` / `orpDoseBlockedCode` sur `GET /data` **et** la trame WS `sensor_data`, **parité stricte d'emblée** (mêmes noms, mêmes valeurs, mêmes règles `null`), dans le même passage de sérialisation.
- Valeurs = `enum class DoseRefusal`, qui devient de ce fait un **contrat de sérialisation** : valeurs explicites, jamais renumérotées, jamais insérées au milieu, toute cause nouvelle **à la fin**. **23 `static_assert`** valeur par valeur + 1 de non-collision avec la sentinelle.
- Codes ajoutés : `15`-`18` `ReadingNaN{Warmup,Frozen,Stale,GateClosed}`, `19`-`22` `Filter{Warmup,Frozen,Stale,GateClosed}`.
- Sentinelles hors énum : `kDoseBlockedCodeNone` = `0` (⇒ champ publié `null`), `kDoseBlockedCodeGuard` = `250` (cause de garde pH/ORP : message à valeurs runtime, non énumérable ⇒ le client lit le texte).
- Le texte reste publié (journal, MQTT, repli) et **plus aucun libellé ne mélange deux causes**. Les causes génériques `3` / `4` subsistent et disent « état du filtre indéterminé » (fail-closed d'observabilité : au pire une cause imprécise, jamais une panne maquillée).
- Le **code est écrit avant le texte** : un lecteur async voit au pire le nouveau code avec l'ancien texte, et le code fait foi.

### Firmware — discrimination sur l'état interne du filtre

- `SensorFilter` expose `warmingUp()`, `stale(nowMs)` (+ `frozen()`, `gateClosed()` déjà présents), tous `const`, sans effet sur la décision.
- Composition par la fonction **pure** `resolveFilterState(frozen, gateClosed, stale, warmingUp)`, **unique lieu de décision de priorité** — **pannes d'abord** : `frozen → gateClosed → stale → warmup → ready`.
- `gateClosed` **avant** `stale` : porte fermée, l'âge croît par conception ; l'annoncer « périmé » accuserait la sonde d'un blocage voulu par le système.
- `DoseInputs::filterState` ne sert qu'à **qualifier** : aucune condition ajoutée à `evaluateDose()`, aucun seuil touché. Défaut `Unknown` (initialiseur de membre obligatoire, l'agrégat n'est pas value-initialisé).

> ⚠️ **Discriminant REFUSÉ par le gate `pool-chemistry`** : fonder la distinction sur la **fraîcheur de la lecture brute** (`getPhRaw()` récent ⇒ « la sonde répond ⇒ warmup »). Le cache brut est alimenté par **toute** lecture, y compris hors plage et y compris une valeur constante : **une sonde figée répond, donc paraît saine**. La panne silencieuse aurait été réintroduite sous un nom rassurant, par le correctif censé la supprimer.

### Firmware — alerte de panne de mesure

- `ph_dose_sensor_fault` / `orp_dose_sensor_fault` (`warning`, non-retained) sur `FilterFrozen` / `FilterStale` / `ReadingNaNFrozen` / `ReadingNaNStale`, **une fois par épisode** après `kDoseSensorFaultAlertDelayMs` = **60 s** de panne **continue**.
- Le délai (12 cycles de lecture EZO) écarte le transitoire de ré-amorçage à la reprise de circulation : sans lui, chaque démarrage de filtration criait « sonde muette ». L'affichage, lui, est **immédiat**.
- **L'épisode n'est pas redaté quand la panne change de nature** (`Frozen` ↔ `Stale`) — sinon un EZO agonisant n'aurait **jamais** déclenché l'alerte.
- Une seule trace de journal par épisode (`publishAlert()` journalise déjà en `warning`).

### Firmware — grâce de ré-amorçage (correctif de revue)

- `rearm()` arme une grâce sur le prédicat « périmé », jusqu'à `maxAgeMs` (20 s), levée par la première mesure acceptée. Sans elle, chaque réouverture de porte affichait une puce rouge « Sonde muette » sur une sonde saine, au démarrage de la filtration.
- La datation n'est **pas** falsifiée : `_hasValid` / `_lastValidMs` ne décrivent que des échantillons réellement acceptés (invariant qui rend `ready()` et `ageMs()` honnêtes).
- **Seul `stale()` lit ce drapeau.** `ready()`, `unstable()` et le chemin du verdict de dosage l'ignorent. `reset()` **n'arme rien** et **désarme** une grâce en cours : une sonde muette au démarrage à froid reste annoncée sans délai.

### Firmware — repliement de `millis()` sur la grâce (2ᵉ passe de revue)

La première écriture de la grâce portait le **3ᵉ défaut de repliement du projet** (après la retentative de `Slope,?` v2.21.2 et `tickPulse`) : le drapeau n'étant jamais remis à faux par son expiration — évaluée dans un getteur `const` —, la comparaison de deadline `(int32_t)(now − until) < 0` **se retournait à 2³¹ ms**. Une panne de sonde était **masquée à nouveau pendant 24,8 jours**, l'appareil réaffichant « en stabilisation » sur une sonde morte. Atteignable : `millis()` se replie à 49,7 j, l'appareil tourne en continu.

Correction **redondante par conception** :

1. **Forme du prédicat** — intervalle **fermé** `[from, from + maxAgeMs[` testé en `(now − _staleGraceFromMs) < maxAgeMs` **non signé**, au lieu d'une demi-droite vers une échéance. Monotone en **durée écoulée** et non en `now` : il ne peut plus se retourner, **indépendamment de tout désarmement** (au pire, repliement complet à 49,7 j = limite intrinsèque du projet).
2. **Désarmement effectif** — `_expireStaleGrace(nowMs)` en tête de `setContext()` **et** de `addSample()`, les deux seuls chemins non-`const` recevant l'horloge. `setContext()` est atteint **toutes les 5 s inconditionnellement** depuis `_updateFilterGates()`, **y compris bus I²C mort**.

Les tests couvrent les deux protections **séparément** (`test_V2251_grace_expiree_ne_se_rearme_pas_apres_2exp31_ms` vs `test_V2251_grace_desarmee_par_setContext_meme_sans_lecture` / `..._par_addSample_sonde_muette`, plus `test_V2251_grace_traverse_le_repliement_de_millis`) : une mutation supprimant l'une **laisse survivre** les tests de l'autre — délibéré, pour que personne ne puisse en retirer une au motif que l'autre suffit.

**Règle générale retenue** ([ADR-0049](../../../docs/adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md#pourquoi-la-forme-du-prédicat-compte-correctif-de-repliement-revue-v2251)) : mémoriser l'**instant d'armement** et tester la **durée écoulée** ; jamais de date de fin, jamais de comparaison signée à une échéance, jamais d'expiration confiée au seul getteur `const` qui la lit.

### Interface

- Table `DOSE_BLOCK_CODES` : code → libellé, ton (`normal` / `fault` / `unknown`), libellé court de chip, appartenance à la famille « stabilisation des mesures ». **Aucun `includes()`** ; `_doseBlockReasonFr()` supprimée.
- Trois chemins **distincts** : code `0`/`null` ⇒ dosage autorisé ; **clé absente** ⇒ information indisponible, **seul** cas de repli sur le texte serveur, affiché tel quel ; **code inconnu** ⇒ « Cause inconnue (code N) », jamais un libellé voisin.
- Code consulté **avant** le test « brut invalide » dans `_classifyFilterState()` ; pannes (rouge) et états transitoires (ambré) dans deux registres **disjoints** ; ton d'alerte réservé aux pannes dans le modal.
- Brute manquante depuis < 1 min (`FILTER_RAW_BLIP_MS`) ⇒ « Lecture EZO manquante » (avertissement) au lieu de « EZO indisponible » (panne).
- La ligne d'état de dosage se **tait** quand `measures_valid === false` **et** que la cause appartient à la famille `stab` — le badge « Stabilisation : ~N min » fait foi. Une panne reste dite.

## Sécurité chimique

Gate `pool-chemistry` : **GO**. Invariants vérifiés — présence d'eau, watchdog, limites jour/heure, anti-rafale, calibration, fail-closed, deadband, ordre des gardes de `canDose()` : **tous inchangés**. Le correctif est **purement additif en observabilité** : aucune condition, aucun seuil, aucun chemin de décision de dosage n'est modifié. La grâce de ré-amorçage est étanche au verdict (lue par `stale()`, pas par `ready()`).

## Critères d'acceptation

- [x] AC1 — Une **vraie panne d'EZO** (sonde figée ou muette) n'affiche **jamais** « Stabilisation » : elle affiche « Sonde figée » / « Sonde muette » en registre de panne, côté pH **et** ORP.
- [x] AC2 — Au démarrage (porte fermée puis warmup), l'interface n'affiche **plus** « EZO indisponible » sur une sonde saine.
- [x] AC3 — L'interface ne fait **plus aucune** analyse de fragments de texte sur `*DoseBlockedReason` (`_doseBlockReasonFr` supprimée).
- [x] AC4 — `phDoseBlockedCode` / `orpDoseBlockedCode` publiés sur `/data` **et** le WS, mêmes valeurs, mêmes règles `null`.
- [x] AC5 — Les valeurs de `DoseRefusal` sont explicites et verrouillées valeur par valeur par `static_assert` ; le mapping code → libellé est **total** (une cause sans libellé ne compile pas).
- [x] AC6 — Un code inconnu produit « Cause inconnue (code N) » ; une clé absente produit un repli sur le texte, affiché tel quel.
- [x] AC7 — Aucun libellé firmware ne décrit plus deux causes physiques distinctes.
- [x] AC8 — Les quatre raisons de `ready() == false` sont discriminées sur l'**état interne** du filtre, jamais sur un cache de lecture brute.
- [x] AC9 — Priorité **pannes d'abord**, `gateClosed` avant `stale` ; verrouillée par tests natifs sur les combinaisons réellement atteignables (figé + warmup après `rearm`, porte fermée + périmé).
- [x] AC10 — Alerte MQTT `ph/orp_dose_sensor_fault` émise **une fois par épisode** après 60 s de panne continue, **sans redatation** sur changement de nature de la panne.
- [x] AC11 — Aucune alerte « sonde muette » au démarrage de la filtration (grâce de ré-amorçage + temporisation de 60 s en défense en profondeur).
- [x] AC12 — `ready()` et le verdict de dosage **inchangés au bit près** ; la grâce n'est lue que par `stale()`.
- [x] AC13 — La ligne d'état de dosage ne juxtapose plus deux libellés contradictoires avec le badge capteur, et reste bavarde en cas de panne.
- [x] AC16 — La grâce de ré-amorçage est **wrap-safe** : intervalle fermé depuis l'instant d'armement **et** désarmement effectif sur un chemin non-`const` atteint toutes les 5 s, même bus I²C mort. Les deux protections sont couvertes par des tests **distincts**.
- [x] AC17 — Le commentaire d'ordre de priorité de `resolveFilterState()` est **aligné sur le code** (`frozen → gateClosed → stale → warmup → ready`) : un mainteneur « corrigeant » l'un d'après l'autre aurait transformé chaque arrêt de circulation en panne de sonde déclarée.
- [x] AC18 — **Une seule** ligne de journal par épisode de panne de sonde (`publishAlert()` journalise déjà) : pas de doublon dans la partition `history` de 64 Ko.
- [x] AC14 — `pio run` SUCCESS, **0 nouveau warning**.
- [x] AC15 — Suite native **entièrement verte** : **778/778** après révision du test de caractérisation et ajout des tests de repliement (itérations 2 et 3).

## Livrables — check-list

- [x] L1 — `src/dosing_logic.h` / `.cpp` : `DoseFilterState`, `resolveFilterState()`, 8 valeurs de `DoseRefusal`, champ `DoseInputs::filterState`, contrat de sérialisation documenté
- [x] L2 — `src/constants.h` : `kDoseBlockedCodeNone`, `kDoseBlockedCodeGuard`, `kDoseSensorFaultAlertDelayMs`
- [x] L3 — `src/pump_controller.h` / `.cpp` : 23 `static_assert`, getters de code, libellés déconflatés, alerte de panne de mesure, ordre code-avant-texte
- [x] L4 — `src/sensor_filter.h` / `.cpp` : `warmingUp()`, `stale()`, grâce de ré-amorçage
- [x] L5 — `src/sensors.h` / `.cpp` : `getPhFilterState()` / `getOrpFilterState()`
- [x] L6 — `src/web_routes_data.cpp` + `src/ws_manager.cpp` : 2 champs en parité stricte
- [x] L7 — `data/app.js` + `data/app.css` : `DOSE_BLOCK_CODES`, `_doseBlockInfo()`, classification du chip, dé-duplication du statut de dosage
- [x] L8 — Tests natifs : prédicats d'état, priorité, grâce, qualification des causes, sentinelles
- [x] L9 — **ADR-0049** + `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/{pump-controller,sensors,ws-manager}.md`, `docs/features/{page-ph,page-orp,page-dashboard}.md`, `CHANGELOG.md`

## Notes techniques

- **Pourquoi un entier et non une chaîne d'identifiant stable** (`"filter_frozen"`) : 1 à 3 o au lieu de 10 à 20 sur un budget de trame contraint, et surtout un entier ne se compare **que par égalité** — une chaîne invite au `startsWith` / `includes`, c'est-à-dire au procédé retiré.
- **Pourquoi conserver le texte** : seule forme lisible dans le journal et dans les alertes MQTT (pas de table de traduction), et seule capable de porter les **valeurs runtime** (`cal=1, requis=2`, mode, nombre de cycles).
- **Quatre codes dupliqués assumés** (15-18 et 19-22 décrivent les mêmes quatre états pour deux gardes) : fusionner perdrait l'information « laquelle des deux gardes a mordu », et renuméroter est désormais interdit.
- **Trois erreurs commises puis corrigées en revue**, toutes de la même famille que le défaut d'origine (une panne rendue invisible, ou une sonde saine accusée) : (i) l'épisode d'alerte redaté à chaque changement de nature de panne ⇒ alerte jamais émise ; (ii) absence de grâce de ré-amorçage ⇒ puce rouge « Sonde muette » sur sonde saine à chaque démarrage de filtration ; (iii) grâce exprimée en **deadline** et jamais désarmée ⇒ re-masquage d'une panne pendant 24,8 j au repliement de `millis()`.
- **Piège de maintenance fermé** : le commentaire d'ordre de priorité de `resolveFilterState()` annonçait un ordre différent de celui du code. Un mainteneur « corrigeant » l'un d'après l'autre aurait placé `stale` avant `gateClosed`, transformant **chaque arrêt de circulation** en panne de sonde déclarée (puce rouge + alerte MQTT à chaque cycle de filtration).
- **Coût payload** : +51 o au pire cas sur `/data` et sur `sensor_data`.

## Dette résiduelle

- ✅ *(résolu — itération 2)* `test_V2251_reouverture_de_porte_premier_cycle_puis_warmup` caractérisait le comportement **antérieur** à la grâce (il assertait `Stale` au premier cycle après réouverture de porte). Révisé en `test_V2251_reouverture_de_porte_annonce_warmup_jamais_stale`, conforme au comportement voulu et livré.
- L'alerte de panne de mesure couvre `Frozen` et `Stale`, **pas** `GateClosed` ni `Warmup` (états normaux). Un blocage durable de la porte de validité reste couvert uniquement par `ph_filter_not_ready` (5 min).
- **La machine d'épisode de panne vit dans la coquille** (`logRefusalOnce()`), donc **hors couverture native** : `_sensorFaultActive` / `_sensorFaultSinceMs` / `_sensorFaultAlerted` ne sont testés qu'indirectement. Extraction en module pur spécifiée par [feature-077](../todo/feature-077-extraction-machine-episode-panne-sonde.md) ([ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)).
- **Détection d'un produit de correction mal déclaré** (défaut C2 de la contre-visite `pool-chemistry`) volontairement hors périmètre : [feature-076](../todo/feature-076-detection-produit-mal-declare.md).
- Le retard d'annonce d'une sonde muette au moment précis d'un ré-amorçage est de 20 s au plus (intrinsèque : on ne peut pas conclure avant la première lecture qui suit).

## Suivi / itérations

| # | Objet | Statut | Fichiers |
|---|---|---|---|
| 1 | Code machine de cause de refus + déconflation des libellés + discrimination sur l'état interne du filtre + alerte de panne de mesure + table `DOSE_BLOCK_CODES` | **livré — revue OK, gate `pool-chemistry` GO, 766/767 natifs** | `src/dosing_logic.{h,cpp}`, `src/constants.h`, `src/pump_controller.{h,cpp}`, `src/sensor_filter.{h,cpp}`, `src/sensors.{h,cpp}`, `src/web_routes_data.cpp`, `src/ws_manager.cpp`, `src/version.h`, `data/app.js`, `data/app.css`, `test/test_native_dosing/`, `test/test_native_sensor_filter/` |
| 2 | Révision du test de caractérisation antérieur à la grâce → `test_V2251_reouverture_de_porte_annonce_warmup_jamais_stale` | **livré — tests verts** | `test/test_native_sensor_filter/test_sensor_filter.cpp` |
| 3 | **Correctifs de 2ᵉ passe de revue** : (a) épisode d'alerte redaté **uniquement** sur la transition non-panne → panne (un EZO alternant `Frozen` ↔ `Stale` n'alertait jamais) ; (b) grâce de ré-amorçage rendue **wrap-safe** par deux protections cumulatives (intervalle fermé depuis l'instant d'armement + désarmement effectif via `_expireStaleGrace()` en tête de `setContext()` / `addSample()`) — sans quoi une panne était re-masquée 24,8 j ; (c) commentaire de priorité de `resolveFilterState()` aligné sur le code ; (d) doublon de journalisation par épisode supprimé (`publishAlert()` journalise déjà) | **livré — tests verts (778/778), revue OK** | `src/sensor_filter.{h,cpp}`, `src/pump_controller.cpp`, `src/dosing_logic.cpp`, `test/test_native_sensor_filter/test_sensor_filter.cpp` |
| 4 | Complément de documentation des correctifs de 2ᵉ passe | **livré** | `docs/adr/0049-*.md` (décision 10 + règle de forme des fenêtres de temps + 2 alternatives), `docs/subsystems/sensors.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md` |
