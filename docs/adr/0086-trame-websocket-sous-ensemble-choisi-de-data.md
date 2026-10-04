# ADR-0086 — La trame WebSocket est un sous-ensemble choisi de `/data`, financé par élagage

- **Statut** : Accepté
- **Date** : 2026-10-04
- **Décideurs** : architect, embedded-developer, web-ui-developer, pool-chemistry, test-engineer, code-reviewer, utilisateur (arbitrage final)
- **Spec(s) liée(s)** : [feature-138](../../specs/features/doing/feature-138-dater-et-rafraichir-l-etat-de-blocage-du-dosage.md)
- **Étend** : [ADR-0071](0071-parite-data-trame-capteurs.md) (parité `/data` ↔ `sensor_data`, mécanisme d'exceptions `kDataOnly`)

## Contexte

Le 2026-10-02, après des pluies diluviennes ayant couvert le fond du bassin de
boue, les deux voies pH et ORP ont été déclarées instables et **tout dosage
automatique s'est arrêté**, chlore compris, au moment précis où la demande en
chlore était maximale. Le code de refus (`phDoseBlockedCode` /
`orpDoseBlockedCode`) était déjà exposé à l'écran, mais sans sa **durée** —
« bloqué » et « bloqué depuis 9 h » n'appellent pas la même réaction, et
l'absence de durée a contribué à un diagnostic faux côté utilisateur. La spec
feature-138 ajoute donc deux champs, `phDoseBlockedForS` / `orpDoseBlockedForS`,
publiés en parité stricte sur `GET /data` et sur la trame WebSocket
`sensor_data`, conformément à la règle posée par [ADR-0071](0071-parite-data-trame-capteurs.md).

Le problème n'est pas la parité elle-même, mais **où trouver la place**. La
trame WebSocket est construite dans un buffer **statique** de taille fixe,
`sWsFrameBuf[kWsFrameBufBytes]` ([`src/ws_manager.cpp`](../../src/ws_manager.cpp)),
et c'est le **seul plafond dur** du projet sur ce chemin : tout dépassement ne
tronque pas, il **saute le push** (silencieusement, hors un warn throttlé). Au
moment d'arbitrer cette spec, la marge résiduelle sous ce plafond était de
**43 octets** (`kWsFrameBufBytes = 3584`, pire cas mesuré 3541 o), et la paire
de champs à ajouter coûte, au pire cas **structurel du type** (méthode imposée
par [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) : la borne du clamp
applicatif, jamais une valeur relevée) :

```
"phDoseBlockedForS":86400,   26 o
"orpDoseBlockedForS":86400,  27 o
                             ───
                             53 o  >  43 o de marge disponible  →  ÉCHEC
```

La trame **aurait sauté** sans intervention — et le pire cas de longueur de
cette trame coïncide structurellement avec l'escalade de sécurité pH (les
causes de refus les plus longues sont celles d'un épisode de dosage anormal),
c'est-à-dire exactement le moment où perdre la trame coûte le plus cher.

ADR-0071 avait déjà identifié, et **refusé**, l'option d'élaguer
`ph_integral` / `ph_integral_term` de la seule trame WS pour se faire de la
marge — refusée **à ce moment-là** parce qu'elle n'était pas nécessaire et que
la famine mémoire documentée par bug-080 rendait son alternative (relever
`kWsFrameBufBytes`) également coûteuse. feature-138 change les deux termes de
cet arbitrage : la marge est désormais **insuffisante sans intervention**, et
il fallait donc trancher, pas reporter.

## Décision

**Option (a) — élaguer `ph_integral` et `ph_integral_term` de la seule trame
WebSocket**, retenue par l'utilisateur le 2026-10-04.

Les deux champs restent publiés **intacts** sur `GET /data`, sur MQTT et donc
dans Home Assistant, où ils sont un contrat externe. Ils deviennent des
exceptions `/data`-seules dans la sentinelle de parité
(`test/test_native_ws_data_parity`, table `kDataOnly`, motif ≥ 20 caractères
chacun), au motif qu'ils n'ont **plus aucun consommateur côté client** depuis
feature-092 (vérifié par grep : les clés lues par `data/app.js` sont
`ph_integral_pct`, `ph_integral_saturated` et l'id DOM
`ph_integral_warning` — trois clés distinctes des deux retirées).

Bilan chiffré : −49 o (élagage) + 53 o (nouveaux champs) = **+4 o net**. Pire
cas de trame : 3541 − 49 + 53 = **3545 o**, sous `kWsFrameBufBytes = 3584` —
marge résiduelle **39 o**. Aucun bump du gabarit `StaticJson<>`, aucun bump de
`kWsFrameBufBytes`.

## Alternatives considérées

- **(a) Élagage de `ph_integral` / `ph_integral_term` de la seule trame WS**
  (**retenue**) — rend exactement 49 o, finance 53 o au prix de 4 o restant à
  trouver sur le gabarit existant (absorbés sans aucun bump). Coût : les deux
  clés quittent la trame temps réel (push 5 s) et ne sont plus consultables
  qu'au polling `/data` (30 s) ou par MQTT — **aucun consommateur UI actuel**
  ne le remarque, vérifié par grep avant la décision.
- **(b) Relever `kWsFrameBufBytes`** (rejetée) — la plus simple en apparence :
  ajouter un buffer plus grand pour absorber 53 o sans rien retirer. Rejetée
  pour deux raisons. *Chiffrage* : le bump historique (3072 → 3584 en v2.21.1)
  coûte **+512 o de `.bss` statique**, et ce chiffre est **linéaire avec la
  taille demandée** — tout bump futur paiera le même tarif. *Mémoire* : ce
  coût n'est **pas gratuit**, contrairement à ce qu'affirmait jusqu'ici le
  commentaire de `kWsFrameBufBytes` dans `constants.h` (« hors heap 8 bits,
  aucun impact sur la famine mémoire ») — voir la correction ci-dessous. Sur
  un projet en famine documentée du tas 8 bits ([ADR-0058](0058-gardes-heap-metrique-8-bits.md),
  [ADR-0082](0082-arene-circulaire-du-journal-en-ram-interne-statique.md)),
  relever un buffer statique pour 53 o de champs alors qu'une option à coût
  nul existe n'est pas défendable.
- **(c) Clés courtes + durée bornée `uint16`** (rejetée) — raccourcir les noms
  de clé (`phBlockedS` / `orpBlockedS` au lieu de `phDoseBlockedForS` /
  `orpDoseBlockedForS`) ramène le pire cas à 39 o, sous les 43 o de marge
  disponible **à l'époque du chiffrage initial**, avec seulement 4 o de marge
  résiduelle. Rejetée parce qu'elle brise la cohérence de nommage avec
  `phDoseBlockedCode` / `orpDoseBlockedReason` déjà publiés (toute la famille
  de champs liés au refus de dosage partage le préfixe `*DoseBlocked*`), pour
  un gain qui ne couvre qu'un seul cycle de plus — la vraie décision
  structurante (élaguer ou relever le plafond) restait entière au cycle
  suivant. Documentée pour mémoire : si une marge d'urgence est un jour
  requise sans toucher au contenu de la trame, cette voie existe.

## Conséquences

### Positives
- Le champ de durée du refus de dosage — directement motivé par un incident
  de sécurité réel (arrêt du chlore en pleine demande) — tient **sous le seul
  plafond dur du projet**, sans dégrader la marge d'un autre champ futur plus
  que nécessaire (39 o restants, contre 43 o avant cet ajout).
- Coût **net quasi nul** en octets de trame : l'élagage compense presque
  intégralement l'ajout (49 o rendus pour 53 o consommés).
- Aucun nouveau coût de RAM statique (`.bss`) : contrairement à l'option (b),
  cette voie ne grandit jamais le buffer.
- Le mécanisme de sentinelle d'[ADR-0071](0071-parite-data-trame-capteurs.md)
  absorbe le changement par construction : deux nouvelles entrées `kDataOnly`
  motivées, pas d'exception sans garde.

### Négatives / dette assumée
- `ph_integral` et `ph_integral_term` ne sont plus consultables en **temps
  réel** (push 5 s) : un futur outil de diagnostic qui voudrait les
  visualiser en direct devra soit les relire sur `/data` (30 s), soit
  rouvrir cette décision.
- La marge résiduelle de la trame WS tombe à **39 o** — moins qu'une seule
  clé courte supplémentaire. Le prochain champ de cette trame **devra**
  consommer un retrait compensatoire ou relever `kWsFrameBufBytes` par un
  nouvel ADR : il n'y a plus de réserve « gratuite » à dépenser en silence.
- Deux entrées supplémentaires dans la table d'exceptions `kDataOnly`
  (2 → 4) : une dette **inventoriée**, pas un relâchement de la garde — le
  compte reste vérifié par la sentinelle native.

### Ce que ça verrouille
- **La trame WebSocket `sensor_data` n'est plus présumée être un miroir de
  `/data`.** C'est un **sous-ensemble choisi**, dimensionné par le budget du
  buffer statique. La règle de parité d'ADR-0071 (« tout champ consommé par
  l'interface doit exister sur les deux canaux ») reste pleinement en
  vigueur — ce que cet ADR change, c'est qu'un champ peut légitimement
  **quitter** la trame WS tout en restant sur `/data`, à condition d'une
  exception motivée et d'un consommateur client vérifié absent.
- **Tout champ WebSocket futur sur cet axe devra être financé par un retrait
  compensatoire**, sauf décision explicite et chiffrée de relever
  `kWsFrameBufBytes` par un nouvel ADR. Ce n'est plus une routine de revue :
  la marge ne le permet plus.
- **Le commentaire de `kWsFrameBufBytes` dans `constants.h` est corrigé** et
  ne doit plus être cité comme justifiant un bump « gratuit » : voir la
  rectification ci-dessous.

### Rectification d'une affirmation fausse

Le commentaire de `kWsFrameBufBytes` (`src/constants.h`) justifiait le bump
3072 → 3584 (v2.21.1) par : *« Coût : +512 o en `.bss` statique — **hors
heap 8 bits, aucun impact sur la famine mémoire**. »* C'est **inexact**, et
cette inexactitude a pesé sur l'arbitrage de cette spec en faisant paraître
l'option (b) gratuite alors qu'elle ne l'est pas :

- Le `.bss` est une section placée en **DRAM** au link, comme le tas.
- Le tas 8 bits que mesure `heapLargestFreeBlock8Bit()` (la métrique qui
  compte réellement, [ADR-0058](0058-gardes-heap-metrique-8-bits.md)) n'est
  jamais que le **reliquat** de cette même DRAM, une fois le `.bss` et le
  `.data` du firmware soustraits au démarrage.
- Le `.bss` statique ne **fragmente** donc pas le tas (il est alloué une
  fois, au link, jamais libéré ni réalloué — aucun impact sur
  `max_alloc_8bit`, la métrique de contiguïté). Mais il **réduit bien le
  total disponible** avant même que le firmware ne tourne.

Un bump de `kWsFrameBufBytes` n'est donc pas « hors sujet » pour la famine
mémoire du projet : il a un coût réel, juste d'une nature différente de
l'allocation dynamique (il ne fragmente pas, il **rétrécit le gâteau**). Le
commentaire a été corrigé dans le code ; c'est ce raisonnement corrigé, et
non l'ancienne distinction heap/`.bss`, qu'il faut relire au prochain
arbitrage d'un bump de ce buffer.

## Références

- Code : [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`_buildSensorJson`, décompte d'octets et élagage), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) (`handleGetData`, champs non élagués côté `/data`), [`src/constants.h`](../../src/constants.h) (`kWsFrameBufBytes`, commentaire corrigé ; `kDoseBlockedForMaxS`), [`src/dose_block_duration_logic.h`](../../src/dose_block_duration_logic.h) (module pur), [`src/pump_controller.{h,cpp}`](../../src/pump_controller.h) (`setRefusalCode()`, `_refusalCodeSinceMs`)
- Test : [`test/test_native_ws_data_parity/test_ws_data_parity.cpp`](../../test/test_native_ws_data_parity/test_ws_data_parity.cpp) (entrées `kDataOnly`), [`test/test_native_dose_block_duration/`](../../test/test_native_dose_block_duration/)
- Spec : [`specs/features/doing/feature-138-dater-et-rafraichir-l-etat-de-blocage-du-dosage.md`](../../specs/features/doing/feature-138-dater-et-rafraichir-l-etat-de-blocage-du-dosage.md)
- Doc : [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2540--durée-du-refus-de-dosage-feature-138-et-élagage-de-ph_integralph_integral_term), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#durée-du-refus-courant-par-voie-feature-138-v2540), [`docs/API.md`](../API.md#champs-ajoutés-en-feature-025-lissage-mesures--régulation-p-temporisée), [`docs/features/page-dashboard.md`](../features/page-dashboard.md#bandeau--dosage-bloqué--feature-138-v2540)
- ADR liés : [ADR-0071](0071-parite-data-trame-capteurs.md) (parité `/data` ↔ `sensor_data`, mécanisme d'exceptions étendu ici), [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) (chiffrage au pire cas structurel du clamp, pas du type), [ADR-0058](0058-gardes-heap-metrique-8-bits.md) et [ADR-0082](0082-arene-circulaire-du-journal-en-ram-interne-statique.md) (famine mémoire 8 bits, contexte de la rectification `.bss`), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module pur `dose_block_duration_logic`)
