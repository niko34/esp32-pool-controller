# feature-085 — Dose de base apprise (feedforward) en complément du PI

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | feature (architecture de régulation) |
| Statut | doing — code livré en v2.28.0 (30/08/2026), observation terrain restante |
| Priorité | Haute — c'est la réponse de fond à la critique du 30/08 |
| Origine | Critique utilisateur : *« le système d'accumulation ressemble plus à un contournement qu'à une solution »* |
| Analyse | `pool-chemistry` du 30/08/2026 — **classé n°1** sur 7 stratégies évaluées |

## Domaines touchés

- `src/pump_controller.cpp` — terme additif dans le calcul de débit
- `src/constants.h` — plafonds et fenêtre d'apprentissage
- `src/history.*` ou NVS — historique des consommations journalières
- Nouveau module pur candidat (ADR-0017) — calcul de la dose de base
- `/data`, WS, MQTT, UI — observabilité

## Sécurité chimique

**Oui, au premier chef.** On ajoute une source de dosage qui ne dépend **pas** de
l'erreur mesurée. Un feedforward mal borné dose même quand il ne faut pas.

## Le problème

Le PI est la réponse canonique à une erreur statique — ce n'est pas un
contournement. Mais il a un défaut réel ici : **il redécouvre par tâtonnement une
charge parfaitement prévisible.**

Votre bassin dégaze du CO₂ de façon régulière : ~80 % de la charge basique
quotidienne, ~1 800 mL d'acide par jour rien que pour maintenir le pH. Cette
valeur est stable, et **l'appareil la connaît déjà** — elle est dans son
historique de `ph_daily_ml`. Pourtant l'intégrale met **2,4 jours de filtration**
à la retrouver (cf. `feature-084`), et le moindre à-coup l'efface.

C'est le cas d'école du **feedforward** : injecter la charge connue, et ne laisser
au retour d'expérience que l'écart résiduel.

## Le principe

```
débit = FF(charge apprise)  +  kp × erreur  +  ki × ∫(erreur − seuil)
        └─ connu d'avance ─┘   └──── correction de l'écart ────────┘
```

**Dose de base** = médiane des `ph_daily_ml` des **7 derniers jours**, répartie
sur la fenêtre de filtration. Médiane et non moyenne : elle résiste aux journées
aberrantes (incident, purge, assistant de mesure).

**Gain attendu** : la charge est couverte **dès la première heure** au lieu de
J+2,4 — environ **+900 mL/j** disponibles immédiatement.

## Conditions bloquantes posées par `pool-chemistry`

1. **UN SEUL chemin vers la pompe.** Le feedforward entre comme **terme additif**
   dans le calcul de débit, jamais comme seconde branche de dosage. **Ne PAS
   faire cohabiter `scheduled` et `automatic`** : deux branches qui dosent en
   parallèle, ce sont deux comptabilités, deux jeux de gardes, et un double
   dosage dès la première divergence.
2. **Plafond** : le feedforward ne dépasse jamais ~**60 %** de la limite
   journalière configurée. Le PI garde la marge restante.
3. **Remise à zéro** du feedforward si : pH < consigne − 0,05 pendant 30 min ;
   mesure périmée ou invalide ; changement de produit de correction ou de
   consigne.
4. **Anti-windup recalculé** sur `(demande totale − FF)` — sinon l'intégrale se
   gèle sur une saturation causée par le feedforward, et le PI devient sourd.

## Pourquoi cette solution répond aussi aux perturbations externes

L'utilisateur cite l'appoint d'eau, l'aération, la pluie. L'analyse les chiffre
comme **marginales** face au dégazage (une pluie de 20 mm = 1,6 % de dilution),
mais elles ont un effet pervers sur le PI : en poussant l'écart sous −0,15, elles
déclenchent le **reset sec** et effacent plusieurs jours d'apprentissage.

**Le feedforward y survit** : il est fondé sur l'historique volumétrique, pas sur
l'erreur instantanée. C'est un argument de robustesse qui s'ajoute à celui de la
vitesse.

## Ce que cette solution évite

L'utilisateur refuse — à raison — de saisir le volume du bassin : *« si je donne
le volume de mon bassin et qu'il est pris en compte, l'appareil ne fonctionnera
pas avec une autre piscine »*.

Le feedforward **n'a besoin ni du volume, ni du TAC, ni de la concentration du
produit**. Il apprend directement la seule grandeur qui compte : **combien de
millilitres par jour cette installation consomme**. Le calcul volumétrique
(stratégie n°7 de l'analyse) est écarté pour cette raison — et parce qu'un modèle
inversé (dose ← ΔpH) est plus fragile qu'une mesure directe de consommation.

## Critères d'acceptation

- [x] La dose de base est apprise, jamais saisie ; aucun paramètre de volume,
      de TAC ou de concentration n'est introduit — médiane 7 j de l'anneau
      `ph_load7` (feature-084), réutilisée via `phIntegralObservedLoad()`
- [x] Un seul chemin de commande vers la pompe, une seule comptabilité de volume
      — terme additif dans la branche automatique, PI borné à `ampFlow − ff`,
      somme clampée ; vérifié en revue
- [x] Les 4 conditions bloquantes ci-dessus sont implémentées et testées —
      suite native `test/test_native_ph_feedforward/`, mutations 6/6
- [x] Le feedforward est **observable** : valeur courante servie sur `/data` ET
      WS (parité — 5 récidives du motif dans ce projet), affichée dans l'UI —
      `ph_ff_ml_per_min` + `ph_ff_status` (contrat figé 0-4), également MQTT +
      auto-discovery HA ; parité prouvée par grep des deux côtés en revue
- [ ] Sur 72 h : quota journalier jamais dépassé, aucun double comptage —
      **observation terrain requise** (itération 2)
- [ ] Fausse pluie simulée (pH −0,3) : le feedforward s'annule en < 30 min —
      **observation terrain requise** ; la tenue de 30 min est prouvée en natif
      (`kPhFfLowHoldMs`, `static_assert` ≤ 30 min), la perturbation réelle non
- [ ] Sonde figée haute : feedforward **et** PI gelés — **couvert en natif**
      (garantie structurelle : pas de session sans mesure valide + latch
      « mesure invalide »), confirmation terrain souhaitée
- [ ] Redémarrage en pleine séance : pas de rafale de rattrapage — **couvert en
      natif** (le FF est un débit, pas un accumulateur : aucun état volumique à
      rattraper), confirmation terrain souhaitée
- [x] Gate `pool-chemistry` : GO amont **et** contre-visite — GO amont sous
      4 conditions (toutes implémentées) + contre-visite GO le 30/08/2026,
      risque résiduel borné ~0,01-0,02 pH

## Ordre recommandé

**`feature-084` d'abord** — elle est quasi gratuite, corrige la cadence de
l'intégrale et améliore la situation même seule. Le feedforward vient ensuite :
il a besoin d'un PI sain pour traiter proprement l'écart résiduel.

## Alternatives évaluées et écartées (analyse du 30/08)

| Stratégie | Verdict |
|---|---|
| PI seul (production actuelle) | insuffisant seul — 2,4 j de convergence |
| **Feedforward appris + PI** | **retenu, n°1** |
| Persistance + cadence de l'intégrale | retenu aussi → `feature-084` |
| Abaisser le TAC 112 → 90 | efficace (~20 %), mais action sur l'eau, accord utilisateur requis |
| Allonger la filtration | +15-20 % net seulement (l'aération dégaze aussi) |
| Réduire pauses et plafonds | 8-16 %, ne pas descendre sous 10 min de mélange |
| Augmenter `kp` | **NON** — insuffisant (2,2 mL/min à kp=12) et dépassement probable |
| Dosage volumétrique calculé | **NON** — exige le volume, et le feedforward fait mieux sans lui |

## Notes techniques (livraison v2.28.0, 30/08/2026)

### Réalisation

- **Module pur** [`src/ph_feedforward_logic.h`](../../../src/ph_feedforward_logic.h)
  (ADR-0017) ; coquille `tickPhFeedforward()` dans `pump_controller.cpp`,
  évaluée **avant** `tickPhIntegralAuto()` dans le même tour (ordre impératif :
  l'anti-windup du PI retranche le FF de son soutenable dans le même tick).
- **Source de la dose** : la ligne « `src/history.*` ou NVS » du périmètre
  initial est résolue par **réutilisation** de l'anneau `ph_load7` de la
  feature-084 via `phIntegralObservedLoad()` — aucune seconde médiane, aucun
  second anneau (verrouillé par l'ADR-0055). Fail-closed sous 3 jours
  d'historique exploitable.
- **Les 4 conditions bloquantes** : (1) terme additif dans l'unique chemin de
  commande ; (2) plafond **double** — `kPhFfMaxSustainableFraction` = 0,60 du
  soutenable **ET** gel `kPhFfDailyQuotaFraction` = 0,60 du quota journalier
  (le gate a montré que le plafond d'actionneur seul est une condition
  affaiblie) ; (3) latch fail-closed — erreur signée < −0,05 tenue 30 min,
  mesure invalide (immédiat), changement de contexte (empreinte R2) — réarmé au
  front d'ouverture de session ; (4) anti-windup sur
  `max(0, soutenable − ff)` via `PhIntegralInputs::ffMlPerMin`.
- **Stabilité** : structurelle, par l'hystérésis de session (bilan de matière —
  la médiane apprise est non expansive). Pas d'hystérésis interne au FF.
- **Tenue produit** RAM-only (journée mixte sautée) : fail-open borné et
  chiffré par le gate à ~39 mL au pire, ~0,01-0,02 pH sur 30-50 m³ —
  persistance NVS jugée non requise (ADR-0055, alternative (e)).
- **Observabilité** : `ph_ff_ml_per_min` (2 décimales, 0 hors session) et
  `ph_ff_status` (contrat figé 0-4) en parité stricte `/data` / WS / MQTT
  (+ 2 entités HA `diagnostic`, discovery 52 → 54, dédup 21 → 23) ; ligne UI
  `#ph_ff_line` dans le bloc Rattrapage accumulé de la page pH.

### Fichiers touchés

`src/ph_feedforward_logic.h` (**nouveau**), `src/pump_controller.{h,cpp}`,
`src/ph_integral_logic.h`, `src/constants.h`, `src/web_routes_data.cpp`,
`src/ws_manager.cpp`, `src/mqtt_manager.{h,cpp}`, `src/version.h`,
`data/index.html`, `data/app.js`, `test/test_native_ph_feedforward/`
(**nouveau**), docs (`docs/adr/0055` **nouveau**, `docs/adr/README.md`,
`docs/README.md`, `docs/subsystems/pump-controller.md`,
`docs/subsystems/ws-manager.md`, `docs/API.md`, `docs/MQTT.md`,
`docs/features/page-ph.md`, `CHANGELOG.md`).

### Verdicts et chiffres

- Visa `pool-chemistry` amont : **GO sous 4 conditions** (toutes implémentées).
- Revue `code-reviewer` : **Approved**.
- Contre-visite `pool-chemistry` : **GO**, risque résiduel borné
  **~0,01-0,02 pH** (fail-open RAM-only de la tenue produit).
- Tests natifs : **932 → 970**, mutations **6/6 tuées**.
- Empreinte : **+3 884 o firmware, +273 o filesystem**.

## Suivi / itérations

| Itération | Périmètre | Statut |
|---|---|---|
| 1 | Feedforward complet : module pur, terme additif, plafond double, latch + tenue produit, anti-windup recalculé, observabilité `/data`/WS/MQTT/UI en parité, ADR-0055 | **livré — tests verts (970/970), revue OK**, gate `pool-chemistry` GO amont + contre-visite GO. Fichiers : voir « Fichiers touchés » ci-dessus |
| 2 | **Déploiement OTA + observation terrain** : 72 h sans dépassement de quota ni double comptage, fausse pluie réelle (annulation < 30 min), confirmation sonde figée et redémarrage en pleine séance (couverts en natif) — journal `Dosage pH : dose de base apprise …` attendu en début de session | ⏳ **à faire** — c'est ce qui maintient la spec dans `doing/` |
