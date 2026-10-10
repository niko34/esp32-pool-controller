# ADR-0094 — Trois niveaux d'affichage d'un message : le bandeau fait autorité, le badge se tait, la pastille coexiste

- **Statut** : Accepté
- **Date** : 2026-10-10
- **Décideurs** : `architect` (confirmation de la règle et du mécanisme de silence)
- **Spec(s) liée(s)** : [feature-148](../../specs/features/doing/feature-148-un-message-un-seul-endroit.md) (un message, un seul endroit) ; suite distincte : feature-149 (valeur figée dans une cause de garde pH, défaut de source firmware, hors périmètre de cet ADR)

## Contexte

Le tableau de bord affichait, à 1 500 px d'intervalle vertical, deux fois la même phrase :
« pH 5.79 sous le plancher de sécurité (6.70) — dosage acide suspendu. » Une fois dans le
bandeau global `#dose-block-banner` (niveau 1), une fois dans le badge de la carte pH
(niveau 2). Signalé par l'utilisateur le 2026-10-10 sur capture téléphone.

Un mécanisme anti-redondance existait déjà et avait un trou : `DOSE_BLOCK_CODE_ALREADY_SOURCED`
(`data/app.js`) listait les codes déjà sourcés ailleurs dans le badge, mais ne couvrait pas le
code signalé (250, garde de sécurité pH/ORP) ni dix autres codes partageant le même défaut de
principe (watchdog inactif, capteur instable, calibration insuffisante, régulation non
automatique, anti-rafale, sonde figée/muette). Le bandeau (`_doseBlockRowData()`) n'a **aucun
filtre par code** : il affiche toute voie dont le ton n'est pas `none`. Tout code absent du
`Set` se duplique donc — ce n'était pas un défaut isolé sur un code, mais sur la méthode : une
liste de codes maintenue à la main, à côté d'un bandeau qui n'en maintient aucune.

Une seconde liste de codes « miroir » (recensant cette fois les codes affichés par le bandeau)
corrigerait le cas présent, mais **rejouerait exactement le même défaut** au prochain code
ajouté côté firmware : le bandeau n'a pas de filtre par code, donc rien ne garantit qu'une liste
tenue à la main en face de lui reste synchronisée. C'est la méthode qui a produit ce défaut, pas
une liste insuffisamment remplie.

## Décision

**Tout message d'état se range en trois niveaux :**

| Niveau | Où | Contenu |
|---|---|---|
| 1 — Bandeau global | tableau de bord **uniquement** | texte narratif, valeurs runtime, durée, actions (CTA). Fait autorité. |
| 2 — Badge de carte | tableau de bord **et** page dédiée | faits courts propres au capteur. Se tait sur une cause portée par le niveau 1 dans la vue courante. |
| 3 — Pastille de section (carte de dosage) | carte de dosage | forme courte (« injection en cours », « pause mélange »). Registre différent — coexistence normale. |

**Critère pour trancher un cas futur** : le texte raconte-t-il une cause (panne, garde,
attente) ? → niveau 1 prioritaire, le niveau 2 se tait sur cette cause. Est-ce un compteur de
capacité, court et sans récit ? → duplication tolérée.

**Le niveau 2 se tait sur toute cause effectivement portée par le niveau 1 dans la vue
courante ; le silence est accordé par une fonction pure dérivée du rendu réel du niveau 1,
jamais par une liste de codes dupliquée.** Concrètement : `doseBlockBannerCarries(prefix, json)`
relit le **même plan pur** que celui réellement rendu par le bandeau (`_doseBlockBannerPlan()`),
pas une classification statique par code. Si ce plan affirme que la ligne `prefix` sera
effectivement affichée (après fusion pH→ORP, après garde de présence DOM, après toute règle de
rendu actuelle ou future du bandeau), le badge se tait sur cette cause. **En cas de doute, on
duplique** : si l'un des trois nœuds DOM du bandeau est absent (pages `/ph` et `/orp`, qui
n'appellent jamais la chaîne menant au bandeau) ou si le plan ne couvre pas la voie demandée,
la fonction renvoie `false` et le badge parle — jamais l'inverse. Un message perdu est jugé pire
qu'un message répété.

**Exception permanente : les quotas.** Les codes 10 (limite journalière), 11 (limite horaire)
et 12 (limite de cycles) restent affichés aux **deux** niveaux. Ce ne sont pas des causes
racontées mais des compteurs de capacité, utiles à lire au niveau de la carte même quand le
bandeau existe déjà — ils empruntent un chemin de poussée distinct de celui concerné par cette
décision.

## Alternatives considérées

- **Une seconde liste de codes, miroir de la première** (recenser manuellement, à côté de
  `DOSE_BLOCK_CODE_ALREADY_SOURCED`, les codes que le bandeau affiche) — **rejetée**. Le
  bandeau n'a aucun filtre par code : il affiche toute voie dont le ton n'est pas `none`, après
  application de ses propres règles de fusion et de garde. Une liste miroir serait donc fausse
  dès le prochain code ajouté au firmware qui ne serait pas répercuté dans les deux listes à la
  fois — on rouvrirait exactement le trou qu'on vient de boucher, avec un code de plus la
  prochaine fois.
- **Un prédicat purement logique, sans garde de présence DOM** (déduire la couverture du seul
  plan, sans vérifier que les nœuds `#dose-block-banner` / `#dose-block-row-ph` /
  `#dose-block-row-orp` existent) — rejetée. Le plan est calculable sur une page qui n'a pas ces
  nœuds (`/ph`, `/orp`) : sans la garde, le badge s'y tairait à tort sur une cause que rien
  n'affiche. La garde de présence DOM, qui renvoie `false` dès qu'un nœud manque, est ce qui
  rend le principe « en cas de doute, on duplique » vérifiable plutôt que déclaratif.

## Conséquences

### Positives
- Un seul mécanisme (`doseBlockBannerCarries()` + `_doseBlockBannerPlan()`) couvre les **onze**
  codes concernés (1, 5, 6, 9, 13, 14, 16, 17, 20, 21, 250), pas seulement celui signalé —
  conséquence directe de dériver le silence du rendu réel plutôt que d'une énumération.
- Aucune page dédiée (`/ph`, `/orp`) n'est affectée : la garde de présence DOM les exclut par
  construction, sans condition par vue à maintenir.
- Un lecteur d'écran n'annonce plus deux fois la même phrase par deux régions `aria-live`
  distinctes sur le tableau de bord.
- Gain de lisibilité mesuré sur téléphone (390 px) : le badge pH passe de 46 px à 28 px de
  hauteur quand le bandeau porte déjà la cause.

### Négatives / dette assumée
- Le badge de carte dépend désormais, pour son silence, d'une fonction qui vit à côté du
  bandeau (`doseBlockBannerCarries`) plutôt que d'une donnée locale : toute évolution du rendu
  du bandeau qui ne passerait pas par `_doseBlockBannerPlan()` romprait silencieusement ce lien
  — d'où le garde-fou documenté dans le code (« toute évolution du rendu se fait dans le plan,
  jamais ailleurs »).
- La distinction entre `DOSE_BLOCK_CODE_ALREADY_SOURCED` (déjà sourcé ailleurs **dans le
  badge**, question statique par code) et `doseBlockBannerCarries()` (porté **par le bandeau**,
  question dynamique par vue) ajoute un second axe à comprendre pour quiconque touche à la
  classification des causes de blocage.

### Ce que ça verrouille
- **Aucun futur message d'état ne s'ajoute sans se ranger dans l'un des trois niveaux.** Un
  nouveau type de message (nouvelle bannière, nouvelle pastille) doit d'abord répondre : porte-
  t-il une cause narrative (niveau 1) ou un fait court propre au capteur (niveau 2) ou une forme
  brève de section (niveau 3) ?
- **Le silence d'un niveau 2 sur un niveau 1 ne peut plus s'implémenter par une liste de codes
  dupliquée.** Toute tentative de ce genre (comparer un code à une énumération maintenue à la
  main en face d'un bandeau qui n'en a pas) est une régression de cette décision — elle a déjà
  produit le défaut que cet ADR corrige.
- **L'exception quotas (10/11/12) est permanente et n'a pas vocation à s'étendre** : tout code
  candidat à une nouvelle exception doit d'abord démontrer qu'il s'agit d'un compteur de
  capacité, pas d'une cause racontée — le critère de la section Décision tranche ce cas.

## Références

- Code : [`data/app.js`](../../data/app.js) — `DOSE_BLOCK_CODE_ALREADY_SOURCED`,
  `dosingLimitSegments()`, `_doseBlockBannerPlan()`, `updateDoseBlockBanner()`,
  `doseBlockBannerCarries()`
- Spec : [feature-148](../../specs/features/doing/feature-148-un-message-un-seul-endroit.md)
- Tests : [`test/test_native_dose_banner_silence/test_dose_banner_silence.cpp`](../../test/test_native_dose_banner_silence/test_dose_banner_silence.cpp)
- Doc : [`docs/features/page-dashboard.md`](../features/page-dashboard.md#règle-des-trois-niveaux-daffichage-dun-message-feature-148), [`CHANGELOG.md`](../../CHANGELOG.md)
- ADR lié : [ADR-0049](0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) (code
  machine de cause de refus — la classification `DOSE_BLOCK_CODES` réutilisée ici, inchangée)
