# Incidents — coredumps de production

Les **pièces à conviction** des crashs analysés : le dump brut relevé sur
l'appareil, et sa trace décodée en texte.

Ils sont versionnés (163 Ko au total) parce qu'ils sont **irremplaçables** : un
coredump n'existe qu'une fois, il est écrasé au crash suivant, et plusieurs specs
de diagnostic s'appuient dessus. Le reste de `builds/` — 2,6 Go d'artefacts de
compilation — reste local et régénérable.

## Ce qu'il faut pour les relire

Un fichier `.txt` se lit tel quel : il porte la pile d'appels déjà résolue.

Un fichier `.bin` a besoin de **l'ELF de la version exacte** qui tournait au
moment du crash. `esp-coredump` vérifie le SHA256 de l'image applicative et
**refuse** un ELF qui ne correspond pas — c'est cette vérification qui a permis de
dater le crash du 05/09 avec certitude.

```bash
export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin:$PATH"
~/.platformio/penv/bin/esp-coredump --chip esp32 info_corefile \
  --core docs/incidents/<fichier>.bin --core-format raw \
  <chemin vers firmware.elf de la bonne version>
```

**Où trouver l'ELF** : dans les assets des releases GitHub à partir de la v2.42.0.
Pour les versions antérieures, il n'existe que dans `builds/` en local — c'est
précisément le manque qui a rendu **indécodable** le coredump du 15/08/2026.

## Les incidents

| Fichier | Version | Tâche | Chemin fautif |
|---|---|---|---|
| `2026-08-29-panic-fopen-page-html` | 2.26.0 | `async_tcp` | service d'une **page HTML** statique |
| `2026-09-03-panic` | 2.33.0 | — | échec d'allocation de 92 o |
| `2026-09-05-panic-fopen-historique` | 2.40.0 | `loopTask` | **sauvegarde de `/history.json`** |

Les trois partagent le **même mécanisme** : `lock_init_generic` appelle `abort()`
faute d'environ 88 octets pour un verrou newlib, sur un `fopen`. Aucun code de
retour à tester, aucun repli — l'appareil redémarre.

⚠️ Ce chemin passe par `malloc` (C), pas `operator new` : **le parachute OOM est
inopérant par construction** dessus.

Le crash du 29/08 est couvert depuis le lot B-3 (`bug-080`), qui refuse de servir
une page plutôt que d'y entrer sans mémoire. **Celui du 05/09 ne l'est pas** : la
garde protège les requêtes HTTP, pas ce que le firmware ouvre pour son propre
compte. C'est l'objet de `bug-100`.

## Analyses

- `specs/features/doing/bug-080-redemarrage-panic-et-fragmentation-heap.md`
- `specs/features/todo/bug-100-abort-sur-fopen-de-l-historique.md`
- `docs/subsystems/oom-guard.md` — le post-mortem RTC, complémentaire du coredump
- [ADR-0066](../adr/0066-sonde-du-tas-au-moment-du-refus.md) — la sonde au moment du refus
- [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md) — la garde 503
