## Métadonnées
- **ID** : feature-005
- **Slug** : page-orp
- **Type** : feature
- **Statut** : doing — itération 3 en préparation
- **Priorité** : normale
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-04-24
- **Spec(s) liée(s)** : feature-003 (page pH) — symétrie à préserver entre les deux écrans

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification (endpoints d'action protégés)
- [x] Historique / persistance (`/api/history/orp`)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — calibration + dosage manuel + mode Programmée
- [x] ux-ui-designer — design du bloc Statistiques compact, arbitrage ergonomie
- [x] embedded-developer — endpoints firmware, WS, pump_controller
- [x] web-ui-developer — vue `/orp`, graphique, calibration guidée
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Type
feature

## Description
Page dédiée **ORP** (`/orp`) regroupant la mesure, la régulation (automatique, programmée, injection manuelle), l'historique des mesures et la calibration de la sonde ORP. C'est le point d'entrée utilisateur pour tout ce qui concerne la mesure et le pilotage du potentiel redox de la piscine (désinfection par chlore liquide).

## Contexte
L'ORP (potentiel redox) est l'un des deux paramètres chimiques régulés par le contrôleur (avec le pH). Une sonde ORP mesure la valeur en continu en mV, une pompe doseuse corrige le taux de chlore en injectant du chlore liquide selon la configuration. La sonde nécessite une calibration régulière (recommandée tous les 3 mois) sans laquelle le dosage devient imprécis et potentiellement dangereux (sous- ou surdosage de chlore). L'utilisateur a besoin d'un seul écran pour :
- voir la valeur actuelle d'ORP mesurée, où il en est de la quantité de chlore injecté dans la journée par rapport à la limite quotidienne de sécurité, et l'état de calibration de la sonde
- choisir le mode de régulation : automatique, programmée ou injection manuelle
- lancer une injection manuelle ponctuelle
- consulter l'historique des mesures
- recalibrer la sonde selon un protocole guidé (1 point ou 2 points), sans quitter la page

## Comportement attendu

### Architecture de la page
La vue `/orp` est accessible via le menu principal. En mode nominal, elle affiche dans cet ordre :

1. **Bloc Statistiques** — mesure ORP courante et dosage du jour. **Présenté sous forme de bandeau compact sans titre** (pas nécessairement un composant `.card` comme les autres sections) : il s'agit d'un affichage d'informations en lecture, pas d'une zone d'action configurable. Le design exact (bandeau horizontal, résumé en pleine largeur, grille d'indicateurs, etc.) est décidé par l'agent `ux-ui-designer` et doit rester cohérent avec celui retenu pour la page `/ph`.
2. **Carte Régulation ORP** — modes de régulation (automatique, programmée, manuelle). Le **mode Automatique** contient également les informations de calibration de la sonde (date, bandeau « Calibré », valeurs de référence) et le bouton **Calibrer** pour lancer l'assistant : la calibration n'est pertinente que pour une régulation asservie à la valeur mesurée de l'ORP, donc indissociable du mode Automatique.
3. **Carte Historique** — graphique des mesures ORP

Lorsque l'utilisateur clique sur le bouton **Calibrer** (disponible uniquement dans le sous-bloc Automatique de la carte Régulation), les cartes **Régulation** et **Historique** sont masquées et remplacées par une carte **Calibration** présentant l'assistant guidé (1 point ou 2 points). Le bloc **Statistiques** reste visible en permanence.

À la fin du protocole de calibration (succès complet) **ou** si l'utilisateur annule la calibration en cours, la carte Calibration disparaît et les cartes Régulation et Historique réapparaissent dans leur état antérieur.

Toutes les valeurs temps réel (mesure ORP courante, dosage du jour, état d'injection, statut de calibration) proviennent du flux WebSocket déjà ouvert par l'application. **Aucun polling REST dédié à cette page.** Les actions utilisateur (sauvegarde de config, démarrage/arrêt d'injection, étapes de calibration) passent par des appels HTTP authentifiés ciblés.

---

### 1. Bloc « Statistiques »
Toujours visible, y compris pendant une calibration active.

**Forme visuelle attendue**
- **Pas de titre** (pas de `card__title`, pas de libellé « Statistiques »)
- **Compact**, en lecture seule, pas de composant `.card` classique avec ombrage et en-tête : privilégier un bandeau/bloc d'informations léger
- Densité maximale d'information utile sans chrome inutile (pas de padding vertical excessif, pas de sous-titres explicatifs)
- Design à arrêter par l'agent `ux-ui-designer`, **aligné avec le bloc Statistiques de `/ph`** pour préserver la symétrie entre les deux pages

**Valeur ORP courante**
- Label :
  - en mode nominal : « Valeur ORP actuelle »
  - pendant une calibration active : « Tension brute capteur »
- Valeur :
  - en mode nominal : ORP en mV entier (ex. `642`), ou `--` si indisponible
  - pendant une calibration active : tension brute du capteur en mV avec 1 décimale, ou `--` si indisponible
- Mention « MAJ auto toutes les 5 secondes » (cadence du push WebSocket de mesure)

**Bloc « Dosage du jour »**
- Affiche le cumul de chlore injecté depuis minuit local sous la forme `X / Y mL` où Y est la limite journalière maximale (`max_orp_ml_per_day`)
- Barre de progression reflétant le ratio avec trois paliers de couleur :
  - vert jusqu'à 75 % de la limite
  - orange entre 75 % et 90 %
  - rouge au-delà de 90 % ou quand la limite est déclarée atteinte par le firmware
- Si la limite n'est pas configurée (0 ou non défini), affiche seulement la valeur injectée et le texte « Limite non configurée »
- Si aucune donnée n'est encore reçue, affiche `— mL` et une barre vide
- Quand la limite est atteinte côté firmware, un statut explicite s'affiche : « Limite atteinte — dosage suspendu jusqu'à minuit »
- Accessibilité : la barre utilise `role="progressbar"` avec `aria-valuenow`, `aria-valuemin`, `aria-valuemax`

> Le bloc Statistiques ne contient **pas** les informations de calibration ni le bouton Calibrer : ces éléments sont affichés dans la carte Régulation ORP (sous-bloc Automatique uniquement) car ils ne sont pertinents qu'en régulation asservie.

---

### 2. Carte « Régulation ORP »
Visible en mode nominal, masquée pendant une calibration active.

**Sélecteur de mode** (onglets ou groupe de boutons)
- **Automatique** — le firmware régule l'ORP en permanence vers la cible configurée
- **Programmée** — le firmware injecte un volume quotidien de chlore liquide défini par l'utilisateur, réparti sur la journée
- **Manuelle** — aucune régulation automatique ; seule l'injection manuelle ponctuelle est disponible

Le mode sélectionné est sauvegardé immédiatement (pas de bouton Sauvegarder pour le choix du mode lui-même). Les sous-blocs associés au mode actif sont seuls visibles.

**Mode Automatique**
- **ORP cible (mV)** : valeur numérique entre 0 et 1000, pas de 1 mV, valeur par défaut 650
- **Bouton Sauvegarder** :
  - désactivé tant qu'aucun champ n'a été modifié (état propre)
  - actif dès qu'un champ a changé
  - affiche un spinner pendant la sauvegarde
  - bascule en état « Sauvegarde réussie » (vert) pendant 2 s après succès, puis revient à l'état propre
  - bascule en état « Erreur » (rouge) en cas d'échec, et redevient cliquable après 2 s pour réessayer

**Informations de calibration** (visibles **uniquement** dans le mode Automatique)
- Date de dernière calibration au format local, ou `—` si jamais calibrée
- Bandeau « Calibré » avec ✓ si la sonde est actuellement calibrée (état reçu via WebSocket), masqué tant qu'une calibration est en cours
- Valeurs de référence retenues à la dernière calibration (offset seul pour une calibration 1 point, offset + slope pour une calibration 2 points)
- **Bouton « Calibrer »** : déclenche l'affichage de la carte Calibration en remplacement des cartes Régulation et Historique (voir §4). Désactivé si une calibration est déjà en cours ou si une injection ORP est en cours. Placement et rendu visuel arrêtés par `ux-ui-designer` au sein du sous-bloc Automatique, de façon cohérente avec `/ph`.
- En modes Programmée et Manuelle, ces informations et ce bouton ne sont pas affichés. Pour calibrer, l'utilisateur bascule d'abord en mode Automatique.

**Mode Programmée**
- L'utilisateur définit un **volume quotidien de chlore liquide à injecter**, exprimé en mL
  - Input numérique, plage raisonnable (ex. 0 à la limite journalière configurée), pas de 10 mL, valeur par défaut 0
  - La saisie est bornée côté UI par la limite journalière de sécurité (`max_orp_ml_per_day`) : toute valeur supérieure est refusée avec un message explicite
  - Un **hint** rappelle la limite journalière en vigueur (« limite de sécurité : Y mL »)
- Le firmware injecte ce volume en continu pendant les plages de filtration, dans la limite de la durée max d'injection par heure (`orp_limit_minutes`) ; il s'arrête pour la journée dès que le quota est atteint et reprend à la fenêtre horaire suivante si la limite horaire a été atteinte entre-temps
- Le volume consommé dans la journée est déjà remonté par le bloc « Dosage du jour » du bloc Statistiques : aucun indicateur supplémentaire nécessaire dans la carte Régulation
- **Bouton Sauvegarder** : même comportement que le mode Automatique (état propre / modifié / spinner / succès 2 s / erreur 2 s)
- Si le volume quotidien est à `0`, aucune injection programmée n'est déclenchée par le firmware ; l'injection manuelle reste disponible

**Injection manuelle** (visible **uniquement** lorsque le mode Manuelle est sélectionné)
- Ce sous-bloc remplace intégralement les paramètres des autres modes quand Manuelle est actif : il n'y a ni ORP cible, ni volume quotidien à saisir dans ce mode, seulement l'injection ponctuelle
- Dans les modes Automatique et Programmée, le sous-bloc Injection manuelle n'est pas affiché
- Texte explicatif : « Démarre la pompe ORP à la puissance configurée. Utile pour corriger manuellement le taux de chlore ou vérifier le fonctionnement de la pompe. Cliquez à nouveau sur le bouton pour arrêter l'injection avant la fin. »
- **Volume (mL)** : input numérique entre 1 et 2000, pas de 5, valeur par défaut 50
- **Hint dynamique** : durée estimée de l'injection en fonction du volume saisi et du débit courant de la pompe
- **Bouton « ▶ Injecter »** :
  - au clic, déclenche une injection pour le volume saisi (`POST /orp/inject/start`)
  - pendant le démarrage : feedback transitoire (« ⏳ Démarrage — ≈ X » avec la durée estimée)
  - pendant l'injection : le bouton reflète l'état via les messages WebSocket (temps restant, pourcentage)
  - un nouveau clic pendant l'injection envoie un ordre d'arrêt (`POST /orp/inject/stop`)
  - l'input volume est désactivé pendant le démarrage et l'injection
  - en cas d'échec du démarrage, le bouton et l'input reviennent immédiatement à l'état initial

**Contraintes de sécurité (héritées du firmware)**
- L'injection manuelle est **soumise aux mêmes limites** que la régulation automatique (cumul journalier et horaire)
- Une injection manuelle peut être refusée par le firmware si la filtration n'est pas active depuis assez longtemps, si le capteur est stale, ou si une limite est atteinte — dans ce cas, le bouton repasse à l'état initial et, si possible, un message d'erreur est remonté

---

### 3. Carte « Historique »
Visible en mode nominal, masquée pendant une calibration active.

**Graphique**
- Graphique ligne Chart.js des mesures ORP dans le temps
- Zone acceptable visuelle (ex. 600–750 mV, bandeau ombré) indiquant la plage de désinfection cible pour une piscine chlorée
- La cible configurée (`orp_target`) est matérialisée sur le graphique

**Sélecteurs de plage**
- Quatre boutons : `Tout`, `30j`, `7j`, `24h`
- Le bouton actif est visuellement distinct
- Le changement de plage recharge le graphique avec la granularité appropriée (échantillonnage brut ou agrégé selon la plage)
- Par défaut, la plage est `Tout` à l'arrivée sur la page ; la plage sélectionnée est conservée entre les visites pendant la durée de la session

---

### 4. Carte « Calibration de la sonde ORP »
**Visible uniquement lorsqu'une calibration est en cours**, à la place des cartes Régulation et Historique.

**En-tête**
- Titre : « Calibration de la sonde ORP »
- Sous-titre : date de la dernière calibration au format local, ou `—` si jamais calibrée

**Texte explicatif**
- Rappelle que la calibration est nécessaire pour des mesures précises
- Prévient du risque de dosage de chlore incorrect (sous- ou surdosage) si la sonde n'est pas calibrée
- Recommande une recalibration tous les 3 mois avec une solution d'étalonnage ORP (ex. 470 mV pour une calibration 1 point, ou 470 mV + 650 mV pour une calibration 2 points)

**Choix du type de calibration**
- Select : « 1 point (offset uniquement) » ou « 2 points (offset + slope) »
- Le protocole affiché sous le select change selon la valeur choisie
- Le type par défaut proposé est « 1 point » (rapide et suffisant pour un entretien courant)
- Le choix est local à la session de calibration ; il n'est pas persisté en config

**Protocole guidé — 1 point (4 étapes)**
Les étapes sont affichées sous forme de liste numérotée avec trois états visuels : neutre, active (en cours), completed (validée).

1. Plonger la sonde dans la solution d'étalonnage ORP
2. Attendre que la tension brute du capteur se stabilise (environ 1 min)
3. Entrer la valeur de référence de la solution d'étalonnage ORP (input mV)
4. Calibrer (mémorisation de l'offset)

**Protocole guidé — 2 points (8 étapes)**
Les étapes sont affichées sous forme de liste numérotée avec les mêmes états visuels.

1. Plonger la sonde dans la solution 1 d'étalonnage ORP
2. Attendre que la tension brute du capteur se stabilise (environ 1 min)
3. Entrer la valeur de référence de la solution 1 d'étalonnage ORP (input mV)
4. Mémoriser le point 1
5. Plonger la sonde dans la solution 2 d'étalonnage ORP
6. Attendre que la tension brute du capteur se stabilise (environ 1 min)
7. Entrer la valeur de référence de la solution 2 d'étalonnage ORP (input mV)
8. Calibrer (mémorisation de l'offset et de la slope)

La valeur temps réel affichée pendant la calibration (tension brute du capteur) est celle du bloc Statistiques ; la carte Calibration ne duplique pas la lecture.

**Boutons**
- **Commencer la calibration** (ou libellé équivalent adapté à l'étape) : libellé dynamique selon l'étape courante
  - 1 point, étape 1/2 : « Étape suivante »
  - 1 point, étape 3 : « Étape suivante » (après saisie de la valeur de référence)
  - 1 point, étape 4 : « Calibrer » (déclenche la persistance de l'offset et de la date de calibration)
  - 2 points, étapes 1-3 : « Étape suivante »
  - 2 points, étape 4 : « Mémoriser le point 1 »
  - 2 points, étapes 5-7 : « Étape suivante »
  - 2 points, étape 8 : « Calibrer » (déclenche la persistance de l'offset, de la slope et de la date)
- **Annuler** : visible dès qu'une calibration est active, demande confirmation avant d'annuler

**Particularité du protocole ORP**
- Contrairement à la calibration pH qui s'appuie sur les commandes firmware Atlas EZO (`/calibrate_ph_neutral`, `/calibrate_ph_acid`), la calibration ORP est **calculée côté client** à partir de la tension brute courante et des valeurs de référence saisies. Les résultats (`orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_date`, `orp_calibration_reference`) sont persistés via `POST /save-config`. Ce fonctionnement existe déjà et doit être conservé.
- Il n'y a donc **pas de nouveaux endpoints firmware** pour la calibration ORP.

**États de transition et sortie de la carte**
- Au démarrage d'une calibration, les cartes Régulation + Historique sont cachées (le bandeau « Calibré », affiché dans le sous-bloc Automatique, est donc retiré de la vue en même temps que la carte Régulation)
- Pendant un calcul de calibration, le bouton est désactivé (évite le double-clic)
- En cas d'échec (tension brute invalide, pas de mesure récente, échec `/save-config`), une alerte explicite est affichée dans la carte Calibration et le protocole est réinitialisé à l'état idle ; l'utilisateur peut réessayer ou annuler pour sortir
- À la fin réussie de la dernière étape (étape 4 en 1 point, étape 8 en 2 points) : toutes les étapes sont marquées comme complétées, la carte Calibration disparaît et les cartes Régulation + Historique réapparaissent ; le sous-bloc Automatique de la carte Régulation met à jour son bandeau « Calibré » et sa date avec la nouvelle valeur
- Sur annulation confirmée : la carte Calibration disparaît, les cartes Régulation + Historique réapparaissent, et l'état de calibration précédent est conservé (si la sonde était calibrée auparavant, le bandeau précédent est restauré)

---

## Cas limites transverses à la page
- **WebSocket déconnecté** : les valeurs temps réel (ORP courant, dosage du jour, état d'injection) affichent `--` ou l'état vide ; les sauvegardes de config et actions de calibration échouent avec un feedback erreur explicite jusqu'à reconnexion
- **Capteur ORP absent ou en erreur** : la valeur affiche `--`, un badge visuel signale l'anomalie sur la page dashboard (hors périmètre de cette spec mais interconnecté)
- **Sonde non calibrée** : un bandeau d'alerte visuel peut être affiché ailleurs dans l'UI ; la page `/orp` autorise malgré tout l'ouverture de toutes les sections, la calibration étant précisément l'endroit où on résout ce problème
- **Régulation inhibée** (mode piloté, filtration arrêtée, stabilisation en cours) : le bloc Dosage du jour reste fonctionnel, l'injection manuelle peut être refusée ; un délai de stabilisation restant peut être affiché via le champ WebSocket `stabilization_remaining_s` (visibilité à préciser selon évolution UI)
- **Limite horaire atteinte** (cumul sur la dernière heure) : distincte de la limite journalière, gérée côté firmware ; pas d'affichage dédié dans cette version
- **Tentative de calibration pendant une injection en cours** : la calibration n'est pas autorisée ; le bouton Calibrer affiche un message explicatif et reste inactif tant que l'injection n'est pas terminée
- **Tension brute capteur invalide** (NaN, `--`, valeur figée depuis > 30 s) : le bouton « Calibrer » de l'étape finale est désactivé ; l'utilisateur doit attendre une mesure fraîche

## Comportement actuel
La page `/orp` existe et présente actuellement **quatre cartes toujours visibles** :
1. Régulation ORP (toggle automatique binaire `orp_enabled` + bloc « Dosage du jour » + ORP cible + bouton Sauvegarder)
2. Injection manuelle (volume + bouton Injecter, carte autonome)
3. Historique (graphique + sélecteurs de plage)
4. Calibration de la sonde ORP (bandeau « Calibré », lecture temps réel, select 1 point / 2 points, protocole 4 ou 8 étapes, boutons Commencer / Annuler)

La valeur ORP courante, la date de dernière calibration et le bandeau « Calibré » sont aujourd'hui affichés dans la carte Calibration et non dans un bloc Statistiques dédié. Il n'existe **pas** de mode « Programmée » : la régulation automatique est pilotée par un toggle binaire `orp_enabled` on/off. Il n'existe **pas** non plus de champ `orp_daily_target_ml`. L'injection manuelle est une carte autonome, distincte de la carte de régulation.

La refonte consiste à :
- créer le **bloc Statistiques compact** qui consolide la lecture ORP courante et le dosage du jour
- fusionner la carte Régulation et la carte Injection manuelle dans une seule carte **Régulation ORP** avec un sélecteur de mode (automatique / programmée / manuelle)
- déplacer les infos de calibration (date, bandeau « Calibré ») et le bouton **Calibrer** dans le sous-bloc Automatique de la carte Régulation, et les masquer en modes Programmée et Manuelle
- rendre l'injection manuelle visible uniquement en mode Manuelle
- masquer les cartes Régulation et Historique pendant la calibration, la carte **Calibration** les remplaçant ; restauration automatique en fin ou annulation
- introduire le mode **Programmée** (nouveau : `orp_regulation_mode` enum + `orp_daily_target_ml`)

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/`, pas de framework, pas de bundler
- Français dans toute l'UI
- Chart.js pour les graphes
- Budget FS `spiffs` 1088 KB — vérifier avec `./build_fs.sh`
- WebSocket pour toute donnée temps réel ; HTTP authentifié pour les actions utilisateur uniquement
- AsyncWebServer : aucun handler bloquant > 50 ms côté firmware
- La transition carte Calibration ↔ cartes Régulation + Historique s'effectue uniquement côté frontend (CSS `is-active` / display toggle), sans rechargement de la vue SPA
- Ne pas casser l'auto-discovery Home Assistant existante si `orp_enabled` est transformé en `select` de mode

## Fichiers potentiellement concernés
- `data/index.html` — section `#view-orp` : création du bloc Statistiques, consolidation Régulation + Injection manuelle, ajout du conteneur Calibration conditionnel, suppression du titre de carte pour le bloc Statistiques
- `data/app.js` — logique d'ouverture / fermeture de la calibration (show/hide des cartes), sélecteur de mode de régulation, déplacement de la lecture ORP courante vers le bloc Statistiques, mise à jour de `orp_regulation_mode` et `orp_daily_target_ml`
- `data/app.css` — styles du bloc Statistiques compact (symétrie avec `/ph`), transitions d'affichage
- `src/config.h` / `src/config.cpp` — ajout `orpRegulationMode` + `orpDailyTargetMl`, migration NVS `orp_enabled` → `orp_regulation_mode`
- `src/web_routes_config.cpp` — exposition et validation des nouveaux champs ; rejet `orp_daily_target_ml > max_orp_ml_per_day`
- `src/web_routes_control.cpp` — endpoints d'injection ORP (`/orp/inject/start`, `/orp/inject/stop`, existants, inchangés)
- `src/ws_manager.cpp` — diffusion des champs `orp`, `orp_raw`, `orp_voltage_mv`, `orp_daily_ml`, `orp_limit_reached`, `orp_inject_remaining_s`, `orp_dosing`, `orp_target`, `orp_enabled` (maintenu en miroir), `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid`, `orp_calibration_date`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_reference`
- `src/sensors.*` — lecture de la sonde ORP (existant, inchangé)
- `src/pump_controller.*` — pilotage de la pompe doseuse ORP + branche « mode programmée » pour ORP (nouveau)
- `src/mqtt_manager.*` — ajout des topics état `orp_regulation_mode`, `orp_daily_target_ml` et décision auto-discovery HA (maintien du switch `orp_enabled` en miroir OU remplacement par `select` + `number`, à trancher par `pool-chemistry` et `web-ui-developer`)

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /orp/inject/start` — démarrage injection manuelle (auth requise, existant)
- `POST /orp/inject/stop` — arrêt injection en cours (auth requise, existant)
- `POST /save-config` — persistance `orp_enabled` (miroir), `orp_target`, `orp_regulation_mode`, `orp_daily_target_ml`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_date`, `orp_calibration_reference` (auth requise)
- `GET /api/history/orp` — historique graphique (plages Tout / 30j / 7j / 24h)

### Champs WebSocket consommés
- Mesure : `orp`, `orp_raw`, `orp_voltage_mv`
- Dosage : `orp_daily_ml`, `orp_limit_reached`, `orp_inject_remaining_s`, `orp_dosing`
- Config : `orp_target`, `orp_enabled` (miroir), `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`
- Calibration : `orp_cal_valid`, `orp_calibration_date`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_reference`
- Inhibition : `stabilization_remaining_s`

### Topics MQTT
- Potentiellement `pool/orp/regulation_mode/state` (string) et `pool/orp/daily_target_ml/state` (number) — auto-discovery HA à préciser selon décision produit

### Documentation à produire / mettre à jour
- `docs/API.md` — endpoints d'injection et nouveaux champs config ORP
- `docs/MQTT.md` — nouveaux topics état si ajoutés, choix d'auto-discovery
- `docs/UPDATE_GUIDE.md` — migration `orp_enabled` → `orp_regulation_mode`
- `README.md` — mention de la nouvelle structure de la page `/orp`
- `CHANGELOG.md` — entrée sous la version cible décrivant la refonte

## Sécurité chimique
- Dosage impliqué : oui — produit : **chlore liquide** (hypochlorite de sodium)
- Limites journalières / horaires concernées : `max_orp_ml_per_day`, limite horaire glissante (existantes côté firmware)
- Filtration requise avant action : oui (même garde que pour le pH)
- Calibration capteur modifiée : oui (uniquement le flux UI ; le calcul offset/slope côté client est inchangé)
- Algorithme PID touché : la branche `automatic` du PID ORP est préservée ; ajout d'une nouvelle branche `scheduled` qui doit respecter les mêmes gardes (filtration active, limites journalière/horaire, capteur non stale)
- Risque identifié : un mauvais dosage de chlore liquide peut créer des niveaux toxiques ou inefficaces ; toute régression sur les limites de sécurité est inacceptable. L'ajout du mode Programmée pour l'ORP reproduit le même cadre de sécurité que celui validé pour le pH dans la feature-003 — `pool-chemistry` doit valider que les gardes sont identiques et que la répartition du volume dans la journée est safe côté chlore.

## Critères d'acceptation
- [ ] La vue `/orp` affiche en mode nominal les trois cartes **Statistiques**, **Régulation ORP**, **Historique** dans cet ordre
- [ ] Le bloc Statistiques contient uniquement la valeur ORP courante et le bloc « Dosage du jour » (les informations de calibration et le bouton Calibrer sont dans la carte Régulation, sous-bloc Automatique)
- [ ] Le bloc Statistiques est compact, sans titre, non présenté comme une `.card` classique — la forme exacte est validée par le rapport de l'agent `ux-ui-designer` et reste cohérente avec le bloc Statistiques de `/ph`
- [ ] Le bloc « Dosage du jour » affiche `X / Y mL` et une barre colorée selon les paliers 75 % / 90 %, alimentée en temps réel via le WebSocket sans polling
- [ ] La carte Régulation ORP présente un sélecteur de mode (automatique / programmée / manuelle) dont la sélection est sauvegardée immédiatement
- [ ] En mode Automatique, l'ORP cible est persistant et relu à l'arrivée sur la page ; les informations de calibration (date, bandeau « Calibré », valeurs de référence) et le bouton **Calibrer** sont affichés dans ce sous-bloc uniquement
- [ ] En mode Programmée, un input volume quotidien (mL) est proposé, borné par la limite journalière de sécurité (`max_orp_ml_per_day`), persistant entre les visites ; la valeur 0 désactive l'injection programmée
- [ ] L'injection manuelle est visible **uniquement lorsque le mode Manuelle est actif** ; le sous-bloc démarre au clic, reflète sa durée estimée, affiche le temps restant et peut être interrompue par un second clic
- [ ] L'injection manuelle est refusée par le firmware dans les mêmes conditions que la régulation automatique (limite atteinte, filtration OFF, capteur stale)
- [ ] Le graphique d'historique affiche les quatre plages (Tout / 30j / 7j / 24h) avec un bouton actif
- [ ] Un clic sur **Calibrer** (disponible uniquement dans le sous-bloc Automatique de la carte Régulation) masque les cartes Régulation et Historique et affiche la carte Calibration à leur place ; le bloc Statistiques reste visible
- [ ] La calibration guidée 1 point enchaîne les 4 étapes avec les bons libellés, persiste `orp_calibration_offset` + `orp_calibration_date` + `orp_calibration_reference` via `/save-config`, affiche une erreur en cas d'échec
- [ ] La calibration guidée 2 points enchaîne les 8 étapes avec les bons libellés, persiste `orp_calibration_offset` + `orp_calibration_slope` + `orp_calibration_date` + `orp_calibration_reference` via `/save-config`, affiche une erreur en cas d'échec
- [ ] À la fin réussie du protocole, la carte Calibration disparaît automatiquement et les cartes Régulation + Historique réapparaissent ; la date et le bandeau « Calibré » dans le sous-bloc Automatique sont mis à jour
- [ ] Annuler une calibration en cours (après confirmation) restaure l'état précédent sans perte du dernier étalonnage valide et ramène les cartes Régulation + Historique
- [ ] La migration `orp_enabled` (booléen) → `orp_regulation_mode` (enum) est silencieuse au premier boot : `orp_enabled=true` → `automatic`, `orp_enabled=false` → `manual`, `orp_daily_target_ml` initialisé à 0
- [ ] Auto-discovery Home Assistant cohérente avec le nouveau modèle de config (switch `orp_enabled` maintenu en miroir OU remplacé par `select` + `number`, à trancher par `pool-chemistry` + `web-ui-developer`)
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition `spiffs` (1088 KB)

## Plan de test manuel
1. Ouvrir `/orp` → les trois cartes Statistiques / Régulation / Historique apparaissent dans l'ordre attendu
2. Vérifier le bloc Statistiques : valeur ORP courante + bloc Dosage du jour uniquement (pas d'infos de calibration, pas de bouton Calibrer). Passer la carte Régulation en mode Automatique → vérifier que la date de dernière calibration, le bandeau « Calibré » (si applicable) et le bouton **Calibrer** apparaissent dans ce sous-bloc. Passer en mode Programmée puis Manuelle → ces éléments disparaissent.
3. Faire évoluer le cumul au-delà de 75 % / 90 % de la limite → les couleurs de la barre changent (vert → orange → rouge)
4. Changer le mode de régulation (Automatique → Manuelle → Automatique) → sauvegarde immédiate, affichage des sous-blocs cohérent ; vérifier que `orp_enabled` est maintenu en miroir (true si mode ≠ `manual`) pour compatibilité HA
5. En mode Automatique, modifier l'ORP cible → bouton Sauvegarder actif → succès vert 2 s, puis état propre
5b. En mode Programmée, saisir un volume quotidien inférieur à la limite journalière → sauvegarde réussie ; saisir une valeur supérieure → refus avec message explicite ; saisir `0` → l'injection programmée est désactivée
6. Lancer une injection manuelle 50 mL → feedback « Démarrage ≈ X » puis temps restant → stopper avant la fin
7. En mode Automatique, cliquer sur **Calibrer** (dans le sous-bloc de la carte Régulation) → les cartes Régulation et Historique disparaissent, la carte Calibration apparaît ; le bloc Statistiques reste visible ; le label du readout devient « Tension brute capteur ».
8. Choisir **1 point** → enchaîner les 4 étapes → à la fin, la carte Calibration disparaît, Régulation + Historique réapparaissent, le bandeau « Calibré » et la date dans le sous-bloc Automatique sont mis à jour
9. Relancer Calibrer → choisir **2 points** → enchaîner les 8 étapes (vérifier le hint « Point 1 mémorisé » après l'étape 4) → à la fin, la carte Calibration disparaît, Régulation + Historique réapparaissent, la date et les références sont mises à jour
10. Relancer Calibrer, annuler à mi-protocole → après confirmation, la carte Calibration disparaît, Régulation + Historique réapparaissent, le bandeau précédent est restauré
11. Couper le WiFi (ou l'ESP) → les valeurs passent à `--`, badge WS déconnecté ; reconnecter → les valeurs reprennent
12. Vérifier sur Home Assistant que l'intégration (switch ou select + number, selon décision) reflète bien l'état courant après chaque changement de mode

## Notes pour agent
- Cette spec **est la jumelle de `feature-003-page-ph`**. Le design du bloc Statistiques, le sélecteur de modes et le pattern « calibration accessible uniquement en Automatique » DOIVENT rester visuellement et fonctionnellement cohérents entre les deux pages. Les choix tranchés par `ux-ui-designer` pour `/ph` sont à reprendre pour `/orp` sans ré-arbitrage, sauf justification explicite liée aux spécificités ORP (unité mV, protocole 1pt/2pt).
- La calibration ORP **n'utilise pas d'endpoints firmware dédiés** : le calcul offset/slope est côté client, et les résultats sont persistés via `POST /save-config`. Ne pas introduire de nouveaux endpoints `/calibrate_orp_*` sauf décision explicite de l'architecte ET validation `pool-chemistry`.
- Toute évolution ultérieure de la page `/orp` doit faire l'objet d'une **nouvelle itération** dans la section « Suivi / itérations » de cette spec. La spec reste le référentiel unique pour l'écran `/orp`.
- Avant de modifier un comportement ici, vérifier qu'on ne casse pas un autre écran : la carte compacte ORP du dashboard, les badges globaux de sensors, les topics MQTT de régulation ORP et l'auto-discovery Home Assistant consomment les mêmes données sous-jacentes.
- La calibration touche directement la sécurité chimique (une mauvaise calibration entraîne un dosage de chlore incorrect, potentiellement toxique ou insuffisant) : toute modification du protocole de calibration ou de la logique de validation → passage obligatoire par l'agent `pool-chemistry`.
- Le mode **Programmée** consiste à choisir un volume quotidien en mL à injecter ; sa répartition effective dans la journée est à la charge du firmware et sort du périmètre de cette spec UI.
- Le volume programmé doit être **systématiquement borné par la limite journalière de sécurité** (`max_orp_ml_per_day`) côté UI **et** côté firmware ; la validation côté firmware est la source de vérité.

## Hors périmètre
- Algorithme de répartition du volume quotidien programmé sur la journée (cadence, nombre d'injections, interaction avec la filtration) — logique firmware à définir conjointement avec la branche scheduled pH livrée en feature-003 itération 2, si possible via une abstraction commune
- Affichage d'un éventuel délai de stabilisation restant (`stabilization_remaining_s`)
- Gestion de la limite horaire côté UI
- Modification du protocole de calibration 1 point / 2 points (les étapes actuelles sont conservées, seule leur présentation change)

---

## Suivi / itérations

### Itération 3 — 2026-04-24 — Couverture stale data + toast reboot inattendu (via feature-003 itération 6)

- Commit(s) : —
- Statut : livré — couvert par feature-003 itération 6
- Contexte :
  - L'indicateur visuel de stale data (grisage du bloc Statistiques lors d'une déconnexion WS) et le toast de reboot inattendu sont implémentés dans feature-003 itération 6 via des fonctions JS partagées (`updateDailyDoseDisplay()`, `_onDisconnect()`).
  - Ces changements couvrent la page `/orp` automatiquement via `#orp-card-stats` — aucun code dédié n'est nécessaire ici.
- Écarts par rapport à l'existant :
  - Aucun fichier modifié spécifiquement pour cette itération.
  - L'implémentation est portée intégralement par feature-003 itération 6.
- Agents requis :
  - `test-engineer` — vérification manuelle sur `/orp`
- Critères d'acceptation :
  - [ ] Sur la page `/orp`, le bloc Statistiques grise (opacity 0.5) lors d'une déconnexion WS
  - [ ] À la reconnexion, le bloc revient à opacity 1 à la réception du premier push sensor_data
  - [ ] Le toast de reboot inattendu s'affiche depuis `/orp` dans les mêmes conditions que depuis `/ph`
- Hors périmètre :
  - Tout code dédié (porté intégralement par feature-003 itération 6)

### Itération 2 — 2026-04-23 — mode Programmée + refonte complète de l'architecture des cartes
- Commit(s) : feature-003-page-ph (branche `feature-005-page-orp`)
- Statut : done — 2026-04-24
- Résumé :
  - Introduction du mode **Programmée** pour l'ORP : l'utilisateur saisit un **volume quotidien en mL** de chlore liquide à injecter, borné par la limite journalière de sécurité (`max_orp_ml_per_day`). Répartition à la charge du firmware.
  - **Refonte complète de l'architecture des cartes** de la page `/orp` pour passer de 4 cartes toujours visibles à la structure cible : Statistiques (compact, sans titre, pas une `.card`) → Régulation → Historique, avec Calibration conditionnelle qui remplace Régulation + Historique pendant le protocole.
  - Injection manuelle visible **uniquement en mode Manuelle**.
  - Infos de calibration + bouton **Calibrer** visibles **uniquement dans le sous-bloc Automatique** de la carte Régulation.
  - Symétrie visuelle et fonctionnelle avec la page `/ph` (feature-003 itération 2 + 3 livrée).

- Écarts par rapport à l'existant :

  **A. Config / firmware (mode Programmée)**
  - **Config** : aujourd'hui un booléen `orp_enabled` (toggle on/off) ; cible = un enum `orp_regulation_mode` à trois valeurs (`automatic` / `scheduled` / `manual`) **plus** un champ `orp_daily_target_ml` pour le volume quotidien programmé. → à ajouter dans `config.h` / `config.cpp`, dans la persistance NVS, et dans le schéma JSON de `POST /save-config`.
  - **Migration** : les installations existantes n'ont que `orp_enabled`. Règle de migration au premier boot : `orp_enabled=true` → `orp_regulation_mode=automatic`, `orp_enabled=false` → `orp_regulation_mode=manual`. `orp_daily_target_ml` initialisé à `0`. À documenter dans `CHANGELOG.md` et `UPDATE_GUIDE.md`.
  - **Régulation firmware** : `pump_controller` actuel pilote l'ORP uniquement en mode PID vers `orp_target` si `orpEnabled`. Ajouter la branche « mode programmée » qui injecte `orp_daily_target_ml` réparti sur 24 h, en respectant les limites journalière et horaire, en vérifiant la filtration active et la fraîcheur du capteur. → passage obligatoire par l'agent `pool-chemistry` avant implémentation.
  - **WebSocket** : diffuser les nouveaux champs `orp_regulation_mode` et `orp_daily_target_ml` dans `ws_manager.cpp` (et mirroring `orp_enabled`).
  - **Endpoint `POST /save-config`** : accepter les nouveaux champs, rejeter `orp_daily_target_ml > max_orp_ml_per_day` avec un message d'erreur exploitable côté UI.
  - **MQTT / Home Assistant** : vérifier si l'auto-discovery actuelle (switch `orp_enabled`) doit être remplacée par un `select` pour le mode et un `number` pour le volume quotidien ; sinon conserver `orp_enabled` comme miroir dérivé (`true` si mode ≠ `manual`) pour ne pas casser les automations utilisateur existantes. Décision à trancher par `pool-chemistry` + `web-ui-developer`, idéalement cohérente avec le choix déjà pris pour `ph_enabled`.

  **B. Frontend (refonte architecture des cartes)**
  - **Bloc Statistiques (nouveau, en tête de vue, compact, pas une `.card`, pas de titre)** — à créer dans `data/index.html` et `data/app.js` :
    - `readout` ORP courant avec label dynamique : « Valeur ORP actuelle » en nominal / « Tension brute capteur » pendant une calibration active ; valeur en mV entier ou mV 1 décimale (tension brute) selon l'état.
    - bloc « Dosage du jour » (déplacé depuis la carte Régulation, conserver la barre de progression colorée et `role="progressbar"`).
    - **Design validé par `ux-ui-designer`**, aligné avec le bloc Statistiques retenu pour `/ph` (itération 3 de feature-003 : grille 2 colonnes avec diviseur vertical, passage en colonne unique < 500 px).
    - **Pas** d'infos de calibration, **pas** de bouton Calibrer dans ce bloc.
  - **Carte Régulation ORP** (existante, à enrichir) :
    - remplacer le toggle binaire `orp_enabled` par un **sélecteur 3 modes** (Automatique / Programmée / Manuelle) avec affichage conditionnel des sous-blocs.
    - sous-bloc **Automatique** : ORP cible + bouton Sauvegarder + **infos de calibration** (date, bandeau « Calibré », valeurs de référence) + bouton **Calibrer** (via un `<hr>` ou séparateur visuel, dans un conteneur `#orp-calibration-info` masqué/affiché par la logique de mode).
    - sous-bloc **Programmée** : input volume quotidien (mL) + hint de limite + bouton Sauvegarder.
    - sous-bloc **Injection manuelle** : **fusionné ici** depuis la carte autonome actuelle ; visible **uniquement** en mode Manuelle (conteneur `#orp-inject-section` avec `display:none` par défaut). Contient : volume + hint durée estimée + bouton ▶ Injecter.
    - retirer le bloc « Dosage du jour » (il est désormais dans Statistiques).
  - **Carte Historique** (existante, inchangée) — graphique + sélecteurs de plage.
  - **Carte Calibration de la sonde ORP** (existante, à refondre en conteneur conditionnel) :
    - garder son contenu (texte explicatif, select 1 point / 2 points, protocoles 4 ou 8 étapes, boutons Commencer / Annuler par type).
    - **supprimer** son propre `readout` ORP courant, son bandeau « Calibré » interne et sa date (tout est dans Statistiques + sous-bloc Automatique Régulation).
    - la placer dans un conteneur masqué par défaut (`display:none`).
  - **Suppression** de la carte **Injection manuelle** autonome (son contenu est migré dans la carte Régulation).
  - **Logique JS (`data/app.js`)** :
    - clic sur **Calibrer** (carte Régulation, sous-bloc Automatique) → masque Régulation + Historique, affiche Calibration, bascule le label du readout vers « Tension brute capteur ».
    - fin réussie du protocole → met à jour la date + bandeau « Calibré » dans le sous-bloc Automatique de Régulation, masque Calibration, réaffiche Régulation + Historique, restaure le label « Valeur ORP actuelle ».
    - **Annuler** avec confirmation → idem sans mise à jour de la date.
    - refactoriser `updateDailyDoseDisplay('orp')` pour pointer vers les sélecteurs du nouveau bloc Statistiques.
    - `updateOrpModeControls()` : masque/affiche les sous-blocs Automatique / Programmée / Manuelle et le conteneur `#orp-calibration-info` + `#orp-inject-section` selon `orp_regulation_mode`.
    - relecture de `orp_regulation_mode` et `orp_daily_target_ml` depuis le flux WS à l'arrivée sur la page et à chaque mise à jour.
  - **CSS (`data/app.css`)** : règles pour le bloc Statistiques compact (à aligner avec `/ph`), transitions show/hide des cartes (pas de rechargement de vue SPA).

  **C. Documentation**
  - `docs/API.md` : ajout des champs `orp_regulation_mode`, `orp_daily_target_ml`, `orp_enabled` (miroir) dans les tableaux GET /get-config et POST /save-config.
  - `docs/MQTT.md` : ajout des topics état `orp_regulation_mode` / `orp_daily_target_ml` et décision d'auto-discovery.
  - `docs/UPDATE_GUIDE.md` : section migration `orp_enabled → orp_regulation_mode` ajoutée.
  - `CHANGELOG.md` : entrée [En cours] 2026-04-23 ajoutée.

- Critères d'acceptation spécifiques à cette itération (en plus des critères généraux de la spec) :
  - [ ] La vue `/orp` affiche en mode nominal, dans l'ordre : **Statistiques → Régulation ORP → Historique** (aucune carte Injection manuelle autonome, aucune carte Calibration visible).
  - [ ] Le bloc Statistiques contient **uniquement** ORP courant + Dosage du jour (pas d'infos calibration, pas de bouton Calibrer, pas de titre, pas de `.card`).
  - [ ] Le bloc Statistiques tient dans une hauteur visuelle réduite par rapport à une `.card` classique (objectif : ≤ 50 % de la hauteur actuelle sur desktop), et reproduit le design retenu pour `/ph` (grille 2 colonnes avec diviseur, responsive colonne unique < 500 px).
  - [ ] Le bloc « Dosage du jour » n'apparaît **qu'une seule fois** dans la page, dans le bloc Statistiques.
  - [ ] La carte Régulation contient le **sélecteur 3 modes** + sous-blocs conditionnels.
  - [ ] Les **infos de calibration (date, bandeau « Calibré », références) et le bouton Calibrer** sont affichés **uniquement** dans le sous-bloc Automatique de la carte Régulation ; absents en modes Programmée et Manuelle.
  - [ ] Le sous-bloc **Injection manuelle** n'est affiché QUE lorsque `orp_regulation_mode === 'manual'`.
  - [ ] En modes Automatique et Programmée, aucun contrôle d'injection manuelle n'est visible dans la carte Régulation.
  - [ ] La carte **Calibration** est **masquée par défaut** (pas de `display` visible à l'arrivée sur la page).
  - [ ] Un clic sur **Calibrer** (disponible uniquement dans le sous-bloc Automatique) masque Régulation + Historique et affiche Calibration ; Statistiques reste visible.
  - [ ] Pendant la calibration, le label du readout (dans Statistiques) devient « Tension brute capteur » et la valeur s'affiche en mV avec 1 décimale.
  - [ ] En fin de protocole (succès ou annulation confirmée), Calibration disparaît, Régulation + Historique réapparaissent, le label redevient « Valeur ORP actuelle ».
  - [ ] Le bouton **Calibrer** est désactivé si une injection ORP est en cours (via `orp_dosing` ou `orp_inject_remaining_s`).
  - [ ] Le mode **Programmée** fonctionne : saisie bornée par `max_orp_ml_per_day`, sauvegarde OK, rejet si > limite, firmware injecte en continu jusqu'au quota journalier (pause si limite horaire atteinte, reprise à la fenêtre suivante), les limites de sécurité sont respectées (validé par `pool-chemistry`).
  - [ ] La migration `orp_enabled` (booléen) → `orp_regulation_mode` (enum) est silencieuse au premier boot (cf. règle ci-dessus).
  - [ ] Auto-discovery Home Assistant cohérente avec le nouveau modèle de config (switch `orp_enabled` maintenu en miroir OU remplacé par `select` + `number`, à trancher par `pool-chemistry` + `web-ui-developer`, idéalement identique au choix fait pour `ph_enabled`).
  - [ ] Aucune régression : calibration 1 point / 2 points, sauvegarde config, graphique historique continuent de fonctionner.
  - [ ] `pio run` compile sans nouveau warning.
  - [ ] `./build_fs.sh` passe sans dépasser la partition `spiffs` (1088 KB).

- Notes techniques de livraison (2026-04-24) :
  - `config.h` / `config.cpp` : champs `orpRegulationMode` + `orpDailyTargetMl` ajoutés dans `MqttConfig` ; migration NVS `orp_enabled → orp_regulation_mode` dans `loadMqttConfig()` ; `orp_enabled` maintenu en miroir.
  - `pump_controller.cpp` : dispatch sur `orpMode` — branche `automatic` (PID inchangé), branche `scheduled` (aveugle au capteur ORP, effectiveDailyMl plafonné par `maxChlorineMlPerDay`, `effectiveFlowMlPerMin = maxFlow * pump2MaxDutyPct/100`, PID reset au bloc exit).
  - `ws_manager.cpp` + `web_routes_config.cpp` : champs `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid` exposés. POST /save-config : validation enum + rejet HTTP 400 si `orp_daily_target_ml > maxChlorineMlPerDay`.
  - `mqtt_manager.*` : topics `orpRegulationModeState` + `orpDailyTargetMlState` ajoutés et publiés dans `publishTargetState()`. HA : `orp_enabled` maintenu en miroir (idem décision `ph_enabled`).
  - `index.html` : section `#view-orp` entièrement réécrite — `.orp-stats#orp-card-stats` (grille 2 colonnes), `#orp-card-regulation` (segmented 3 modes + sous-blocs conditionnels + `#orp-calibration-info` + `#orp-inject-section`), `#orp-card-history` (inchangé), `#orp-card-calibration` (display:none par défaut, calibration 1pt/2pt).
  - `app.css` : `.orp-stats` / `.orp-stats__col` alignés avec `.ph-stats` / `.ph-stats__col`.
  - `app.js` : `updateOrpModeControls()`, `showOrpCalibrationCard()` / `hideOrpCalibrationCard()`, autosave segmented ORP, `bindOrpCalibration()` avec show/hide calibration card, `collectConfig()` / `applyConfig()` adaptés. `updateFeatureVisibility("orp")` rendu no-op (entrée retirée des maps).
  - Build : `pio run` SUCCESS (RAM 16.4 %, Flash 96.6 %, 0 nouveau warning). `./build_fs.sh` : 643 919 B / 1 088 KB (59 % utilisé).
  - Décision HA : `orp_enabled` maintenu comme miroir booléen (idem `ph_enabled` feature-003). Pas de remplacement par `select` HA pour cette itération.
  - Point hors périmètre livré : label readout ORP « Tension brute capteur » pendant la calibration (prévu mais non listé comme item C — implémenté dans `showOrpCalibrationCard()` sans régresser le reste).

- Hors périmètre de cette itération :
  - Modification du protocole de calibration 1 point / 2 points (présentation change, contenu fonctionnel préservé)
  - Refonte équivalente d'autres pages (dashboard, filtration)
  - Introduction de nouveaux topics MQTT au-delà de ceux strictement nécessaires au mode Programmée

### Itération 1 — 2026-04-23 — baseline de la page `/orp`
- Commit(s) : —
- Statut : point de départ documentaire (aucune livraison associée à cette itération)
- Résumé :
  - Première version propre de la spec, servant de **référence de base** pour la page `/orp`.
  - Documente d'une part le **comportement existant** dans le firmware et l'UI actuels (4 cartes toujours visibles : Régulation ORP, Injection manuelle, Historique, Calibration) — voir la section « Comportement actuel ».
  - Décrit d'autre part l'**architecture cible** souhaitée (bloc Statistiques compact + Régulation (avec sous-blocs Automatique / Programmée / Manuelle, calibration et injection manuelle conditionnelles) + Historique + carte Calibration conditionnelle qui remplace Régulation + Historique pendant le protocole) — voir « Comportement attendu » et « Architecture de la page ».
  - Les écarts entre existant et cible (création du bloc Statistiques compact, fusion Régulation + Injection manuelle avec sélecteur de mode, masquage conditionnel pendant la calibration, introduction du mode Programmée, déplacement des infos de calibration dans le sous-bloc Automatique, injection manuelle uniquement en mode Manuelle) constituent le périmètre de l'itération 2.
  - Cette spec reprend les choix déjà tranchés pour `/ph` (feature-003, itérations 2 et 3 livrées) afin de préserver la symétrie fonctionnelle et visuelle des deux pages.
