# ADR-0135 — L'accueil montre, les réglages règlent ; la couleur n'est que sur la donnée

- **Statut** : Accepté
- **Date** : 2026-11-18
- **Décideurs** : Nicolas + Claude (`ux-ui-designer`, `pool-chemistry`, `architect`)
- **Spec(s) liée(s)** : [feature-195](../../specs/features/doing/feature-195-langage-visuel-du-tableau-de-bord.md) · [feature-161](../../specs/features/done/feature-161-choisir-le-mode-depuis-les-cartes.md) · [feature-164](../../specs/features/done/feature-164-regler-les-valeurs-depuis-les-cartes.md)

## Contexte

Le tableau de bord était devenu un **formulaire de réglage posé sur l'écran d'accueil** : la carte pH contenait trois boutons de mode, quatre boutons de consigne et un bouton Sauvegarder, et la mesure (32 px) occupait moins de place que les commandes (≈ 290 px). Relevé sur capture : **huit surfaces bleues** `#2563eb` sur un seul écran (dont trois pleine largeur), trois grammaires différentes pour dire l'état d'une carte, titres en capitales espacées, page de ≈ 1 880 px à 390 px de large.

Deux contraintes opposées pèsent sur la correction :

- l'utilisateur avait demandé, le 2026-10-13, de **choisir le mode depuis la carte** (« c'est pratique plutôt que d'aller dans la page de réglage ») : feature-161, 162, 164 ;
- `pool-chemistry` a posé en feature-161 le gate **C5** : aucune confirmation modale sur un changement de mode, la protection contre l'arrêt accidentel étant **la visibilité permanente du mode sur la carte**.

## Décision

1. **L'accueil montre, un volet règle, les pages de réglage configurent.** La mesure est l'élément dominant de chaque carte (48 px, virgule française, chiffres de largeur fixe).
2. **La couleur n'est que sur la donnée** (jauges, puce d'état). Le chrome est neutre ; la sélection d'un segment est à l'encre (`--text`) ; **au plus une action bleue par carte**. Cette règle engage **tous les écrans suivants**.
3. **Une pastille d'état unique par carte**, même grammaire pour les cinq cartes : le texte est toujours en `--text`, la couleur n'est que dans une puce de 8 px — aucun verdict porté par la couleur seule. **Vert** = fonctionnement attendu ; **gris** = repos ou information normale ; **ambre** = l'appareil s'écarte de son fonctionnement automatique attendu et cela demande un regard. Pour pH/ORP, ambre ⟺ `doseBlockVariant(...) === 'danger'` (tones `fault` / `unknown`), en réutilisant la table `DOSE_BLOCK_CODES` : « Filtration arrêtée », stabilisation, limites et anti-rafale (codes 2, 7, 8, 10-14) ne sont **jamais** ambre — sinon l'ambre s'allumerait toutes les nuits sur les deux cartes.
4. **Arbitrage de l'utilisateur : « Cacher celles qu'on règle rarement. »** Les commandes de pH et d'ORP (mode, consigne, Sauvegarder) passent derrière un bouton « Modifier » **replié**. La filtration garde **Forcer marche / Forcer arrêt** sur sa face (le geste du bord du bassin) et son sélecteur à 4 modes **entier** dans le volet ; « Revenir en Auto » apparaît quand elle est forcée. L'éclairage garde ses trois segments. La température n'a rien à régler.
5. **Le volet est piloté par le clic seul** : aucune fonction `render*` ne l'ouvre ni le ferme ; le bouton « Modifier » n'est jamais désactivé. Le texte du mode pH/ORP reste **permanent** dans la pastille (gate C5) et le texte C7 « La régulation automatique ne dose plus… » reste **hors du volet**, au-dessus du bouton. Les bandeaux « dosage bloqué » et « chloration de secours » restent au niveau de la vue, ni repliables ni masquables.
6. **Écrivain unique** pour chaque pastille (`setDashPill`), un seul formateur de nombre partagé entre `/data` et WebSocket (`fmtFr`) — piège de parité à quatre récidives.
7. **Portée tableau de bord uniquement.** Les jetons sont **scopés** `.card--status` (`--dash-radius`, `--dash-shadow`…) ; `--card-radius`, `--shadow`, `--card-border` (43 cartes de l'application) ne bougent pas.

## Alternatives considérées

- **Option A — aucune commande sur l'accueil, tout part vers les pages de réglage** (rejetée) : annule feature-161, 162 et 164, demandées explicitement par l'utilisateur ; supprime au passage le geste « Forcer » du bord du bassin.
- **Option B — statu quo recoloré** (rejetée) : mêmes commandes, couleurs neutres. La page reste un formulaire, la mesure ne gagne pas de place.
- **Option C intégrale — tout replier, filtration comprise** (écartée par l'utilisateur) : replie derrière un tap ce qu'il avait demandé à garder à portée. Il a **refusé les deux extrêmes** (A et statu quo) et tranché : replier ce qu'on règle rarement, garder ce qu'on touche au bord du bassin.
- **Ambre si « dosage bloqué » au sens large** (invalidée par `pool-chemistry`, condition B1) : voir décision 3.
- **Interrupteur ou bouton à libellé variable pour la filtration** (écartés) : l'interrupteur annonce un état alors qu'on change un *mode* ; le libellé variable est ambigu en Auto hors créneau. Montrer 2 segments sur 4 aurait **coupé un composant en deux**.

## Conséquences

### Positives
- La mesure se lit à bout de bras ; l'état se lit dans une seule grammaire ; page nettement plus courte.
- Un tap de plus pour régler pH/ORP — c'est le seul gain de protection mesurable contre l'arrêt accidentel de la régulation (le déplacement du texte de mode vers la pastille est jugé *préservé*, pas *renforcé*).

### Négatives
- ⚠️ **R1 — perte assumée** : l'avertissement « consigne pH à la borne haute » n'est plus visible en permanence sur l'accueil, il est dans le volet. Il **informe**, il ne protège pas (la protection réelle est le plafond du firmware) ; `renderPhTargetWarning` continue de s'exécuter volet replié.
- ⚠️ **R2 — non fait** : « Volume quotidien » et « Limite de sécurité » partent dans le volet alors qu'en mode Programme ils disent ce que l'appareil va faire.
- **Un tap de plus** pour changer un mode ou une consigne pH/ORP.
- **Incohérence provisoire accueil / réglages** : `.mode-select`, `.state-badge`, `.segmented--num` et `.card__option` ont deux apparences (encre sur l'accueil, bleu sur les pages de réglage). C'est un contexte d'emploi déclaré dans `DESIGN_SYSTEM.md`, pas une troisième forme.
- La puce « Hors gel » passe de l'ambre au **gris**, texte inchangé : ce n'est **pas** un correctif — le libellé annonce toujours une protection qui n'existe pas ([bug-197](../../specs/features/todo/bug-197-le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas.md)).

### Ce que ça verrouille
- Tout écran ajouté ou refondu **respecte « la couleur n'est que sur la donnée »** ; les pages de réglage s'aligneront dans un lot ultérieur **si la direction est validée** (hors ligne : `login.html`, `wifi.html`, `wizard.html` gardent leur ancien fond).
- Toute nouvelle carte d'accueil emploie la pastille unique et son vocabulaire de teintes ; toute nouvelle teinte ambre doit rentrer dans la définition « écart à regarder », sinon la couleur ne veut plus rien dire.
- Le motif « bouton de dérogation » (Forcer) est réservé à la carte d'accueil, et « Revenir en Auto » ramène **toujours** en Auto (pas de mémorisation côté navigateur).
- Test de non-régression B1 : `./tools/check_dash_pill_vectors.sh` (mutation exigée : passer un `tone: 'normal'` de `DOSE_BLOCK_CODES` en `fault` doit tuer un vecteur).

## Références

- [`data/app.css`](../../data/app.css) — `.dash-pill`, `.dash-act`, `.card__fold`, jetons `--dash-*`
- [`data/app.js`](../../data/app.js) — `setDashPill`, `dashModePillTone`, `dashFiltrationTone`, `fmtFr`, `bindDashFolds`
- [`test/dash_pill_vectors.js`](../../test/dash_pill_vectors.js)
- [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md) · [`docs/features/page-dashboard.md`](../features/page-dashboard.md)
