## Métadonnées
- **ID** : feature-107
- **Slug** : refus-de-plan-de-dosage-visible
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à définir (`src/version.h`)
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `feature-105` (le test qui a rendu ce refus visible), `feature-073` (accumulateur sous plancher), `feature-025` (mécanisme `DoseBlockedReason`), `feature-064` (alerte `pump_unusable`)

## Domaines touchés
- [x] Régulation (`pump_controller`) → **`pool-chemistry` obligatoire**
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] Frontend (`data/`)
- [ ] Capteurs — [ ] Filtration / Éclairage — [ ] UART — [ ] OTA — [ ] Authentification — [ ] Historique

## Agents requis
- [x] spec-reader — [x] architect — [x] pool-chemistry — [x] ux-ui-designer
- [x] embedded-developer — [x] web-ui-developer — [x] test-engineer — [x] code-reviewer — [x] doc-writer

## Description
Rendre visibles les deux refus de dosage qui n'apparaissent aujourd'hui ni à l'écran,
ni en MQTT — et pour l'un des deux, nulle part du tout. Le dosage peut s'arrêter sans
qu'aucune trace ne l'explique.

## Contexte

### Ce qui existe déjà, et qu'il ne faut PAS réinventer
Le projet dispose d'un mécanisme complet « dosage bloqué + motif », posé par
`feature-025` et affiné en v2.25.1 :

- `PumpControllerClass::logRefusalOnce(pumpIndex, cause, DoseRefusal code)`
  (`src/pump_controller.cpp:588`) pose **le code puis le texte** — dans cet ordre
  précis, commenté : un lecteur asynchrone verrait au pire le nouveau code avec
  l'ancien texte, et **le code fait foi**.
- `enum class DoseRefusal` (`src/dosing_logic.h:136`) énumère déjà **18+ causes**.
- Exposition `/data` et WebSocket : `phDoseBlockedCode` / `phDoseBlockedReason` et
  leurs équivalents ORP.
- Rendu dans `data/app.js:5430` (`_doseBlockInfo`), qui traite déjà proprement le cas
  d'un **code inconnu d'une page plus ancienne que le firmware** : « un doute visible,
  pas une approximation plausible ».

**La tuyauterie est donc entière. Elle n'est simplement pas branchée sur deux refus.**

### Les deux angles morts

**1. Le refus de PLAN — totalement silencieux.**
`src/pump_controller.cpp:3345` (pH) et `:3904` (ORP) :
```c
if (plan.refused) {
  phPlanRefused = true;
  flow = 0.0f;
}
```
`phPlanRefused` est une variable **locale** qui ne sert qu'à supprimer : elle bloque la
décision de dosage et, délibérément, ne nourrit pas le compteur de non-débit (exclusion
commentée, elle évite le retour de l'inondation d'alertes de l'incident 2.20.2). Aucun
`systemLogger`, aucun code de blocage, aucun topic. La régulation s'arrête **sans trace**.

Déclencheur : volume de référence incalculable — débit à pleine puissance nul ou négatif
(pompe jamais pesée, configuration corrompue). `pool-chemistry` (2026-09-06) a confirmé
que le refus lui-même est **correct** : refuser plutôt que créditer un volume inconnu,
sans dette de rattrapage. C'est son invisibilité qui pose problème, pas son verdict.

**2. Le refus de GATE — journalisé, mais invisible à l'écran et en MQTT.**
`src/pump_controller.cpp:3200` : pompe non caractérisée (`cal_duty=0`), débit implausible,
ou amplitude sous le plancher de rotation. Il produit un `systemLogger.critical` **throttlé
à 30 min** (`kPulseRefusalLogIntervalMs`), avec un message distinct par cause — mais
n'appelle pas `logRefusalOnce` : ni code, ni motif, ni MQTT, ni écran.

Ce cas est **beaucoup plus probable que le premier** : une pompe jamais pesée au banc est
un état de configuration réel, pas une corruption. Le traiter ensemble est un choix de
périmètre assumé — corriger le refus invisible en laissant son voisin, plus probable,
invisible à l'écran serait incohérent. **À trancher explicitement si l'on veut réduire.**

**3. MQTT n'expose aucun motif de blocage.** `phDoseBlockedCode` vit dans `/data` et le
WebSocket, pas en MQTT — `mqtt_manager.cpp:2476` le cite comme précédent de « code brut »
sans qu'un capteur existe. Home Assistant ne peut donc pas alerter sur un dosage bloqué.

## Comportement attendu

### Firmware
- Les deux refus appellent `logRefusalOnce(...)` avec un code **nouveau et distinct** par
  cause. Ils héritent ainsi, sans code neuf, du journal, du code et du motif publiés.
- Le **throttling existant est conservé** : c'est lui qui a évité l'inondation de 2.20.2.
  Le refus de plan doit être journalisé **à cadence limitée**, pas à chaque tick.
- L'exclusion du compteur de non-débit **reste inchangée** : rendre le refus visible ne
  doit pas le transformer en alerte de non-débit. Ce sont deux signaux différents.

### MQTT / Home Assistant
- Un capteur de diagnostic par pompe publiant le **code brut** (pas de libellé français
  dans le topic, comme `PhFfStatus` : le mapping appartient aux clients),
  `entity_category: diagnostic`, sans `state_class` ni `unit_of_measurement`.
- Republication à la (re)connexion, sur le modèle de `pump_unusable` (feature-064) : un
  état de blocage est **permanent** tant que la cause dure, et le broker peut être absent
  au moment où il apparaît.

### Interface
- L'écran doit dire **que le dosage est bloqué et pourquoi**, pas seulement l'absence
  d'injection. `_doseBlockInfo` sait déjà rendre un code inconnu ; les nouveaux codes
  doivent recevoir un **libellé explicite** et une tonalité cohérente avec la gravité
  (une pompe non caractérisée est une faute de configuration, réparable par l'utilisateur ;
  un plan refusé est une anomalie).
- `ux-ui-designer` arbitre **où** : carte de la page pH / ORP, bandeau du tableau de bord,
  ou les deux. Un blocage de régulation qui ne se voit que sur la page d'un capteur
  particulier reste facile à manquer.

### Cas limites
- Blocage sur **une seule** des deux pompes : l'autre régulation continue et doit le rester.
- Cause qui disparaît (pompe pesée, configuration corrigée) : le code doit **repasser à
  `None`** et l'écran se nettoyer — voir `resetRefusalLogState` / `clearGuardRefusalCause`.
- Page web **plus ancienne** que le firmware : déjà géré, ne pas régresser.
- Broker MQTT absent à l'apparition du blocage : couvert par la republication.

## Comportement actuel
Refus de plan : rien, nulle part. Refus de gate : un `critical` toutes les 30 min dans le
journal système, et rien d'autre.

## Contraintes techniques
- **Aucune modification de la logique de refus elle-même.** On rend visible, on ne change
  ni un seuil, ni une limite, ni une décision. `pool-chemistry` doit vérifier que le
  verdict de refus est strictement inchangé.
- `logRefusalOnce` pose le code **avant** le texte — ordre commenté et intentionnel, à ne
  pas inverser.
- Codes ajoutés à `DoseRefusal` : **contrat public** (publiés en `/data`, WS et bientôt
  MQTT). Numéros à ne jamais réattribuer.
- Le refus de plan est **structurellement improbable** (la gate valide déjà `flow > 0`) et
  **mort en production** avec les constantes actuelles (`kPulseKickMs`=400 < `kPulseMinMs`=10000).
  Ce n'est pas une raison de le laisser muet : c'est un filet, et un filet muet ne se voit
  pas céder.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` (l. ~3200, ~3345, ~3900) — les 4 sites de refus
- `src/dosing_logic.h` — nouvelles valeurs de `DoseRefusal`
- `src/constants.h` — cadence de journalisation si une nouvelle est nécessaire
- `src/mqtt_manager.cpp` — capteur de diagnostic + auto-discovery + republication
- `data/app.js` — libellés des nouveaux codes
- `data/index.html`, `data/app.css` — si une nouvelle surface est retenue
- `test/test_native_dosing/` — tests de la logique pure

## Livrables — check-list
- [ ] A1 — nouvelles valeurs `DoseRefusal` (une par cause distincte), documentées
- [ ] A2 — refus de PLAN : appel `logRefusalOnce`, cadence limitée, pH **et** ORP
- [ ] A3 — refus de GATE : appel `logRefusalOnce` en **conservant** le `critical` throttlé
      existant et ses messages distincts par cause
- [ ] A4 — l'exclusion du compteur de non-débit est **inchangée** et re-commentée
- [ ] A5 — retour à `None` quand la cause disparaît, sur les deux pompes indépendamment
- [ ] B1 — capteur MQTT de diagnostic par pompe (code brut) + auto-discovery HA
- [ ] B2 — republication à chaque (re)connexion (modèle `pump_unusable`)
- [ ] B3 — libellés UI des nouveaux codes, tonalité par gravité
- [ ] B4 — surface retenue par `ux-ui-designer` (page capteur, tableau de bord, ou les deux)
- [ ] D1 — tests natifs des nouvelles causes, **validés par mutation**
- [ ] D2 — non-régression : 1291 cas existants
- [ ] C1 — `docs/API.md` (champs `/data`), `docs/MQTT.md` (topic + HA), `docs/features/page-ph.md`
      et `page-orp.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`
- [ ] C2 — `docs/user/src/guide-regulation.html` : un blocage de régulation est un fait
      utilisateur. Régénérer les PDF et **signaler la capture d'écran à refaire**

## Sécurité chimique
- Dosage impliqué : **oui** — pH (acide) et ORP (chlore)
- Limites concernées : **aucune modification**
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- **Risque identifié** : qu'en rendant le refus visible on modifie par inadvertance le
  moment où il est prononcé, ou qu'on le transforme en alerte de non-débit (ce qui
  ramènerait l'inondation de l'incident 2.20.2). Le verdict et le silence du compteur de
  non-débit doivent rester **strictement** identiques.

## ADR attendu ?
- **Probable** : non — le mécanisme existe, on l'étend. Sauf si `architect` juge que le
  contrat public des codes `DoseRefusal` ou le nouveau topic MQTT verrouillent un choix
  futur, auquel cas critère 4.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `/data` — nouvelles valeurs possibles de `phDoseBlockedCode` / `orpDoseBlockedCode`

### Champs WebSocket
- Aucun champ neuf : nouvelles **valeurs** des champs existants

### Topics MQTT
- **Nouveau** capteur de diagnostic par pompe + auto-discovery HA (à nommer par `architect`)

### Documentation à produire / mettre à jour
- Vivantes : `docs/features/page-ph.md`, `page-orp.md`, `docs/subsystems/pump-controller.md`
  (et `mqtt-manager.md` si le contrat change)
- Transverses : `docs/API.md`, `docs/MQTT.md`, `CHANGELOG.md`
- Utilisateur : `docs/user/src/guide-regulation.html` + PDF + capture d'écran

## Critères d'acceptation
- [ ] Un refus de plan produit une entrée de journal, un code non nul et un motif lisible
- [ ] Un refus de gate produit **en plus** de son `critical` existant un code et un motif
- [ ] Les deux apparaissent à l'écran avec un libellé explicite, et en MQTT en code brut
- [ ] La cause levée, code et motif repassent à `None` et l'écran se nettoie
- [ ] Le compteur de non-débit ne bouge pas, dans aucun des deux cas
- [ ] Aucun seuil, aucune limite, aucun verdict de refus modifié
- [ ] `pio run` sans warning nouveau ; `pio test -e native` sans régression
- [ ] Tous les libellés utilisateur en français

## Plan de test manuel
1. Forcer une pompe non caractérisée (`cal_duty = 0`) : vérifier journal, code, motif,
   écran, topic MQTT, et que l'autre pompe continue de réguler.
2. Corriger la configuration : vérifier le retour à `None` et le nettoyage de l'écran.
3. Couper le broker, provoquer le blocage, rebrancher : l'état doit être republié.
4. Ouvrir une page web d'une version antérieure : le code inconnu doit produire un doute
   visible, pas une approximation.

## Notes pour agent
- **Ne pas réinventer** : `logRefusalOnce` fait déjà journal + code + motif. Le travail
  est de l'**appeler**, pas de le doubler.
- L'exclusion du compteur de non-débit est un **correctif d'un incident réel** (2.20.2,
  inondation d'alertes). La conserver telle quelle, et re-commenter pourquoi.
- Le refus de plan est aujourd'hui du code mort en production. Les tests de `feature-105`
  couvrent la logique pure ; c'est la **coquille** qui reste à couvrir ici.

## Hors périmètre
- **Corriger la cause** d'un refus (peser la pompe, valider la configuration à la saisie) :
  cette spec rend visible, elle ne répare pas. Une validation à la saisie serait une spec
  distincte — et probablement utile.
- Le comportement du compteur de non-débit lui-même.
- `bug-106` (journée archivée à zéro sur compteur en recul) : sans rapport.
