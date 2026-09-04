# Documents utilisateur

Documents destinés à l'**utilisateur final** de l'appareil — par opposition au
reste de `docs/`, qui documente le firmware pour ses développeurs.

## Convention

Ces documents sont **dérivés** : ils vulgarisent une source technique qui reste
la référence. En cas de doute ou de divergence, la source fait foi.

| Document | Source de vérité | Régénérer quand… |
|---|---|---|
| `guide-regulation-v<version>.pdf` | [`docs/subsystems/regulation-vue-d-ensemble.md`](../subsystems/regulation-vue-d-ensemble.md), via la source imprimable [`src/guide-regulation.html`](src/guide-regulation.html) (maintenue par Claude au fil des cycles) | le comportement visible de la régulation change (nouvelle entrée CHANGELOG touchant la régulation) |
| `guide-d-utilisation-v<version>.pdf` | les 9 pages de [`docs/features/`](../features/), via la source imprimable [`src/guide-utilisation.html`](src/guide-utilisation.html) + les captures réelles de [`screenshots/`](../../screenshots/) | une page de l'interface change de comportement visible (entrée « Fonctionnalités » du CHANGELOG) — refaire aussi la capture de l'écran concerné |

- **Nom versionné obligatoire** (`-v2.34`, …) : un PDF décrit l'état du produit
  à une version donnée — son nom doit dire laquelle. La flotte étant
  **mono-appareil**, on ne conserve **que la version courante** : les PDF de la
  version précédente sont supprimés au même cycle que la régénération (pas
  d'archive à maintenir). État courant : **v2.34**.
- Génération : **filière locale** pour les DEUX guides depuis le 02/09/2026 —
  les sources `src/guide-*.html` partagent le style de la collection
  (`src/collection.css`, calqué sur l'identité du manuel Claude Design
  d'origine) et sont maintenues par Claude à chaque cycle qui change le
  comportement documenté, puis :
  ```
  ./tools/build_user_pdfs.sh          # les deux PDF d'un coup
  ```
  Le script lit la version dans `src/version.h` (et l'injecte dans couverture,
  pied de page et colophon), redimensionne les captures d'écran à la
  résolution d'impression (1200 px ≈ 180 dpi — les originaux de `screenshots/`
  restent intacts) et génère via Chrome headless. Gain mesuré : guide
  d'utilisation 13,9 → 3,7 Mo sans perte visible.
  Toujours générer depuis les `.md` sources **dans leur état courant** (pas du
  CHANGELOG ni d'un état antérieur — leçon des relectures du 01/09/2026 :
  quatre divergences trouvées entre le PDF et la mécanique réelle, dont une
  héritée d'un état pré-v2.32.0). Claude Design n'est plus dans la boucle
  (ses canvas restent sur claude.ai, à titre d'archive).
- Ces documents ne vont **jamais** dans `data/` (filesystem embarqué : 320 Ko).
