# ADR-0137 — Un état qui court-circuite l'évaluation de dosage nomme sa cause dans le canal de refus, et les codes sont émis par le firmware seul

- **Statut** : Accepté
- **Date** : 2026-11-21
- **Décideurs** : Nicolas + Claude (`architect`, `pool-chemistry`, `code-reviewer`)
- **Spec(s) liée(s)** : [bug-200](../../specs/features/done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) ; généralise [ADR-0107](0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)

## Contexte

Quand l'appareil redémarre trop souvent, il suspend **tout** le dosage (auto, programmé, manuel), sans durée limite. `updateDosingCore()` sort alors **avant** `evaluateDose()` et avant l'enregistrement d'un code de refus. Conséquence mesurée en lecture de code : le tableau de bord affichait « Mode Auto » en vert, et le bandeau de blocage **mentait** : la cause précédente restait **gelée**, sa durée continuant de courir (« Filtration arrêtée, bloqué depuis 3 j » alors que la filtration tournait). Une sortie anticipée qui ne pose pas de code est invisible, et laisse en plus la cause d'avant affichée.

## Décision

1. Tout état qui court-circuite l'évaluation de dosage **nomme sa cause dans le canal unique des refus** (`DoseRefusal`, `setRefusalCode()`). Ici : `DoseRefusal::MaintenanceMode = 26`, ajouté **en fin d'énum**, posé par `postMaintenanceCause()` **après** la coupe des pompes. Pastille, bandeau et durée en découlent ; la durée se ré-ancre car `setRefusalCode()` redate à chaque changement de code.
2. **Les codes `DoseRefusal` sont émis par le firmware seul.** Un client ne frappe jamais un code et ne le déduit pas d'autres champs ; `maintenance_mode` (booléen) ne pilote jamais la pastille.
3. `postMaintenanceCause()` **contourne volontairement le rang 0** (`_refusalCauseFromGuard`). Légitime uniquement parce que l'arrêt ne se lève qu'au redémarrage.

## Alternatives considérées

- **Classer côté client** (`maintenance_mode` → bandeau, rejetée) : `/data` aurait continué de servir la cause gelée à tout autre consommateur (MQTT, scripts, Home Assistant).
- **Ne rien changer au firmware, masquer le bandeau** (rejetée) : l'utilisateur ne saurait toujours pas que l'eau n'est plus surveillée.
- **Poser la cause avant la coupe des pompes** (rejetée) : l'affichage ne doit jamais précéder la décision exécutée.

## Conséquences

### Positives
- Une seule voix pour la cause ; `/data`, WebSocket et le client disent la même chose.
- Le piège de rang 0 de `postSuspendCause()` n'est pas rejoué.

### Négatives / dette assumée
- ⚠️ **La sortie OTA (`pump_controller.cpp:3748`) a le même défaut** et n'est **pas corrigée** : cause précédente gelée, durée qui court. Elle dure quelques minutes et le propriétaire vient de la déclencher ; à traiter dans une fiche de suivi.
- Le code 26 est un code de plus à traiter dans la table client (`DOSE_BLOCK_CODES`), les puces et le switch de `canDose()`.

### Ce que ça verrouille
- Espace de noms `DoseRefusal` = firmware seul.
- ⛔ Si un futur lot rend l'arrêt **levable à chaud**, le contournement du rang 0 redevient le défaut de [bug-172](../../specs/features/done/bug-172-la-cause-de-refus-affichee-est-figee-quand-le-dosage-est-suspendu.md) (cause chimique perdue définitivement) : il faudra un nouvel ADR.
- `kDoseRefusalMaxValue` suit la dernière valeur (sentinelle `static_assert`).

## Références

- Code : `src/dosing_logic.h` (`MaintenanceMode`), `src/pump_controller.cpp` (`postMaintenanceCause`, `updateDosingCore`), `src/web_routes_control.cpp` (`/rescue/start` 409)
- Doc : [pump-controller.md](../subsystems/pump-controller.md), [page-dashboard.md](../features/page-dashboard.md)
