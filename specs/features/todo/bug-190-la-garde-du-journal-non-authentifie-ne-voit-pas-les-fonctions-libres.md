# bug-190 — La garde du journal non authentifié ne regarde que les lambdas : une fonction libre passe inaperçue

## Métadonnées
| | |
|---|---|
| Type | bug — **outillage CI / sécurité** |
| Statut | todo |
| Priorité | normale — ⚠️ **angle mort d'une garde**, pas un défaut livré |
| Version cible | à définir |
| Relevé par | `security-reviewer`, passe P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md), 2026-11-17 |
| Specs liées | [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) (origine de la garde) · [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (ce qui l'a révélé) |

## Domaines touchés
- [x] **Outillage / CI** (`tools/check_unauth_log_guard.sh`, `.github/workflows/hygiene-specs.yml`)
- [x] Authentification (périmètre couvert par la garde)
- [ ] Capteurs — [ ] Régulation — [ ] Frontend — [ ] MQTT — [ ] OTA

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE (la garde est un contrôle de sécurité)
- [x] spec-reader · [x] architect · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `embedded-developer` — ⚠️ seulement si des appels de `src/` doivent bouger
- [ ] `pool-chemistry` · [ ] `ux-ui-designer` — sans objet

## Le fait

`tools/check_unauth_log_guard.sh` interdit tout `systemLogger` atteignable **sans
authentification**. C'est l'acquis de [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md), et il est réel.

⛔ **Mais sa règle 3 ne reconnaît que les LAMBDAS** — les handlers écrits directement dans
le `server->on(...)`. Une **fonction libre** appelée depuis un de ces lambdas lui est
**invisible**.

### Comment ça s'est vu

[`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a ajouté **trois** `systemLogger` sur le chemin d'authentification. Ils vivent
dans des fonctions libres (`logAuthFailure`, `logLoginSuccess`). ⇒ La garde est restée
**verte** sans les avoir examinés.

⭐ **Ils sont corrects** — mais cela a été établi par la **lecture** de deux revues, ⛔ pas
par le script. Et c'est exactement le genre de certitude qui ne survit pas au prochain lot :
personne ne relira à la main dans six mois, et la garde continuera de dire « OK ».

⚠️ **Le défaut n'est donc pas un bug livré, c'est une garde qui promet plus qu'elle ne
tient.** C'est le même travers que [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md) et [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) viennent de corriger
ailleurs : un commentaire, ou ici un script, qui affirme une propriété que le code ne tient
pas. ⇒ Le projet traite ces cas-là, il ne les laisse pas passer.

## Comportement attendu

La règle 3 doit **suivre l'appel** : une fonction libre qui prend un
`AsyncWebServerRequest*` et qui est appelée depuis un handler avant la garde
d'authentification doit être jugée **comme si son corps était dans le handler**.

### Cas limites à traiter explicitement

- Une fonction libre appelée depuis **plusieurs** handlers, dont certains authentifiés et
  d'autres non ⇒ ⛔ le cas le plus permissif doit l'emporter (si **un seul** chemin est
  anonyme, elle est jugée anonyme).
- Une chaîne d'appels à **deux niveaux** (handler → fonction A → fonction B) : dire
  jusqu'où le contrôle remonte, et ⛔ **l'écrire dans l'en-tête du script** plutôt que de
  laisser croire à une profondeur infinie.
- L'exemption `AUTH-LOG-EXEMPT` doit continuer de fonctionner, et ⛔ rester le **seul**
  moyen de déroger.
- ⚠️ Faux positifs : une fonction libre qui prend un `AsyncWebServerRequest*` mais n'est
  appelée que depuis un chemin authentifié ne doit pas bloquer la CI sans raison.

## Comportement actuel

Règle 3 vérifiée sur les lambdas seulement. Les fonctions nommées appelées depuis un lambda
**ne sont pas jugées** — c'est d'ailleurs écrit noir sur blanc dans `docs/BUILD.md`
(« ⚠️ les fonctions nommées appelées depuis un lambda ne sont pas jugées »), donc le projet
le **savait** sans l'avoir traité.

## Contraintes techniques

- Le script reste en **bash**, sans dépendance nouvelle : les jobs d'hygiène tournent en
  ~1 s, sans Python ni PlatformIO.
- ⛔ Il doit rester lançable **en local** à l'identique de la CI.
- ⚠️ Un contrôle d'appel en bash a des limites réelles : ⛔ **les écrire** dans l'en-tête
  du script plutôt que de les taire — c'est très exactement le défaut qu'on corrige.

## Livrables — check-list

- [ ] A1 — Règle 3 étendue aux fonctions libres prenant un `AsyncWebServerRequest*`
- [ ] A2 — En-tête du script mis à jour : ce qu'il prouve **et ce qu'il ne prouve pas**
      (profondeur d'appel couverte, faux positifs possibles)
- [ ] D1 — ⭐ **Validation par MUTATION du script lui-même** : introduire un `systemLogger`
      non borné dans une fonction libre appelée depuis un handler anonyme, et vérifier que
      la garde **refuse**. ⛔ Une garde qui n'a jamais refusé ne prouve rien.
- [ ] D2 — Mutation inverse : un cas légitime (chemin authentifié, ou `AUTH-LOG-EXEMPT`)
      ne doit **pas** être refusé
- [ ] D3 — Le script reste vert sur le dépôt tel quel
- [ ] C1 — `docs/BUILD.md` (l'avertissement actuel devient caduc, ou se précise)

## Critères d'acceptation

- [ ] Un `systemLogger` non borné dans une fonction libre atteignable sans authentification
      **fait échouer** la garde — démontré par mutation
- [ ] Les trois lignes `[Auth] …` de bug-183 sont **effectivement examinées** par la règle,
      et passent
- [ ] Aucun faux positif sur le dépôt courant
- [ ] Le job tourne toujours en ~1 s, sans dépendance nouvelle

## Notes pour agent

- ⚠️ **Ne pas élargir la garde au point de la rendre ingérable.** Mieux vaut un contrôle à
  une profondeur d'appel, **dont la limite est écrite**, qu'un contrôle qui prétend tout
  voir et produit des faux positifs qu'on finira par désactiver.
- ⭐ Le modèle à suivre est `tools/check_secret_compare_guard.sh` (bug-183) : son en-tête
  dit explicitement ce qu'il ne prouve pas.

## Hors périmètre

- La rétention des traces de sécurité → [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md).
- Les autres règles du script (1, 2, 4, 5) — ⛔ ne pas y toucher sans arbitrage.
