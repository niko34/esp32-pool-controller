# feature-136 — Le budget horaire de dosage survit au redémarrage

## Métadonnées
- **ID** : feature-136
- **Slug** : budget-horaire-de-dosage-survit-au-redemarrage
- **Type** : feature — **fermeture d'une exposition chimique résiduelle**
- **Statut** : todo
- **Priorité** : **haute** — c'est la seule des trois specs de suivi du 20/09 qui porte un risque de **sur-dosage**, pas de sous-dosage
- **Version cible** : 2.54+
- **Dernière mise à jour** : 2026-09-20
- **Spec(s) liée(s)** : `specs/features/done/bug-134-…` (**c'est là que le défaut a été trouvé**, et c'est lui qui a emporté le GO de ce lot) · ADR-0034

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** — budget horaire, anti-cycling, pause de mélange
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] **Historique / persistance** — NVS, usure, atomicité
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT**
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

`DosingState` (`src/pump_controller.h`) est **entièrement en RAM** : `usedMs`,
`windowStart`, `lastStartTime`, `cyclesToday`. Seuls les **millilitres
journaliers** et les minutes de filtration sont persistés en NVS
(`src/config.cpp`).

**Conséquence : tout redémarrage remet à zéro le budget horaire, l'anti-cycling
et la pause de mélange.** À 10 min/h de pH, **deux redémarrages dans l'heure
autorisent jusqu'à 20 minutes d'acide**, seul le plafond journalier faisant
barrage.

## Contexte — comment ce défaut a été trouvé

Relevé le **2026-09-20** par le gate `pool-chemistry` de `bug-134`, en instruisant
une tout autre question : *« les 15 min de pire cas avant reboot sont-elles
acceptables ? »*

Sa réponse a inversé le compromis apparent :

> **Retarder un reboot n'assouplit rien ; le déclencher à tort assouplit une
> garde de dosage.**

C'est cet argument qui a emporté le GO de `bug-134`. Mais il laisse une dette
explicite : **`bug-134` réduit la FRÉQUENCE des redémarrages inutiles, il ne
supprime pas l'exposition.** Un redémarrage **légitime** — OTA, coupure secteur,
watchdog matériel, auto-guérison mémoire — rend exactement le même budget neuf.

### Ce que le projet sait déjà, et qui aggrave le sujet

- Le contrôleur **redémarre volontairement** dans plusieurs cas : watchdog de
  connectivité, auto-guérison mémoire, disjoncteur de boot-loop de l'historique.
- `kAutoHealMaxReboots = 3` autorise jusqu'à **trois** redémarrages automatiques
  dans une fenêtre avant d'entrer en mode maintenance.
- Le journal du 19/09 portait déjà `Auto-guérison : 1 reboot(s) auto récent(s)`.
- Les déploiements OTA sont **hebdomadaires** selon le rythme du projet.

### Ce que cela fait concrètement

| Grandeur | Persistée ? | Effet d'un redémarrage |
|---|---|---|
| millilitres journaliers | ✅ NVS | conservés |
| minutes de filtration | ✅ NVS | conservées |
| **`usedMs`** (budget horaire) | ❌ RAM | **remis à zéro** |
| **`windowStart`** (début de fenêtre) | ❌ RAM | **remis à zéro** |
| **`lastStartTime`** (anti-cycling) | ❌ RAM | **remis à zéro** |
| **`cyclesToday`** | ❌ RAM | **remis à zéro** |

## Comportement attendu

- Le **budget horaire** survit au redémarrage : un appareil qui a consommé 8 des
  10 minutes de sa fenêtre en retrouve 8 consommées, pas 0.
- L'**anti-cycling** survit : on ne redémarre pas une pompe juste après un reboot
  si le délai minimal n'est pas écoulé.
- La **pause de mélange** survit : elle existe pour laisser le produit se
  disperser avant de mesurer à nouveau, et un redémarrage ne la rend pas caduque.
- La **fenêtre glissante** reprend à son vrai point d'origine, pas à l'instant du
  démarrage.

### Cas limites — et ils sont le cœur du sujet
- **Horloge invalide au démarrage** (RTC morte, NTP pas encore synchronisé) :
  impossible de savoir combien de temps s'est écoulé. **Que vaut le budget dans
  ce cas ?** Fail-closed (budget considéré comme consommé) est le comportement
  sûr, mais il bloquerait le dosage au pire moment. **C'est l'arbitrage central.**
- **Coupure longue** (plusieurs heures) : la fenêtre horaire est de toute façon
  périmée, le budget doit repartir. À partir de quelle durée ?
- **Redémarrage pendant une injection en cours** : `usedMs` n'a pas été
  comptabilisé jusqu'au bout. Compter le temps écoulé, ou l'ignorer ? Le compter
  est conservateur.
- **Usure NVS** : persister à chaque tick est exclu. Quelle granularité ? Quel
  coût en écritures par an ?
- **Perte de la NVS** (effacement, partition corrompue) : fail-closed.

## Comportement actuel

`DosingState` est reconstruit **vide** à chaque démarrage. Seuls les compteurs
journaliers sont restaurés depuis la NVS — le journal le montre :
`[Config] Compteurs journaliers restaurés: pH=480 mL, ORP=1141 mL`.

Aucun message ne signale que le budget **horaire**, lui, vient d'être remis à zéro.

## Contraintes techniques

- **Usure NVS** : c'est la contrainte dimensionnante. Une écriture par cycle de
  dosage est probablement acceptable ; une par tick ne l'est pas. À chiffrer en
  écritures par an, avec l'endurance réelle (~100 k cycles, avec wear-levelling).
- **L'horloge doit être valide** pour recalculer une fenêtre glissante. Le projet
  dispose d'une RTC DS3231 **et** de NTP, plus un `isTimeValid()` déjà utilisé
  ailleurs. La mémoire RTC de l'ESP32 survit à un reset logiciel mais **pas** à
  une coupure secteur — les deux cas doivent être distingués.
- Le dosage s'exécute sous `_mutex` : ne pas allonger la section critique, et
  **aucune écriture NVS** en la tenant.
- La logique de fenêtre glissante doit être **pure** (ADR-0017), testable
  nativement, y compris ses cas d'horloge invalide.
- `esp_task_wdt_reset()` sur tout chemin allongé.
- Ne pas toucher aux plafonds eux-mêmes, ni à la garde de présence d'eau, ni aux
  limites journalières.

## Fichiers potentiellement concernés
- `src/pump_controller.{h,cpp}` — `DosingState` et son cycle de vie
- `src/config.cpp` — persistance NVS (le patron des compteurs journaliers existe)
- un module pur `src/dosing_window_logic.{h,cpp}` (ADR-0017)
- `src/constants.h` — clés NVS, granularité de persistance, seuil de péremption
- `docs/subsystems/pump-controller.md`, `docs/adr/`

## Livrables — check-list
- [ ] A1 — `usedMs`, `windowStart`, `lastStartTime`, `cyclesToday` persistés
- [ ] A2 — **Granularité de persistance** choisie et **chiffrée en écritures par an**
- [ ] A3 — Reconstruction de la fenêtre glissante au démarrage, à partir d'une horloge **vérifiée**
- [ ] A4 — **Comportement sur horloge invalide**, tranché et testé (fail-closed attendu)
- [ ] A5 — Seuil de **péremption** d'une fenêtre après une longue coupure
- [ ] A6 — Journalisation au démarrage : dire **ce qui a été restauré**, comme le font déjà les compteurs journaliers
- [ ] D1 — Tests natifs de la logique de fenêtre : reprise à chaud, coupure courte, coupure longue, horloge invalide, horloge qui recule
- [ ] D2 — **Mutation** : ne pas persister `usedMs` ⇒ un test doit mourir ; ignorer `windowStart` au redémarrage ⇒ un autre
- [ ] C1 — `docs/subsystems/pump-controller.md`
- [ ] C2 — `CHANGELOG.md`
- [ ] C3 — ADR si le compromis usure NVS / fidélité du budget est structurant

## Sécurité chimique
- **Dosage impliqué : OUI, directement.** C'est le budget d'injection lui-même.
- **Risque du défaut actuel** : **sur-dosage**. Deux redémarrages dans l'heure
  autorisent le double du budget horaire de pH. C'est le seul des trois suivis du
  20/09 qui va dans la direction dangereuse.
- **Risque du correctif** : l'inverse. Un budget restauré à tort — horloge
  fausse, fenêtre mal reconstruite — **bloquerait** le dosage sans raison. Le
  fail-closed doit être choisi sciemment, et son coût énoncé.
- Limites journalières, garde de présence d'eau, PID : **non touchés**.
- **Gate `pool-chemistry` bloquant avant toute écriture de code**, et second
  passage après implémentation.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte imposée : toute garde temporelle
  future devra choisir entre RAM et NVS) · 2 (décision invisible : la granularité
  de persistance encode un arbitrage usure/fidélité) · 4 (verrouillage : un
  format persisté est un contrat qui survit aux mises à jour)
- **Sujet pressenti** : « Ce qu'un redémarrage a le droit d'oublier »

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WebSocket ni topic MQTT nouveau.
- **Une ligne de journal nouvelle au démarrage** (ce qui a été restauré).
- Comportement observable : après un redémarrage, le dosage peut être refusé
  alors qu'il était autorisé auparavant — **c'est voulu**, et cela doit être dit
  clairement dans le CHANGELOG pour ne pas passer pour une régression.

## Critères d'acceptation
- [ ] Après un redémarrage, le budget horaire consommé est **conservé**
- [ ] L'anti-cycling et la pause de mélange survivent
- [ ] Horloge invalide au démarrage ⇒ **fail-closed**, et c'est testé
- [ ] Une coupure plus longue que la fenêtre fait repartir le budget
- [ ] Le coût en écritures NVS par an est **chiffré** et tient dans l'endurance
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués

## Plan de test manuel
1. Doser jusqu'à consommer une partie du budget horaire, redémarrer, vérifier que
   le reliquat est correct.
2. Redémarrer après une coupure secteur de plus d'une heure : le budget repart.
3. Redémarrer RTC débranchée et sans réseau : le dosage est refusé jusqu'à ce que
   l'horloge soit valide.
4. Suivre le compteur d'écritures NVS sur une semaine et extrapoler.

## Notes pour agent

⚠️ **Ce lot va dans la direction dangereuse, contrairement aux deux autres suivis
du 20/09.** bug-135 et bug-137 corrigent des gardes qui refusent **à tort** —
erreur fail-closed, sans urgence. Celui-ci corrige une garde qui **autorise à
tort**. C'est pourquoi il est en priorité haute.

⚠️ **L'horloge est le point dur, pas la persistance.** Écrire quatre champs en
NVS est mécanique. Savoir **combien de temps s'est écoulé** quand l'appareil
redémarre sans horloge fiable ne l'est pas — et s'y tromper transforme une
protection en blocage.

⚠️ Le projet a déjà une mémoire RTC utilisée par le disjoncteur de boot-loop et
par la dispense de purge : **regarder comment ces mécanismes distinguent un reset
logiciel d'une coupure secteur** avant d'en réinventer un.

## Hors périmètre
- Les **valeurs** des plafonds horaires et journaliers.
- Le mode maintenance et `kAutoHealMaxReboots` — leur révision est un sujet à part,
  déjà signalé comme tel dans bug-134.
- La réduction du **nombre** de redémarrages : c'est bug-134, livrée.
