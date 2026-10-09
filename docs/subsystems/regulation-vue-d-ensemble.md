# Régulation chimique — vue d'ensemble (pH et chlore/ORP)

**Commencer ici pour comprendre la régulation.** Ce document raconte la chaîne de bout en bout et renvoie vers les détails ; il ne les duplique pas. La référence exhaustive est [pump-controller.md](pump-controller.md) (sections empilées au fil des versions) et les ADR cités. Pour chaque étage : ce qu'il décide, ce qu'il s'interdit, où est le détail.

Le principe directeur, présent à chaque étage : **fail-closed**. Toute donnée douteuse, tout état ambigu, toute garde non prouvée se résout par « on ne dose pas ». Un bassin sous-dosé se rattrape ; un surdosage, non.

---

## 1. Le voyage d'une correction, de bout en bout

```
        CHAÎNE pH                                    CHAÎNE ORP (chlore)
        ─────────                                    ───────────────────
  électrode pH                                 électrode ORP
        │ signal analogique haute impédance          │ signal analogique
        ▼                                             ▼
  [0] module Atlas EZO pH                      [0] module Atlas EZO ORP
      acquisition + calibration À BORD             acquisition + calibration À BORD
      compensation T° (Nernst) à bord              (potentiométrique direct, sans T°)
        │ valeur DÉJÀ conditionnée,                   │ idem
        │ lue en I²C toutes les 5 s                   │
        │ = « brut » firmware (+ offset en sortie)    │ = « brut » firmware
        ▼                                             ▼
  [1] filtres de validité                      [1] filtres de validité
      rejet aberrations, médiane 13 + EMA          rejet aberrations, médiane 13 + EMA
      NaN si stale > 20 s                          NaN si stale > 20 s
      gel si eau stagnante / purge                 gel si eau stagnante / purge
        │ pH filtré CORRIGÉ                           │ ORP filtré
        ▼                                             ▼
  [2] gardes de seuils pH ────────────────────► [2] inhibition par pH haut
      refus d'acidifier < 6,70 corrigé              STOP chlore si pH corrigé > 7,60
      refus d'alcaliniser > 7,60 corrigé            OU pH brut > 8,10
      butées brutes 6,20 / 8,10                     OU pH inconnu (mode auto)
        │                                             │
        ▼                                             ▼
  [3] hystérésis de session                    [3] hystérésis de session
      start 0,05 / stop 0,01 pH                    start 15 mV / stop 2 mV
      décidée sur un SIGNAL LENT (10 min)
      + durée minimale 30 min murales
        │                                             │
        ▼                                             ▼
  [4] demande (mL/min moyen)                   [4] demande (mL/min moyen)
      FF + kp·e + ki·∫(e − stop)                   kp·e   (P pur, Ki = 0, Kd = 0)
        │                                             │
        ▼                                             ▼
  [5] plafonds                                 [5] plafonds
      amplitude, soutenable, FF ≤ 60 %,            amplitude, limite horaire,
      limite horaire, quota journalier             quota journalier (boost : ×1,5 borné)
        │                                             │
        ╘═════════════════╤═══════════════════════════╛
                          ▼
  [6] ordonnanceur d'impulsions (COMMUN, par chimie)
      période fixe 60 s · pouls proportionnel à la demande · pouls minimal 10 s
      accumulateur sous le plancher · kick 400 ms à duty 255 · amplitude calibrée
      plancher de rotation MIN_ACTIVE_DUTY · gate de caractérisation fail-closed
        ╒═════════════════╧═══════════════════════════╕
        ▼                                             ▼
  [7] pause mélange 15 min                     [7] pause mélange 20 min
      (armée à l'ARRÊT de l'injection)             (idem, plus conservateur)
        ╘═════════════════╤═══════════════════════════╛
                          ▼
  [8] comptabilité des volumes (COMMUN)
      crédit par tick au débit pesé (kick au majorant de flotte) · quotas journaliers
      · suivi bidon · fenêtre horaire glissante — + anneau de charge 7 j (pH SEUL)
                          ▼
  [9] gardes transverses (évaluées EN AMONT à chaque tour, fail-closed)
      présence d'eau · watchdog · calibration EZO · stabilisation ·
      anti-cycling / anti-rafale · OTA / maintenance / assistant de débit
                          ▼
  [10] observabilité : /data ≡ WebSocket ≡ MQTT (parité stricte),
       codes machine figés, journal edge-triggered
```

> Le schéma suit le **flux logique** ; dans `update()`, les gardes [9] sont évaluées **avant** tout calcul (et les gardes [2] avant `canDose()`). Voir l'ordre exact dans [Boucle d'exécution](pump-controller.md#boucle-dexécution).

**Vocabulaire — deux échelles de temps, à ne pas confondre :**

| Terme | Ce que c'est | Échelle |
|---|---|---|
| **Pouls** | Une marche de pompe dans une période de 60 s — proportionnelle à la demande, jamais < 10 s (en dessous, la petite dose est accumulée, versée plus tard, jamais perdue) | secondes |
| **Injection** — la doc dit aussi **session** (contexte hystérésis/feedforward) ou **cycle** (contexte compteur de cycles/jour) : trois noms pour le MÊME épisode | L'épisode complet du démarrage à l'arrêt du dosage, composé d'un ou plusieurs pouls enchaînés de minute en minute. Ouvert par l'hystérésis [3], fermé par elle ou par un plafond de session. C'est **lui** que compte la limite de cycles/jour, et c'est **son arrêt** qui arme la pause mélange | minutes |

La confusion classique : croire que la pause mélange suit **chaque pouls**. Non — les pouls s'enchaînent de minute en minute pendant une injection ; la pause ne s'arme qu'à la **fin de l'épisode**, et bloque le *suivant* (~1 injection / 15-20 min, donc).

### [0] Le module EZO — la mesure commence AVANT le firmware

**Décide** : la valeur physique elle-même. Ce que le firmware appelle « brut » n'est pas un échantillon d'ADC : c'est une mesure **déjà acquise, calibrée et conditionnée** par un circuit Atlas Scientific EZO dédié (pH `0x63`, ORP `0x62`), qui possède son propre processeur et son propre étage analogique de précision — c'est lui qui gère le signal haute impédance de l'électrode, hors de portée d'un ADC générique. La **calibration vit dans le module** (points Cal 1/2/3 stockés à bord, pente/zéro relisibles — [ADR-0014](../adr/0014-migration-atlas-ezo.md), feature-021 : l'ESP32 ne persiste plus aucune donnée de calibration pH/ORP). Côté pH, la **compensation de température** (équation de Nernst) est calculée à bord : la séquence `RT,<temp>` + `R` retourne une valeur déjà compensée. La fenêtre d'acquisition de **900 ms** après chaque commande (`kEzoReadDelayMs`, imposée par le firmware Atlas) est le temps que le module consacre à sa propre acquisition — la transaction est atomique sous mutex I²C pour qu'aucune autre lecture ne s'y intercale.

**S'interdit** : le firmware ne « corrige » jamais cette mesure en amont — l'offset logiciel pH s'applique en **sortie** ([ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)), et toute recalibration passe par le module lui-même (commandes Cal), jamais par une transformation côté ESP32.

**Détail** : [sensors.md](sensors.md) (classe `AtlasEzoSensor`, séquence RT+R, mutex I²C), [ADR-0014](../adr/0014-migration-atlas-ezo.md).

### [1] Lecture et filtres de validité

**Décide** : la valeur qui entre dans la régulation. Le firmware lit les modules EZO toutes les 5 s (`kPhOrpSensorIntervalMs`) ; la valeur reçue — déjà propre au sens du signal, cf. [0] — traverse un rejet des vraies aberrations (NaN, hors plage, spike dur isolé), une **médiane sur 13 échantillons** (~65 s) puis une EMA lente — la régulation ne consomme **que** le filtré, jamais le brut. Ce second étage de filtrage ne fait pas double emploi avec le module : il ne vise pas le bruit électrique de l'électrode (traité à bord de l'EZO) mais la **variabilité du procédé** — bulles, stratification, panache d'injection passant devant la sonde — et les incidents de transmission. Côté pH, l'offset logiciel est ajouté **en sortie** des accesseurs ([ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Deux gels indépendants : la mesure devient `NaN` après 20 s sans lecture (`kSensorStaleTimeoutMs`), et le verdict `measure_validity` déclare les mesures **non représentatives** quand l'eau ne circule pas (sondes en eau stagnante) puis pendant le délai de purge à la reprise — la porte d'alimentation du filtre se ferme, rien n'apprend sur de l'eau morte.

**S'interdit** : lisser une aberration (on bloque à ≥ 10 rejets consécutifs plutôt que de fabriquer une valeur), figer la valeur filtrée, tirer la mesure vers la consigne.

**Détail** : [sensors.md](sensors.md#chaîne-de-filtrage) ([ADR-0027](../adr/0027-filtre-tendance-centrale.md)), [measure-validity.md](measure-validity.md) ([ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)).

### [2] Gardes de seuils pH — et inhibition croisée du chlore

**Décide** : si le produit demandé a même le **droit chimique** d'être injecté, avant toute considération de régulation. Côté pH : refus d'acidifier sous 6,70 **corrigé**, refus d'alcaliniser au-dessus de 7,60 **corrigé**, plus deux butées de dernier recours sur le pH **brut** (6,20 / 8,10) qui restent valides si l'offset est faux. Trois refus en 24 h (mode `automatic` seul) basculent la régulation pH en manuel, avec trace persistée et acquittement obligatoire. Côté ORP, c'est ici que vit **le seul couplage pH → chlore** : le dosage chlore est suspendu si le pH corrigé dépasse `kPhOrpInhibit` (7,60, relâche à − 0,05 d'hystérésis), si le pH brut dépasse la butée 8,10, **ou si le pH est inconnu** en régulation automatique — `canDose(1)` ne consulte jamais le pH, ce filet est l'unique garde du couplage.

**S'interdit** : mélanger les référentiels (un seuil chimique porte sur le pH corrigé, une butée anti-offset-faux sur le brut — jamais l'inverse), et compter un refus quand la mesure n'existe pas (refus fantôme).

**Détail** : [Gardes de sécurité pH](pump-controller.md#gardes-de-sécurité-ph-refonte-v2250) ([ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)), [Inhibition ORP](pump-controller.md#2-inhibition-du-dosage-orp-sur-ph-élevé-isorpinhibitedbyph), [Fail-closed sur pH indisponible](pump-controller.md#3-fail-closed-sur-ph-indisponible-régulation-orp-automatique).

### [3] Hystérésis de session

**Décide** : quand une **session** de dosage s'ouvre et se ferme. Même mécanique des deux côtés (`shouldStartDosing` / `shouldContinueDosing`), seuils propres à chaque chimie (`PumpProtection`, [config.h](../../src/config.h)) : ouverture à `phStartThreshold` = 0,05 pH / `orpStartThreshold` = 15 mV, poursuite jusqu'à `phStopThreshold` = 0,01 / `orpStopThreshold` = 2 mV. La bande [stop ; start] absorbe le bruit et le rebond — c'est elle, structurellement, qui stabilise le feedforward pH (voir § 2). Nuance v2.26.0 : côté pH, la bande morte du calcul suit l'hystérésis (start hors session, stop en session) ; côté ORP elle reste `orpStartThreshold` en permanence.

**S'interdit** : rouvrir une session avant que l'erreur soit remontée au seuil de **démarrage** (pas d'oscillation sur le seuil d'arrêt), et doser moins de `minInjectionTimeMs` = 30 s une fois démarré.

> **v2.36.0 — côté pH, la porte lit un SIGNAL LENT** ([ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)). Le bruit de la filtrée (σ = 0,043 sur 60 s, crête-à-crête 15 min ≈ 0,150) est **plus large que la bande 0,05 / 0,01** : près de la consigne, c'est lui qui ouvrait et fermait les sessions, avec 15 min de pause de mélange à chaque fermeture (rapport cyclique tombé à ~1/3). Le gate `pool-chemistry` a caractérisé ce bruit — **τc ≈ 50 s, processus quasi blanc** — donc **le moyenner marche** : la décision de porte lit désormais une **moyenne glissante de 10 min** (`kPhDecisionWindowMs`) et une session tient **30 min murales** minimum (`kPhSessionMinWallMs`), sauf garde et sauf sous-dépassement franc.
>
> **La distinction essentielle** : le signal **lent** décide **uniquement de la porte** (ouverture, fermeture, régime de débit de prolongation). **P, I et feedforward continuent de lire la filtrée ACTUELLE** — on ne ralentit pas la boucle, on stabilise sa porte. La bande morte, elle, garde exactement les mêmes valeurs et le même rôle : **déplacement du point d'équilibre = 0,000 pH**, condition d'acceptation du gate. Élargir la bande (0,17 pH nécessaires) a été **refusé** : avec de l'acide seul, le bassin ne saurait pas remonter.
>
> Corollaire observable : `isPhDosing()` — donc `/data ph_dosing`, le WS `ph_dosing`, MQTT `{base}/ph_dosing` / `ph_dosing_active` et `history.phDosing` — dit maintenant **« une session est ouverte »**, pompe éventuellement à l'arrêt. Pour « la pompe tourne maintenant », c'est le calendrier d'impulsion ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)).

**Détail** : [Porte de session pH robuste au bruit](pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360), [Algorithme (résumé), point 4](pump-controller.md#algorithme-résumé).

### [4] Calcul de la demande

**Décide** : un **débit moyen** en mL/min — jamais un volume à rattraper. Côté pH (depuis v2.26.0/v2.28.0) : `débit = FF(charge apprise) + kp·e + ki·∫(e − seuil d'arrêt)` — feedforward pour la charge connue, PI pour l'écart résiduel (voir § 2). Côté ORP : `kp·e` **pur** — `Ki = 0` et `Kd = 0`, délibérément (voir § 3). Les coefficients suivent la vitesse de régulation (`slow`/`normal`/`fast`), posés ensemble par `applyRegulationSpeed()` pour que `kp`, `ki` et `integralMax` ne divorcent jamais. **Depuis la v2.32.0, cette vitesse est un rouage interne, plus un réglage** : elle dérive de la taille du bassin choisie par l'utilisateur (« Moins de 20 m³ » → lente, « 20 à 60 m³ » → normale, « Plus de 60 m³ » → rapide — un petit bassin réagit fort à chaque mL, il faut le corriger doucement ; un grand, l'inverse) et n'apparaît plus dans l'interface. Le champ reste dans l'API et la NVS pour les cas experts ([ADR-0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)).

**S'interdit** : un terme dérivé (amplifierait le bruit — impératif sur les deux chimies, [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md)), synthétiser un `dt` (tout delta > 10 s est **rejeté**, jamais crédité d'un bloc), et accumuler de l'intégrale quand le dosage est durablement impossible (anti-windup asymétrique : la charge se gèle, la décharge jamais).

**Détail** : [Terme intégral pH](pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260) ([ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)), [Feedforward](pump-controller.md#dose-de-base-apprise--feedforward-ph-v2280) ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)).

### [5] Plafonds

**Décide** : combien de cette demande est **livrable**. Quatre bornes empilées : le débit d'**amplitude** calibré (la demande est clampée à `ampFlow` ; côté pH, `computePID` est borné à `ampFlow − ff`), le débit moyen **soutenable** (`amplitude × limite horaire / 60` — c'est lui qui gèle l'anti-windup et plafonne le FF à 60 %), la **limite horaire** configurée (min/h de temps pompe-ON dans une fenêtre glissante d'1 h, [ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)), et le **quota journalier** (`maxPhMlPerDay` / `maxChlorineMlPerDay`). Point de fond documenté depuis la v2.27.0 : sur l'installation de référence, c'est **l'actionneur** (la limite horaire) qui borne la correction, pas le correcteur — et la v2.29.0 rend cette limite visible et alertable.

**S'interdit** : dépasser un plafond « pour rattraper », et relever silencieusement la limite horaire (chaque min/h de plus augmente le volume injecté à l'aveugle pendant la pause mélange).

**Détail** : [Le point de fond : limité par l'actionneur](pump-controller.md#cadence-mémoire-et-charge-observée-de-lintégrale-ph-v2270), [Limite d'actionneur pH](pump-controller.md#limite-dactionneur-ph--exposition-et-alerte-de-sous-dimensionnement-feature-086-v2290) ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)).

### [6] Ordonnanceur d'impulsions (proportionnel de temps)

**Décide** : comment réaliser ce débit moyen. Depuis l'[ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md), il n'existe plus de modèle duty↔débit : la pompe tourne à une **amplitude unique dont le débit a été pesé**, et c'est la **fraction du temps de marche** qui module la dose. Période fixe de 60 s, pouls proportionnel à la demande, chaque pouls ouvert par un **kick** de 400 ms à duty 255 (couple de décollage). Un pouls ne descend jamais sous 10 s ([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)) : une demande plus faible est **accumulée** dans un report en mL et libérée par impulsions minimales exactes ([ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md)) — jamais refusée (l'ancien refus créait une zone morte), jamais de rafale de rattrapage (dette écrêtée fail-closed). En amont, la **gate de caractérisation** refuse tout dosage — automatique ET programmé — si le débit de la pompe n'est pas connu (pesée ou calibration de type, [ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) ou si l'amplitude est sous le plancher de rotation `MIN_ACTIVE_DUTY` = 98.

**S'interdit** : interpoler un débit (les deux seuls débits connus sont la mesure à l'amplitude et le majorant de flotte à 255, [ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)), tirer deux pouls dans une période, et clamper silencieusement une amplitude invalide (refus + `critical`).

**Détail** : [Dosage en proportionnel de temps](pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210), [Accumulateur sous-plancher](pump-controller.md#accumulateur-sous-le-pouls-minimal-feature-073-v2220), [Pouls minimal 10 s](pump-controller.md#pouls-minimal-porté-à-10-s-v2253).

### [7] Pause mélange

**Décide** : le rythme de la boucle. À l'**arrêt** de chaque injection — l'épisode complet, PAS chaque pouls, cf. le vocabulaire en tête de section ; et jamais au démarrage (bug historique corrigé) — une pause d'homogénéisation bloque l'injection **suivante** : 15 min pH (`kPhMixingDelayMs`), 20 min ORP (`kOrpMixingDelayMs`, plus conservateur). C'est elle qui fait de la régulation une boucle **temporisée** : on dose, on laisse le bassin s'homogénéiser, on **redécide** sur une mesure redevenue représentative. Précision importante : **la mesure, elle, ne s'arrête jamais** (lecture EZO toutes les 5 s, pause ou pas) — ce qui attend, c'est la décision de redoser. Cadence résultante : ~1 injection / 15-20 min par chimie. Indépendante du sas ponctuel de **stabilisation** (démarrage de filtration, post-calibration) — les deux sont des OR dans `canDose()`.

**S'interdit** : interrompre l'injection en cours (la pause s'applique au cycle **suivant**), et laisser l'intégrale se charger d'un bloc pendant l'attente (le sur-crédit de la pause est borné et chiffré).

**Détail** : [Pause mélange hydraulique](pump-controller.md#pause-mélange-hydraulique-feature-025), [Chronologie stabilisation + pause mélange](pump-controller.md#cycle-de-régulation--chronologie-stabilisation--pause-mélange).

### [8] Comptabilité des volumes

**Décide** : ce que l'appareil **croit avoir versé** — et donc quand les quotas mordent. Le volume est crédité par delta de tick : la part du kick au **majorant de flotte** à 255 (74 mL/min — surestimer est fail-safe, les limites mordent plus tôt), le plateau au débit pesé. Jamais zéro sur une pompe alimentée (prescription P4). Alimente : les cumuls journaliers persistés en NVS ([ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md)) avec reset à minuit indépendant de la filtration, la fenêtre horaire glissante, le **suivi de bidon** optionnel (`ProductConfig` : volume restant et seuil d'alerte, deux chimies), et — côté pH seulement — l'**anneau de charge sur 7 jours** `ph_load7` (mL et minutes de circulation par journée), la mémoire dont vivent le feedforward et la restauration d'intégrale (§ 4).

**S'interdit** : dériver un volume d'un modèle de débit (c'était le « dosage fantôme » corrigé en v2.21.0), et compter un cycle qui n'a pas commandé de débit (invariant v2.20.5, [Comptabilisation d'un démarrage](pump-controller.md#comptabilisation-dun-démarrage-de-cycle--invariant-v2205)).

**Détail** : [Comptabilité du volume (P4)](pump-controller.md#comptabilité-du-volume-p4), [Anneau de charge](pump-controller.md#4-lanneau-de-charge-acide-sur-7-jours) ([ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md)).

### [9] Gardes transverses

**Décide** : si quoi que ce soit a le droit de tourner ce tour-ci. `canDose(pumpIndex)` enchaîne 10 conditions fail-closed dans un ordre figé — watchdog actif, **présence d'eau** (source unique `resolveWaterPresence()` selon le mode d'installation, [ADR-0026](../adr/0026-mode-installation.md)), mesure filtrée valide et filtre prêt/stable, calibration EZO suffisante, stabilisation, pause mélange, mode, quotas, anti-rafale (6 démarrages/min, 20/15 min). S'y ajoutent l'anti-cycling (`maxCyclesPerDay`, configurable borné [10 ; 60]) et les courts-circuits en tête d'`update()` : OTA, mode maintenance, assistant de mesure du débit — pompes forcées à 0.

**S'interdit** : réordonner ou contourner une garde (toute nouvelle garde **s'ajoute**, aucune ne se déplace), et ouvrir un chemin d'activation de pompe qui ne passerait pas par elles (le mode manuel a ses propres gardes miroirs).

**Détail** : [Garde-fous `canDose`](pump-controller.md#garde-fous-candose), [Présence d'eau](pump-controller.md#présence-deau-selon-le-mode-dinstallation-feature-056).

### [10] Observabilité

**Décide** : ce que l'utilisateur et Home Assistant voient. Règle de parité stricte : tout champ publié l'est **à l'identique** sur `GET /data`, sur le WebSocket et (sélectivement) sur MQTT — mêmes noms, mêmes arrondis, mêmes règles `null`. La cause de refus de dosage est exposée en **texte** français ET en **code machine figé** ([ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)) ; l'intégrale, le feedforward, le calendrier d'impulsion ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)) et la limite d'actionneur ont chacun leurs champs lecture seule. Le journal est **edge-triggered** : une ligne par transition, jamais par tour.

**S'interdit** : un setter sur un accesseur d'observabilité (lecture seule stricte — aucun de ces chemins n'entre dans une décision de dosage), et renuméroter un code publié.

**Détail** : [API.md](../API.md), [ws-manager.md](ws-manager.md), [MQTT.md](../MQTT.md).

---

## 2. La division des rôles côté pH : feedforward et PI

Depuis la v2.28.0, la demande pH est la somme de deux contributeurs aux rôles disjoints :

- **Le feedforward est la charge CONNUE du bassin.** Le dégazage de CO₂ (~80 % de la charge basique mesurée) est stable d'un jour à l'autre : le redécouvrir chaque matin par tâtonnement coûtait ~2,4 jours de convergence au PI. Le FF applique d'emblée la **médiane des mL/min journaliers sur 7 jours** (anneau `ph_load7`) — la charge est couverte **dès la première heure de filtration**. C'est un **débit**, jamais un accumulateur : aucune dette à rattraper après un reboot, donc aucune rafale possible. Il est **appris, jamais saisi** — aucun volume de bassin, aucun TAC, aucune concentration en entrée ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)).
- **Le PI est l'écart RÉSIDUEL du jour.** Baignade, pluie, appoint d'eau, dérive saisonnière naissante : tout ce que la médiane ne connaît pas encore. Le P répond immédiatement à l'erreur ; l'intégrale ([ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) porte la part de charge permanente que le FF ne couvre pas — c'est elle qui a supprimé le droop du P pur (pH tenu à 7,50-7,66 pour une consigne à 7,30, l'équilibre théorique exact d'un proportionnel seul).

Trois couplages les tiennent ensemble :

| Couplage | Règle | Pourquoi |
|---|---|---|
| **Plafond partagé 60/40** | FF ≤ 60 % du débit soutenable (`kPhFfMaxSustainableFraction`) ; `computePID` borné à `ampFlow − ff`, somme clampée à `ampFlow` | Le PI garde toujours ≥ 40 % d'autorité pour l'écart résiduel — le FF, aveugle à l'erreur, ne peut pas confisquer l'actionneur |
| **Anti-windup sur `soutenable − FF`** | Le gel de charge de l'intégrale compare la demande à `max(0, soutenable − ff)` — d'où l'ordre impératif `tickPhFeedforward()` avant `tickPhIntegralAuto()` dans le même tour | Sinon l'intégrale se gèlerait sur une saturation **causée par le FF** et le PI deviendrait sourd |
| **Quota partagé, le FF cède le premier** | FF = 0 dès 60 % du quota journalier consommés (`kPhFfDailyQuotaFraction`) ; l'intégrale ne gèle qu'à 80 % (`static_assert` d'ordre) | L'autorité aveugle à l'erreur doit céder avant l'autorité asservie |

**L'argument de stabilité est le bilan de matière**, pas un réglage : le volume journalier livré converge vers la charge réelle du bassin quel que soit le FF — un FF trop fort ferme la session (l'erreur retombe sous le seuil d'arrêt) et cesse avec elle, un FF trop faible laisse le PI compléter. La médiane apprise est donc **non expansive** : pas d'auto-amplification d'un jour sur l'autre. L'hystérésis de session [3] est le mécanisme ; le plafond 60 % n'est qu'une ceinture. Voir [Stabilité : par l'hystérésis de session](pump-controller.md#stabilité--par-lhystérésis-de-session-structurellement).

### Le latch — le disjoncteur du feedforward

Le FF est la seule source de dosage **aveugle à l'erreur mesurée** : sans mesure
digne de confiance, personne ne peut constater qu'il dose à tort. Sa protection
est donc un **latch** — un *disjoncteur*, au sens propre : une fois déclenché,
il **ne se réarme pas tout seul** quand la cause disparaît, contrairement à une
simple condition de blocage. Trois déclencheurs : **mesure invalide**
(immédiat, sans tenue) ; **erreur signée < −0,05 pH tenue 30 min** (le scénario
« fausse pluie » : le bassin n'a manifestement pas besoin de la dose apprise) ;
**changement de contexte** (consigne, vitesse, type de produit — la mémoire a
été apprise pour un autre point de fonctionnement).

Le réarmement a lieu à un instant unique et observable : le **front d'ouverture
de la session de dosage suivante** (exception : la tenue « type de produit »
prime et attend une journée entière archivée sous le nouveau régime, § 4).
Pourquoi cette rigidité ? **Anti-battement** — une mesure qui vacille entre
valide et invalide ferait battre la dose marche/arrêt ; et **traçabilité** —
chaque pose et chaque levée a sa ligne de journal motivée. Une mesure redevenue
bonne ne rouvre donc PAS le latch : il reste fermé jusqu'à la prochaine session,
et c'est voulu (constaté en conditions réelles le 02/09/2026 : bouchon d'eau
morte au démarrage → latch à 06h59 → mesure saine à 07h30 → latch toujours
fermé, réarmement à la session suivante).

**Détail** : [Latch de sécurité et réarmement](pump-controller.md#latch-de-sécurité-condition-n3-et-réarmement) ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)).

---

## 3. L'asymétrie pH ↔ chlore

Toute la **machinerie de livraison** est commune ; toute l'**intelligence accumulative** est côté pH ; le **couplage croisé** est côté ORP. Ce n'est pas un retard de développement, c'est une décision.

| | Partagé (les deux chimies) | Réservé au pH | Propre à l'ORP |
|---|---|---|---|
| **Livraison** | Proportionnel de temps, kick, accumulateur sous-plancher, gate de caractérisation, amplitude calibrée ([ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)-[0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)) | — | — |
| **Rythme** | Pause mélange (15 min pH / 20 min ORP), stabilisation, hystérésis de session (seuils propres), anti-cycling, anti-rafale | — | — |
| **Limites** | Limite horaire glissante, quota journalier, `maxCyclesPerDay`, suivi bidon (`ProductConfig`), gardes `canDose` | — | — |
| **Correcteur** | Kd = 0 impératif, mode `scheduled` aveugle à la mesure ([ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md)) | **Intégrale** (v2.26.0, [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) · **mémoire / anneau 7 j + persistance conditionnelle** (v2.27.0, [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md)/[0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)) · **feedforward** (v2.28.0, [ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)) · **détection de sous-dimensionnement** (v2.29.0, [ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) | **P pur** (Ki = 0 maintenu, [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md)) |
| **Couplages** | — | Gardes de seuils + offset ([ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)/[0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) | **Inhibition par pH haut** (> 7,60 corrigé / > 8,10 brut / pH inconnu en auto) · cible relevable par le Boost ([ADR-0025](../adr/0025-mode-boost.md), +60 mV borné 850, jamais abaissée) |

Les trois raisons de l'asymétrie :

**(a) L'ORP n'est pas une mesure linéaire de la masse de chlore dosée.** Le potentiel redox dépend du pH (la fraction de HClO s'effondre au-dessus de 7,6 — c'est le fondement de l'inhibition), de la température, et du **stabilisant** : l'acide cyanurique déprime l'ORP à concentration de chlore constante (constaté sur le bassin de référence, épisode de surconsommation de juillet 2026 à 15 mg/L de CYA). Une intégrale ou une mémoire de charge construite sur l'erreur ORP accumulerait donc un **mélange indiscernable de chimie réelle et d'artefacts de mesure** — et la servirait en produit. Côté pH, l'erreur mesure bien la grandeur qu'on corrige ; côté ORP, non. La bonne boucle de calibration de la **cible** ORP est externe à l'appareil : la mesure directe du chlore (bandelette / DPD) par l'utilisateur, qui recale `orpTarget` sur son eau (c'est la pratique effective sur le bassin de référence). L'appareil asservit l'ORP ; l'utilisateur asservit la signification de l'ORP.

**(b) L'asymétrie du risque.** Un pH hors consigne est un problème de confort et d'efficacité (eau agressive, chlore moins actif) — les excursions se mesurent en dixièmes d'unité et se corrigent en jours. Un **surdosage de chlore est un problème de santé**. Le sens sûr n'est pas le même : côté pH, on a accepté d'ajouter de l'autorité (intégrale, FF, persistance) parce que chaque ajout est plafonné et que le défaut résiduel est un sous-dosage ; côté chlore, **le sens sûr est de sous-doser** — tout mécanisme qui accumule ou mémorise une autorité de dosage y serait une surface de surdosage nouvelle, à contre-sens du risque. C'est le même raisonnement qui fait de l'inhibition pH-haut une suspension totale (le vrai correctif est de baisser le pH, pas de pousser plus de chlore) et qui borne le Boost par deux plafonds.

**(c) Le problème résolu n'existait que côté pH.** Toute la lignée v2.26.0 → v2.29.0 répond à un défaut observé et chiffré : le pH **bloqué hors consigne pendant des semaines** par le droop d'un P pur sous charge basique permanente (~1 800 mL/j de besoin, équilibre théorique vérifié au terrain). L'ORP, lui, atteint sa cible en P pur + gardes : sa charge (consommation de chlore) est compensée tant que l'erreur persiste, et son droop n'a jamais été le défaut diagnostiqué ([ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md), statut). On ne déploie pas une machinerie d'apprentissage sur une boucle qui converge.

**Un feedforward chlore resterait concevable** — la photolyse UV estivale est une charge régulière, apprenable sur le même principe d'anneau. Mais il exigerait son propre cycle complet avec gate `pool-chemistry` : plafonds propres (le sens du risque impose des fractions plus basses), latch propre (les artefacts ORP du point (a) rendent la « mesure défavorable » plus ambiguë), et une réponse à la question du (a) — apprendre des mL/min sur une grandeur qui ne mesure pas linéairement le produit. Aucun défaut observé ne le justifie aujourd'hui.

---

## 4. L'apprentissage en continu (pH) : une mémoire glissante, pas une calibration

La « connaissance » du bassin n'est jamais figée : c'est la **médiane des mL/min journaliers sur une fenêtre glissante de 7 jours** (anneau `ph_load7`, une écriture NVS par jour), recalculée à chaque clôture de journée. Une journée n'y compte que si elle est **exploitable** : ≥ 2 h de circulation (`kPhLoadDayMinFiltrationMin`), ≤ 7 jours calendaires d'âge, et un rapport mL/min **vraisemblable** (≤ 1,10 × le débit soutenable **courant** — le seuil est une fonction de la configuration, [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)). Une journée à volume nul mais bien circulée est **conservée** : c'est une observation légitime de charge faible ; l'écarter tirerait la médiane du mauvais côté.

Les vitesses d'adaptation qui en résultent :

| Événement | Réaction de la mémoire |
|---|---|
| **Journée aberrante** (incident, purge, assistant de débit, fête au bord du bassin) | **Ignorée** — la médiane sur 7 encaisse jusqu'à 3 journées folles ; le filet de vraisemblance écarte en plus les journées physiquement impossibles |
| **Dérive saisonnière** (montée en température, fréquentation) | **Suivie en ~3-4 jours** — la médiane bascule quand la majorité de la fenêtre reflète le nouveau régime |
| **Changement de TYPE de produit** (réglage `ph_minus` ↔ `ph_plus` — PAS un simple changement de bidon) | **Latch** du FF jusqu'à ce qu'une **journée entière** ait été archivée sous le nouveau régime (la première clôture, mixte, est sautée) ; l'intégrale est purgée immédiatement (le signe de l'erreur s'inverse). L'appareil ne détecte rien chimiquement : c'est le réglage « type de produit » qui le lui déclare |
| **Changement ou recharge de bidon** (même produit) | **Aucun effet sur l'apprentissage** — la mémoire porte sur la consommation du bassin, pas sur le contenant. Seul le suivi de volume de la page Produits est concerné (bouton « Nouveau bidon », qui remet le compteur à zéro) |
| **Changement de consigne, de vitesse ou de plafond** (empreinte de contexte) | **Latch** du FF jusqu'à la session suivante ; changement de vitesse ⇒ purge de l'intégrale (le `ki` acquis ne vaut plus) |
| **Excursion franche de l'autre côté de la consigne** (pluie acide…) | FF retenu après 30 min d'erreur défavorable tenue (`kPhFfLowHoldMs`) ; purge sèche de l'intégrale après 15 min d'excursion soutenue (`kPhIntegralResetHoldMs`) — un transitoire n'efface plus des jours d'apprentissage |
| **Hiver, filtration courte** (< 2 h/jour) | **Auto-désactivation fail-closed** : plus aucune journée exploitable → moins de 3 jours dans la fenêtre → FF en `kNoHistory`, restauration d'intégrale interdite (plafond nul). **Le PI seul suffit** à ce régime de faible charge |

**Fail-closed du premier démarrage** : aucune dose « de mémoire » avant **3 journées exploitables** (`kPhIntegralRestoreMinHistoryDays`) — un appareil neuf ne restaure rien et son FF ne dose rien. Pendant ce temps, la régulation n'attend pas : le PI fonctionne **dès la première heure** (le P immédiatement, l'intégrale se charge en ~2 jours de filtration). La mémoire accélère la régulation ; elle n'est jamais sa condition.

Même la valeur d'intégrale persistée n'est **jamais appliquée au boot** : elle attend 5 min de confirmation par la mesure courante, sous sept garanties cumulatives ([ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md), R1-R7). Ce qui est restauré n'est pas une autorisation de doser, c'est la vitesse d'une correction déjà décidée par le bassin.

**Détail** : [Anneau de charge](pump-controller.md#4-lanneau-de-charge-acide-sur-7-jours), [Critère de jour exploitable](pump-controller.md#critère-de-jour-exploitable--les-trois-filtres-nécartent-que-par-le-haut), [Persistance R1-R7](pump-controller.md#3-persistance-nvs-conditionnelle-r1-r7), [Latch du FF](pump-controller.md#latch-de-sécurité-condition-n3-et-réarmement).

---

## 5. Les constantes de temps du système

Du plus court au plus long. Les noms font foi ([`constants.h`](../../src/constants.h) sauf mention) ; les valeurs sont indicatives **sauf** celles gravées par un ADR (colonne Source).

| Échelle | Constante | Valeur actuelle | Rôle | Source |
|---|---|---|---|---|
| 400 ms | `kPulseKickMs` | 0,4 s | Kick de décollage à duty 255 en tête de chaque pouls | [ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md) |
| 1 s | `kPhIntegralChargeQuantumMs` | 1 s | Quantum du tick intégral (incrément représentable en `float`) | [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) |
| 5 s | `kPhOrpSensorIntervalMs` | 5 s | Cadence de lecture des sondes EZO pH/ORP | [sensors.md](sensors.md) |
| 10 s | `kPulseMinMs` | **10 s (gravé)** | Pouls minimal — en deçà, la demande s'accumule | [ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md) |
| 10 s | `kPhIntegralMaxTickDtMs` | 10 s | `dt` maximal crédité à l'intégrale (un trou n'est jamais crédité d'un bloc) | [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) |
| 20 s | `kSensorStaleTimeoutMs` | 20 s | Mesure périmée → NaN → refus de dosage | condition pool-chemistry #1 |
| 30 s | `minInjectionTimeMs` ([`config.h`](../../src/config.h)) | 30 s | Durée minimale d'une injection démarrée (temps pompe-ON) | [ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) |
| ~65 s | fenêtre médiane du filtre (13 échantillons) | ~65 s | Tendance centrale > 1 période d'oscillation | [ADR-0027](../adr/0027-filtre-tendance-centrale.md) |
| 60 s | `kPulsePeriodMs` | **60 s (gravé)** | Période du proportionnel de temps | [ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)/[0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md) |
| 3-5 min | `kStabilizationDurationOrpMs` / `PhMs` | 3 / 5 min | Sas post-calibration EZO, par sonde | feature-021 |
| 10 min | `kPurgeFloorMs` (gravé) | **10 min** | Plancher d'attente à la reprise de circulation (dosage ET validité des mesures), de la physique de renouvellement d'eau en conduite — universel. **Depuis la v2.62.0 (feature-139)**, ce n'est plus une purge « adaptative » sur stabilité des bruts (plancher 10 min / plafond 90 min, retiré) : c'est un plancher fixe, sauté si l'arrêt d'eau a duré moins de `kWaterStopGraceS` (3 min). Le réglage `stabilizationDelayMin` est retiré depuis la v2.34.0 | [measure-validity.md](measure-validity.md), [ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md) (supersede [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)) |
| 10 min | `kPhDecisionWindowMs` | 10 min | Fenêtre du **signal lent** de décision de porte pH (σ 0,043 → 0,017 ; retard ≈ 7 min) | [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) |
| 15 min | `kPhIntegralResetHoldMs` | 15 min de branche | Tenue d'excursion avant purge sèche de l'intégrale (= pause mélange, `static_assert`) | v2.27.0 |
| 30 min | `kPhSessionMinWallMs` | **30 min murales** | Durée minimale d'une session pH — `static_assert` : ≥ 2 × `kPhDecisionWindowMs` | [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) |
| 15-20 min | `kPhMixingDelayMs` / `kOrpMixingDelayMs` | 15 / 20 min | Pause mélange — le métronome de la boucle | feature-025, [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) |
| 30 min | `kPhFfLowHoldMs` | **≤ 30 min (verrou)** | Latch « fausse pluie » du feedforward (erreur défavorable tenue) | [ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md) |
| 1 h | `kPhIntegralDecayHalfLifeMs` | 1 h | Demi-vie d'oubli de l'intégrale sous la consigne | [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) |
| 1 h | fenêtre de la limite horaire (`ph/orpInjectionLimitMinutes`) | glissante 1 h | Budget de temps pompe-ON — le plafond d'actionneur | [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)/[0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) |
| 4-8 h | `kPhIntegralTiFastS` / `NormalS` / `SlowS` | **4 / 6 / 8 h (gravés)** | Constantes de temps de l'intégrale par vitesse | [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) |
| 6 h | `kPhIntegralPersistMaxAgeS` | 6 h (= 1 Ti) | Péremption de l'instantané d'intégrale persisté (R3) | [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) |
| 24 h | `kPhIntegralStaleResetMs` | 24 h | Péremption de l'intégrale sans évaluation | [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) |
| 24 h | `kPhGuardRefusalWindowMs` | 24 h glissantes | Fenêtre des 3 refus de garde avant bascule en manuel | [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) |
| 3-7 j | `kPhIntegralRestoreMinHistoryDays` / `…HistoryWindowDays` | **3 / 7 j (gravés)** | Fenêtre d'apprentissage : 3 journées exploitables requises, 7 conservées | [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md)/[0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) |
| 3 j | `kPhActuatorLimitConsecutiveDays` | **3 j (gravé)** | Journées saturées consécutives avant l'alerte de sous-dimensionnement | [ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) |

Les seuils d'**hystérésis de session** (sans dimension de temps mais rythmant tout) : `phStartThreshold` 0,05 / `phStopThreshold` 0,01 pH, `orpStartThreshold` 15 / `orpStopThreshold` 2 mV (`PumpProtection`, [`config.h`](../../src/config.h)). Depuis la v2.36.0, les deux valeurs pH sont **initialisées depuis des `constexpr`** de [`constants.h`](../../src/constants.h) (`kPhStartThresholdDefault` / `kPhStopThreshold`) — elles n'étaient de toute façon jamais relues depuis la NVS ni exposées par l'API, et cette promotion permet de **verrouiller par `static_assert`** que la bande reste au-dessus du bruit résiduel du signal lent. S'y ajoutent `kPhDoorCloseMarginPh` = 0,03 (marge de fermeture de la porte, sans effet sur la bande d'actionnement) et `kPhSessionUndershootFloorPh` = 0,05 (plancher de sous-dépassement, identique par `static_assert` à `kPhFfAdverseErrorPh`).

---

## 6. Ce que la régulation ne fait JAMAIS (les invariants)

1. **Jamais de volume de bassin dans un calcul de dosage** ([ADR-0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)). La taille (`small`/`medium`/`large`) ne fait que **présélectionner** des réglages modifiables ([`regulation_defaults.h`](../../src/regulation_defaults.h)) ; la charge réelle est **apprise** (anneau `ph_load7`, feedforward). Preuve consignée : `poolSize` n'apparaît dans aucun chemin de dosage, invariant tenu par mutation en test natif. Principe fondateur : l'appareil doit fonctionner correctement sur n'importe quelle piscine, pas seulement celle déclarée.

2. **Jamais deux chemins vers la pompe** ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md), condition bloquante n°1 ; précédent [ADR-0025](../adr/0025-mode-boost.md)). Tout contributeur de dosage entre comme **terme du calcul de débit** de la branche existante — jamais comme branche parallèle : une seule comptabilité de volume, un seul jeu de gardes. Le Boost lui-même ne déplace que des cibles et plafonds *effectifs*, il n'ouvre aucun chemin d'injection.

3. **Jamais de dose sans présence d'eau, jamais de régulation automatique sans mesure valide.** La présence d'eau (source unique `resolveWaterPresence()`, [ADR-0026](../adr/0026-mode-installation.md)) conditionne **toutes** les voies — auto, programmée, manuelle. La branche automatique exige en plus une mesure filtrée valide, un filtre prêt et stable, une calibration EZO suffisante ([Garde-fous `canDose`](pump-controller.md#garde-fous-candose)). Le mode programmé est aveugle à la mesure **par conception** ([ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md)) mais reste soumis à la présence d'eau, aux gardes de seuils pH et à la gate de caractérisation. Corollaire : aucun dosage sans watchdog actif (condition #2 de `canDose`), et aucune dose « de mémoire » sans 3 jours d'historique (§ 4).

4. **Jamais de code de statut renuméroté** ([ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)). `DoseRefusal` (causes de refus 1-22), `PhFfStatus` (0-4) et leurs semblables sont des **contrats figés** : on ajoute en fin, on ne renumérote ni ne supprime jamais — les clients (UI, HA, scripts) traduisent un code par table, pas en cherchant un fragment dans une phrase française.

5. **Les quotas de `SafetyLimits` survivent à tout reset de réglages.** La restauration des valeurs d'usine ([feature-088/090](pump-controller.md#restauration-des-valeurs-dusine-de-régulation-feature-088-v2310)) ne touche **jamais** `maxPhMlPerDay` / `maxChlorineMlPerDay`, ni la calibration des pompes, ni le produit déclaré, ni l'offset, ni la trace de bascule de sécurité — prouvé par mutation. Même règle en sens inverse : écrire l'offset pH **ne réinitialise aucun compteur** de dosage (sinon des enregistrements répétés contourneraient les limites). Ces champs décrivent un état physique ou une sécurité, pas un réglage.

---

## Fichiers liés

- [pump-controller.md](pump-controller.md) — la référence détaillée (API, machine à états, journal, constantes)
- [sensors.md](sensors.md), [measure-validity.md](measure-validity.md) — l'amont de la chaîne
- [filtration.md](filtration.md) — présence d'eau, planning, `scheduledMinutesToday()`
- [ws-manager.md](ws-manager.md), [mqtt-manager.md](mqtt-manager.md), [../API.md](../API.md) — l'observabilité
- ADR de la lignée régulation : [0002](../adr/0002-mode-programmee-volume-quotidien.md), [0016](../adr/0016-regulation-p-temporisee-vs-pid.md), [0020](../adr/0020-budget-horaire-dosage-unique.md), [0021](../adr/0021-repartition-scheduled.md), [0025](../adr/0025-mode-boost.md), [0026](../adr/0026-mode-installation.md), [0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md), [0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md), [0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)-[0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)
