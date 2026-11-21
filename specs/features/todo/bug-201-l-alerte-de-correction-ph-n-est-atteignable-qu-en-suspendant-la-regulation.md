## Métadonnées
- **ID** : bug-201
- **Slug** : l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation
- **Type** : bug
- **Statut** : todo
- **Priorité** : **haute**
- **Version cible** : 2.97.0
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [bug-200](../done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) (même famille) · [feature-168](../done/feature-168-correction-ph-sans-borne.md) (le gate C12 dont la contrepartie n'a jamais été payée)

## Domaines touchés
- [x] Frontend (`data/`)
- [x] Régulation — **consommée**, pas modifiée → `pool-chemistry` pour avis
- [ ] Web — routes HTTP / WebSocket *(rien à ajouter : les deux clés sont déjà servies)*

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry
- [x] ux-ui-designer
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'alerte « correction de mesure pH à confirmer » est écrite, juste, et servie par le firmware —
mais **le seul écran qui l'affiche est inatteignable sans suspendre la régulation**. Le
propriétaire ne peut pas la lire sans geler son dosage.

## Contexte
Signalé par l'utilisateur le 2026-11-18, mot pour mot : **« je ne vois pas l'alerte »**. Sa
machine est dans cet état en ce moment (`ph_offset = 1.2`, `ph_offset_stale = true`,
`ph_offset_stale_cause = "boot"`, lu sur `/get-config` de l'appareil).

### Faits vérifiés dans le code (relus un par un)
| Fait | Emplacement |
|---|---|
| La bannière existe et son texte est juste | `data/app.js:2522` (`renderPhOffsetStaleBanner`) |
| Elle vit dans `#ph_tune_stale_banner` | `data/index.html:1358` |
| ⛔ Qui est **à l'intérieur** de `#ph-card-tune`, marqué `hidden` | `data/index.html:1347` |
| ⛔ Lequel n'est dévoilé que par le flux maintenance | `data/app.js:10524` (`MAINTENANCE_PAGES.ph.cardIds`) |
| ⛔ Dont l'entrée émet `POST /calibration/enter`, **qui bascule la régulation en Manuel** | `data/app.js:10443` + commentaire `:10529-10531` |

### ⭐ La contradiction, et elle est écrite dans le fichier lui-même
`data/index.html:1337-1340` commente la carte ainsi :
> « **Toujours visible** (quel que soit le mode de régulation) car elle corrige la **MESURE**, pas
> la régulation. »

**Le commentaire est vrai. Le code ne l'est pas.** La carte porte `hidden` et figure dans la liste
des cartes de maintenance. ⇒ Personne n'a comparé l'intention écrite au comportement rendu.

### Le principe violé, nommé
Les gates de ce projet posent la même règle sous quatre formes (C3 segments `disabled` ⛔ jamais
`hidden` · C5 la visibilité permanente du mode **est** la protection · C10 consigne désactivée
⛔ jamais masquée · `index.html:183-186` « ni masquable ni repliable ») :

> ⭐ **Quand un gate renonce à BLOQUER, la contrepartie qu'il achète est la VISIBILITÉ PERMANENTE.**

Or feature-168 (C12) a **explicitement renoncé à bloquer** — décision utilisateur du 2026-10-16,
« je ne veux pas bloquer » — en échange d'un bandeau. ⇒ **La contrepartie n'a jamais été payée** :
« visible » a été implémenté comme « présent dans le DOM d'un sous-écran de maintenance », pas
comme « vu ». C'est un **défaut d'exécution de C12**, pas une demande de confort.

⚠️ La seconde voie de sortie est également défaillante : l'alerte MQTT de C12 est émise sous
`if (wifiConnected)` dans `setup()` (`src/main.cpp:491` englobant `:515-524`). Un démarrage hors
ligne ⇒ **aucune trace** sauf le journal, qui tient une minute.

## Comportement attendu
1. **`#ph-card-tune` est visible en permanence sur `#/ph`**, hors flux maintenance. Elle corrige la
   **mesure**, pas la sonde : elle ne justifie aucun gel de dosage. C'est ce que son propre
   commentaire annonce déjà.
2. ⛔ **La carte d'étalonnage EZO (`#ph-card-calibration`) reste, elle, dans le flux maintenance** —
   elle touche la sonde, son gel est légitime. ⚠️ Ne pas sortir les deux d'un bloc.
3. Les deux boutons « Annuler la correction » / « Garder » restent le **seul** chemin qui éteint le
   drapeau (`setPhOffsetStamped()` → `phOffsetBootUnconfirmed = false`, `src/config.cpp:196`).
   ⛔ Ne rien en retirer : les supprimer rendrait l'alerte **ineffaçable**.
4. Les trois textes de C15 (`boot` / `calibration` / `expiration`) restent **mot pour mot**.

### Cas limites
- `ph_offset` absent ou `NaN` ⇒ ⛔ aucune bannière. La garde `isFinite` n'est **pas** redondante
  avec `typeof === "number"` : `typeof NaN === "number"`.
- `ph_offset_stale` à `1` et non `true` ⇒ ⛔ aucune bannière (comparaison `=== true` stricte).
- `ph_offset === 0` avec `stale = true` ⇒ ⛔ aucune bannière.
- Entrer **puis sortir** du flux maintenance ne doit pas re-cacher la carte.

## Comportement actuel
La carte est `hidden`. Pour la voir, il faut appuyer sur le bouton d'entrée en maintenance, ce qui
bascule la régulation en Manuel et gèle le dosage. Aucun propriétaire ne fera ce geste pour
« aller voir s'il y a une alerte » — et s'il le fait, il paie l'information d'un arrêt de
régulation.

## Contraintes techniques
- ⚠️ **RESPONSIVE** : 390 px d'abord ; la carte doit tenir hors maintenance sans casser la page pH.
- ⛔ **Aucun champ firmware à ajouter** : `ph_offset_stale` et `ph_offset_stale_cause` sont déjà
  servis par **appel à l'accesseur** sur `/get-config` (`src/web_routes_config.cpp:310-311`) **et**
  sur la trame WS (`src/ws_manager.cpp:1691-1692`). ⛔ Ne pas rouvrir une 5ᵉ forme du piège de parité.
- ⛔ **Ne jamais recalculer le verdict côté navigateur** à partir de `ph_offset` et d'un seuil
  (ADR-0100) : lire la clé servie.
- ⚠️ Le mot « **sonde** » est **interdit** dans ces textes : la correction compense un décalage
  **d'installation** (courants de fuite de la pompe de filtration), pas une sonde. Acquis des
  incidents du 25/08 et du 11/10, protégé par C8 révisé et par `src/ph_offset.h:55-63`.
- La valeur s'affiche à **2 décimales signées** (`_fmtPhOffset2`, `data/app.js:2507`),
  ⛔ pas `_fmtPh` (1 décimale) : convention assumée, le chiffre reproduit un gabarit de journal.

## Fichiers potentiellement concernés
- `data/app.js` — retirer `ph-card-tune` de `MAINTENANCE_PAGES.ph.cardIds` (`:10524`) et traiter ce que cela implique à l'entrée/sortie du flux
- `data/index.html` — retirer `hidden` de `#ph-card-tune` (`:1347`)
- `data/app.css` — mise en page de la carte hors maintenance (`.flow-maintenance`)
- `docs/features/page-ph.md`, `docs/DESIGN_SYSTEM.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] B1 — `#ph-card-tune` visible en permanence sur `#/ph`, étalonnage EZO **inchangé** dans le flux maintenance
- [ ] B2 — Entrée **et** sortie du flux maintenance laissent la carte visible
- [ ] C1 — `docs/features/page-ph.md`, `docs/DESIGN_SYSTEM.md` (contexte d'emploi de `.flow-maintenance`), `CHANGELOG.md`
- [ ] D1 — Capture à 390 px, page pH **sans** entrer en maintenance : la bannière est lisible
- [ ] D2 — ⭐ **Mutation exigée** : remettre `ph-card-tune` dans `cardIds` doit **tuer** un test
- [ ] D3 — Capture des trois causes (`boot` / `calibration` / `expiration`) avec `-c`

## Sécurité chimique
- Dosage impliqué : **indirectement** — la correction décide des injections d'acide
- Limites concernées : aucune n'est modifiée
- Filtration requise : sans objet
- Calibration capteur modifiée : **non** — ⛔ c'est précisément le point : la correction de mesure
  **n'est pas** un étalonnage
- Algorithme PID touché : non
- Risque identifié : ⛔ **ne pas inhiber le dosage** tant que la correction n'est pas confirmée
  (verdict `pool-chemistry` du 2026-11-18, **NO GO**). Motif : la cause `boot` se lève à **chaque
  démarrage** pour **tout** écart supérieur à 0,50 ; cet appareil a une correction **légitime et
  permanente** de +1,20, donc l'inhibition s'armerait **à chaque mise à jour**. En saison, un
  bassin privé d'acide 48 h part à pH 8,2-8,4, où la fraction de chlore actif tombe sous 25 % —
  **une panne sanitaire certaine en échange d'un risque hypothétique**. Le principe du projet
  s'applique : *les plafonds protègent, le reste informe*.
