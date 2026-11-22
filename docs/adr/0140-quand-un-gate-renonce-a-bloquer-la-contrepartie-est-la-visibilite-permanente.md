# ADR-0140 — Quand un gate renonce à BLOQUER, la contrepartie qu'il achète est la VISIBILITÉ PERMANENTE — et le flux maintenance n'est pas une garde

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas + Claude (`pool-chemistry`, `ux-ui-designer`, `architect`)
- **Spec(s) liée(s)** : [bug-201](../../specs/features/done/bug-201-l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation.md) (conditions `bug-201 (C40)` à `(C51)`) ; prolonge [feature-168](../../specs/features/done/feature-168-correction-ph-sans-borne.md) (`feature-168 (C12)`), [ADR-0103](0103-gestes-d-entretien-quittent-la-vue-courante.md) et [ADR-0138](0138-l-integrite-d-une-valeur-persistee-est-deleguee-a-nvs-jamais-a-l-utilisateur.md)

## Contexte

`feature-168` a **renoncé à bloquer** (décision utilisateur du 2026-10-16, « je ne veux pas bloquer ») : une correction de mesure pH au-delà de ±0,50 est acceptée et **signalée** par un bandeau, plutôt que refusée. Le bandeau était la contrepartie. Or il vivait dans `#ph-card-tune`, elle-même enfouie dans le **mode réglage** (ADR-0103, `.flow-maintenance`), dont l'entrée émet `POST /calibration/enter` et **bascule la régulation en Manuel**. Pour lire l'alerte sur sa correction, il fallait **geler son dosage**.

Signalé le 2026-11-18 : « je ne vois pas l'alerte ». Il voyait une pastille rouge, mais elle parlait de l'étalonnage de la sonde, pas de la correction.

Trois questions se posaient, avec des réponses crédibles de chaque côté : où mettre la carte, que faire de ses boutons, et que protège réellement le flux maintenance.

## Décision

1. **Le flux maintenance n'est pas une garde : il gèle le dosage pour lire une information.** Une barrière qui ne fait que **retarder** l'effet de ce qu'elle garde n'est pas une barrière, c'est un **péage** — et il se paie en dosage gelé. Les vraies gardes ne dépendent pas du flux : filets BRUTS fixes 5,50 / 8,50 (qui ne bougent jamais avec la correction), plancher et plafond corrigés, inhibition ORP, limites par injection / horaire / journalière, et le gate propre de `POST /tune-ph-offset` (mesure fraîche, 10 min de filtration, sonde étalonnée ≥ 2 points).
2. **Quand un gate renonce à BLOQUER, la contrepartie qu'il achète est la VISIBILITÉ PERMANENTE.** « Visible » veut dire *vu* sans geste coûteux, pas « présent dans le DOM d'un sous-écran ». C'est un défaut d'**exécution** de `feature-168 (C12)`, pas une demande de confort. Même principe que les autres gates du projet (segments `disabled` et jamais `hidden`, mode toujours affiché, consigne désactivée et jamais masquée).
3. **`#ph-card-tune` est permanente, avec ses boutons** (« forme A »). Sortir la seule ligne de fait en laissant les boutons derrière le péage aurait donné la pire combinaison : l'information en permanence, et le geste de **réparation** au prix d'un gel de dosage — l'asymétrie « détruire est gratuit, réparer demande une trousse d'analyse » retournée contre l'utilisateur. L'étalonnage EZO (`#ph-card-calibration`) **reste** dans le flux : il touche la sonde, son gel est légitime.
4. **Corollaire : la friction se déplace sur le geste destructif.** Quand on retire le péage, on doit ferrer ce qu'il couvrait réellement. « Supprimer la correction » reçoit un second geste explicite dès que `|correction| > ph_offset_unusual_abs` (clé servie), avec la conséquence chiffrée sur les valeurs servies. Jamais de friction sur les gestes vertueux (« Corriger », « Je l'ai vérifiée — garder »).
5. **`.flow-maintenance` change de sens.** Elle ne signifie **pas** « visible seulement en mode réglage » : elle signifie « **SURVIT** au mode réglage ». `#ph-card-tune` en est la première carte à sortir de `cardIds` tout en gardant la classe, sinon la règle `#view-ph.is-flow-maintenance > :not(.flow-maintenance)` la masquerait pendant le flux d'étalonnage — au moment où la cause `calibration` vient de se lever. Voir [DESIGN_SYSTEM §5.25](../DESIGN_SYSTEM.md#525-flow-maintenance-survit-au-mode-réglage-bug-201-v300).

## Alternatives considérées

- **Inhiber le dosage tant que la correction n'est pas confirmée** (rejetée, NO GO `pool-chemistry` du 2026-11-18, maintenu le 2026-11-22) — punirait le seul mode de défaillance que NVS ne produit pas (ADR-0138 : une corruption fait *disparaître* la valeur) ; en saison, un bassin privé d'acide 48 h part à pH 8,2-8,4, fraction de chlore actif sous 25 %. Une panne sanitaire certaine contre un risque hypothétique. *Les plafonds protègent, le reste informe.*
- **Forme B : bannière seule permanente, réglage derrière le flux** (rejetée, proposée par l'ergonomie) — voir point 3.
- **Laisser tel quel et ajouter un lien vers le mode réglage** (rejetée) — conserve le péage.
- **Retirer la classe `.flow-maintenance` de la carte plutôt que son entrée de `cardIds`** (rejetée) — masque la carte pendant l'étalonnage (point 5).
- **Un seul texte de friction pour les deux signes** (rejetée) — faux pour une correction négative, dont le retrait fait lire plus haut.
- **Friction par `confirm()` natif, mot à saisir ou appui long** (rejetée) — non découvrable, inaccessible ou hostile avec un doigt mouillé.

## Conséquences

### Positives
- La ligne « Correction +1,20 pH — posée le … » (promesse de `feature-170 (C28)`) est enfin visible sans rien geler.
- Le geste vertueux (accorder sur un test) ne coûte plus une suspension de régulation.
- Un seul chemin destructif sur la carte, toujours au même endroit (`bug-201 (C48)`), avec friction chiffrée.
- Aucun champ firmware ajouté (`bug-201 (C40)`) : les clés sont déjà servies.

### Négatives / dette assumée
- Une carte de plus sur la page pH en usage courant ; compensée par quatre retraits dans la carte héros.
- Le texte de friction est figé à l'armement (valeurs absolues possiblement périmées, l'écart qui porte le sens non).
- Le bandeau dit « remettez la correction à zéro » alors que le bouton dit « Supprimer la correction » : la phrase est comparée octet pour octet avec le firmware, la changer touche `src/`.

### Ce que ça verrouille
- Tout futur gate qui renonce à bloquer devra payer en visibilité permanente (sans passer par le flux maintenance) et ferrer le geste destructif plutôt que de retarder l'effet.
- `.flow-maintenance` = « survit au mode réglage ». Retirer la classe d'un élément permanent est interdit.
- Aucun bouton d'annulation hors de la page pH (`bug-201 (C43)`) ; un lien est acceptable.

## Références

- Code : `data/index.html` (`#ph-card-tune`, `#ph_tune_remove_confirm`), `data/app.js` (`MAINTENANCE_PAGES.ph.cardIds`, `phTuneRemoveNeedsFriction`, `phTuneRemoveWarningParts`), `data/app.css` (`.field__hint--refus`, `.ph-tune-confirm__actions`, règle `.is-flow-maintenance`)
- Tests : `test/ph_card_permanent_vectors.js`, `test/ph_tune_remove_vectors.js`, `tools/check_ph_card_permanent_vectors.sh`
- Doc : [`docs/features/page-ph.md`](../features/page-ph.md#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710), [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md)
