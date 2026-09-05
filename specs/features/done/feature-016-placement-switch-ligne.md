## Métadonnées
- **ID** : feature-016
- **Slug** : placement-switch-ligne
- **Type** : feature (refactor UI)
- **Statut** : **clôturée — écartée** (2026-07-05, non implémentée, verdict ux-ui-designer)
- **Priorité** : basse (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-05

## Note de clôture (2026-07-05)

Spec **écartée** sur arbitrage formel `ux-ui-designer` (demandé par la spec elle-même en B2, verdict tranché : **garder le pattern actuel** libellé-gauche / toggle-droite) :

1. **Convention plateforme dominante** : Réglages iOS, Android, Home Assistant utilisent tous le toggle calé à droite — l'usage principal du projet est iPhone Safari 375 px ; casser cette convention coûte plus qu'elle ne rapporte (repère moteur appris).
2. **Le problème n'existe pas sur la cible** : à 375 px, le « grand vide » dénoncé fait quelques dizaines de px ; c'est une gêne desktop minoritaire, mieux traitée le cas échéant par un `max-width` des cards de réglages.
3. **Scannabilité** : la colonne verticale de toggles alignés permet de balayer l'état de N réglages d'un coup d'œil ; collés à des titres de longueurs variables, ils zigzaguent.
4. **Zone de tap** : accoler le switch (52×30 px) au texte cliquable du `<label>` augmente le risque de mis-tap.
5. **Fausse analogie feature-001** : les badges du `card__head` sont de la *lecture* (regroupement informatif) ; un switch est une *cible d'action* dont la position droite est un repère.

**Porte de sortie** : si la gêne desktop se re-manifeste, requalifier en micro-spec « limiter la largeur des cards de réglages sur desktop » (max-width), sans toucher au placement des toggles.

État du code à la clôture : 10 switchs, pattern homogène `.row` (app.css l.1363). Références périmées dans la spec d'origine (onglet Avancé réorganisé par 045/046, budget FS 1088 KB → 576 KB layout v3). Aucun code modifié au titre de cette spec.
- **Spec(s) liée(s)** : feature-001 (placement uniforme des badges d'état dans `card__head`) — même esprit, appliqué cette fois aux switchs

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non applicable (refactor visuel pur, aucun chemin de dosage touché)
- [x] ux-ui-designer — arbitrage du pattern HTML/CSS exact (positionnement, espacement, alignement vertical) si plusieurs options crédibles
- [ ] embedded-developer — non applicable (frontend uniquement)
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Uniformiser le placement des **switchs** (toggles `<label class="switch">`) dans les lignes de configuration : le switch doit être positionné **à droite du titre/libellé de la ligne**, **collé au titre avec un espace standard**, et non à l'extrême droite de la ligne. Objectif : cohérence visuelle inter-pages, lecture plus fluide (titre + switch côte à côte), suppression de la sensation d'« orphelin » du switch quand le libellé est court et la ligne large.

## Contexte

Aujourd'hui, plusieurs lignes de configuration utilisent un pattern flexbox avec `justify-content: space-between` qui pousse le switch à l'extrême droite de la ligne, créant un grand vide entre le libellé et le contrôle. Le pattern est cohérent visuellement quand les lignes sont denses et alignées en colonne, mais devient illisible quand le libellé est court (« Activer », « Utiliser NTP », etc.) et que la ligne fait toute la largeur de la card.

Pages concernées (au minimum, à confirmer par l'audit ux-ui-designer / architect) :
- **Filtration** (`#view-filtration`) — switchs liés au mode auto, force ON/OFF, etc.
- **Éclairage** (`#view-lighting`) — switch d'activation, mode auto
- **Suivi des produits** (`#view-products` ou panel Dosages) — switchs de tracking pH/ORP, alertes stock
- **Paramètres → MQTT** — switch « Activer MQTT »
- **Paramètres → Heure** — switch « Utiliser NTP »
- **Paramètres → Avancé** — switchs de mode expert, logs sensors, niveau debug, etc.

Cohérent avec feature-001 qui a uniformisé le placement des badges d'état dans le `card__head`. Cette feature applique le même esprit aux switchs dans les lignes.

## Comportement attendu

### Côté UI

Pour chaque ligne contenant un switch :
- Le titre/libellé reste à gauche
- Le switch est positionné **immédiatement à droite du titre**, séparé par un espace standard (à fixer par ux-ui-designer, probablement le `gap` flexbox du projet, ex. 10-12 px)
- L'alignement vertical reste centré
- Le pattern visuel est **identique sur toutes les pages listées** (cohérence inter-pages)
- Si une ligne contient en plus une description courte sous le titre (sous-libellé), le switch reste aligné sur le titre principal

### Cas limites

- Ligne où le titre est très long (jusqu'à wrap sur 2 lignes) : le switch reste à droite du titre, pas wrap.
- Ligne avec un sous-libellé descriptif : alignement vertical centré sur l'ensemble titre + sous-libellé OU sur le titre seul (à arbitrer par ux-ui-designer).
- Ligne avec d'autres contrôles (ex. switch + bouton « Tester ») : le switch suit toujours le titre, le bouton reste à sa place actuelle (à confirmer par audit).
- Responsive (≤ 500 px) : le pattern doit rester lisible, sans wrap intempestif du switch sous le titre.

## Comportement actuel

Sur les pages listées, le switch est typiquement à l'extrême droite de la ligne via `justify-content: space-between` ou un pattern équivalent. Le grand vide entre titre et switch crée une impression d'incohérence quand le libellé est court.

## Contraintes techniques

- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — vérifier `./build_fs.sh` après modification
- AsyncWebServer : aucun handler bloquant > 50 ms (non concerné, frontend uniquement)
- Conserver tous les IDs HTML existants (`#mqtt_enabled`, `#time_use_ntp`, `#filtration_*`, `#lighting_*`, `#tracking_ph_enabled`, etc.) — utilisés par `data/app.js` pour les addEventListener et `collectXxxConfig()`
- Préserver l'accessibilité (libellés cliquables `<label>`, `aria-*` si présent)
- Ne PAS toucher aux switchs du dashboard (qui ne semblent pas exister, à confirmer)
- Préférer une solution CSS générique (sélecteur sur conteneur) plutôt que de modifier chaque ligne HTML une par une, si possible

## Fichiers potentiellement concernés

- `data/index.html` — peut nécessiter quelques ajustements de structure DOM (selon l'arbitrage ux-ui-designer)
- `data/app.css` — règle CSS générique pour les conteneurs de ligne avec switch, ou nouvelle classe utilitaire
- `data/app.js` — aucune logique métier modifiée, mais à vérifier que les sélecteurs (event listeners, collectXxxConfig) restent valides
- `docs/features/page-filtration.md`, `docs/features/page-lighting.md`, `docs/features/page-dosages.md`, `docs/features/page-settings.md` — mise à jour de la doc visuelle si captures changent
- `CHANGELOG.md` — entrée Frontend

## Livrables — check-list

- [ ] B1 — Audit exhaustif des lignes contenant un switch sur les pages listées (filtration, lighting, dosages/produits, MQTT, Heure, Avancé). Lister les patterns DOM/CSS actuels (à charge de spec-reader / architect).
- [ ] B2 — Décision UX du pattern uniforme (à charge ux-ui-designer) : structure HTML retenue, classe CSS, espacement standard.
- [ ] B3 — Implémentation HTML/CSS sur toutes les lignes identifiées en B1 (à charge web-ui-developer).
- [ ] B4 — Vérification que les sélecteurs JS (`addEventListener`, `collectXxxConfig`) fonctionnent toujours après le refactor.
- [ ] D1 — Test visuel multi-pages humain : vérifier que toutes les pages listées affichent les switchs collés au titre de leur ligne.
- [ ] D2 — Test responsive (≤ 500 px) : pas de wrap intempestif.
- [ ] D3 — `./build_fs.sh` SUCCESS, partition `spiffs` 1088 KB OK.
- [ ] C1 — Mise à jour `docs/features/page-*.md` pour les pages touchées si la structure visuelle change.
- [ ] C2 — Entrée `CHANGELOG.md` Frontend.

## Nouvelles signatures / API

- Endpoints HTTP : aucun nouveau
- Champs WebSocket : aucun
- Topics MQTT : aucun nouveau
- Fonctions publiques : aucune
- Constantes : aucune nouvelle

## Sécurité chimique

- Dosage impliqué : non
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : aucun (refactor visuel pur)

## ADR attendu ?

- **Probable** : non
- **Critère(s) candidat(s)** : aucun (refactor visuel mineur, aucune décision structurante, aucun verrouillage long terme)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-filtration.md` — si la structure visuelle des lignes avec switch est documentée, mettre à jour
- `docs/features/page-lighting.md` — idem
- `docs/features/page-dosages.md` — idem (Suivi des produits)
- `docs/features/page-settings.md` — sections MQTT, Heure, Avancé

#### Docs transverses
- `CHANGELOG.md` — entrée Frontend sous la version cible

#### Docs immuables
- aucun ADR

## Critères d'acceptation

- [ ] AC1 — Sur chaque page listée, tous les switchs des lignes de configuration sont placés immédiatement à droite du titre/libellé, séparés par un espace standard cohérent inter-pages
- [ ] AC2 — Aucune régression fonctionnelle : tous les `addEventListener('change', ...)` et `collectXxxConfig()` fonctionnent comme avant (vérifié par test manuel d'au moins un switch par page)
- [ ] AC3 — Le responsive (≤ 500 px) reste lisible : pas de wrap intempestif du switch sous le titre, ou si wrap, justifié visuellement
- [ ] AC4 — Tous les libellés utilisateur restent en français (aucune modification de wording)
- [ ] AC5 — `./build_fs.sh` passe sans dépasser la partition `spiffs` 1088 KB
- [ ] AC6 — Les cards qui contiennent des switchs et qui ne sont PAS dans la liste explicite (s'il y en a) restent intactes (pas de débordement de scope)

## Plan de test manuel

1. **Filtration** : ouvrir la page, vérifier que les switchs (mode, force ON/OFF, etc.) sont collés au titre de leur ligne. Cliquer chaque switch et vérifier qu'il déclenche bien la sauvegarde / l'action attendue.
2. **Éclairage** : idem pour les switchs de la page.
3. **Suivi des produits** : vérifier les switchs de tracking pH/ORP. Cliquer pour activer/désactiver, vérifier que le formulaire est sauvegardé.
4. **Paramètres → MQTT** : switch « Activer MQTT » bien à droite du titre. Click + save, vérifier que le badge passe à « Connecté »/« Déconnecté ».
5. **Paramètres → Heure** : switch « Utiliser NTP » bien collé au titre. Click + save, vérifier la sync NTP.
6. **Paramètres → Avancé** : switchs (mode expert, logs sensors, niveau debug). Click + save.
7. **Responsive** : redimensionner la fenêtre à 320 px de large, vérifier qu'aucun switch ne wrap sous son titre de manière chaotique.
8. **Cards exclues** : vérifier que les pages non listées ne sont pas impactées (Dashboard, Wi-Fi, Sécurité, Régulation pH/ORP si non listées, Calibrations, Système).

## Notes pour agent

- Les switchs utilisent typiquement la classe `.switch` (à confirmer par audit). Le HTML est probablement de la forme :
  ```html
  <label class="switch">
    <input type="checkbox" id="xxx_enabled">
    <span class="switch__slider"></span>
  </label>
  ```
- L'arbitrage UX peut consister à :
  - Choix A : changer le `justify-content: space-between` du conteneur de ligne en `flex-start` + `gap` (similaire à ce qui a été fait dans feature-001 avec `:has(.pill)`)
  - Choix B : ajouter une classe utilitaire spécifique aux lignes avec switch
  - Choix C : wrapper titre + switch dans un sub-div pour les grouper
- Cohérence avec feature-001 : le pattern retenu doit être visuellement homogène avec le placement des badges dans `card__head` (gap standard, alignement centré).
- Veiller à ne pas casser les `<label class="switch">` qui doivent rester cliquables sur leur intégralité (ne pas isoler le checkbox du span slider).

## Hors périmètre

- Pas de changement de logique métier, ni d'ajout de nouveaux switchs.
- Pas de refonte du composant `.switch` lui-même (taille, couleurs, animation).
- Pas de modification du dashboard (cards `card--status` n'ont pas de switchs concernés a priori).
- Pas de modification des switchs/toggles dans le `card__head` (ex. cards Produits pH/ORP qui ont déjà un toggle dans l'en-tête, pattern différent traité par feature-001 / structure existante).
- Pas de migration de toutes les pages : seulement celles listées explicitement.
