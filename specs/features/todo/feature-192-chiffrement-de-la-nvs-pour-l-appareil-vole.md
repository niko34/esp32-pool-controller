# feature-192 — Chiffrer la NVS : aujourd'hui, démonter l'appareil donne le mot de passe

## Métadonnées
| | |
|---|---|
| Type | feature — **durcissement, sécurité physique** |
| Statut | todo |
| Priorité | basse — ⚠️ **exige un accès PHYSIQUE à l'appareil**, et il vit au bord d'un bassin privé |
| Version cible | à définir |
| Relevé par | `security-reviewer`, recommandé en P2 de [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md) puis **re-recommandé** en P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) — ⛔ jamais ouvert jusqu'ici |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) · [ADR-0132](../../../docs/adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md) (stockage en clair **assumé**) · [`bug-187`](../doing/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md) |

## Domaines touchés
- [x] **Authentification** · [x] **Historique / persistance** (`src/config.cpp`, NVS)
- [x] OTA — ⚠️ une migration de format NVS se déploie **par OTA uniquement**
- [ ] Capteurs — [ ] Régulation — [ ] Frontend — [ ] MQTT

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE, **P1 sur le design exigée** (le choix du
      mode de chiffrement et de la gestion de clé n'est pas lisible dans un diff)
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` · [ ] `ux-ui-designer` — sans objet

## Le fait

Tout ce que l'appareil garde est écrit **en clair** dans la NVS : mot de passe
administrateur, mot de passe MQTT, SSID et mot de passe Wi-Fi.

⇒ Qui **démonte l'appareil** et lit la flash obtient l'ensemble. ⛔ Et le mot de passe Wi-Fi
ouvre le réseau du domicile, pas seulement la piscine.

⭐ **[ADR-0132](../../../docs/adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md) assume ce stockage en clair explicitement** : ce n'est pas un oubli,
c'est une décision qui n'avait jamais été remise en cause. Cette fiche est l'endroit où on
la remet en cause.

### ⚠️ Ce que ça ne change PAS

⛔ Le chiffrement de la NVS ne protège **rien** contre un attaquant qui passe par le réseau :
l'appareil déchiffre pour fonctionner. Il protège **uniquement** l'appareil **volé, perdu,
revendu ou jeté**.

⇒ ⚠️ **Ne pas le vendre comme un durcissement du chemin d'authentification.** Les deux
portes réelles de ce côté-là sont ailleurs : [`bug-189`](../done/bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md) (essais en ligne) et le HTTP clair
sur le Wi-Fi (décision assumée).

## Pourquoi la priorité est basse, et pourquoi la fiche existe quand même

**Basse** : l'appareil vit au bord d'un bassin **privé**. Le scénario exige un accès
physique, et quelqu'un qui l'a peut déjà faire bien pire à une installation de dosage
chimique.

**Mais elle existe** parce que deux passes d'audit l'ont recommandée indépendamment, et
parce que ⭐ **l'appareil est VENDU à des clients** : un appareil revendu, remplacé sous
garantie ou mis au rebut emporte le mot de passe Wi-Fi de son ancien propriétaire. ⚠️ C'est
ce point-là, pas le vol, qui justifie la fiche.

## Comportement attendu

Les secrets stockés ne sont pas lisibles par simple lecture de la flash.

### ⛔ Les questions à trancher en P1, avant toute ligne de code

1. **Où vit la clé ?** Le chiffrement NVS de l'ESP32 s'appuie sur eFuse. ⚠️ Les eFuses sont
   **irréversibles** : une erreur de manipulation **brique définitivement** la carte.
2. **Comment migre un appareil DÉJÀ EN SERVICE ?** ⛔ L'OTA est le seul canal de
   déploiement. Un firmware qui ne sait plus lire la NVS existante rend l'appareil
   **inaccessible**, sans aucune reprise possible. ⇒ La migration doit être **lisible dans
   les deux sens** pendant au moins une version, ou ne pas se faire.
3. **Qu'advient-il du secours OTA ?** La page de secours doit continuer de fonctionner.
4. **Que coûte-t-il ?** Temps de démarrage, usure de la flash, place en flash.
5. **La fabrication change-t-elle ?** ⛔ Si provisionner un appareil neuf devient plus
   complexe, le dire — c'est une contrainte de **production**, pas de logiciel.

### Cas limites
- Appareil déjà en service, NVS en clair, qui reçoit le nouveau firmware par OTA
- Retour arrière vers une version antérieure (possible par OTA)
- Appareil neuf sorti de fabrication
- Mode secours OTA après un filesystem corrompu

## Comportement actuel

NVS en clair, aucun chiffrement, aucun eFuse brûlé. Décision tracée dans [ADR-0132](../../../docs/adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md).

## Contraintes techniques

- ⛔ **L'OTA est le SEUL canal de déploiement** : pas de port série. Une migration ratée
  n'est pas rattrapable.
- ⛔ **Aucune solution ne doit exiger d'équipement ou de configuration chez le client.**
- ⚠️ **Les eFuses sont irréversibles.** Toute étape qui en brûle un doit être isolée,
  documentée, et ⛔ jamais exécutée au fil d'un déploiement ordinaire.
- Flash 8 Mo, layout v5 ([ADR-0068](../../../docs/adr/0068-partitions-layout-v5-flash-8-mo.md)) : une partition de clés changerait la table de
  partitions ⇒ ADR obligatoire.
- ⚠️ **Essai sur appareil DÉDIÉ.** ⛔ Jamais sur l'appareil de production — même règle que
  [`bug-186`](bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md).

## Livrables — check-list

- [ ] A0 — ⭐ **ÉTUDE D'ABORD** : réponses écrites aux cinq questions ci-dessus, avec le
      coût et le risque de chaque option. ⛔ **Point d'arrêt : rien ne se code avant
      l'arbitrage de l'utilisateur sur cette étude.**
- [ ] A1 — Chemin de migration d'un appareil en service, **prouvé sur appareil dédié**
- [ ] A2 — Chiffrement effectif des secrets
- [ ] A3 — Secours OTA toujours fonctionnel
- [ ] D1 — Essai de bout en bout sur appareil dédié : mise à jour, redémarrage, retour
      arrière
- [ ] D2 — Preuve de non-lisibilité : relire la flash et montrer que les secrets n'y sont
      plus en clair
- [ ] C1 — `docs/subsystems/config.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`, **ADR**
      (mise à jour ou supersession d'[ADR-0132](../../../docs/adr/0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md) sur le point du stockage en clair)

## Critères d'acceptation

- [ ] Une lecture de la flash ne rend **aucun** secret en clair — démontré, pas affirmé
- [ ] Un appareil en service migre par OTA **sans devenir inaccessible**
- [ ] Un retour arrière ne brique pas l'appareil
- [ ] Le secours OTA fonctionne toujours
- [ ] `pio run` sans nouveau warning

## Notes pour agent

- ⛔ **Le pire résultat possible de ce lot est un appareil briqué chez un client.** Ce
  risque est d'un tout autre ordre que le défaut qu'on corrige. ⇒ **L'étude A0 est un point
  d'arrêt dur**, pas une formalité.
- ⚠️ Si l'étude conclut que le rapport risque/bénéfice est défavorable, ⭐ **c'est une
  conclusion valide** : on l'écrit dans l'ADR et on ferme la fiche. ⛔ Ne pas coder pour
  justifier la fiche.

## Hors périmètre

- Les essais en ligne → [`bug-189`](../done/bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md).
- Le HTTP clair sur le Wi-Fi — décision produit assumée, ⛔ pas de spec.
- Le mot de passe du point d'accès → [`bug-187`](../doing/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md).
