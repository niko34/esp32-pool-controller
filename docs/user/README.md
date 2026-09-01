# Documents utilisateur

Documents destinés à l'**utilisateur final** de l'appareil — par opposition au
reste de `docs/`, qui documente le firmware pour ses développeurs.

## Convention

Ces documents sont **dérivés** : ils vulgarisent une source technique qui reste
la référence. En cas de doute ou de divergence, la source fait foi.

| Document | Source de vérité | Régénérer quand… |
|---|---|---|
| `guide-regulation-v<version>.pdf` | [`docs/subsystems/regulation-vue-d-ensemble.md`](../subsystems/regulation-vue-d-ensemble.md) | le comportement visible de la régulation change (nouvelle entrée CHANGELOG touchant la régulation) |
| `guide-d-utilisation-v<version>.pdf` | les 9 pages de [`docs/features/`](../features/) (`page-dashboard`, `page-ph`, `page-orp`, `page-temperature`, `page-filtration`, `page-lighting`, `page-dosages`, `page-settings`, `page-wizard`) | une page de l'interface change de comportement visible (entrée « Fonctionnalités » du CHANGELOG) |

- **Nom versionné obligatoire** (`-v2.32`, …) : un PDF décrit l'état du produit
  à une version donnée — son nom doit dire laquelle. Conserver l'ancien tant
  que des appareils de cette version existent.
- Génération : Claude Design, à partir du `.md` source **dans son état
  courant** (pas du CHANGELOG ni d'un état antérieur — leçon des relectures du
  01/09/2026 : quatre divergences trouvées entre le PDF et la mécanique réelle,
  dont une héritée d'un état pré-v2.32.0).
- Ces documents ne vont **jamais** dans `data/` (filesystem embarqué : 320 Ko).
