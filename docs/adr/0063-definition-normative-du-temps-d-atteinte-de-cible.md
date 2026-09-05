# ADR-0063 — Définition normative du temps d'atteinte de cible : minutes de circulation depuis la première grandeur de décision plausible du jour, tenue 30 min, un départ par jour

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : `architect`, `pool-chemistry` (gate bloquant), `embedded-developer`, `code-reviewer`, utilisateur
- **Spec(s) liée(s)** : [feature-096](../../specs/features/done/feature-096-indicateur-temps-d-atteinte-de-cible.md) (item N5 du plan 2026-09)

## Contexte

Le plan de septembre 2026 enchaîne plusieurs versions de la régulation
(feature-094 porte de session, feature-085 feedforward, feature-084 intégrale,
feature-086 limite d'actionneur). Chacune prétend faire converger le bassin plus
vite. **Rien dans l'appareil ne le mesure.**

Jusqu'ici, le temps d'atteinte se lisait à la main dans un CSV exporté, sur un
Mac qui devait rester allumé, avec une définition différente à chaque analyse :
quelle grandeur observer ? à partir de quand ? pendant combien de temps la cible
doit-elle tenir ? Deux matinées dépouillées à la main (02 et 04/09) donnent
~4 h 45 (pH) et ~5 h 50 (ORP) pour résorber 0,1-0,2 d'écart — mais ces chiffres
ne sont **comparables ni entre eux ni avec ceux de demain** tant que la
définition n'est pas figée.

Il faut trancher **maintenant**, avant le déploiement groupé (094 + 096 + 095 +
bug-089), parce que cette métrique va **arbitrer les comparaisons de versions**
du plan entier. Une métrique qu'on redéfinit après coup ne prouve rien.

## Décision

Le **temps d'atteinte de cible** d'une régulation, pour une journée civile
donnée, est le nombre de **minutes de circulation** écoulées entre :

- **t0** — le premier instant du jour où la **grandeur de décision** de cette
  régulation existe, est **plausible** et où l'eau circule ; **un seul départ
  par jour civil**, jamais reposé ;
- et le **début** de la première période où l'écart à la consigne **effective**
  reste sous le **seuil de démarrage vivant** de la régulation pendant
  **30 minutes de circulation consécutives** (`kTimeToTargetHoldMin`).

Cette définition se décline en six règles normatives.

**R1 — On juge sur la GRANDEUR DE DÉCISION, pas sur la mesure filtrée.** Un juge
qui n'observe pas la grandeur sur laquelle le régulateur agit ne mesure pas le
même objet. La règle est **uniforme** ; sa réalisation est **asymétrique** parce
que les deux régulateurs le sont : pour le pH c'est le **signal lent** de la
feature-094 (moyenne 10 min, `PumpController::getPhDecisionValue()`), pour l'ORP
c'est la **filtrée**, **qui EST sa grandeur de décision**.

**R2 — « Atteint » signifie « la régulation ne redémarrerait pas ».** La bande
est celle des **seuils de démarrage vivants** (`phStartThreshold` = 0,05 pH,
`orpStartThreshold` = 15 mV), relus à chaque tick. Définition opérationnelle,
pas arbitraire. Une sortie de bande remet la **tenue** à zéro, **jamais le
chronomètre**.

**R3 — La base de temps est `SafetyLimits::filtTotalMin`** (feature-093),
réutilisée telle quelle et en lecture seule. Elle compte des minutes de
**PRÉSENCE D'EAU**, pas de « filtration en marche » — et elle doit être nommée
honnêtement : **« minutes de circulation » partout, jamais « de filtration »**.
C'est l'assiette physiquement juste : sans circulation, ni mélange ni mesure
représentative, la régulation ne progresse pas.

**R4 — On juge contre la consigne EFFECTIVE, on surveille la consigne
CONFIGURÉE.** Un **boost chlore ne lève PAS** le drapeau « consigne modifiée » :
juger contre 530 mV pendant que le régulateur vise 630 déclarerait une atteinte
fictive. Le boost est tracé par son propre bit (bit 1).

**R5 — Règle de partage du bit 4 « régulation non automatique » : ce que le
régulateur CHOISIT compte ; ce qu'on lui IMPOSE de l'extérieur porte le bit.**
Quota journalier, limite horaire, actionneur sous-dimensionné ⇒ **aucun bit**,
vraie non-atteinte, **c'est précisément ce que la métrique doit voir**. Mode
manuel, mode maintenance, assistant de mesure de débit ⇒ **bit 4**. Les journées
marquées bit 4 sont **exclues des moyennes de comparaison de versions**.

**R6 — Garde de vraisemblance.** L'indicateur **refuse de poser t0** si l'écart
initial dépasse **1,0 pH** ou **300 mV** (avertissement unique par voie et par
jour), et pose t0 au **premier échantillon plausible**. Elle refuse seulement de
démarrer : **elle n'invente jamais de valeur**.

**Garde anti-dérive (condition d'acceptation du gate `pool-chemistry`)** :

> Le KPI est un **instrument**. Aucune de ses sorties n'est entrée de la boucle :
> toute rétroaction (kp, seuils, consignes, durées) exige une **nouvelle spec**
> et un **gate `pool-chemistry`**. Le test de grep matérialise l'interdit ; le
> supprimer, c'est fermer la boucle.

## Alternatives considérées

- **Mesurer sur la filtrée brute des deux côtés** (rejetée) — elle **mesure un
  objet que le régulateur pH n'utilise pas** depuis la feature-094 : la décision
  d'ouvrir et de fermer une session se prend sur le signal lent. Et son bruit la
  rend inexploitable comme critère de tenue : σ = 0,043, fluctuation ±0,08 à
  10 min ([ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md))
  **contre un seuil de 0,05** — la tenue de 30 min serait cassée en permanence
  par le bruit, et l'indicateur déclarerait « non atteinte » des journées
  parfaitement régulées.
- **Un lissage propre au KPI** (rejeté) — recrée exactement le même défaut sous
  un autre nom : un second filtre, avec ses propres constantes, à maintenir en
  cohérence avec celui de la régulation. Deux définitions du « pH courant »
  divergeraient au premier réglage.
- **Base de temps murale** (rejetée) — une nuit sans circulation ferait vieillir
  une journée de régulation. Une journée de 6 h de filtration et une de 14 h ne
  seraient pas comparables, alors que c'est exactement ce qu'on veut comparer.
- **Base « filtration en marche » plutôt que présence d'eau** (rejetée) — sur
  les installations en mode d'installation externe
  ([ADR-0026](0026-mode-installation.md)), l'eau peut circuler sans que le relais
  de filtration de l'appareil soit commandé. `filtTotalMin` compte la **présence
  d'eau résolue**, qui est la condition physique réelle du mélange et de la
  mesure. Recréer un compteur « relais ON » aurait dupliqué une base de temps
  existante pour un critère moins juste.
- **Le boost lève le drapeau « consigne modifiée »** (rejetée) — mélange deux
  circonstances de nature différente dans un même bit, et rend impossible de
  filtrer les journées de boost sans filtrer aussi les changements de consigne.
  Deux bits distincts coûtent zéro octet (le champ est un `uint8`).
- **Namespace NVS mutualisé avec `pool-daily`** (rejetée) — l'argument n'est ni
  l'espace ni la corruption de page (les namespaces partagent les mêmes pages
  physiques) : c'est le **droit de destruction en masse**. Le namespace est
  l'unité de `clear()`. **Moindre privilège : un non-garde-fou ne détient pas la
  clé d'écriture des garde-fous.** Un module d'instrumentation ne doit pas
  détenir de handle en écriture sur le namespace des compteurs de dosage.
  `filt_total` y est lu en **lecture seule**.
- **État vivant en RAM RTC seule** (rejetée) — la RAM RTC ne survit pas au
  `POWERON` : une coupure secteur en pleine journée aurait effacé t0, et la
  journée aurait redémarré à zéro avec un temps d'atteinte faussement court.
  D'où la **persistance à deux étages** : `t0FiltMin`, `e0`, la date et les
  drapeaux écrits en NVS **une seule fois**, à la pose de t0 (c'est l'unique
  donnée irrécupérable), la clôture écrivant la seconde et dernière fois —
  **exactement 2 écritures par jour et par régulation**. Tout le reste se
  recalcule.
- **`#error` de compilation pour interdire la dépendance inverse** (rejetée) —
  il aurait cassé le build du module lui-même, qui doit inclure
  `pump_controller.h` pour lire la grandeur de décision. Remplacé par un **test
  de grep bidirectionnel** exécuté à chaque `pio test` : aucune occurrence de
  `time_to_target` ni de `ttt` dans `src/pump_controller.{h,cpp}`.

## Conséquences

### Positives

- Les comparaisons de versions du plan 2026-09 reposent sur **une seule
  définition**, embarquée, journalisée et archivée — plus de CSV externe, plus de
  Mac allumé, plus de définition qui glisse d'une analyse à l'autre.
- La métrique **voit** les vraies non-atteintes (quota, limite horaire,
  actionneur sous-dimensionné) au lieu de les excuser : c'est exactement ce
  qu'elle doit dénoncer.
- Les journées **non comparables** sont identifiées par un champ de bits, donc
  filtrables mécaniquement plutôt qu'à l'œil.
- Coût NVS borné et prouvé : **2 écritures/jour/régulation**, 188 octets.
- Le sens unique est **vérifié à chaque `pio test`**, pas seulement écrit dans un
  commentaire.

### Négatives / dette assumée

- **Le zéro du chronomètre pH est indexé sur `kPhDecisionWindowMs`** : le signal
  lent n'existe pas avant que sa moyenne se soit constituée, donc t0 pH démarre
  **~10 min après** l'ouverture de la porte de mesure. **Toute modification
  future de cette constante crée une discontinuité de série** entre les journées
  archivées. Un test le démontre : la même courbe du 04/09/2026 donne **233 min**
  si t0 part du brut et **223 min** sur la grandeur de décision — les 10 min
  d'écart sont exactement la fenêtre. Atténuation : l'anneau NVS porte une
  estampille `serieStamp` et le démarrage émet un warning explicite si elle a
  changé — la discontinuité est **lisible plutôt que silencieuse**, elle n'est
  pas supprimée.
- Les minutes des **journées orphelines** (appareil éteint plus d'une journée
  civile) sont irrécupérables : elles sont closes à **0 min** en non-atteinte.
  Mieux vaut une journée manquante qu'une journée fausse.
- Une journée peut être déclarée « non atteinte » alors que la régulation a fait
  tout ce qu'elle pouvait sous ses propres brides. **C'est voulu** (R5) — mais
  cela impose de lire le KPI avec les bandeaux de limite d'actionneur
  ([ADR-0056](0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) à côté.
- La tenue de 30 min **retarde** la déclaration d'atteinte : l'UI annonce « en
  cours » pendant une demi-heure après la convergence réelle. Compromis assumé —
  le temps rendu est celui du **début** de la tenue, la valeur n'est pas faussée.

### Ce que ça verrouille

- **L'encodage de `state` et `flags` est figé dès le premier déploiement.** Ils
  sont archivés en NVS *et* servis à l'UI : ce sont des contrats. `state` 4-255
  et les bits 5-7 sont **réservés** ; un client teste des bits, jamais l'égalité.
  Toute évolution de `TttDay` / `TttHistory` incrémente `kTttHistorySchema` et
  **rejette** les enregistrements de l'ancien schéma.
- **`kTimeToTargetHoldMin`, `kTimeToTargetHistoryDays`,
  `kTimeToTargetOffDayEndMin`, `kTttMaxPlausibleE0Ph/Orp` et
  `kPhDecisionWindowMs` ne sont plus de simples réglages** : les modifier change
  le sens de toutes les journées déjà archivées.
- **Le sens de la dépendance est verrouillé** : `time_to_target` → régulation,
  jamais l'inverse. Supprimer le test de grep revient à autoriser la fermeture de
  la boucle.
- Le namespace NVS `pool-kpi` est **disjoint** de `pool-daily` : aucun module
  d'instrumentation futur ne doit ouvrir `pool-daily` en écriture.
- Aucun topic MQTT n'est publié pour cette métrique dans cette version — un ajout
  futur devra passer par le coût de l'auto-discovery (cf. bug-080 lot B).

## Références

- Code (pur) : [`src/time_to_target_logic.h`](../../src/time_to_target_logic.h), [`src/time_to_target_logic.cpp`](../../src/time_to_target_logic.cpp)
- Code (coquille) : [`src/time_to_target.h`](../../src/time_to_target.h), [`src/time_to_target.cpp`](../../src/time_to_target.cpp)
- Constantes : [`src/constants.h`](../../src/constants.h) — `kTimeToTarget*`, `kTttMaxPlausibleE0*`
- Transport NVS : [`src/config.cpp`](../../src/config.cpp) — `loadTttHistory()` / `saveTttHistory()`, namespace `pool-kpi`
- Garde de sens unique : `test/test_native_time_to_target/test_time_to_target.cpp` — `test_H_pump_controller_ignore_totalement_l_indicateur`
- Spec : [`specs/features/done/feature-096-indicateur-temps-d-atteinte-de-cible.md`](../../specs/features/done/feature-096-indicateur-temps-d-atteinte-de-cible.md)
- Doc composant : [`docs/subsystems/time-to-target.md`](../subsystems/time-to-target.md)
- ADR amont : [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) (signal lent de décision pH), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0056](0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) (limite d'actionneur, non marquée bit 4)
