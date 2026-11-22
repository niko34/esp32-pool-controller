# ADR-0144 — L'appareil ne constate jamais la présence d'eau : il ne nomme que son absence

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas + Claude (`architect`, `pool-chemistry` sur le **texte**, `ux-ui-designer`, `code-reviewer`)
- **Spec(s) liée(s)** : feature-210 ; prolonge [ADR-0026](0026-mode-installation.md) (les trois archétypes de câblage et la résolution unique) ; ⭐ **le lot qui comblerait le manque est feature-209**, pas celui-ci

## Contexte

La garde de dosage lit un verdict unique, `resolveWaterPresent()` ([`src/dosing_logic.h`](../../src/dosing_logic.h)), dont la source est nommée par le code lui-même :

```cpp
enum class WaterSource : uint8_t {
  FiltrationCommanded,  // Managed : état commandé par PoolController
  PoweredAssumed,       // Powered : présumé par câblage
  ExternalSignal        // External : dernier signal externe
};
```

et dont l'entrée s'appelle `bool filtrationCommandedOn;`. ⛔ **Aucun des trois modes ne lit un débit** : l'appareil sait ce qu'il a **demandé**, ce que son **câblage impose**, ou ce qu'on lui a **dit**. Il ne sait jamais ce qui **coule**. Le mot « commandé » est dans le code ; il n'était **nulle part à l'écran**.

À l'écran, c'est l'inverse qui était écrit, **en trois exemplaires** : « Ce réglage **garantit qu'aucun produit n'est injecté sans circulation d'eau** » — carte « Mode d'installation » (Réglages), étape 3 de l'assistant de première mise en service, et guide utilisateur. Une **quatrième** occurrence survivait au chapitre 10 du même guide (« il conditionne la sécurité “jamais d'injection sans circulation d'eau” »). Même famille de défaut que le libellé « hors gel » ([ADR-0141](0141-l-appareil-est-temoin-du-froid-jamais-agent.md)), avec cette fois le verbe **garantit**.

Deux incidents réels du bassin de référence sont venus de cet écart, et aucune garde n'a rien vu :

| Date | Fait | Ce que la garde a vu |
|---|---|---|
| 23/08/2026 | 1ᵉʳ dosage réel, **bypass des sondes fermé**, 765 mL injectés, mesures non représentatives | « filtration commandée » ⇒ rien |
| 11/10/2026 | **Chlore injecté dans la dérivation ORP fermée**, sonde dans un hypochlorite ~1000× concentré | ⇒ rien |

Le lot qui corrige le texte a dû trancher une question que la fiche n'avait pas posée : **puisque l'appareil publie déjà son verdict** (`water_present` et `filtration_state_stale` voyagent dans la trame capteurs, [`src/ws_manager.cpp`](../../src/ws_manager.cpp)), **faut-il l'afficher ?** Il y avait de quoi dire, en permanence, *ce que l'appareil croit en ce moment*. [ADR-0140](0140-quand-un-gate-renonce-a-bloquer-la-contrepartie-est-la-visibilite-permanente.md) pousse même dans ce sens — *quand un gate renonce à bloquer, la contrepartie est la visibilité permanente*. ⚠️ Mais ici le gate **ne renonce pas** : il bloque, fail-closed strict. La question n'est donc pas la même, et elle mérite d'être tranchée une fois pour toutes : les gates ne gardent aucune mémoire entre deux passages, et « pourquoi n'affiche-t-on pas l'état de l'eau ? » reviendra.

## Décision

**L'appareil ne constate jamais la présence d'eau. L'interface parle quand l'eau MANQUE, et se tait quand elle est supposée présente.**

1. ⛔ **`water_present` n'est affiché nulle part lorsqu'il autorise.** Aucune ligne « eau présente : oui », aucune pastille verte, aucun état positif dérivé de ce champ. Afficher une autorisation transformerait une **déduction** en **constatation** — c'est précisément le défaut que ce lot retire, rejoué sur le signal le plus critique du produit. Le champ reste publié (`/data`, WebSocket, MQTT) pour les intégrations : ⛔ publier n'est pas afficher.
2. ✅ **L'absence, elle, se nomme toujours**, par le canal existant des refus (`DoseRefusal`, [ADR-0137](0137-un-etat-qui-court-circuite-le-dosage-nomme-sa-cause-dans-le-canal-de-refus.md)) : « Filtration arrêtée », « Signal filtration périmé ». Une absence est **vérifiée** — elle découle d'un état commandé connu, ou d'un silence mesuré — là où une présence ne l'est pas. L'asymétrie est dans la physique, pas dans l'ergonomie.
3. **Ce qui est affiché en permanence, c'est la LIMITE, pas l'état.** Un `.callout--info` « Ce que l'appareil sait » (Réglages › Installation) porte, **jamais replié**, « L'appareil ne mesure pas l'eau qui circule. Il se fie au raccordement indiqué ici. Une vanne fermée ou une pompe désamorcée ne lui apparaissent pas. » — un **fait** vrai tous les jours, donc ⛔ ni `aria-live`, ni variante ambre (réservée aux **conditions**), ni formulation alarmante. Seules les **précautions** se replient.
4. **Un texte entier par raccordement, et le silence sur un mode inconnu.** `installScopeText()` ([`data/app.js`](../../data/app.js)) rend trois chaînes **distinctes** et `null` pour toute autre valeur. ⛔ **Le texte ne se replie JAMAIS sur `managed`**, bien que `loadConfig()` le fasse (`cfg.install_mode || "managed"`) pour alimenter `applyInstallMode()` : **replier pour MASQUER une interface est sûr, replier pour AFFIRMER un fait ne l'est pas.**
5. **Vocabulaire opposable, tenu par la CI.** ⛔ Ni « vérifie », ni « détecte », ni « contrôle », ni « garantit », ni « surveille », ni « s'assure », ni « confirme » à propos de la circulation — l'appareil **commande**, **présume** ou **reçoit**. ⛔ Aucun texte ne présente un raccordement comme dangereux (les trois sont normaux), ⛔ aucun n'invite à couper la régulation. `tools/check_install_scope_vectors.sh` (job CI `install-scope-vectors`) garde les trois interdits **et l'absence de la promesse fausse dans les quatre fichiers** où elle vivait.
6. **Une information vraie dans les trois raccordements ne vit pas sur une page masquée dans deux d'entre eux.** `applyInstallMode()` masque `#nav-filtration`, `#dashboard-filtration-card` et `#filtration-content` dès que le mode n'est pas `managed` : la page Filtration est **injoignable par la navigation** en `powered` et `external`. L'encadré vit donc dans **Réglages › Installation** ; la page Filtration ne reçoit qu'une phrase **absorbée** dans son aide existante, et sa note d'une ligne cesse d'être un cul-de-sac en indiquant où se change le réglage.

## Alternatives considérées

- **Afficher l'état courant de `water_present`, qualifié** (rejetée) — « Eau présumée présente (filtration commandée) ». Défendable : le champ est déjà servi, et ADR-0140 valorise la visibilité permanente. ⛔ Écartée pour trois raisons. (a) **Le qualificatif ne survit pas à la lecture** : l'œil retient « eau présente » et oublie « présumée » — c'est exactement ce qui s'est produit avec « hors gel » (ADR-0141) et avec « garantit » ici même. (b) **C'est une ligne verte permanente** : une information qui dit toujours la même chose cesse d'être lue, et le jour où elle bascule, personne ne la regarde. (c) ⚠️ **En `powered`, elle vaudrait `true` 100 % du temps** — un indicateur dont la valeur est constante n'informe de rien, mais rassure.
- **Une pastille d'état « circulation » sur le tableau de bord** (rejetée) — même défaut que ci-dessus, aggravé par la place : l'accueil **montre** ([ADR-0135](0135-l-accueil-montre-les-reglages-reglent-la-couleur-n-est-que-sur-la-donnee.md)), et la couleur n'y est que sur la **donnée**. Une pastille verte sur une déduction colorerait une non-mesure.
- **Afficher l'absence ET la présence, pour la symétrie** (rejetée) — la symétrie est précisément ce qui est **faux** : l'absence est établie (état commandé `OFF`, ou silence de plus de 3 min du tiers), la présence ne l'est pas. Traiter les deux de la même façon suppose une mesure qui n'existe pas.
- **Poser l'avertissement sur la page Filtration, près du mode Programme** (rejetée, c'était la proposition initiale de la fiche) — la page est masquée en `powered` et `external`. ⚠️ **Le mode le plus aveugle dans le logiciel aurait eu le moins d'endroits pour être prévenu.**
- **Un encadré ambre permanent** (rejetée) — l'ambre est réservé aux **conditions** dans ce projet (froid, stock faible). Un ambre permanent sur un réglage structurel désensibiliserait l'ambre partout ailleurs et présenterait comme dangereux un mode qui est le mode normal. ⚠️ *Une alerte dont on apprend qu'elle ne veut rien dire ne se lit plus le jour où elle veut dire quelque chose.*
- **Replier tout l'encadré derrière un `<details>`** (rejetée) — l'utilisateur n'ouvre pas ce qui est replié (acquis de feature-170). La **limite** reste dépliée ; seules les précautions se replient.
- **Ajouter la garde manquante plutôt que la dire** (hors périmètre, pas rejetée) — c'est feature-209. ⛔ Ce lot ne touche pas une ligne de `src/` : dire ce qu'on ne sait pas et apprendre à le savoir sont deux travaux différents, et le premier ne doit pas attendre le second.

## Conséquences

### Positives
- Plus aucune promesse de vérification de débit dans le produit, l'assistant ou les guides ; le mot du code (« commandé ») est enfin celui de l'écran.
- La précaution qui agit **avant** une injection — ouvrir vannes et dérivations — est dite à l'endroit et au moment où elle est actionnable (assistant, étape 3).
- Le choix du raccordement est **décrit par ce qu'il permet de savoir**, pas seulement par le câblage : trois textes distincts, rattachés au `radiogroup` par `aria-describedby`.
- ⛔ Aucun champ firmware ajouté, aucune ligne de `src/` modifiée (hors `version.h`) : les clés nécessaires étaient déjà servies.

### Négatives / dette assumée
- ⚠️ **L'utilisateur ne peut toujours pas savoir, à l'écran, si l'appareil s'autorise à doser en ce moment** — sauf quand il **refuse**, où la cause est nommée. C'est le prix assumé de la règle ; il se paie à chaque diagnostic.
- Le mode `powered` reste le seul sans aucune garde logicielle (`waterPresent = true` en permanence) ; l'encadré le dit, il ne le répare pas. ⚠️ Sa protection réelle est **le câblage** (pompe à l'arrêt ⇒ appareil et pompes doseuses hors tension, ADR-0026), pas le logiciel.
- La phrase de la page Filtration n'est visible qu'en `managed` — elle vit dans une carte masquée ailleurs. Acceptable parce que l'information complète est dans Réglages, mais ⛔ ce n'est pas une duplication fiable.
- ⚠️ **[ADR-0026](0026-mode-installation.md) porte la même formule trop forte** (« plus de risque d'injection en eau stagnante » pour le mode `external`) et ⛔ **n'est pas modifiable** : un ADR accepté est immuable. Le présent ADR la **rectifie** — la garde de fraîcheur évite de doser sur un **signal perdu**, pas d'injecter dans une eau stagnante. Un signal `ON` récent mais faux autorise le dosage.
- `docs/user/src/guide-regulation.html` énonce encore, dans ses principes, « Jamais de dose sans eau en circulation » : formulation de même famille, **hors périmètre de ce lot**, à reprendre avec feature-209.

### Ce que ça verrouille
- ⛔ **Aucune surface (UI, MQTT auto-discovery, écran déporté) ne peut afficher un état positif de présence d'eau** sans superseder cet ADR, et sans répondre aux trois raisons (a)(b)(c) ci-dessus.
- ⛔ **Un texte ne se replie jamais sur une valeur par défaut quand il AFFIRME un fait.** Le repli reste légitime pour masquer ou désactiver une interface.
- ⛔ Le vocabulaire de perception est interdit à propos de la circulation dans l'interface, les guides et le journal. Le contrôle est automatisé ; le contourner demande de modifier le harnais, geste visible en revue.
- ⭐ **feature-209 (capteur de circulation) supersedera cet ADR**, pas l'amendera : le jour où une **constatation** existe, la règle « ne jamais affirmer la présence » n'a plus lieu d'être — mais elle devra alors distinguer *mesuré* de *déduit*, appareil par appareil.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) (`WaterSource`, `filtrationCommandedOn`, `resolveWaterPresent`), [`src/filtration.cpp`](../../src/filtration.cpp) (`resolveWaterPresence`), [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`install_mode`, `water_present`, `filtration_state_stale`), [`data/app.js`](../../data/app.js) (`installScopeText`, `renderInstallScope`, `applyInstallMode`), [`data/index.html`](../../data/index.html) (`#install-scope`, `#filtration-mode-scope`, `#filtration-unmanaged-note`), [`data/wizard.html`](../../data/wizard.html) (étape 3)
- Tests : `test/install_scope_vectors.js`, `tools/check_install_scope_vectors.sh` (CI `hygiene-specs.yml`, job `install-scope-vectors`)
- Doc : [`docs/features/page-settings.md`](../features/page-settings.md), [`docs/features/page-filtration.md`](../features/page-filtration.md), [`docs/subsystems/filtration.md`](../subsystems/filtration.md), [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md), [`docs/user/src/guide-utilisation.html`](../user/src/guide-utilisation.html)
- ADR liés : [ADR-0026](0026-mode-installation.md) (rectifié sur un point, voir ci-dessus), [ADR-0137](0137-un-etat-qui-court-circuite-le-dosage-nomme-sa-cause-dans-le-canal-de-refus.md), [ADR-0140](0140-quand-un-gate-renonce-a-bloquer-la-contrepartie-est-la-visibilite-permanente.md), [ADR-0141](0141-l-appareil-est-temoin-du-froid-jamais-agent.md)
