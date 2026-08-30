# ADR-0055 — Feedforward appris additif plafonné (dose de base pH)

- **Statut** : Accepté
- **Date** : 2026-08-30
- **Décideurs** : pool-chemistry (visa amont GO sous 4 conditions + contre-visite GO, 30/08/2026), architect, utilisateur (interdiction du paramètre de volume)
- **Spec(s) liée(s)** : feature-085 (dose de base apprise), s'appuie sur feature-084 (anneau de charge `ph_load7`)

## Contexte

Le PI (ADR-0052) redécouvre par tâtonnement une charge **parfaitement prévisible** : le dégazage de CO₂ (~80 % de la charge basique quotidienne) est stable d'un jour à l'autre, et l'appareil le connaît déjà — c'est la médiane 7 jours de l'anneau de charge `ph_load7` créé par la feature-084. Pourtant l'intégrale met **2,4 jours de filtration** à retrouver cette valeur, et le moindre à-coup (pluie, appoint) l'efface via le reset sec. La critique utilisateur du 30/08 (*« le système d'accumulation ressemble plus à un contournement qu'à une solution »*) a déclenché une analyse `pool-chemistry` de 7 stratégies : le feedforward appris est classé n°1.

Le danger est symétrique du bénéfice : un feedforward est la **seule source de dosage qui ne dépend pas de l'erreur mesurée**. Mal borné, il dose même quand il ne faut pas, et sans mesure valide personne ne peut constater qu'il dose à tort.

## Décision

La dose de base apprise entre comme **terme additif** dans le calcul de débit de la branche automatique pH :

```
débit = FF(charge apprise) + kp·e + ki·∫(e − seuil)
avec  FF = min(charge observée, 0,60 × débit soutenable)
```

- **Source** : la médiane 7 jours de l'anneau `ph_load7` (feature-084), **réutilisée telle quelle** via `phIntegralObservedLoad()` — aucune seconde médiane, aucun second anneau. Fail-closed sous 3 jours d'historique exploitable (même seuil que le plafond de restauration R1).
- **Plafond double** : (1) plafond d'**actionneur** `kPhFfMaxSustainableFraction` = 0,60 du débit moyen soutenable — le PI garde au moins 40 % d'autorité pour l'écart résiduel ; (2) **gel de quota** `kPhFfDailyQuotaFraction` : FF = 0 dès que le volume pH du jour atteint 60 % de `maxPhMlPerDay`.
- **Latch de sécurité** (réarmé au front d'ouverture de la session automatique suivante) : erreur signée < −0,05 tenue 30 min de temps de branche, mesure invalide (fail-closed immédiat, sans tenue), changement de contexte de régulation (produit / consigne / vitesse / plafond, même empreinte R2 que la persistance).
- **Tenue produit** : au changement de `phCorrectionType`, le latch est tenu jusqu'à l'archivage d'une journée **entière** sous le nouveau régime (la journée mixte est sautée). RAM-only.
- **Anti-windup recalculé** : le soutenable vu par le tick intégral devient `max(0, soutenable − ff)` ; symétriquement, le PI instantané est borné à `ampFlow − ff` et la somme reste clampée à `ampFlow`.

## Alternatives considérées

- **(a) Seconde branche de dosage type `scheduled`** (rejetée par `pool-chemistry`, condition bloquante n°1) — deux branches qui dosent en parallèle, ce sont **deux comptabilités de volume, deux jeux de gardes**, et un double dosage dès la première divergence. Le terme additif passe par le chemin existant (`computePID` → `planPulse` → comptabilité) : un seul chemin vers la pompe.
- **(b) Réguler le FF lui-même** (boucle sur le FF, hystérésis interne, ou asservissement du FF à l'erreur) (rejetée) — inutile et source d'un second point de réglage. La stabilité est **structurelle, par hystérésis de session** : le FF ne vit que sous `shouldDose` ; s'il sur-corrige, l'erreur retombe sous le seuil d'arrêt, la session se ferme et le FF cesse **avec elle** — sa condition d'existence est précisément la session qu'il vient de fermer. L'argument de fond est le **bilan de matière** : à l'équilibre, le volume journalier livré converge vers la charge réelle du bassin **quel que soit le FF** (un FF trop grand raccourcit les sessions, un FF trop petit laisse le PI compléter) — la médiane apprise sur ces volumes est donc **non expansive**, elle ne peut pas s'auto-amplifier d'un jour sur l'autre. Le plafond d'actionneur n'est qu'une **ceinture**, pas le mécanisme.
- **(c) Dosage volumétrique calculé** (dose ← ΔpH × volume × TAC, stratégie n°7 de l'analyse) (rejetée) — exige le **volume du bassin**, dont la saisie est **interdite par l'utilisateur** (*« si je donne le volume de mon bassin et qu'il est pris en compte, l'appareil ne fonctionnera pas avec une autre piscine »*), et un modèle inversé est plus fragile qu'une mesure directe de consommation. Le feedforward apprend la seule grandeur qui compte : combien de mL/jour **cette** installation consomme.
- **(d) Plafond d'actionneur seul, sans gel de quota** (rejetée par le gate, condition n°2) — « 60 % du soutenable » n'implique **pas** « 60 % du quota » : c'est une condition **affaiblie**. Contre-exemple du gate : quota 300 mL/j avec 12 h de filtration ⇒ 0,42 mL/min suffisent à consommer 100 % du quota, alors que le plafond d'actionneur autorise ~1,30 mL/min — le FF viderait **seul** la limite journalière et le PI n'aurait plus aucune marge.
- **(e) Persister la tenue produit en NVS** (rejetée) — borne chiffrée par le gate : un reboot qui lève la tenue laisse un FF encore borné par le plafond d'actionneur et le gel de quota, soit **~39 mL au pire** avant la clôture de journée suivante, ~0,01-0,02 pH sur un bassin de 30-50 m³. Fail-open **borné et chiffré**, la persistance est jugée non requise.

## Conséquences

### Positives
- La charge connue est couverte **dès la première heure de filtration** (au lieu de J+2,4 pour l'intégrale seule) : sur l'installation de référence, FF = 1,30 mL/min pour une charge de 2,86, soit ~45 % de la charge immédiatement, le PI conservant 0,87 mL/min d'autorité.
- Robustesse aux perturbations : fondé sur l'historique volumétrique et non sur l'erreur instantanée, le FF **survit** aux excursions qui déclenchent le reset sec de l'intégrale.
- Aucun paramètre nouveau : ni volume, ni TAC, ni concentration — la dose de base est apprise, jamais saisie.
- Aucun état volumique : le FF est un **débit**, pas un accumulateur — pas de « dette » à rattraper après un reboot (pas de rafale).

### Négatives / dette assumée
- La tenue produit RAM-only est un fail-open borné (~39 mL au pire, cf. (e)).
- Le FF ajoute un troisième contributeur au débit (FF + P + I) : le diagnostic d'un débit observé exige désormais les champs d'observabilité (`ph_ff_ml_per_min`, `ph_ff_status`) — servis en parité `/data` / WS / MQTT dès cette version.
- Ne change **rien** à la limitation par l'actionneur (feature-086) : sur une installation à limite horaire basse, la consigne peut rester hors d'atteinte, FF ou pas.

### Ce que ça verrouille
- **Un seul chemin de commande vers la pompe** : tout futur contributeur de dosage entre comme terme du calcul de débit de la branche automatique, jamais comme branche parallèle.
- Le contrat du code d'état `PhFfStatus` (0 inactif, 1 actif, 2 latch, 3 historique insuffisant, 4 quota) est **figé** — jamais renuméroté, jamais inséré au milieu (même règle que `DoseRefusal`, ADR-0049).
- `kPhFfDailyQuotaFraction < kPhIntegralFreezeDailyFraction` (`static_assert`) : le gel du FF mord **avant** le gel d'anti-windup de l'intégrale — l'autorité aveugle à l'erreur cède toujours la première.
- Pas d'hystérésis interne au FF : la stabilité appartient à l'hystérésis de session existante, en ajouter une seconde créerait un double réglage.
- L'anneau `ph_load7` reste la **source unique** de la charge observée (restauration R1 **et** FF) : créer une seconde médiane est interdit.

## Références

- Code : `src/ph_feedforward_logic.h` (module pur, ADR-0017), `src/pump_controller.cpp` (`tickPhFeedforward`, terme additif, tenue produit), `src/ph_integral_logic.h` (`PhIntegralInputs::ffMlPerMin`), `src/constants.h` (`kPhFf*`)
- Tests : `test/test_native_ph_feedforward/`
- Spec : `specs/features/doing/feature-085-dose-de-base-apprise-feedforward.md`
- Visa `pool-chemistry` du 30/08/2026 (GO amont sous 4 conditions + contre-visite GO)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module pur), [ADR-0049](0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) (contrat de code figé, tenue en durées observées), [ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md) (le PI que le FF complète), [ADR-0053](0053-persistance-conditionnelle-integrale-ph.md) (anneau de charge, empreinte de contexte R2), [ADR-0054](0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) (soutenable = fonction de la configuration)
