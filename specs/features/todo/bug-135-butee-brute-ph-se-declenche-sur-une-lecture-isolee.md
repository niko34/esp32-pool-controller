# bug-135 — La butée de sécurité brute pH se déclenche sur une lecture isolée

## Métadonnées
- **ID** : bug-135
- **Slug** : butee-brute-ph-se-declenche-sur-une-lecture-isolee
- **Type** : bug — **garde de sécurité qui refuse à tort**, pas qui autorise à tort
- **Statut** : todo
- **Priorité** : normale — l'erreur actuelle est **fail-closed** (acide refusé, jamais injecté à tort) : aucune urgence dans la direction dangereuse
- **Version cible** : 2.54+
- **Dernière mise à jour** : 2026-09-20
- **Spec(s) liée(s)** : `specs/features/done/feature-097-…` (**même cause racine**, autre chemin de code) · ADR-0039 (la butée brute s'évalue sans offset) · bug-137 (la perturbation à la source)

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — nature du signal brut
- [x] **Régulation (pump_controller, PID)** — c'est une garde de dosage
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT** : c'est une garde chimique
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La butée de sécurité qui interdit l'injection d'acide sous un pH brut de **6,20**
compare un **seuil dur** à une **lecture isolée**. Or le pH brut de ce bassin
n'est pas un signal stable : il **alterne entre deux niveaux** séparés d'environ
0,21, avec des sauts entre lectures consécutives atteignant **0,353**.

La butée se déclenche donc sur du bruit, refusant des injections légitimes.

## Contexte — la mesure qui l'a révélée

Trouvée en instruisant feature-097, pas en cherchant un défaut de dosage.

### L'épisode du 2026-09-20

```
07:33:01  fin de purge par stabilité, porte de mesure ouverte
07:34:03  CRIT [Dosage pH] pH BRUT 6.01 sous la butée de sécurité 6.20
          (offset=0.50) — injection d'acide refusée quelle que soit la
          valeur corrigée
07:40:09  [Dosage pH] pH brut remonté à 6.25 (butée 6.20 + hystérésis 0.05)
          — butée brute levée
```

**Au même instant, l'estimateur central valait 6,51.** La butée a donc refusé une
injection sur une excursion de bruit, pas sur un pH réellement dangereux. Elle
s'est relevée seule six minutes plus tard, par hystérésis.

### La nature du signal — mesurée, pas supposée

Trace de 300 échantillons uniques à 5 s (25 min), eau établie, porte ouverte.
Fixture versionnée : [`test/fixtures/f097_purge_noise_20260920.csv`](../../test/fixtures/f097_purge_noise_20260920.csv).

| Grandeur | Valeur |
|---|---|
| écart-type du pH brut | **0,1175** |
| saut moyen entre deux lectures à 5 s | 0,173 = **1,47 σ** |
| saut maximal | **0,353** |
| autocorrélation au lag 1 | **−0,440** |
| changements de signe autour de la médiane | 197 / 299 |

Bloc réel de la minute 22, tel quel :
`7.051 · 6.747 · 7.083 · 6.752 · 7.083 · 6.743 · 7.085 · 6.775 · 7.079 · 6.749 · 7.079 · 7.068`

Deux niveaux séparés d'environ 0,21, en alternance.

**À vraie valeur 6,51, la distance à la butée vaut 0,31 ≈ 3,1 σ.** Le
déclenchement à tort est donc rare **par lecture**, mais **certain plusieurs fois
par jour** à 12 lectures par minute.

### Même cause racine que feature-097, autre chemin de code

feature-097 a corrigé le **détecteur de stabilité de purge**, qui utilisait une
**médiane** sur un signal bimodal — où la médiane n'est pas une moyenne robuste
mais un **interrupteur** qui bascule sur le niveau majoritaire. La correction fut
de passer à la **moyenne interquartile**.

Ici, le défaut est plus simple et plus direct : **aucune statistique du tout**.
Un seuil de sûreté est comparé à un échantillon unique.

## Comportement attendu

- La butée brute s'évalue sur une **statistique robuste** du brut, pas sur une
  lecture isolée.
- Elle reste **du brut sans offset** — ADR-0039 : c'est ce qui lui donne son sens.
  N'introduis pas l'offset par la porte de derrière.
- **Sa valeur de 6,20 ne bouge pas**, et son intention de sûreté est intégralement
  conservée. On supprime la loterie mono-échantillon, rien d'autre.
- Le **retard** introduit sur un arrêt de sûreté reste borné et explicitement
  chiffré.

### Cas limites
- **Sonde réellement en dérive vers l'acide** : la butée doit mordre, et vite.
  C'est le critère qui protège de la sur-correction.
- **Lectures aberrantes** (défaut I²C, sonde débranchée) : la statistique ne doit
  pas les moyenner comme si de rien n'était.
- **Fenêtre incomplète** au démarrage, après une purge, après un NaN prolongé :
  que vaut la butée tant qu'il n'y a pas assez d'échantillons ? **Fail-closed**
  par défaut.
- **Hystérésis de 0,05** à la remontée : conservée, et évaluée sur la même
  grandeur que le déclenchement.

## Comportement actuel

Comparaison directe d'un échantillon brut isolé à `6,20`, avec hystérésis de
`0,05` à la remontée. Message journalisé :
« pH BRUT x.xx sous la butée de sécurité 6.20 (offset=0.50) — injection d'acide
refusée quelle que soit la valeur corrigée : offset positif probablement erroné,
contrôler le pH au réactif ».

⚠️ **Ce libellé est trompeur et doit être corrigé dans ce lot.** Il accuse
l'offset d'être « probablement erroné ». L'offset corrige un décalage de
**mesure** — des courants de fuite — et **n'est pas une suspicion de sonde** ; le
principe a été rappelé à plusieurs reprises par l'utilisateur, et la sonde est
prouvée saine (`EZO pH slope : acide=101,0 % base=100,0 % zéro=−1,03 mV`, relevé
du 19/09). Le message envoie donc l'utilisateur sur une fausse piste, au moment
précis où il cherche à comprendre.

## Note du 2026-10-05 — correctif d'étiquetage appliqué hors cycle (pas une clôture)

Un défaut **d'étiquetage** distinct a été corrigé dans `src/pump_controller.cpp`
(même garde, même bloc), validé GO par un gate `pool-chemistry` dédié :
en production avec `ph_offset = 0,50`, le bandeau affichait « pH brut sous la
butée de sécurité (5.94 < 6.20) » pendant que la carte pH affichait **6.4**
(corrigé) — irréconciliable pour l'utilisateur, alors que la décision de
blocage elle-même était correcte.

Cause : à `offset == kPhOffsetMax`, `kPhFloorRawHard = kPhFloorCorrected −
kPhOffsetMax` rend les deux inégalités (brut < butée, corrigé < plancher)
**identiques** — les deux gardes mordent alors ensemble par construction. La
butée brute prenait la main inconditionnellement sur la cause de refus
(`setGuardRefusalCause`) même dans ce cas non discriminant, affichant un
message qui ne cite que le brut alors que c'est le corrigé qui fait foi à
l'écran.

Correctif : la cause de refus de la butée brute (et son log critique +
alerte MQTT « offset probablement erroné ») ne sont désormais émis **que si
discriminants** (la butée mord ET la garde corrigée ne mord pas). Quand les
deux mordent, c'est la cause **corrigée** qui prime, posée inconditionnellement.
Aucun seuil, aucune hystérésis, aucune condition de blocage modifiée —
`blocked = rawHard || low` est strictement inchangé. `pio run` et
`pio test -e native` verts (56 suites / 2023 cas), zéro régression.

**Ce qui reste dans le périmètre de bug-135** : ce correctif ne touche QUE
l'étiquetage/la priorité des causes de refus. Le défaut de fond — une
comparaison à un seuil dur sur une **lecture brute isolée**, sans statistique
robuste — est intact et reste entièrement à traiter ici (A1-A5, D1-D4). Ne pas
considérer ce lot comme un début de résolution de la question de la fenêtre
glissante : les deux chantiers sont orthogonaux, l'un corrige un message,
l'autre une méthode d'échantillonnage.

## Contraintes techniques

- **ADR-0039** : la butée s'évalue sur le **brut sans offset**. Non négociable.
- La statistique doit être **pure** (ADR-0017) pour être testable nativement.
- Aucune allocation sur le chemin de dosage ; tampons statiques bornés.
- Le dosage tourne sous `_mutex` ; ne pas allonger la section critique.
- `kSensorStaleTimeoutMs` et le rejet des NaN continuent de s'appliquer en amont.
- Ne pas toucher aux limites journalières, horaires, ni à la garde de présence
  d'eau.

## ⚠️ L'arbitrage central — le retard sur un arrêt de sûreté

`pool-chemistry` a validé le **principe** d'évaluer la butée sur une statistique
de bloc, **en posant une objection qu'il faut traiter de front** :

> Une médiane de **20 lectures** introduirait jusqu'à **100 s de retard** sur un
> arrêt de sûreté. Une statistique **courte — 5 lectures, 25 s, σ ≈ 0,044** —
> donne l'essentiel du gain sans ce retard.

C'est le cœur de la spec : **plus la fenêtre est longue, plus la butée est
robuste au bruit, et plus elle est lente à mordre sur une vraie dérive.**
L'arbitrage doit être chiffré dans les deux sens, pas tranché par confort.

Points à instruire :
- Quelle est la **pente maximale** d'une vraie dérive vers l'acide, et combien de
  temps peut-on se permettre de la rater ?
- Une statistique courte (5 lectures) suffit-elle à ramener le taux de faux
  déclenchement à un niveau acceptable ? σ passe de 0,099 à ≈ 0,044, donc la
  distance de 0,31 passe de 3,1 σ à **7 σ**. À chiffrer proprement.
- Faut-il une statistique **asymétrique** — robuste à la montée, réactive à la
  descente ? C'est une piste, pas une recommandation.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` — l'évaluation de la butée et son message
- `src/sensors.{h,cpp}` — si une statistique glissante doit y être exposée
- un module pur `src/ph_guard_logic.{h,cpp}` si la statistique s'extrait (ADR-0017)
- `src/constants.h` — taille de fenêtre, hystérésis
- `docs/subsystems/pump-controller.md`, `docs/adr/`

## Livrables — check-list
- [ ] A1 — Statistique robuste retenue, **avec son retard chiffré** sur un arrêt de sûreté
- [ ] A2 — Butée évaluée sur cette statistique, toujours sur le **brut sans offset** (ADR-0039)
- [ ] A3 — Comportement **fail-closed** sur fenêtre incomplète, NaN, sonde muette
- [ ] A4 — **Message corrigé** : il n'accuse plus l'offset d'être erroné
- [ ] A5 — Valeur 6,20 et hystérésis 0,05 **inchangées**, évaluées sur la même grandeur
- [ ] D1 — Rejeu de l'épisode du 20/09 sur la fixture réelle : **aucun déclenchement** à vraie valeur 6,51
- [ ] D2 — Test de **vraie dérive** : la butée mord toujours, et le retard mesuré est conforme à A1
- [ ] D3 — Lectures aberrantes (14,0 et 0,0) : comportement figé par un test, dans les **deux** directions
- [ ] D4 — **Mutation** : revenir à la lecture isolée doit tuer D1 ; allonger la fenêtre au-delà du retard admis doit tuer D2
- [ ] C1 — `docs/subsystems/pump-controller.md`
- [ ] C2 — `CHANGELOG.md`
- [ ] C3 — ADR si le compromis robustesse/retard mérite d'être figé

## Sécurité chimique
- **Dosage impliqué : OUI** — c'est une garde d'injection d'acide.
- Limites journalières et horaires : **non touchées**.
- Filtration requise avant action : **non touchée**.
- Calibration : **non touchée**.
- Algorithme PID : **non touché**.
- **Risque identifié** : la sur-correction. Une fenêtre trop longue rendrait la
  butée lente sur une vraie dérive vers l'acide. Le sens actuel de l'erreur est
  **fail-closed** ; il ne faut pas l'inverser en cherchant à supprimer les faux
  positifs. **Gate `pool-chemistry` bloquant, avant toute écriture de code.**

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte imposée : toute garde future sur le
  brut devra choisir sa statistique) · 2 (décision invisible dans le code : la
  taille de fenêtre encode un arbitrage robustesse/retard qui ne se lit nulle
  part) · 3 (alternatives crédibles : médiane longue, statistique courte,
  asymétrique, statu quo)
- **Sujet pressenti** : « Ce qu'une garde de sûreté a le droit de lire »

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WebSocket ni topic MQTT nouveau.
- **Un message de journal change** (le libellé trompeur) → `CHANGELOG.md`.
- Comportement observable : moins de refus d'injection à tort.

## Critères d'acceptation
- [ ] Rejeu de la fixture du 20/09 : **aucun déclenchement** de butée à vraie valeur 6,51
- [ ] Une vraie dérive vers l'acide déclenche **toujours** la butée, avec un retard ≤ celui chiffré en A1
- [ ] Fenêtre incomplète, NaN, sonde muette ⇒ **fail-closed**
- [ ] La valeur 6,20 et l'hystérésis 0,05 sont inchangées
- [ ] Le message n'accuse plus l'offset
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert, zéro régression ; mutants tués

## Plan de test manuel
1. Observer sur ≥ 5 jours le nombre de déclenchements de butée, et le comparer au
   relevé actuel.
2. Vérifier qu'aucune injection légitime n'est refusée en régime normal.
3. Ne **jamais** provoquer une vraie acidification pour tester la butée — le
   comportement se vérifie en **tests natifs de prédicat**.

## Notes pour agent

⚠️ **Le piège de ce lot est la sur-correction**, comme pour feature-097 et
bug-134. Supprimer les faux positifs en rendant la butée lente reviendrait à
échanger une gêne contre un risque. L'erreur actuelle est **fail-closed** : il
n'y a **aucune urgence** dans la direction dangereuse, donc aucune raison de se
précipiter.

⚠️ **Ne pas confondre les grandeurs.** Le brut, le brut + offset, la médiane de
bloc et la valeur filtrée sont **quatre** grandeurs différentes. Le journal les
affiche parfois côte à côte sans le signaler — c'est ce qui rend ces épisodes
difficiles à lire.

## Hors périmètre
- **La perturbation à la source** (courants de fuite de la pompe de filtration) —
  c'est bug-137. Corriger le signal rendrait cette spec largement sans objet,
  mais c'est du matériel et l'échéance n'est pas la même.
- Le détecteur de stabilité de purge — **feature-097, déjà livrée**.
- L'offset pH lui-même : il corrige un décalage de mesure, il n'est **pas** en
  cause, et le remettre en question serait repartir sur la fausse piste que ce
  lot ferme.
