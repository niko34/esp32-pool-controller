## Métadonnées
- **ID** : bug-208
- **Slug** : la-barre-du-dosage-du-jour-se-remplit-vers-un-plafond-inatteignable
- **Type** : bug (affichage — ⛔ aucune garde de dosage en cause)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-201](../done/bug-201-l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation.md)
  (trouvé en instruisant son étude d'ergonomie) · ADR-0053 / feature-086 (« limité par l'actionneur »)

## Domaines touchés
- [x] **Frontend (`data/`)** — carte héros des pages pH et ORP
- [ ] Régulation · [ ] Capteurs · [ ] MQTT · [ ] OTA · [ ] Authentification
- [ ] Web — routes HTTP / WebSocket *(les deux valeurs sont déjà servies)*

## Agents requis
- [x] spec-reader · [x] ux-ui-designer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] pool-chemistry — ⚠️ **à cocher si** la correction change ce que l'utilisateur croit être sa
      limite : c'est un chiffre qui oriente ses réglages de dosage. En cas de doute, le cocher.

## Description
Sur la page pH, la barre « **DOSAGE DU JOUR** » se remplit vers **2 000 mL**, alors que la ligne
juste en dessous annonce « **Maximum journalier : 1 768 mL/j** ». ⇒ La barre ne pourra **jamais**
se remplir : elle vise un plafond que l'appareil n'atteindra pas.

## Ce que j'ai vérifié sur l'appareil (2026-11-22, v2.99.0)
⛔ **Ce ne sont PAS deux affichages de la même chose** — l'hypothèse « doublon » est fausse :

| Clé servie | Valeur | Ce que c'est |
|---|---|---|
| `max_ph_ml_per_day` (`/get-config`) | **2000** | le plafond que l'utilisateur a **réglé** |
| `ph_daily_max_ml` (`/data`) | **1768** | ce que la pompe peut **réellement délivrer** en un jour |

⭐ La seconde est **dérivée du matériel** (débit de la pompe × `ph_limit_minutes` par heure) —
c'est l'acquis d'ADR-0053 / feature-086, *« limité par l'actionneur »*. ⛔ Les deux chiffres sont
donc **utiles tous les deux**, et en retirer un ferait disparaître un fait.

🔴 **Le défaut est que la BARRE prend le mauvais dénominateur** : elle se remplit vers le plafond
**réglé** (2000) alors que la limite **opposable** est l'autre (1768). ⇒ À 1 768 mL, le dosage
s'arrête avec une barre à **88 %**, et rien à l'écran n'explique pourquoi.

⚠️ **Et c'est l'inverse qui serait dangereux** : si un jour le réglage descendait **sous** la
capacité de la pompe, la barre se remplirait trop vite. ⇒ Le dénominateur correct est
**le minimum des deux**, pas l'un des deux.

## Comportement attendu
1. La barre se remplit vers la limite **réellement opposable** : `min(max_ph_ml_per_day, ph_daily_max_ml)`.
2. ⚠️ Quand les deux diffèrent, l'écran **dit pourquoi** — ⛔ sans jargon : le plafond réglé n'est
   pas atteignable parce que la pompe ne peut pas débiter autant dans une journée.
3. ⛔ **Ne pas masquer le chiffre réglé** : l'utilisateur doit pouvoir constater que son réglage
   est au-dessus de ce que son matériel sait faire — c'est une information d'installation.
4. ⚠️ **Vérifier le même défaut sur la page ORP** : `max_orp_ml_per_day` = 2000 et
   `orp_daily_target_ml` = 1500 sont servis de la même façon.

### Cas limites
- Les deux clés **égales** ⇒ ⛔ aucune explication affichée, rien ne cloche.
- Une clé **absente** (firmware antérieur) ⇒ se rabattre sur celle qui existe, ⛔ jamais sur un
  nombre écrit en dur côté client (**ADR-0100**).
- `ph_daily_max_ml` à **0** ou non fini ⇒ ⛔ ne pas diviser par zéro, ⛔ ne pas afficher 0 %.

## Contraintes techniques
- ⛔ **Aucun champ firmware à ajouter** : les deux valeurs sont **déjà servies**.
- ⛔ **Aucun seuil recalculé côté navigateur** (ADR-0100) — on compare deux clés servies, on n'en
  dérive pas une troisième.
- ⚠️ **390 px d'abord.** La carte héros pH porte déjà 13 éléments (compté sur capture le
  2026-11-22) : ⛔ toute explication ajoutée doit être **compensée**, cf. bug-201.
- Français, sans jargon de code.

## Fichiers potentiellement concernés
- `data/app.js` — la barre et son dénominateur (`ph_daily_max_ml`, autour de `:7326`)
- `data/index.html` — carte héros pH, et son équivalent ORP
- `docs/features/page-ph.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] B1 — la barre se remplit vers la limite opposable, pages pH **et** ORP
- [ ] B2 — l'écart entre réglé et atteignable est **expliqué**, sans jargon
- [ ] D1 — vecteurs : les deux clés égales · réglé > atteignable · réglé < atteignable · clé
      absente · clé nulle. **Mutation** : remettre le dénominateur réglé ⇒ un vecteur **meurt**
- [ ] D2 — capture à 390 px, avant / après
- [ ] C1 — docs + CHANGELOG

## Sécurité chimique
- Dosage impliqué : ⛔ **non** — aucune limite n'est modifiée, seul leur **affichage** l'est.
- ⚠️ Risque indirect, et c'est la raison d'être de cette fiche : un propriétaire qui croit son
  plafond à 2 000 mL **règle sa consigne en conséquence**. Un chiffre faux à l'écran oriente de
  vraies décisions de dosage.

## Comment ce bug a été trouvé
En instruisant bug-201, l'`ux-ui-designer` a relevé « deux plafonds pour une même notion » et
proposé de **fusionner les deux lignes**. ⛔ Vérification faite sur l'appareil : ce n'est pas un
doublon, et fusionner aurait **effacé une information vraie**.
⭐ *Le constat était juste, le diagnostic était faux* — et c'est la vérification sur l'appareil,
pas la lecture du code, qui a fait la différence.
