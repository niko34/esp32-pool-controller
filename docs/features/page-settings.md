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

#### Sélecteur « Taille du bassin » (feature-090, v2.32.0)

Sélecteur segmenté `#pool_size` en **première position** du panneau ([`data/index.html`](../../data/index.html) `#pool_size_block`), 3 libellés : **« Moins de 20 m³ » / « 20 à 60 m³ » / « Plus de 60 m³ »** (`small` / `medium` / `large`). Il **présélectionne** les défauts de régulation adaptés au volume — il n'entre jamais dans un calcul de dosage ([ADR-0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)).

- **Changement de taille** (`_onPoolSizeClick`, [`data/app.js`](../../data/app.js)) : `loadConfig()` **frais d'abord** (colonne « actuel » = état réel du contrôleur), puis **dialogue avant/après** (mécanisme 088, `_buildRegulationResetSummary`) listant les **8 réglages** — valeurs courantes → défauts de la nouvelle taille, lignes qui changent en gras. La **vitesse de correction y figure** : c'est le **seul endroit où elle reste visible** (masquée ailleurs, voir ci-dessous), l'utilisateur DOIT voir qu'elle change avec la taille.
- **Confirmation** → `POST /save-config` avec **`pool_size` seul** (l'UI n'envoie jamais les 8 paramètres : le firmware applique les défauts, journalise avant → après et retune le PID par le hook différé), puis rechargement complet de la config + toast succès. **Annulation** → le sélecteur revient à la taille précédente, **rien n'est modifié** (aucun appel réseau).
- **Défauts par taille servis par le firmware** : le dialogue lit l'objet `regulation_defaults_by_size` de `/get-config` — aucune colonne codée en JS (leçon 087/088, source unique [`src/regulation_defaults.h`](../../src/regulation_defaults.h)).
- **Rétrocompatibilité** (`syncPoolSize()`) : face à un **ancien firmware** (champ `pool_size` absent de `/get-config`), le sélecteur est **masqué** et le sélecteur de vitesse **ré-affiché** — rien de cassé, rien d'inventé. Les caches `_poolSizeCache` / `_regulationDefaultsBySizeCache` / `_regulationSpeedCache` réinjectent les champs absents d'une trame WS `config` partielle (même mécanique que `_regulationDefaults`).

- `regulation_speed` (`slow` | `normal` | `fast`) — sélectionne le jeu de paramètres PID. Gains exacts détaillés dans [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#algorithme-résumé).
  > ℹ️ **v2.32.0 (feature-090)** : le bloc `#regulation_speed_block` est **masqué** quand le firmware expose `pool_size` — la vitesse **dérive de la taille du bassin** (slow/normal/fast pour small/medium/large) et reste visible **uniquement** dans le dialogue de changement de taille. Le réglage reste **intact dans l'API, la NVS et MQTT** (cas experts, rétrocompat) et garde sa valeur tant que la taille n'est pas changée.
  > ⚠️ **v2.26.0** : ce sélecteur pilote désormais **trois** grandeurs côté pH (`Kp`, `Ki = Kp/Ti`, `integralMax`) et non plus le seul `Kp`. Un changement **effectif** de vitesse **remet le rattrapage accumulé à zéro** — servir une intégrale acquise avec un `Ki` jusqu'à 2,25× différent produirait plus du double de l'autorité autorisée. La charge est ré-identifiée en un `Ti` (4 à 8 h). Un enregistrement des réglages **sans changement de vitesse ne purge rien** ([ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)).
- `ph_limit` / `orp_limit` (min/h) — mapping firmware : `ph_limit_minutes`, `orp_limit_minutes` (CHANGELOG 2026-04-24).
- `ph_daily_limit` / `orp_daily_limit` (mL) — mapping : `max_ph_ml_per_day` / `max_chlorine_ml_per_day` ([`config.h:141`](../../src/config.h:141) `SafetyLimits`).
- `max_cycles_per_day` (feature-060, v2.12.0) — champ **« Cycles de dosage max / jour (par pompe) »** (`#max_cycles_per_day`, [`data/index.html:1573`](../../data/index.html:1573)) : garde anti-emballement (garde 9 anti-cycling), borne le nombre de démarrages de dosage par pompe et par jour. Défaut **24** (était 48 — abaissé en v2.20.5 ; ⚠️ la valeur étant persistée en NVS, un appareil **déjà provisionné garde son 48** et doit être réglé à la main dans ce champ), saisie **10–60** (`min`/`max` HTML + pré-clamp JS indicatifs — le **serveur clampe systématiquement** dans [10, 60] et journalise un warn, voir [`docs/API.md`](../API.md)). Aide contextuelle sous la grille : « garde anti-emballement — borne le nombre de démarrages de dosage par pompe et par jour (10-60). Le volume reste plafonné par les limites journalière et horaire. » Détails et fondement chimique : [`pump-controller.md`](../subsystems/pump-controller.md#limite-de-cyclesjour-configurable-garde-9-feature-060-v2120).
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
> | `MIN_ACTIVE_DUTY` | 98 | Plancher de rotation (v2.21.0 : Dmin mesuré 82 + 16 counts de marge — une amplitude sous ce seuil est **refusée**, jamais clampée) |
> | `integralMax` (pH, **v2.26.0**) | 43 200 / **16 200** / 7 200 pH·s selon la vitesse | Anti-windup du terme intégral pH — **dérivé de l'autorité** `U × Ti / Kp` ([ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)), jamais posé à la main. La valeur historique de 50 subsiste comme **défaut de structure volontairement inerte** (Ki = 0 par défaut, cas ORP). |
> | `kPhIntegralAuthorityMlPerMin` (**v2.26.0**) | 6,0 mL/min | Débit maximal que le terme intégral pH seul peut demander (= 2 × la charge mesurée). **Constante de sécurité chimique.** |
> | `kPhMaxError` / `kOrpMaxError` | 1.0 / 200 mV | Normalisation erreur PID |
>
> Toute modification de ces valeurs exige un nouveau build firmware + passage obligatoire par l'agent `pool-chemistry`.
>
> ℹ️ `maxCyclesPerDay` a **quitté ce tableau en v2.12.0** (feature-060) : il est désormais exposé dans l'UI (champ « Cycles de dosage max / jour (par pompe) » ci-dessus), borné en dur [10-60] côté serveur — l'élargissement des bornes reste, lui, soumis à `pool-chemistry`.

#### Bouton « Restaurer les valeurs par défaut » (feature-088, v2.31.0)

Bloc `#regulation_reset_block` ([`data/index.html`](../../data/index.html)) placé **sous le bouton Sauvegarder** du panneau Régulation : bouton `#regulation_reset_btn` + texte d'aide listant le périmètre. Remet les **8 réglages de tuning** aux valeurs d'usine via [`POST /reset-regulation-defaults`](../API.md#post-reset-regulation-defaults--critical). **v2.32.0 (feature-090)** : les valeurs restaurées sont les défauts **de la taille de bassin courante** — l'objet `regulation_defaults` de `/get-config` reflète cette colonne (contrat 088 intact : mêmes clés, à plat) ; sur `medium`, ce sont les valeurs v2.30.0.

- **Visibilité conditionnelle** : le bloc est `hidden` par défaut et n'apparaît que si `/get-config` fournit l'objet `regulation_defaults` (`syncRegulationDefaults()`, appelée par `loadConfig()`). Sur un **firmware antérieur à 2.31.0**, le bouton reste masqué — rien de cassé, rien d'inventé.
- **Aucune valeur d'usine codée côté client** : le module JS (`kRegulationResetFields`, [`data/app.js`](../../data/app.js)) ne porte que les **libellés et unités** d'affichage ; les valeurs viennent exclusivement de `regulation_defaults` (source unique firmware [`src/regulation_defaults.h`](../../src/regulation_defaults.h)).
- **Clic** → `loadConfig()` **frais d'abord** (la trame WS `config` peut avoir remplacé `window._config` sans porter tous les champs — voir [bug-089](../../specs/features/todo/bug-089-parite-ws-config-champs-manquants.md)) → **dialogue récapitulatif « actuel → usine »** sur les 8 paramètres (`confirmDialog` avec contenu DOM `messageNode`) :
  - chaque ligne affiche `Libellé : valeur actuelle → valeur d'usine` ; les lignes qui **changent réellement** sont **en gras** (tolérance flottants 0,005 pour le pH), les autres en grisé ;
  - pied de dialogue : « N paramètre(s) seront modifiés (en gras). Les calibrations, modes et limites de sécurité ne sont pas touchés. » ;
  - **si rien ne change** : « Tous les paramètres sont déjà aux valeurs d'usine. » et le bouton **Restaurer est désactivé** (`confirmDisabled`).
- **Après confirmation** : `POST /reset-regulation-defaults`, puis **rechargement complet de la config** (`loadConfig()` — tous les champs de la page sont rafraîchis, aucun état à moitié appliqué) + toast succès « Valeurs d'usine restaurées ». Si le rechargement échoue, le toast succès invite à recharger la page.
- **Erreurs** : `503` (mutex config occupé — **rien n'a été modifié** côté firmware) → toast « Erreur serveur (503) — réessayez » ; session expirée (401) → « Session expirée — reconnectez-vous » (redirection login déjà déclenchée par `authFetch`) ; erreur réseau → « Erreur réseau — réessayez ». Le bouton est désactivé pendant toute l'opération (anti double-clic).
- **Cache `_regulationDefaults`** : les valeurs d'usine reçues sont mises en cache et réinjectées dans une config partielle (trame WS sans `regulation_defaults`) pour que le bouton ne « clignote » pas entre deux pushs.

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
- ⚠️ **Curseurs d'amplitude retirés en v2.23.0** ([ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md) — l'amplitude est **gelée à 50 %**, valeur usine) : les sliders `#pumpN_max_duty` n'existent plus. À la place, une **ligne lecture seule** par pompe (`#pumpN_cal_info`, classe `.pump-cal-info`) : « Amplitude d'impulsion : X % — débit Y ml/min (**valeur usine**\|**mesuré**) » — X = `pumpN_max_duty_pct` **réel** de `/get-config` (peut différer de 50 si hérité), provenance selon `pumpN_flow_weighed` ; si `cal_duty = 0` : « … — débit non caractérisé ». Le champ serveur reste vivant + validé [40..100] (modifiable **par API seule**, précédent `screen_enabled`), et `pumpN_max_duty_pct` est **purgé des deux payloads de save** du frontend (`save()` global ET save pompes) — immunité contre les sauvegardes d'onglets périmés.
- **Action principale par pompe : « Mesurer le débit » (`#pumpN_measure_btn`, v2.24.0, feature-075)** — ouvre l'[assistant guidé plein écran](#assistant--mesure-du-débit--feature-075-v2240) (carte `#pump-flow-wizard`, section dédiée ci-dessous). ⚠️ **Terminologie v2.24.0 : le mot « pesée » a disparu de toute l'UI** (grep `data/` vide) au profit de « **mesure du débit** » ; l'indication de provenance « pesé » devient « **mesuré** ».
- **« Saisie avancée » (dépliant discret, v2.24.0)** — le formulaire à champ unique de la v2.23.0 (feature-074) subsiste pour l'utilisateur expert, replié sous un lien `#pumpN_adv_toggle` (`aria-expanded`, panneau `#pumpN_adv` masqué par défaut) : champ `#pumpN_cal_flow` (débit mesuré, ml/min, pré-rempli avec la valeur courante — jamais pendant une saisie, patron `ph_offset`) + bouton `#pumpN_cal_save_btn` « Enregistrer » (→ `POST /save-config`, clé `pumpN_pulse_flow_ml_per_min` seule). Validation client : **vide/0/NaN refusés** (toast `error`, débit ∈ ]0..200]) ; le serveur reste l'autorité (400 remonté par `sendConfigDetailed`). Le duty calibré est **déduit côté serveur** de l'amplitude courante et le drapeau `pumpN_flow_weighed` est posé — l'indication de la ligne lecture seule passe à « mesuré » (rafraîchie via `loadConfig`). La mesure est **facultative** (raffinement de CET appareil — un appareil neuf dose sur la valeur usine du modèle).
- **Badge « Débit non caractérisé — mesure requise »** (`#pumpN_uncal_badge`) — alimenté par les champs `/data` `ph/orp_pump_flow_uncharacterized` (routés par chimie → pompe physique via `ph_pump`/`orp_pump`). **Restreint en v2.23.0 au cas `cal_duty = 0`** (effacement explicite via API / NVS hérité jamais calibré — seul état encore fail-closed) : la distinction « valeur usine vs mesuré » relève de la ligne lecture seule, jamais de ce badge. Champs absents (firmware plus ancien) → état conservé, pas de scintillement.
- ⚠️ **Champ « Débit nominal des pompes » (`pump_max_flow_ml_per_min`) supprimé en v2.21.0** — le modèle linéaire duty↔débit est retiré ([ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) ; le débit vient exclusivement de la pesée par pompe.
- Tests pompe : `POST /pump1/on`, `/pump1/off`, `/pump2/on`, `/pump2/off` — arrêt auto après 10 s côté firmware. Les boutons de l'UI démarrent toujours à **pleine puissance** ; le paramètre `?duty=N` ajouté en v2.20.5 (0-255, mesure du seuil de rotation au banc) est **volontairement absent de l'interface** — usage `curl` uniquement, voir [API.md](../API.md#post-pump1on-pump1off--write).
- `sensor_logs_enabled` (bool) — verbosité logs capteurs pour diagnostic.
- **Card "Signaux capteurs (brut / lissé)"** (feature-057, v2.7.0) — voir section dédiée ci-dessous.
- **Card "Diagnostic crash"** — voir section dédiée ci-dessous.
- **Card "Logs"** — voir section dédiée ci-dessous.
- **Card « Écran LVGL » — supprimée en v2.17.1** : la card (déjà masquée, toggle rendu inerte en v2.17.0 par le retrait complet du support écran, feature-065) est retirée du DOM ([`data/index.html`](../../data/index.html)) et le champ `screen_enabled` n'est plus lu/envoyé par `save()` ([`data/app.js`](../../data/app.js)) ni exposé par `/get-config` / `/save-config` — un client legacy qui l'envoie encore est ignoré sans erreur (voir [API.md](../API.md)). La clé NVS n'est plus écrite ni lue (clé orpheline inoffensive sur les appareils existants).

> ℹ️ **Cartes retirées en v2.5.0** (feature-045) : « Debug oscillation pH » et « Diagnostic EZO », ajoutées pour la campagne de diagnostic d'oscillation pH (2026-05/06). Code récupérable via `git revert` du commit de la feature-045 ; état complet figé au tag `v2.4.0`.
>
> **Réintroduction partielle en v2.7.0** (feature-057) : la trace des signaux est de retour sous la card « Signaux capteurs (brut / lissé) » (ci-dessous), **fidèle** (haute cadence) et **étendue à l'ORP**. « Diagnostic EZO » et « pause WiFi » restent retirés.

#### Assistant « Mesure du débit » (feature-075, v2.24.0)

Assistant pas à pas qui remplace le formulaire nu de la feature-074 comme parcours principal de caractérisation du débit réel d'une pompe doseuse. Carte **unique** `#pump-flow-wizard` ([`data/index.html`](../../data/index.html)) paramétrée par pompe, **plein écran** : la classe `is-flow-wizard` sur `#view-settings` masque `.settingsHead`, `#settings-segmented` et `.settingsPanels` (pattern de l'écran de calibration pH). Ouvert par `#pumpN_measure_btn` (« Mesurer le débit ») ; le titre affiche la pompe et sa chimie (« Mesure du débit — Pompe 1 (pH) », résolue via `ph_pump`). Pendant l'assistant, les deux boutons d'entrée sont neutralisés (**une pompe à la fois** — verrou aussi côté serveur, `409 assist_busy`). Logique : `bindFlowWizard()` ([`data/app.js`](../../data/app.js)).

**Principe de sécurité central** : les marches de pompe (purge, mesure) sont **minutées côté serveur** ([`POST /flow-assist/start`](../API.md#post-flow-assiststart--write)) — duty et durée **fixés par le firmware, jamais envoyés par le client**. Le compte à rebours affiché est purement local (tick 500 ms), resynchronisé par **un seul** `GET /data` à l'échéance (aucun nouveau rythme réseau). Le **dosage automatique est suspendu** pendant la session et rétabli à la sortie (`/flow-assist/exit`) ou par le **timeout serveur de 15 min**.

**Déroulé — 6 étapes** (`#flow-wizard-steps`, pattern `.calibration-steps`) :

| # | Étape | Contenu / action |
|---|-------|------------------|
| 1 | **Préparation** | Callout warn : « Ne mesurez jamais avec de l'acide ou du chlore : placez le tuyau d'aspiration dans un seau d'eau claire. La pompe est volumétrique — l'eau délivre le même débit que le produit. » Matériel (seau d'eau claire, récipient gradué), annonce de la suspension du dosage et du **coût budget** (`#flow-wizard-budget` : « ≈ X ml par purge, ≈ Y ml pour la mesure », calculé sur le débit majorant `flow_assist.flow255[n-1]` servi par `/data` — repli 61 si firmware ancien). **v2.25.2** : le majorant servi est désormais **74,0 mL/min pour les deux pompes** ([ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)) — le coût budget annoncé pour une purge de 60 s passe donc de ~61-67 à **~74 mL**. |
| 2 | **Purge à l'eau claire** | Bouton « Purger (60 s) » → `POST /flow-assist/start` (`phase=purge`, duty 255 serveur) ; compte à rebours + bouton « ⏹ Arrêter » (jamais désactivé). Le produit résiduel part par le circuit d'injection normal. Lien « Purger à nouveau (N restantes) » — **3 purges max par session** (`409 purge_limit` au-delà). |
| 3 | **Débranchement** | « Débranchez le tuyau d'injection et placez-le dans le récipient gradué. » (étape informative, aucun appel réseau) |
| 4 | **Mesure** | Bouton « Lancer la mesure (120 s) » → `POST /flow-assist/start` (`phase=measure`, amplitude exacte d'exploitation côté serveur) ; compte à rebours + « ⏹ Arrêter ». À l'échéance, `verifyRunEnd()` vérifie l'état serveur (**un** `GET /data`) : temps restant annoncé → recalage ; `flow_assist.aborted = true` → la mesure est **refusée** (callout « Mesure interrompue — la valeur ne serait pas fiable », bouton « Relancer la mesure ») — **jamais de routage vers la saisie**. |
| 5 | **Volume recueilli** | Champ `#flow-wizard-volume` (« Quel volume lisez-vous ? (ml) », virgule acceptée) ; conversion affichée en direct (« 56 ml en 2:00 → ≈ 28.0 ml/min », durée réelle servie au start) ; **avertissement de vraisemblance non bloquant** hors 20-35 ml/min ; refus client hors ]0..200]. « Enregistrer le débit » → chemin **`POST /save-config` de la feature-074** (clé `pumpN_pulse_flow_ml_per_min` — duty déduit serveur, drapeau « mesuré » posé). Filet : si `flow_assist.aborted` est vrai au moment d'enregistrer (client hors ligne à l'échéance), la saisie est refusée et l'assistant revient à l'étape 4. |
| 6 | **Remontage** | « Rebranchez le tuyau d'injection » + note « les premières doses injecteront l'eau restée dans le tuyau (négligeable) ». « Terminer » → `POST /flow-assist/exit`. |

Bouton **« Annuler »** visible aux étapes 1-5 → `POST /flow-assist/exit` (arrêt pompe + rétablissement du dosage), sans confirmation (action réversible, timeout serveur en filet).

**Cas limites :**

- **WiFi coupé pendant une marche** : callout `#flow-wizard-offline` (« Connexion perdue. La pompe s'arrêtera d'elle-même à la fin du temps. ») piloté par la bannière `app--offline` ; « Arrêter » reste actif (tentative envoyée quand même — la minuterie serveur fait foi en cas d'échec).
- **Fermeture de l'onglet en pleine marche** : garde `beforeunload` (posée **seulement** pendant une marche) ; si l'utilisateur ferme quand même, la minuterie serveur arrête la pompe à l'échéance et la suspension expire par le timeout 15 min. L'assistant ne reprend pas à la reconnexion (état client perdu, assumé).
- **Expiration de session (15 min sans interaction)** : détectée par `window._flowWizardOnData` (branché sur chaque `/data` reçu — aucun rythme ajouté) → callout `#flow-wizard-expired` (« L'assistant a expiré (15 min d'inactivité) — le dosage automatique est rétabli. Reprenez depuis la purge. ») + retour à l'étape 1. À l'étape 6 (débit déjà enregistré), l'expiration ne change rien.
- **Marche interrompue par le serveur** (perte de filtration) : drapeau `flow_assist.aborted` — purge : toast + retour au bouton « Purger » ; mesure : **jamais enregistrable** (double verrou : routage `verifyRunEnd` + filet à l'enregistrement). Le volume déjà pompé est crédité prorata au majorant côté firmware (P4).
- **Refus 409 au démarrage d'une marche** : messages français dédiés (`FLOW_ASSIST_REFUSALS` — `assist_busy`, `purge_limit`, `already_injecting`, `invalid_amplitude` + les codes des gardes manuelles partagés `filtration_off`, `daily_limit`…), toast `error`, l'assistant reste à l'étape courante.

#### Card Signaux capteurs (brut / lissé) — feature-057

Réintroduction **fidèle** (haute cadence) de l'ancienne card « Debug oscillation pH » (retirée en v2.5.0), **étendue au pH ET à l'ORP**. Permet de diagnostiquer visuellement le **bruit** ou une **oscillation** de la chaîne de mesure en comparant, sur une fenêtre temporelle continue, la valeur **brute** Atlas EZO à la valeur **lissée** (médiane + EMA, feature-025).

Les données proviennent d'un **ring buffer firmware volatil** de 300 échantillons alimenté à chaque cycle capteur (~5 s), et non du WebSocket — la fidélité de la trace est donc garantie sur toute la fenêtre (voir [`docs/subsystems/sensors.md#trace-des-signaux-capteurs--feature-057`](../subsystems/sensors.md#trace-des-signaux-capteurs--feature-057)). Le buffer est **perdu au reboot**.

**Chargement paresseux** : la trace n'est récupérée qu'à l'**activation de l'onglet Avancé** (pas de polling permanent) via `GET /debug/sensor_trace`.

**Pagination transparente (v2.10.1, bug-crash-sensor-trace-badalloc)** : la route est paginée (50 échantillons max/page — l'ancienne réponse unique de ~20 Ko pouvait provoquer un `std::bad_alloc` → reboot sur heap fragmenté). `loadSensorTrace()` ([`data/app.js`](../../data/app.js)) agrège les pages **séquentiellement** (`?offset=N`, enchaînement via les champs `count`/`returned`, borne dure de **12 itérations** — 6 en nominal pour 300 échantillons) avant de tracer les courbes ; rendu et comportement **inchangés** vus de l'utilisateur. Si le firmware répond **503** (heap trop fragmenté même pour une page), la card affiche « **Mémoire fragmentée, réessayez.** » et masque les graphes.

**Contenu affiché :**

| Élément | Description |
|---------|-------------|
| Compteur | « Échantillons : N » (N = `count`, 0..300). |
| Dispersion pH / ORP | **feature-071, v2.20.7** — dispersion **instantanée** du drapeau `unstable`, lue sur `GET /debug/sensor_filter_state`. Voir sous-section dédiée ci-dessous. |
| Graphe pH | 2 séries uPlot : pH **brut** vs pH **lissé**. |
| Graphe ORP | 2 séries uPlot : ORP **brut** vs ORP **lissé** (mV). |
| Stats min / max / « Δ 25 min » | Par série : pH brut, pH lissé, ORP brut, ORP lissé (+ T° min/max). **Δ renommé « Δ 25 min » en v2.20.7** : c'est le crête-à-crête `max − min` sur **tout** le buffer (~300 pts ≈ 25 min), dérive lente comprise — **sans rapport d'échelle** avec la dispersion du drapeau `unstable` (mesuré sur une même série : crête-à-crête 0,279 pour une dispersion réelle de 0,055). C'est cette confusion qui a motivé la feature-071. |
| Dispersion trace pH / ORP (indicatif) | **feature-071, v2.20.7** — dispersion **reconstruite** sur la trace affichée. Voir sous-section dédiée ci-dessous. |
| Courbes de dispersion pH / ORP | **feature-072, v2.20.8** — sous-graphe sous chaque graphe brut/lissé : **trajectoire** de la dispersion reconstruite, avec ligne de seuil. Voir sous-section dédiée ci-dessous. |

**Boutons :**

| Bouton | Endpoint | Action |
|--------|----------|--------|
| Recharger la trace | `GET /debug/sensor_trace` (paginé) | Récupère l'état courant du buffer (agrégation des pages) et redessine les graphes. |
| Vider le buffer | `POST /debug/sensor_trace_clear` | Réinitialise le ring buffer firmware puis les graphes (fenêtre d'observation propre). |

**Cas limites :**
- Valeurs `null`/NaN (filtre non amorcé, EZO injoignable) → **trou dans la courbe**, pas de crash.
- Buffer vide → message « aucune donnée », graphes masqués — les lignes « Dispersion pH/ORP » restent affichées (hors bloc graphes, voir ci-dessous).
- Réponse **503** (heap fragmenté, v2.10.1) → message « Mémoire fragmentée, réessayez. », graphes masqués — plus jamais de reboot de l'appareil.

##### Dispersion du drapeau `unstable` — feature-071 (v2.20.7)

Deux lignes « **Dispersion pH** » / « **Dispersion ORP** » affichent la valeur **instantanée** publiée par [`GET /debug/sensor_filter_state`](../API.md#get-debugsensor_filter_state--pas-dauth-cohérent-avec-autres-debug) (v2.20.6) : l'amplitude `max − min` de la fenêtre médiane de 13 échantillons, **la grandeur qui décide du drapeau `unstable`** et donc de l'inhibition du dosage automatique — directement comparable aux résultats des campagnes de mesure.

- **Format** : `valeur / seuil` (seuil = champ `dispersion_max` de la route : 0,5 pH / 100 mV ; arrondis de la route : 3 décimales pH, 1 décimale mV), suivi d'une pill « **stable** » (verte, `pill ok`) ou « **INSTABLE** » (rouge, `pill bad` — l'inhibition du dosage est en cours).
- **Sémantique EXACTE du tiret** — `dispersion: null` (warmup ou porte d'alimentation fermée, filtration arrêtée) → `— · pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage)`, **sans pill**. Jamais « stable » : `unstable: false` sans données signifie « **aucun jugement rendu** », pas « la mesure est stable » — piège de lecture réel documenté dans [`docs/API.md` §Marge d'instabilité](../API.md#marge-dinstabilité-v2206). L'UI ne juge que si `dispersion` est un nombre.
- **Indépendance** : ces lignes vivent **hors du bloc graphes** (`#trace_charts`) — elles s'affichent même buffer de trace vide, et une panne de `/debug/sensor_filter_state` les passe à « **indisponible** » sans toucher au reste de la carte. `loadFilterState()` et `loadSensorTrace()` ([`data/app.js`](../../data/app.js)) sont deux promesses volontairement jamais chaînées ni await-ées l'une sur l'autre.
- **Déclencheurs** : les mêmes que la trace — activation de l'onglet Avancé + bouton « Recharger la trace ». Pas de polling permanent.

##### Dispersion reconstruite sur la trace (indicatif) — feature-071 (v2.20.7)

Deux lignes « **Dispersion trace pH/ORP (indicatif)** » reconstruisent la dispersion firmware sur les points **bruts** de la trace affichée, pour comparaison avec les rapports de campagne (médiane/max) : fenêtre glissante de **13 points** (= `kSensorFilterCentralWindow`), affichage `médiane · max` des dispersions de fenêtres.

- Une fenêtre est **écartée** si elle contient un trou (`null`/NaN) ou un **saut consécutif** ≥ 1,0 pH / 300 mV (`kPhFilterSpikeReject` / `kOrpFilterSpikeReject`, dupliquées de [`constants.h`](../../src/constants.h)) : le firmware rejette ces pics **avant** la fenêtre médiane, la trace brute les contient encore. Précédent réel : reconstruction naïve → faux max 5,425 sur un transitoire de redémarrage de filtration, là où le firmware voyait 0,308.
- **Tiret** si la trace compte moins de 13 points ou si aucune fenêtre n'est valide.

⚠️ **Deux limites de fidélité, d'où le libellé « indicatif »** :
1. La fenêtre firmware est **gelée** circulation arrêtée (porte d'alimentation, [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)) alors que la trace enregistre tout : hors régime établi, la reconstruction inclut des points que le firmware n'a jamais ingérés. Une note sous les stats le rappelle : **la valeur instantanée fait foi**.
2. Le critère de rejet diffère : la reconstruction écarte les fenêtres sur **sauts consécutifs**, alors que le firmware rejette chaque échantillon sur `|brut − médiane|` — approximation assumée (nuance relevée en revue de code).

##### Courbes d'évolution de la dispersion — feature-072 (v2.20.8)

La ligne « médiane · max » (feature-071) résume la trace mais ne distingue pas un plateau tranquille d'excursions répétées qui frôlent le drapeau. La feature-072 ajoute la **trajectoire** : dans chaque bloc de graphe (pH puis ORP), un **sous-graphe de 120 px** ([`data/app.css`](../../data/app.css) `.trace-chart--dispersion`) placé sous le graphe brut/lissé trace la dispersion de chaque fenêtre glissante de 13 points, en **valeur absolue** (pH d'un côté, mV de l'autre — décision utilisateur explicite : **pas** de normalisation en % du seuil).

**Lecture du graphe :**

- **Courbe ambre `#d97706`** — identité visuelle unique de la dispersion, distincte des violets pH et des verts ORP. Tracé **linéaire à points visibles** (pas de spline : un point valide isolé entre deux trous doit se voir). Chaque point est positionné au **temps du dernier échantillon de sa fenêtre** (série alignée sur les échantillons bruts, calculée par `_traceDispersionStats()` [`data/app.js`](../../data/app.js) — même calcul que la ligne médiane · max, série exposée sans duplication). Tooltip « Dispersion : x » (3 décimales pH, 1 décimale mV).
- **Ligne de seuil en pointillé rouge `#dc2626`** (0,5 pH / 100 mV), exclue du tooltip, avec légende « Seuil 0,50 » / « Seuil 100 mV ». **L'échelle Y inclut toujours le seuil** (option `yInclude` de `createTraceChart`) : une courbe très basse montre la **marge** au drapeau `unstable`, jamais un zoom sur le bruit.
- **Même axe X** que les graphes brut/lissé au-dessus → corrélation verticale directe : on voit le transitoire sur le brut ET le trou correspondant dessous.

**Sémantique des trous — une information, pas un manque :** un trou dans la courbe = **fenêtre écartée** par la reconstruction 071 (trou `null`/NaN dans le brut, ou saut consécutif ≥ 1,0 pH / 300 mV = transitoire rejeté par le firmware avant sa fenêtre médiane). Aucune interpolation par-dessus. La note muted de la carte le rappelle : *« Un trou dans la courbe de dispersion signale une fenêtre écartée (transitoire rejeté), pas une absence de mesure. »* Moins de 13 points de trace (ou aucune fenêtre valide) → **bloc masqué**, pas d'erreur — cohérent avec le tiret des stats.

**Source du seuil :** `dispersion_max` de [`GET /debug/sensor_filter_state`](../API.md#get-debugsensor_filter_state--pas-dauth-cohérent-avec-autres-debug) si la route a répondu (mémorisé puis ré-appliqué au graphe déjà tracé : légende + échelle Y + ligne pointillée), sinon **repli JS** (constantes commentées `kPhDispersionSeuilFallback` / `kOrpDispersionSeuilFallback` = `kPhFilterMaxDispersion` / `kOrpFilterMaxDispersion` de [`constants.h`](../../src/constants.h)). Les deux fetchs `loadFilterState()` / `loadSensorTrace()` restent **indépendants** (invariant 071) : la courbe s'affiche même si la route échoue, avec le seuil de repli.

**Statut « indicatif » :** hérité de la reconstruction 071 (fenêtre firmware gelée circulation arrêtée, critère de rejet approché) — porté par la légende « **Dispersion pH (indicatif)** » / « **Dispersion ORP (indicatif)** ». Hors régime établi, la valeur instantanée fait foi.

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
| Restaurer les valeurs d'usine de régulation (feature-088, v2.31.0 — corps vide, dialogue avant/après) | `POST /reset-regulation-defaults` | CRITICAL |
| Signaler l'état d'une filtration externe (mode `external`) | `POST /filtration/external-state?running=…` | WRITE |
| Changer mot de passe admin | `POST /auth/change-password` (body `{current, new}`) | CRITICAL |
| Scanner Wi-Fi | `POST /wifi/scan` | WRITE |
| Déconnecter Wi-Fi | `POST /wifi/disconnect` | CRITICAL |
| Vérifier une update | `GET /check-update` | READ |
| Installer une update GitHub | `POST /download-update` | CRITICAL |
| Update manuelle (fichier .bin) | `POST /update` (multipart) | CRITICAL |
| Test pompe 1/2 | `POST /pump1/on` / `/pump2/on` | WRITE |
| Arrêt test pompe | `POST /pump1/off` / `/pump2/off` | WRITE |
| Assistant débit : lancer purge/mesure minutée | `POST /flow-assist/start?pump=…&phase=…` | WRITE |
| Assistant débit : arrêt anticipé (session conservée) | `POST /flow-assist/stop` | WRITE |
| Assistant débit : sortie (dosage auto rétabli) | `POST /flow-assist/exit` | WRITE |
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
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config`, `/reset-regulation-defaults` (feature-088), `/reboot`, `/reboot-ap`, `/factory-reset`
- [`src/web_routes_auth.cpp`](../../src/) / [`src/auth_manager.cpp`](../../src/) — endpoints `/auth/*`
- [`src/web_routes_wifi.cpp`](../../src/) — `/wifi/scan`, `/wifi/status`, `/wifi/disconnect`, `/wifi/ap/disable`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — `/pump1/*`, `/pump2/*`, `/flow-assist/*` (feature-075)
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
