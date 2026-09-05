## Métadonnées
- **ID** : feature-090
- **Slug** : taille-bassin-preselections-regulation
- **Type** : feature
- **Statut** : done (livré en v2.32.0, 2026-08-31 — tests verts 1020/1020, mutations 6/6, revue Approved, contre-visa pool-chemistry GO)
- **Priorité** : haute
- **Version cible** : 2.32.0
- **Dernière mise à jour** : 2026-08-31
- **Spec(s) liée(s)** : feature-087 (valeurs d'usine recalibrées), feature-088
  (restauration des défauts — mécanisme de dialogue avant/après et source
  unique `regulation_defaults.h` à faire évoluer)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (nouveau champ NVS `poolSize`)
- [x] Frontend (`data/`) — écran Régulation + assistant de premier démarrage

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (table des défauts par taille + invariant)
- [x] ux-ui-developer — non nécessaire en agent séparé : patterns existants
      (sélecteur segmenté, dialogue 088, étape wizard existante)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Un réglage « Taille du bassin » (< 20 m³ / 20-60 m³ / > 60 m³) en première
position de l'écran Régulation pH/ORP, qui présélectionne les défauts de
régulation adaptés. La vitesse de correction disparaît de l'UI et dérive de la
taille. La taille est aussi demandée dans l'assistant de premier démarrage, à
l'étape du mode d'installation (titre adapté, court : « Installation et
bassin »).

## Objectif (formulation utilisateur, 31/08/2026)

Simplifier le paramétrage avec un réglage que l'utilisateur COMPREND — la
taille de son bassin — et s'en servir pour présélectionner les bons réglages
de régulation, sans lui enlever la possibilité de les modifier ensuite.

## INVARIANT ABSOLU (→ ADR + test par mutation)

**La taille du bassin ne sert qu'à PRÉSÉLECTIONNER des réglages modifiables.
Elle n'entre JAMAIS dans un calcul de dosage, de débit, de quota ou de
correction.** La régulation continue d'apprendre la consommation réelle
(anneau `ph_load7`, feedforward). Principe utilisateur fondateur : « si je
donne le volume de mon bassin et qu'il est pris en compte, l'appareil ne
fonctionnera pas avec une autre piscine ». La taille sélectionne des défauts ;
le comportement reste piloté par les réglages (modifiables) et l'apprentissage.

## Table des défauts par taille — À FAIRE VISER par pool-chemistry

| Réglage | < 20 m³ | **20-60 m³** | > 60 m³ |
|---|---|---|---|
| Limite injection pH | 5 min/h | **10 min/h** | 15 min/h |
| Limite injection ORP | 5 min/h | **10 min/h** | 15 min/h |
| Vitesse de correction (masquée) | slow | **normal** | fast |
| Délai de stabilisation | 5 min | **5 min** | 10 min |
| Consigne pH | 7,3 | 7,3 | 7,3 |
| Consigne ORP | 600 mV | 600 mV | 600 mV |
| Volumes quotidiens mode programmé | 0 | 0 | 0 |

**Statut des colonnes** : 20-60 m³ = adossée aux mesures du bassin de
référence (30-50 m³, v2.30.0, visa du 31/08). < 20 et > 60 = EXTRAPOLATIONS
(à étiqueter comme telles dans le code et la doc), logique : excursion de
mélange aveugle ~constante (~0,10-0,13 pH) à travers les tailles, capacité
journalière qui suit le besoin, mélange plus lent au-delà de 60 m³ (délai 10).
Vitesse : petit bassin = gain du procédé élevé (chaque mL a plus d'effet) →
correction douce ; grand bassin → l'inverse. Les consignes pH/ORP ne dépendent
PAS de la taille (chimie de l'eau, pas volume).

## Comportement attendu

### Écran Régulation pH/ORP
- Sélecteur « Taille du bassin » en PREMIÈRE position (3 options, libellés
  français : « Moins de 20 m³ », « 20 à 60 m³ », « Plus de 60 m³ »).
- Changement de taille → **dialogue avant/après** (RÉUTILISER le mécanisme
  feature-088) listant les défauts de la nouvelle taille face aux valeurs
  courantes ; confirmation → application + persistance + rechargement.
  Annulation → le sélecteur revient à la taille précédente, rien n'est modifié.
- La vitesse de correction disparaît de l'UI (le réglage reste dans l'API,
  la NVS et MQTT — masquage UI uniquement).
- Le bouton « Restaurer les valeurs par défaut » (088) restaure désormais les
  défauts DE LA TAILLE SÉLECTIONNÉE (l'objet `regulation_defaults` de
  /get-config reflète la taille courante).

### Assistant de premier démarrage
- La taille est demandée à l'étape du mode d'installation.
- Titre de l'étape : **« Installation et bassin »** (court).
- Le choix applique les défauts de la taille (pas de dialogue avant/après au
  premier démarrage : il n'y a rien à écraser).

### Migration flotte (appareils existants)
- Nouveau champ NVS absent → défaut = **20-60 m³** : les défauts de cette
  taille sont les défauts actuels de la v2.30.0 ⇒ **zéro changement de
  comportement à la mise à jour**.
- La vitesse garde sa valeur NVS tant que l'utilisateur ne change pas de
  taille. Aucun réglage existant n'est modifié par la mise à jour seule.

### Cas limites
- Changement de taille pendant une session de dosage : mêmes mécanismes que
  la 088 (hook différé, hash de contexte — la vitesse peut changer ⇒ purge
  intégrale existante).
- Ancien client (page en cache) face au nouveau firmware : rien ne casse ;
  nouveau client face à ancien firmware : sélecteur masqué (détection par
  présence du champ).
- MQTT/HA : `regulation_speed` reste publié tel quel (contrat inchangé) ;
  la taille est-elle exposée ? (à trancher : probablement oui dans /get-config
  seulement, pas de nouvelle entité HA).

## Contraintes techniques
- `regulation_defaults.h` évolue de constantes simples vers une **table à
  trois colonnes** (source unique, static_assert par colonne, template
  `applyRegulationDefaults(cfg, size)` — les acquis 088 se généralisent).
- Le POST `/reset-regulation-defaults` (088) devient dépendant de la taille
  courante — vérifier la compat du contrat (le JSON de réponse garde les
  mêmes clés).
- Flash à 98,0 % (marge ~36 Ko) : budget firmware ≤ 3 Ko, FS ≤ 2 Ko gzippé.
- Français partout ; aucun commit sans demande.

## Fichiers potentiellement concernés
- `src/regulation_defaults.h` — table par taille
- `src/config.h/.cpp` — champ `poolSize` (NVS), application des défauts
- `src/web_routes_config.cpp` — save/get de la taille, évolution du reset
- `data/index.html`, `data/app.js` — sélecteur, masquage vitesse, wizard
- `docs/API.md`, `docs/features/page-settings.md`, doc wizard, `CHANGELOG.md`
- Nouvel ADR : l'invariant taille ≠ entrée de dosage + la table

## Livrables — check-list
- [x] A1 — Table des défauts par taille dans `regulation_defaults.h` (visée)
- [x] A2 — Champ `poolSize` NVS + application des défauts au changement
- [x] A3 — Reset 088 dépendant de la taille ; `regulation_defaults` de
      /get-config reflète la taille courante
- [x] A4 — Invariant « la taille n'entre dans aucun calcul de dosage » prouvé
      par grep + mutation
- [x] B1 — Sélecteur en première position + dialogue avant/après au changement
- [x] B2 — Vitesse de correction masquée en UI (API/NVS/MQTT inchangés)
- [x] B3 — Étape wizard « Installation et bassin » (taille + mode d'installation)
- [x] C1 — docs (API, page-settings, wizard, CHANGELOG) + ADR
- [x] D1 — Tests natifs de la table (bornes par colonne, mutations) et de
      l'application par taille
- [x] D2 — Migration : appareil existant sans champ ⇒ 20-60, aucun réglage
      modifié (test + revue)

## Sécurité chimique
- Dosage impliqué : indirectement (défauts de limites et de vitesse)
- Risque identifié : extrapolations < 20 / > 60 non mesurées (à viser) ;
  changement de taille destructif pour les réglages personnalisés (mitigé
  par le dialogue avant/après) ; l'invariant d'usage de la taille

## ADR attendu ?
**Oui** : (a) l'invariant « taille = présélection, jamais entrée de calcul » ;
(b) la table des défauts par taille avec le statut mesure/extrapolation et la
logique d'excursion constante ; (c) vitesse dérivée masquée (alternative
écartée : la supprimer — refusée, elle reste dans l'API pour les cas experts
et la rétrocompat).
→ **Livré : [ADR-0057](../../../docs/adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)**
(les trois décisions + alternatives écartées : entité HA pour `pool_size`,
table dupliquée en JS).

## Notes d'implémentation (clôture, 2026-08-31 — v2.32.0)

### Livraison
- **Firmware** : `src/regulation_defaults.h` (enum `PoolSize`, table
  `kRegulationDefaultsBySize[3]`, `poolSizeTo/FromString` fail-safe → Medium,
  `applyRegulationDefaults(cfg, size)`, `regulationDefaultsRaiseDosing(target,
  cur…)` paramétré, verrous `static_assert` : bornes par colonne + monotonie +
  **verrou de migration Medium == littéraux v2.30.0 en dur**) ; `src/config.h/.cpp`
  (champ `poolSize`, clé NVS `pool_size`, restauration 088 sur la colonne
  courante) ; `src/web_routes_config.cpp` (bloc `pool_size` de `/save-config`
  AVANT le parse des clés individuelles — POST combiné = taille puis raffinage ;
  `/get-config` : `pool_size`, `regulation_defaults` = colonne courante [contrat
  088 intact], `regulation_defaults_by_size` ; journal avant→après, WARNING si
  capacité de dosage augmentée ; retune PID différé, hash de contexte 088).
- **Frontend** : `data/index.html` (`#pool_size_block` en 1ʳᵉ position,
  `#regulation_speed_block` masquable), `data/app.js` (`syncPoolSize`,
  `_onPoolSizeClick` — dialogue avant/après 088 incluant la vitesse [seul
  endroit où elle reste visible], POST `pool_size` seul, annulation sans effet,
  caches de parité WS, rétrocompat ancien firmware : sélecteur masqué + vitesse
  ré-affichée), `data/wizard.html` (étape 3 « Installation et bassin », POST
  combiné `{install_mode, pool_size}`, pas de dialogue au premier démarrage).
- **Empreinte** : **+2 784 o firmware, +1 201 o filesystem** (budgets ≤ 3 Ko /
  ≤ 2 Ko respectés).

### Verdicts
- **pool-chemistry** : visa de la table le 31/08 (colonnes Small/Large =
  EXTRAPOLATIONS étiquetées, excursion de mélange aveugle ~constante), puis
  **contre-visa GO** sur le code figé (verrou de migration, prédicat WARNING,
  invariant).
- **test-engineer** : `pio run` SUCCESS 0 warning ; `pio test -e native`
  **1020/1020** verts (suite `test_native_regulation_defaults` étendue :
  familles D4 colonnes Small/Large en littéraux durs, D5 sérialisation
  fail-safe, D6 migration valeur inconnue ⇒ défauts v2.30.0 exacts).
- **code-reviewer** : **Approved** (une mineure, consignée ci-dessous).
- **doc-writer** : API.md, page-settings.md, **page-wizard.md (créée)**,
  pump-controller.md, UPDATE_GUIDE.md, CHANGELOG v2.32.0, ADR-0057.

### Validation par mutation — 6/6 mutants tués
| # | Mutant | Résultat |
|---|--------|----------|
| 1 | Altération d'une valeur de colonne Small (littéraux durs en D4) | tué (test) |
| 2 | Altération d'une valeur de colonne Large | tué (test) |
| 3 | Champ hors périmètre ajouté à `applyRegulationDefaults` (sentinelles D2, trois tailles) | tué (test) |
| 4 | Inversion du sens de `regulationDefaultsRaiseDosing` sur transition de taille | tué (test) |
| 5 | Rupture de monotonie de la table | tué (**échec de compilation**, `static_assert`) |
| 6 | Retouche de la colonne Medium (verrou de migration v2.30.0) | tué (**échec de compilation**, `static_assert`) |

### Preuve de l'invariant (A4)
`grep -rln "poolSize" src/` → uniquement `config.h`, `config.cpp`,
`regulation_defaults.h`, `web_routes_config.cpp` — jamais `pump_controller.*`
ni aucun chemin de dosage/débit/quota. Consigné dans l'ADR-0057 ; complété par
mutation (le champ `poolSize` n'est pas copié par `applyRegulationDefaults`).

### Mineure de revue consignée (non bloquante)
**Divergence RAM/NVS possible sur POST combiné avec clé refusée** : si un POST
`{pool_size, <clé ensuite refusée en 400>}` franchit le bloc taille puis échoue
sur une validation ultérieure, les défauts déjà appliqués en RAM ne sont pas
annulés alors que la persistance de fin de handler n'a pas lieu. **Motif
préexistant** de `/save-config` (même comportement pour d'autres clés),
**inatteignable par l'UI** (le sélecteur envoie `pool_size` seul ; le wizard
combine avec `install_mode`, jamais refusé). Anomalie séparée à ouvrir si
souhaité.

### Arbitrages tranchés en cours de cycle
- « La taille est-elle exposée à MQTT/HA ? » (question ouverte de la spec) :
  **non** — `/get-config`/`/save-config` seulement, pas d'entité HA (écritures
  automatisées contraires à l'invariant, cf. ADR-0057).
- Sur > 60 m³, le quota journalier d'usine 300 mL/j (protection, hors périmètre
  des 8 champs) plafonne en pratique la promesse 15 min/h (~11 min/j à
  28 mL/min) — assumé, documenté (UPDATE_GUIDE, ADR-0057, CHANGELOG),
  l'alerte ADR-0056 guide le relèvement manuel.
