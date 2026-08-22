# Page Paramètres — `/settings`

- **Fichier UI** : [`data/index.html:1065`](../../data/index.html:1065) (section `#view-settings`)
- **URL** : `http://poolcontroller.local/#/settings`

## Rôle

Page de configuration système. Structurée en **9 onglets segmentés** ([`data/index.html:1217`](../../data/index.html:1217)) qui pilotent chacun une zone distincte de la configuration firmware. Aucun onglet n'interagit directement avec les capteurs ou les pompes en production — tout passe par `POST /save-config` (CRITICAL) ou des endpoints dédiés.

## Structure — 9 onglets

| Onglet | ID panel | Rôle |
|--------|----------|------|
| Installation | `panel-install` | **Mode d'installation** (câblage) : 3 cartes radio (feature-056). 1ᵉʳ onglet. |
| Wi-Fi | `panel-wifi` | Statut SSID/IP/mDNS + lien vers l'assistant de reconfiguration |
| MQTT | `panel-mqtt` | Activation + broker (host/port/topic/auth) |
| Heure | `panel-time` | NTP ou heure manuelle, timezone, serveur NTP |
| Sécurité | `panel-security` | Changement mot de passe admin |
| Régulation pH / ORP | `panel-regulation` | Vitesse PID, limites horaires/journalières, délai stabilisation (mode pilote/continu **retiré** — fusionné dans l'onglet Installation, feature-056) |
| Système | `panel-system` | Infos système (runtime), OTA GitHub, OTA manuel, reboot, factory reset |
| Avancé | `panel-dev` | Historique des mesures (export/import/purge CSV), affectation pompes, puissance max, débit nominal, tests pompe, diagnostic crash, logs |
| À propos | `panel-about` | Liens projet + versions des libs — contenu statique |

## Données consommées (`GET /get-config` + WebSocket `/ws` + `GET /get-system-info`)

### Installation (`panel-install`, feature-056)

Nouvelle section **en 1ᵉʳ onglet** (feature-056, v2.19.0). Un `radiogroup` (`name="install_mode"`) de **3 cartes radio** décrit le câblage réel et remplace l'ancien sélecteur mode pilote/continu **et** l'ancien toggle « Gérer la filtration » (fusion — voir [ADR-0026](../adr/0026-mode-installation.md)) :

| Valeur | Carte | Câblage | Dosage autorisé si… |
|--------|-------|---------|---------------------|
| `managed` | PoolController pilote la filtration | alim permanente + sortie 12 V → contacteur | filtration commandée ON |
| `powered` | Alimenté par le circuit de filtration | alim reliée à la phase de la pompe de filtration | en permanence (eau présumée présente) |
| `external` | Filtration externe signalée | alim permanente, filtration tierce qui signale son état | signal ON **et** récent (< 3 min) |

- Champ WS/config : `install_mode` (string). Sauvegarde via bouton dédié `#install_save_btn` → `POST /save-config`.
- **Callout signal externe** (`#install-ext-status`, affiché uniquement en mode `external`) : pill `#install-ext-pill` (« Aucun signal » / « Marche » / « Arrêt » / « Périmé ») alimentée par les champs WS `filtration_ext_known` / `filtration_ext_on` / `filtration_ext_age_s` / `filtration_state_stale`. Rappelle les deux canaux de signalement (`POST /filtration/external-state`, MQTT `.../filtration_external_state/set`) et le fail-safe à 3 min.
- **Aide intégrée** (`<details class="tips">` `#install-ext-tips`, dans le callout external) : bloc repliable montrant des exemples prêts à l'emploi pour signaler l'état de la filtration — commande HTTP `curl`, publication MQTT `mosquitto_pub`, et une automatisation Home Assistant complète (recopie de l'état d'une prise + republication toutes les minutes). `fillExternalTips()` (app.js) renseigne dynamiquement l'URL réelle (`location.host`) et le topic MQTT réel (`window._config.topic`, défaut `pool/sensors`).

### Wi-Fi (`panel-wifi`)
- Depuis `GET /wifi/status` : `ssid`, `ip`, `mode` (STA/AP), `mdns_hostname`.

### MQTT (`panel-mqtt`)
- `mqtt_enabled`, `mqtt_server`, `mqtt_port`, `mqtt_topic`, `mqtt_username`, `mqtt_password`
- Statut dynamique via WS : pill `mqtt_status_pill` alimentée par `mqtt_connected`. Le badge est **temps réel** (feature-015) — il bascule en moins de 5 s suivant la détection firmware d'une coupure broker, sans nécessiter de recharger la page. Le champ `mqtt_connected` est poussé à chaque tick `sensor_data` (5 s) en plus du snapshot `config` au load.
- **Placement DOM** (feature-001) : le badge `mqtt_status_pill` est placé comme frère direct du `<h2>` à l'intérieur du `card__head` (sorti du titre), pour cohérence inter-pages avec les cards Filtration et Éclairage. Une règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }` garantit qu'il ne se comprime pas.

### Heure (`panel-time`)
- `time_use_ntp` (bool), `time_timezone` (enum — voir [`config.h:169`](../../src/config.h:169) `TIMEZONES`), `time_ntp_server`
- Heure courante : `GET /time-now` pour pré-remplir le champ manuel.

### Sécurité (`panel-security`)
- Password admin : endpoint dédié `/auth/change-password` (ne passe **pas** par `/save-config`).

> ℹ️ **Card CORS retirée en v2.11.2** (feature-028, [ADR-0023](../adr/0023-politique-cors-retrait.md)) : le mécanisme CORS a été entièrement supprimé (politique même-origine stricte). Le champ `#auth_cors_origins` et le bouton `#cors-save-btn` n'existent plus, et `auth_cors_origins` a disparu de `/get-config` / `/save-config`.

### Régulation (`panel-regulation`)
> ℹ️ **Mode pilote/continu retiré en v2.19.0 (feature-056)** : `regulation_mode` n'est plus dans cet onglet ni dans `/get-config`. Il est fusionné avec l'ancien toggle « Gérer la filtration » dans le **mode d'installation** (onglet Installation, `install_mode`). Voir [ADR-0026](../adr/0026-mode-installation.md).

- `regulation_speed` (`slow` | `normal` | `fast`) — sélectionne le jeu de paramètres PID. Gains exacts détaillés dans [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#algorithme-résumé).
- `ph_limit` / `orp_limit` (min/h) — mapping firmware : `ph_limit_minutes`, `orp_limit_minutes` (CHANGELOG 2026-04-24).
- `ph_daily_limit` / `orp_daily_limit` (mL) — mapping : `max_ph_ml_per_day` / `max_chlorine_ml_per_day` ([`config.h:141`](../../src/config.h:141) `SafetyLimits`).
- `max_cycles_per_day` (feature-060, v2.12.0) — champ **« Cycles de dosage max / jour (par pompe) »** (`#max_cycles_per_day`, [`data/index.html:1573`](../../data/index.html:1573)) : garde anti-emballement (garde 9 anti-cycling), borne le nombre de démarrages de dosage par pompe et par jour. Défaut **48**, saisie **10–60** (`min`/`max` HTML + pré-clamp JS indicatifs — le **serveur clampe systématiquement** dans [10, 60] et journalise un warn, voir [`docs/API.md`](../API.md)). Aide contextuelle sous la grille : « garde anti-emballement — borne le nombre de démarrages de dosage par pompe et par jour (10-60). Le volume reste plafonné par les limites journalière et horaire. » Détails et fondement chimique : [`pump-controller.md`](../subsystems/pump-controller.md#limite-de-cyclesjour-configurable-garde-9-feature-060-v2120).
- `stabilization_delay_min` — voir [`pump_controller.h`](../../src/pump_controller.h) `stabilizationDelayMs`.

> ⚠️ **Paramètres de sécurité NON exposés dans l'UI** (en dur dans le firmware, décision volontaire — voir [docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif](../subsystems/pump-controller.md#paramètres-en-dur-récapitulatif)) :
>
> | Paramètre | Valeur | Rôle |
> |-----------|--------|------|
> | `PH_DEADBAND` | 0.01 | Zone morte du PID pH |
> | `ORP_DEADBAND` | 2.0 mV | Zone morte du PID ORP |
> | `phStartThreshold` / `phStopThreshold` | 0.05 / 0.01 | Hystérésis démarrage/arrêt pH |
> | `orpStartThreshold` / `orpStopThreshold` | 10 / 2 mV | Hystérésis démarrage/arrêt ORP |
> | `minInjectionTimeMs` | 30 000 ms | Durée minimale d'une injection (protection pompe) |
> | `MIN_ACTIVE_DUTY` | 80 | PWM minimum pour qu'une pompe tourne effectivement |
> | `integralMax` | 50 | Anti-windup PID |
> | `kPhMaxError` / `kOrpMaxError` | 1.0 / 200 mV | Normalisation erreur PID |
>
> Toute modification de ces valeurs exige un nouveau build firmware + passage obligatoire par l'agent `pool-chemistry`.
>
> ℹ️ `maxCyclesPerDay` a **quitté ce tableau en v2.12.0** (feature-060) : il est désormais exposé dans l'UI (champ « Cycles de dosage max / jour (par pompe) » ci-dessus), borné en dur [10-60] côté serveur — l'élargissement des bornes reste, lui, soumis à `pool-chemistry`.

### Système (`panel-system`)
- **Card « Infos système »** en **première position** (déplacée depuis Avancé en v2.5.1, feature-046) : Firmware, Build, Uptime, Chip, CPU, **Mémoire utilisable**, PSRAM, Flash, Firmware (flash), FS, Wi-Fi RSSI, MAC — alimentée par `GET /get-system-info` (`loadSystemInfo()`, chargée au démarrage de la page) + bouton **Actualiser** (`#refresh_info_btn`). La version firmware s'affiche sur la ligne « Firmware » (`sys_firmware_version`, depuis `FIRMWARE_VERSION` dans [`version.h`](../../src/version.h)).
- **Ligne « Mémoire utilisable »** (`#sys_free_heap` + note `#sys_heap_raw`, **remplace « Heap libre » en v2.19.0**, [ADR-0038](../adr/0038-metrique-memoire-allouable-8-bits.md)) : affiche la mémoire **réellement allouable** par le firmware, au format `<libre> · plus gros bloc : <bloc>` (ex. `51 KB · plus gros bloc : 17 KB`), depuis les champs `free_heap_8bit` et `max_alloc_8bit`. Le **plus gros bloc** est affiché à côté du total libre parce que c'est lui qui décide si une allocation passe : un total confortable avec un bloc effondré = mémoire fragmentée.
  - **Note secondaire** en dessous (`<small class="kv__note">`, gris, 11 px) : « *Mesure brute : X KB, dont une zone technique inutilisable pour les allocations courantes.* » — c'est l'ancien `free_heap`. Le libellé dit explicitement pourquoi ce chiffre est **plus grand** que la valeur principale, pour qu'un écart de plusieurs dizaines de kilo-octets ne soit pas pris pour une anomalie.
  - **Affichage conditionnel** : la note n'apparaît que si **les deux** mesures sont présentes (sinon la comparaison n'a pas de sens) ; sur un firmware **≤ 2.18.9**, la ligne retombe silencieusement sur la mesure brute seule et la note reste masquée — aucun `undefined` affiché.
  - Arrondi au Ko, `—` tant que la réponse n'est pas chargée. Aucun seuil, aucune couleur : c'est une information de diagnostic, pas une alerte.
- **Ligne « PSRAM »** (`#sys_psram`, ajoutée en v2.18.0, feature-066) : PSRAM du module (WROVER-E) au format `libre / totale KB libres` (champs `psram_size` / `psram_free` de `/get-system-info`, voir [API.md](../API.md)) ; affiche **« absente »** si `psram_size = 0` (module WROOM) et `—` tant que la réponse n'est pas chargée.
- **Ligne « Firmware (flash) »** (`#sys_app_usage`, ajoutée en v2.5.2, feature-047) : occupation de la partition app active au format `X KB / Y KB (Z%)` — taille du binaire (`sketch_size`, `ESP.getSketchSize()`) / taille de la partition OTA courante (`ota_partition_size`) — même format que la ligne FS. Affiche `—` si les champs sont absents (compatibilité ancien firmware). Rend visible la marge flash gagnée par le repartitionnement layout v3 ([ADR-0019](../adr/0019-partition-app-1664k.md)).
- OTA GitHub : `/check-update` → info release, `/download-update` → téléchargement + flash.
  - **Sur module sans PSRAM (WROOM), les deux boutons sont refusés côté firmware** (v2.18.6, [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md)) : `503` immédiat avec `code: "psram_required"`, aucun effet de bord, aucune session TLS ouverte. Le repli est l'OTA manuel ci-dessous. Deux autres refus `503` existent, **temporaires** : `psram_low` (PSRAM présente mais saturée) et `tls_low_memory` (heap dégradé). Voir [API.md](../API.md#garde-tls-sortante-psram-libre--heap-v2186) et [UPDATE_GUIDE.md](../UPDATE_GUIDE.md).
  - **État « mise à jour en ligne indisponible » de la carte (v2.18.7)** — l'indisponibilité **permanente** est rendue visible **avant** tout clic, plutôt que découverte au refus :
    - Déclencheur : `applyOnlineUpdateAvailability(psramSize)`, appelée par `loadSystemInfo()` avec le champ `psram_size` de `GET /get-system-info`. Neutralisation **uniquement** si `psram_size === 0` ; si le champ est **absent** (ancien firmware), la fonction ne touche à rien (compatibilité ascendante).
    - Effets dans la carte « Mise à jour en ligne (GitHub) » : callout permanent `#online_update_unavailable` (classe `callout--warn`, `role="status"` + `aria-live="polite"` pour que son apparition soit annoncée aux lecteurs d'écran sans interrompre la navigation ; markup statique dans [`data/index.html`](../../data/index.html), masqué par défaut) affiché en tête de carte — il explique l'absence de PSRAM (module ESP32-DevKitC-VE nécessaire) et **renvoie explicitement vers la carte « Mise à jour manuelle »** située en dessous ; boutons **« Vérifier »** (`#check_update_btn`) et **« Mettre à jour »** (`#install_update_btn`) **désactivés**.
    - Persistance : le flag interne `onlineUpdateBlocked` est réappliqué dans les blocs `finally` des deux boutons, de sorte qu'un cycle de requête ne réactive jamais « Vérifier » sur un matériel incompatible. Si la lecture suivante de `/get-system-info` annonce de la PSRAM, un blocage précédent est levé.
    - **Discrimination des trois refus par le champ `code`, jamais par le texte** (v2.18.7) : le front lit `err.code` (propagé sur l'objet `Error` depuis le corps JSON) et le compare aux constantes `OTA_CODE_PSRAM_REQUIRED` / `OTA_CODE_PSRAM_LOW` / `OTA_CODE_TLS_LOW_MEMORY`. Le message `error` du firmware reste libre d'évoluer sans casser l'UI.

      | `code` reçu | Callout + boutons | Toast | Suite |
      |---|---|---|---|
      | `psram_required` | callout affiché, boutons **neutralisés** (`applyOnlineUpdateAvailability(0)`) | message firmware **tel quel, sans préfixe**, type `info` | — |
      | `psram_low` | **inchangés** — l'utilisateur peut réessayer | message firmware, type `info` | `loadSystemInfo()` : l'état réel de la carte provient de `/get-system-info` (source de vérité), jamais d'une valeur inventée par le front |
      | `tls_low_memory` | **inchangés** | message firmware, type `info` | `loadSystemInfo()` |

    - Filet de sécurité : si une requête part malgré la neutralisation, le `503 psram_required` est affiché **tel quel, sans préfixe** et en toast de type `info` (v2.18.7) — plus de « Erreur vérification mise à jour : », plus de tonalité d'échec, puisqu'il s'agit d'une caractéristique matérielle et non d'une panne. Même traitement sur le bouton d'installation, qui affiche en plus le message dans la ligne d'état de la carte (`#github_update_status`) et masque la barre de progression ; pour les deux refus temporaires, la ligne d'état porte le message et le bouton reste utilisable.
- OTA manuel : `POST /update` (multipart `.bin`) — voir la card ci-dessous.

#### Card « Mise à jour manuelle » (v2.20.1)

Champs : `#update_file` (input `accept=".bin"`), `#update_type` (`<select>` Firmware / Système de fichiers), `#update_btn`, `#update_progress` + `#update_progress_bar`, `#update_status`.

**Déduction automatique du type à la sélection du fichier** — le `<select>` valait `firmware` par défaut et y **revenait à chaque rechargement de page**, donc juste après un flash de firmware réussi (qui redémarre l'appareil et recharge l'interface). Envoyer `littlefs.bin` dans la foulée partait avec le type `firmware`, et l'échec était indéchiffrable. Depuis la v2.20.1 :

- `detectUpdateType(name)` ([`data/app.js`](../../data/app.js)) déduit `filesystem` / `firmware` / `null` du **nom** du fichier. Le nom est testé pour `littlefs`, `spiffs`, `filesystem`, et `fs.bin` **ancré** sur un séparateur (`^`, `.`, `-`, `_`) — sans cet ancrage, `refs.bin` ou `defs.bin` seraient classés « système de fichiers » et le type serait pré-réglé silencieusement à tort. C'est le **miroir** de `kFsSuffixes` dans [`src/ota_type_logic.h`](../../src/ota_type_logic.h) : toute évolution de la règle touche les **deux** côtés.
- Le `<select>` est **pré-réglé** sur le type déduit — jamais verrouillé, l'utilisateur peut toujours le changer.
- Le type **n'est pas persisté** entre deux visites : mémoriser le dernier type utilisé recréerait exactement le même piège, dans l'autre sens.

**Notice `#update_type_notice`** (`callout`, `role="status"` + `aria-live="polite"`, masquée par défaut) :

| Situation | Classe | Message |
|---|---|---|
| Type déduit == type sélectionné | `callout--info` | « Type réglé sur « … » d'après le nom du fichier (…). Modifiable si besoin. » |
| Type déduit ≠ type sélectionné | `callout--warn` | « Le type sélectionné (« … ») ne correspond pas au nom du fichier (…). Une image envoyée avec le mauvais type échoue, ou écrit dans la mauvaise partition. » |
| Nom non concluant (aucun `null`) | — | notice **masquée** : indéterminé = silence total, aucune supposition affichée |

`display:block` est posé **avant** l'écriture du texte, dans une frame séparée (`requestAnimationFrame`) : la région live doit exister dans l'arbre d'accessibilité avant sa mutation, sinon rien n'est annoncé.

**Confirmation au clic, franchissable** — si l'incohérence est **maintenue** au moment d'envoyer, un `confirmDialog` (« Type de mise à jour incohérent », boutons **Envoyer quand même** / **Annuler**, style `danger`) demande validation. **Jamais de blocage dur** : un clic suffit à passer outre, et une annulation sort sans modifier aucun état. Le cas d'usage légitime existe (image renommée, binaire de test).

**Affichage de la cause d'échec** — la réponse `FAIL: <cause>` du serveur est désormais affichée dans `#update_status` (`parseUpdateFailure()` : retire le préfixe `FAIL:`, borne à 200 caractères, retombe sur « erreur HTTP … — voir le Journal » si le corps est vide). La ligne prend la classe `is-error`, la barre passe en `error`, et les trois contrôles sont réactivés pour permettre une nouvelle tentative. Le succès reste conditionné à `status === 200` **et** corps exactement `OK`. Voir [API.md](../API.md#reponses-post-update-v2201).

> ℹ️ **Cartes retirées/déplacées en v2.5.1** (feature-046) :
> - « Version du firmware » (`#sys_current_firmware_version`) supprimée, redondante avec la ligne « Firmware » d'Infos système.
> - « Historique des mesures (température, pH, ORP) » déplacée vers l'onglet **Avancé** (1ʳᵉ position).

### Avancé (`panel-dev`)
- **Card « Historique des mesures (température, pH, ORP) »** en **première position** (déplacée depuis Système en v2.5.1, feature-046, avant « Configuration des pompes ») : export CSV (`#history_export_btn` → `GET /get-history?range=all`), import CSV (`#history_import_file` + `#history_import_btn` → `POST /history/import`), suppression (`#history_clear_btn` → `POST /history/clear`). Handlers JS inchangés (sélection par id, indépendants de l'emplacement DOM).
- `ph_pump` / `orp_pump` (1 ou 2) — affectation des deux pompes doseuses.
- `pump1_max_duty` / `pump2_max_duty` — puissance max en régulation (%, défaut 50).
- `pump_max_flow_ml_per_min` — débit nominal pour calcul volume injecté (défaut `kPumpMaxFlowMlPerMin = 90.0` [`constants.h`](../../src/constants.h)).
- Tests pompe : `POST /pump1/on`, `/pump1/off`, `/pump2/on`, `/pump2/off` — arrêt auto après 10 s côté firmware.
- `sensor_logs_enabled` (bool) — verbosité logs capteurs pour diagnostic.
- **Card "Signaux capteurs (brut / lissé)"** (feature-057, v2.7.0) — voir section dédiée ci-dessous.
- **Card "Diagnostic crash"** — voir section dédiée ci-dessous.
- **Card "Logs"** — voir section dédiée ci-dessous.
- **Card « Écran LVGL » — supprimée en v2.17.1** : la card (déjà masquée, toggle rendu inerte en v2.17.0 par le retrait complet du support écran, feature-065) est retirée du DOM ([`data/index.html`](../../data/index.html)) et le champ `screen_enabled` n'est plus lu/envoyé par `save()` ([`data/app.js`](../../data/app.js)) ni exposé par `/get-config` / `/save-config` — un client legacy qui l'envoie encore est ignoré sans erreur (voir [API.md](../API.md)). La clé NVS n'est plus écrite ni lue (clé orpheline inoffensive sur les appareils existants).

> ℹ️ **Cartes retirées en v2.5.0** (feature-045) : « Debug oscillation pH » et « Diagnostic EZO », ajoutées pour la campagne de diagnostic d'oscillation pH (2026-05/06). Code récupérable via `git revert` du commit de la feature-045 ; état complet figé au tag `v2.4.0`.
>
> **Réintroduction partielle en v2.7.0** (feature-057) : la trace des signaux est de retour sous la card « Signaux capteurs (brut / lissé) » (ci-dessous), **fidèle** (haute cadence) et **étendue à l'ORP**. « Diagnostic EZO » et « pause WiFi » restent retirés.

#### Card Signaux capteurs (brut / lissé) — feature-057

Réintroduction **fidèle** (haute cadence) de l'ancienne card « Debug oscillation pH » (retirée en v2.5.0), **étendue au pH ET à l'ORP**. Permet de diagnostiquer visuellement le **bruit** ou une **oscillation** de la chaîne de mesure en comparant, sur une fenêtre temporelle continue, la valeur **brute** Atlas EZO à la valeur **lissée** (médiane + EMA, feature-025).

Les données proviennent d'un **ring buffer firmware volatil** de 300 échantillons alimenté à chaque cycle capteur (~5 s), et non du WebSocket — la fidélité de la trace est donc garantie sur toute la fenêtre (voir [`docs/subsystems/sensors.md#trace-des-signaux-capteurs--feature-057`](../subsystems/sensors.md#trace-des-signaux-capteurs--feature-057)). Le buffer est **perdu au reboot**.

**Chargement paresseux** : la trace n'est récupérée qu'à l'**activation de l'onglet Avancé** (pas de polling permanent) via `GET /debug/sensor_trace`.

**Pagination transparente (v2.10.1, bug-crash-sensor-trace-badalloc)** : la route est paginée (50 échantillons max/page — l'ancienne réponse unique de ~20 Ko pouvait provoquer un `std::bad_alloc` → reboot sur heap fragmenté). `loadSensorTrace()` ([`data/app.js`](../../data/app.js)) agrège les pages **séquentiellement** (`?offset=N`, enchaînement via les champs `count`/`returned`, borne dure de **12 itérations** — 6 en nominal pour 300 échantillons) avant de tracer les courbes ; rendu et comportement **inchangés** vus de l'utilisateur. Si le firmware répond **503** (heap trop fragmenté même pour une page), la card affiche « **Mémoire fragmentée, réessayez.** » et masque les graphes.

**Contenu affiché :**

| Élément | Description |
|---------|-------------|
| Compteur | « Échantillons : N » (N = `count`, 0..300). |
| Graphe pH | 2 séries uPlot : pH **brut** vs pH **lissé**. |
| Graphe ORP | 2 séries uPlot : ORP **brut** vs ORP **lissé** (mV). |
| Stats min / max / Δ | Par série : pH brut, pH lissé, ORP brut, ORP lissé (+ T° min/max). |

**Boutons :**

| Bouton | Endpoint | Action |
|--------|----------|--------|
| Recharger la trace | `GET /debug/sensor_trace` (paginé) | Récupère l'état courant du buffer (agrégation des pages) et redessine les graphes. |
| Vider le buffer | `POST /debug/sensor_trace_clear` | Réinitialise le ring buffer firmware puis les graphes (fenêtre d'observation propre). |

**Cas limites :**
- Valeurs `null`/NaN (filtre non amorcé, EZO injoignable) → **trou dans la courbe**, pas de crash.
- Buffer vide → message « aucune donnée », graphes masqués.
- Réponse **503** (heap fragmenté, v2.10.1) → message « Mémoire fragmentée, réessayez. », graphes masqués — plus jamais de reboot de l'appareil.

Voir [`docs/API.md#trace-des-signaux-capteurs-feature-057-v270`](../API.md#trace-des-signaux-capteurs-feature-057-v270) pour le format JSON.

#### Card Diagnostic crash

Positionnée juste avant la card "Logs" dans le panneau Avancé (la card "Infos système" qui la précédait a été déplacée vers l'onglet Système en v2.5.1). Chargée au démarrage de la page via `GET /coredump/info`.

**Contenu affiché :**

| Élément | Description |
|---------|-------------|
| Statut | "Disponible" (badge vert) ou "Aucun coredump" (badge gris) |
| Tâche | Nom FreeRTOS de la tâche crashée (`task`) — affiché si disponible |
| Exception | Code cause + libellé (`exc_cause` / `exc_cause_str`) — affiché si disponible |
| Adresse PC | Valeur hexadécimale du Program Counter (`pc`) — affichée si disponible |

**Boutons :**

| Bouton | Endpoint | Actif quand |
|--------|----------|-------------|
| Actualiser | `GET /coredump/info` | Toujours |
| Télécharger | `GET /coredump/download` → téléchargement de `coredump.bin` | Coredump disponible |
| Effacer | `DELETE /coredump` avec confirmation via `confirmDialog` (variante danger) | Coredump disponible |

**Workflow de décodage (hint affiché dans la card) :**

```bash
./tools/decode_coredump.sh coredump.bin
```

Le script utilise `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO pour produire un backtrace lisible avec noms de fonctions et numéros de ligne.

Voir [`docs/API.md#diagnostic-crash-coredump`](../API.md#diagnostic-crash-coredump) pour les détails des endpoints.

#### Card Logs

Liste défilante des derniers logs poussés via WebSocket + filtres par niveau (DEBUG/INFO/WARN/ERROR/CRITICAL). Quatre boutons dans la barre d'action :

**Chargement paginé transparent (v2.10.4, [ADR-0032](../adr/0032-reponses-http-bornees.md))** : `/get-logs` renvoie des pages bornées (~30 entrées) avec un flag `truncated` ; `loadLogs()` boucle avec `?since=<dernier timestamp reçu>` tant que `truncated === true` — gardes anti-boucle : **max 8 itérations** et curseur strictement croissant. Le rendu est identique vu de l'utilisateur (journal complet). Les modes complet et incrémental (auto-refresh 5 s) partagent la même boucle, seul le curseur de départ diffère ; l'absence du flag `truncated` (ancienne API) est traitée comme dernière page (compatibilité). Si le firmware répond **503** (`heap fragmenté, réessayez`), la boucle s'arrête sur les pages déjà reçues, sans message dédié ; une erreur réseau affiche « Erreur chargement logs. ».

| Bouton | Style | Action |
|--------|-------|--------|
| Actualiser | ghost | Force un rechargement complet via `GET /get-logs` (boucle de pagination ci-dessus) |
| Effacer (écran) | ghost | Vide **uniquement la vue navigateur locale** (`#logs_content`, `allLogEntries`, `lastLogTimestamp`). Les logs côté ESP32 sont intacts ; un rechargement les fait réapparaître. Tooltip : *« Vide uniquement la vue actuelle, les logs restent côté ESP32 »*. |
| Télécharger | ghost | Téléchargement de `pool_logs.txt` via `GET /download-logs` (flux chunked v2.10.4 — lignes > 256 o tronquées « … », cf. [`docs/API.md`](../API.md#get-download-logs--write)) |
| Effacer (firmware) | **danger (rouge)** | Confirmation via `confirmDialog` (variante danger) puis `DELETE /logs` (cf. [`docs/API.md`](../API.md#delete-logs--write)). Vide RAM + buffer pending + supprime `/system.log` côté ESP32, vide aussi la vue locale, toast de succès `Logs effacés (RAM + fichier)`. Tooltip : *« Vide la mémoire et supprime le fichier persistant côté ESP32 »*. |

Toggles complémentaires :
- `Auto (5s)` — rafraîchissement automatique
- `Scroll auto` — suivi de fin
- `sensor_logs_enabled` — verbosité capteurs (« Log des sondes »)
- `debug_logs_enabled` — switch **« Logs DEBUG activés »** placé immédiatement sous « Log des sondes ». Default `false`. Quand le switch est désactivé, `Logger::debug()` court-circuite immédiatement côté firmware (early return, aucune allocation, aucun push WS, aucune écriture buffer). Effet immédiat (pas de redémarrage requis), persistance NVS sous la clé `debug_logs`. Les niveaux `INFO`/`WARNING`/`ERROR`/`CRITICAL` ne sont **pas** affectés. Le filtre UI `#log_level_debug` (case « DEBUG » de la barre de filtres) reste indépendant : il pilote uniquement l'affichage côté navigateur des entrées DEBUG déjà produites.

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Sauvegarder config (tous champs sauf password, dont `install_mode`) | `POST /save-config` | CRITICAL |
| Signaler l'état d'une filtration externe (mode `external`) | `POST /filtration/external-state?running=…` | WRITE |
| Changer mot de passe admin | `POST /auth/change-password` (body `{current, new}`) | CRITICAL |
| Scanner Wi-Fi | `POST /wifi/scan` | WRITE |
| Déconnecter Wi-Fi | `POST /wifi/disconnect` | CRITICAL |
| Vérifier une update | `GET /check-update` | READ |
| Installer une update GitHub | `POST /download-update` | CRITICAL |
| Update manuelle (fichier .bin) | `POST /update` (multipart) | CRITICAL |
| Test pompe 1/2 | `POST /pump1/on` / `/pump2/on` | WRITE |
| Arrêt test pompe | `POST /pump1/off` / `/pump2/off` | WRITE |
| Export historique CSV | `GET /get-history?range=all` | READ |
| Import historique CSV | `POST /history/import` (multipart) | CRITICAL |
| Purger historique | `POST /history/clear` | CRITICAL |
| Redémarrer | `POST /reboot` | CRITICAL |
| Réinitialisation d'usine | `POST /factory-reset` | CRITICAL |
| Lire heure serveur | `GET /time-now` | READ |
| Statut coredump | `GET /coredump/info` | WRITE |
| Télécharger coredump | `GET /coredump/download` | WRITE |
| Effacer coredump | `DELETE /coredump` | WRITE |
| Effacer logs (firmware) | `DELETE /logs` | WRITE |
| Télécharger logs | `GET /download-logs` | WRITE |
| Recharger la trace signaux capteurs | `GET /debug/sensor_trace` | pas d'auth (`/debug/*`) |
| Vider la trace signaux capteurs | `POST /debug/sensor_trace_clear` | pas d'auth (`/debug/*`) |

Auth = le niveau minimum requis, voir [`docs/API.md`](../API.md).

## Confirmations et retours utilisateur (v2.12.0, feature-031)

Plus aucun dialogue natif `alert()` / `confirm()` sur cette page (ni ailleurs dans l'UI) :

- **Messages informatifs** (succès de sauvegarde, erreurs réseau, import/export…) → toasts `showToast` (types `error` / `success` / `info` / `warning` — ce dernier stylé depuis la v2.18.6 ; durée adaptative et retour à la ligne, voir [composants transverses](README.md#showtoastmessage-type--notification-éphémère)).
- **Actions destructives ou sensibles** → modale maison `confirmDialog` (voir [`docs/features/README.md#composants-transverses`](README.md#composants-transverses)), en **variante danger (bouton rouge)** pour les actions irréversibles : purge historique, effacement logs firmware, effacement coredump, déconnexion Wi-Fi, redémarrage, réinitialisation des sondes, reset usine.
- **Reset usine** : la **double confirmation** est conservée — deux modales enchaînées (« Réinitialiser » puis « Tout effacer »), un refus à n'importe quelle étape annule tout.
- Les emojis ⚠ ont été retirés des messages de confirmation : la variante danger (rouge) porte la sévérité.

## Règles firmware

- **Reboot obligatoire** pour appliquer : bascule Wi-Fi STA ↔ AP, changement de timezone (nouvel env TZ).
- **Factory reset** ([`web_routes_config.cpp`](../../src/web_routes_config.cpp) `/factory-reset`) : efface NVS + config LittleFS + calibrations, redémarre en mode AP `PoolControllerAP` sur `192.168.4.1`.
- **OTA** : partition active préservée, partitions app0/app1 = 1408 KB chacune, historique isolé sur une partition dédiée `history` 64 KB, coredump sur partition dédiée 64 KB — voir [ADR-0009](../adr/0009-partition-coredump.md) et [`partitions.csv`](../../partitions.csv).
- **Mot de passe** : hashé par `hashPassword()` avec salt ([`auth_*.cpp`](../../src/)), stocké en NVS, jamais renvoyé par l'API.
- **Pas de CORS** : politique même-origine stricte depuis v2.11.2 ([ADR-0023](../adr/0023-politique-cors-retrait.md)).

## Cas limites

- **Mot de passe < 8 car. / sans chiffre / sans spécial** : bloqué côté UI ([`data/index.html:1241`](../../data/index.html:1241)) — aucun appel réseau.
- **OTA en cours** : `PumpController.setOtaInProgress(true)` arrête les pompes doseuses (voir [`pump_controller.cpp`](../../src/pump_controller.cpp)) et la filtration reste à son état courant.
- **Import CSV malformé** : ligne rejetée individuellement, le reste est importé. Colonnes attendues : `datetime, ph, orp, temperature, filtration, dosing, granularity` — voir [`web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303).
- **Test pompe** : timeout 10 s côté firmware ; si la connexion réseau meurt, la pompe s'arrête quand même.
- **Factory reset pendant une injection** : l'injection est interrompue par le reboot, les cumuls NVS sont effacés.

## Interaction MQTT / Home Assistant

Aucune entité HA pour les paramètres — la page Settings est purement admin (config ESP32).
Le champ `mqtt_enabled` pilote directement la présence / l'absence du client MQTT dans `loop()` ([`mqtt_manager.cpp`](../../src/mqtt_manager.cpp)).

## Fichiers

- [`data/index.html:1065`](../../data/index.html:1065) — structure HTML (tabs + panels)
- [`data/app.js`](../../data/app.js) — handlers des 9 onglets (`setupInstall` (feature-056), `setupWifi`, `setupMqtt`, `setupTime`, `setupSecurity`, `setupRegulation`, `setupSystem`, `setupDev`)
- [`src/config.h`](../../src/config.h) — structs `MqttConfig`, `AuthConfig`, `PumpControlParams`, `SafetyLimits`, `PumpProtection`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config`, `/reboot`, `/reboot-ap`, `/factory-reset`
- [`src/web_routes_auth.cpp`](../../src/) / [`src/auth_manager.cpp`](../../src/) — endpoints `/auth/*`
- [`src/web_routes_wifi.cpp`](../../src/) — `/wifi/scan`, `/wifi/status`, `/wifi/disconnect`, `/wifi/ap/disable`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — `/pump1/*`, `/pump2/*`
- [`src/web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303) — historique (`/get-history`, `/history/import`, `/history/clear`)
- [`src/ota.cpp`](../../src/) / [`src/update.cpp`](../../src/) — `/update`, `/check-update`, `/download-update`

## Specs historiques

Aucune spec dédiée. Les changements récents (renommage `ph_limit_minutes`, suppression `min_pause`, ajout `stabilization_delay_min`) sont tracés dans [`CHANGELOG.md`](../../CHANGELOG.md).

## Card « Identification des sondes de température » (feature-020, panel Avancé)

Sur le PCB v2, le bus OneWire (GPIO 5) supporte 2 sondes DS18B20 (eau piscine + circuit électronique). Comme leurs adresses ROM 1-Wire sont uniques à la fabrication et que l'ordre de scan n'est pas garanti entre PCB, l'utilisateur doit identifier explicitement laquelle est laquelle.

La card s'affiche dans le panel **Avancé** et présente :

- **Pill de statut** : « 0/2 », « 1/2 », « 2/2 ✓ » selon l'avancement
- Pour chaque sonde détectée : adresse ROM en hex majuscule (`28FF1A2B3C4D5E6F`), T° brute mise à jour en temps réel (polling 2 s sur `/sensors/onewire/scan`), un badge « ✓ Eau » ou « ✓ Circuit » une fois identifiée
- Boutons d'assignation : « C'est l'eau de la piscine » + « C'est le circuit interne » (masqués une fois la sonde identifiée)
- Bouton « Réinitialiser l'identification » (visible une fois 2/2 identifiées)

### Workflow utilisateur

1. Ouvrir Paramètres → Avancé → card « Identification des sondes »
2. Tenir l'une des 2 sondes dans la main pendant ~30 secondes
3. Observer dans la card laquelle voit sa T° monter
4. Cliquer le bon bouton sur la sonde qui chauffe
5. La 2ᵉ sonde est automatiquement identifiée comme l'autre rôle (auto-permutation)

### Chip Dashboard

Une chip de notification ambré (pattern `.chip` existant) apparaît sur le Dashboard tant que `sondes_identified === false && sondes_detected >= 1`. Au clic, redirige vers la card d'identification.

Voir [ADR-0013](../adr/0013-identification-sondes-onewire.md) pour la décision (alternatives écartées : convention « plus chaude », index OneWire, QR usine, refus strict).
