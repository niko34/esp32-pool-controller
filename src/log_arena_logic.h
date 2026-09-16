#ifndef LOG_ARENA_LOGIC_H
#define LOG_ARENA_LOGIC_H

// =============================================================================
// log_arena_logic — Décisions PURES de l'arène circulaire du journal (ADR-0017)
// =============================================================================
// feature-129 (phase A), option B′ : deux objets `.bss` remplacent le
// `std::vector<LogEntry>` de `String` qui fractionnait le plus gros bloc libre
// du tas 8 bits (A1 : 160 o alloués → 7 680 o de contiguïté détruits).
//
//   sLogArena[kLogArenaBytes]   texte des messages, file circulaire d'octets
//   sLogSlots[kMaxLogEntries]   descripteurs POD (LogSlot), file circulaire
//
// Ce module ne fait QUE décider : où écrire, combien évincer, comment couper,
// comment formater l'en-tête, si une ligne doit être persistée. La coquille
// impure (logger.cpp, phase C — PAS ce lot) porte le `memcpy`, le mutex et
// l'I/O flash.
//
// INVARIANT (ADR-0017) : zéro dépendance Arduino, zéro mutex, zéro I/O. Aucun
// `String`, aucun `#include <Arduino.h>`. <stdint.h>/<stddef.h> uniquement (et
// <constants.h>, lui-même self-contained — stddef/stdint/time.h).
//
// CONTRAINTE DE PILE : ce module ne déclare et ne manipule JAMAIS un tableau
// local de taille kLogMessageMaxBytes — log() s'exécute depuis async_tcp, qui a
// une pile courte. Les fonctions ci-dessous ne font QUE des calculs sur des
// index/longueurs ; le `memcpy` réel reste dans la coquille impure.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Niveau d'une entrée de journal. Défini ICI (et non dans logger.h) car
 *        ce module pur en a besoin ; `logger.h` le réexporte (`#include
 *        "log_arena_logic.h"`) pour tout le reste du firmware — aucun
 *        changement de nom ni de valeur, mêmes cinq niveaux, même ordre.
 */
enum class LogLevel {
  DEBUG,
  INFO,
  WARNING,
  ERROR,
  CRITICAL
};

/**
 * @brief Descripteur POD d'une entrée de journal (20 o, `.bss`, tableau
 *        `sLogSlots[kMaxLogEntries]`, phase C).
 *
 * `seq` porte un numéro d'ordre ABSOLU (jamais réinitialisé, jamais dérivé
 * d'un index d'anneau) : `log_persist_overrun` (C6) et `logShouldPersist` (C7)
 * en ont tous deux besoin, et le dériver d'un index de tête cesse d'être
 * fiable dès que l'éviction par OCTETS désynchronise l'anneau du compteur de
 * poussées (une éviction peut faire disparaître plusieurs entrées d'un coup).
 */
struct LogSlot {
  uint32_t timestamp;  // millis() au moment de l'écriture
  uint32_t wallSec;    // time(nullptr) au moment de l'écriture (0 = horloge non fixée)
  uint32_t seq;        // numéro d'ordre ABSOLU depuis le boot
  uint16_t off;         // offset de début du message dans sLogArena
  uint16_t len;         // longueur du message en octets (déjà tronqué/clampé)
  uint8_t  level;       // LogLevel, stocké en uint8_t pour tenir le POD à 20 o
  uint8_t  flags;       // kLogFlagTruncated, etc.
};

constexpr uint8_t kLogFlagTruncated = 1u << 0;

/**
 * @brief Plan d'écriture d'un message de `len` octets dans l'arène circulaire.
 */
struct LogArenaPlan {
  uint16_t writeOff;    // offset où écrire (0 si `wrapped`, sinon `head`)
  uint16_t tailDrop;    // octets abandonnés en fin d'arène (politique « pas d'enjambement »)
  uint16_t evictBytes;  // octets à libérer côté queue AVANT d'écrire (via logRingEvictCount)
  bool     wrapped;     // l'écriture repart-elle à l'offset 0 ?
};

/**
 * @brief Décide où écrire `len` octets dans l'arène, et combien évincer avant.
 *
 * Politique C4 (« pas d'enjambement », condition bloquante pool-chemistry) :
 * si la place contiguë en fin d'arène (`cap - head`) est insuffisante pour
 * `len`, cette place est ABANDONNÉE (`tailDrop`) et l'écriture repart à
 * l'offset 0 — un message n'est JAMAIS scindé en deux, ce qui permet à tout
 * lecteur de rendre un `const char*` contigu. Le gaspillage est borné par
 * `kLogMessageMaxBytes` (`tailDrop < len`, puisque le wrap ne se déclenche que
 * lorsque l'espace restant est strictement inférieur à `len`).
 *
 * `tailDrop` est COMPTÉ dans `evictBytes` : sans cela, l'appelant sous-estime
 * l'occupation réelle de l'anneau après le saut et finit par réécrire une
 * entrée pas encore évincée (recouvrement silencieux).
 *
 * `used` lève l'unique ambiguïté d'un anneau circulaire à deux index
 * (`head == tail` : anneau VIDE si `used == 0`, PLEIN si `used > 0`) — un
 * compteur d'occupation à côté de deux index est le remède classique à cette
 * ambiguïté ; c'est son SEUL rôle ici, la géométrie fait le reste via `tail`.
 *
 * @param cap  capacité totale de l'arène (kLogArenaBytes).
 * @param head prochain offset d'écriture avant cette décision.
 * @param tail offset de début du plus ancien message encore vivant.
 * @param used octets actuellement occupés (compteur tenu par l'appelant).
 * @param len  longueur du message à écrire (déjà ≤ kLogMessageMaxBytes).
 * @return     le plan d'écriture ; `evictBytes` est à passer tel quel à
 *             `logRingEvictCount()` (`bytesNeeded`) avant d'écrire.
 */
LogArenaPlan logArenaPlan(uint16_t cap, uint16_t head, uint16_t tail, uint16_t used, uint16_t len);

/**
 * @brief Longueur à conserver de `src` (≤ `len`) pour tenir dans `maxBytes` en
 *        réservant la place du marqueur de troncature AVANT le clamp UTF-8
 *        (condition bloquante C2, modèle `mqtt_manager.cpp:761-762`).
 *
 * Si `len <= maxBytes`, rien à faire : retourne `len`, `truncated = false`.
 * Sinon : `keep = maxBytes - sizeof(kLogTruncTail)` (budget du marqueur réservé
 * D'ABORD), puis `keep = utf8ClampLen(src, keep)` (jamais de coupe au milieu
 * d'un caractère) ; `truncated = true`. L'appelant écrit alors `src[0..keep)`
 * suivi du marqueur `kLogTruncTail`.
 *
 * @param src       octets sources (pas nécessairement NUL-terminés au-delà de `len`).
 * @param len       longueur de `src`.
 * @param maxBytes  budget total disponible pour le message (kLogMessageMaxBytes).
 * @param truncated [out] vrai si une troncature a eu lieu.
 * @return          nombre d'octets de `src` à conserver (hors marqueur).
 */
size_t logTruncatePlan(const char* src, size_t len, size_t maxBytes, bool& truncated);

/**
 * @brief Nombre de créneaux (à partir de `oldest`, dans l'ordre chronologique)
 *        à évincer pour libérer au moins `bytesNeeded` octets.
 *
 * Parcourt `slots[(oldest + i) % n]` pour `i` croissant, cumule `.len`, et
 * s'arrête dès que le cumul atteint `bytesNeeded` (ou après `n` créneaux si
 * `bytesNeeded` dépasse la capacité totale de l'anneau — best-effort borné).
 *
 * @param slots       tableau de descripteurs (sLogSlots), non modifié.
 * @param n           nombre de créneaux dans le tableau (kMaxLogEntries).
 * @param oldest      index du créneau le plus ancien (point de départ).
 * @param bytesNeeded octets à libérer (typiquement `LogArenaPlan::evictBytes`).
 * @return            nombre de créneaux à évincer (0 si `bytesNeeded == 0`).
 */
size_t logRingEvictCount(const LogSlot* slots, size_t n, size_t oldest, size_t bytesNeeded);

/**
 * @brief Formate l'en-tête d'une ligne persistée : `"AAAA-MM-JJTHH:MM:SS NIV "`.
 *
 * `wallSec` est interprété comme des secondes depuis l'epoch DÉJÀ ajustées au
 * fuseau souhaité par l'appelant (ce module fait une décomposition calendaire
 * grégorienne pure, sans dépendre de `TZ`/`localtime_r` — l'ajustement fuseau,
 * lui, reste un calcul impur à la charge de la coquille, phase C).
 *
 * `wallSec == 0` (horloge non fixée) produit le repli `"????-??-??T??:??:?? "`
 * — jamais une ligne datée « 1970 », qui laisserait croire à une mesure réelle.
 *
 * @param out   tampon de sortie.
 * @param n     capacité de `out`.
 * @param wallSec secondes depuis l'epoch (0 = horloge non fixée).
 * @param level niveau de l'entrée.
 * @return      valeur de type snprintf() : nombre d'octets qui AURAIENT été
 *              écrits hors NUL (peut dépasser `n` si `out` est trop petit).
 */
int logFormatHeader(char* out, size_t n, uint32_t wallSec, LogLevel level);

/**
 * @brief Une entrée doit-elle rejoindre le tampon de persistance ?
 *
 * Condition bloquante C7 : les lignes émises AVANT que la persistance ne soit
 * activée (`seq < persistEnableSeq`, rattrapage de démarrage) sont filtrées à
 * `WARNING` et au-dessus — sans ce filtre, un démarrage verse plus de
 * `kMaxLogFileBytes` (8192 o) d'`INFO` et déclenche la rotation immédiatement,
 * emportant les lignes de sécurité qu'on vient d'écrire. Après activation, la
 * règle courante s'applique, inchangée : tout sauf `DEBUG`.
 *
 * @param seq              numéro d'ordre absolu de l'entrée (LogSlot::seq).
 * @param persistEnableSeq numéro d'ordre au moment où la persistance a été activée.
 * @param level            niveau de l'entrée.
 */
bool logShouldPersist(uint32_t seq, uint32_t persistEnableSeq, LogLevel level);

/**
 * @brief Index (dans `slots`) du PROCHAIN créneau vivant à flusher, ou `n` si
 *        aucun (rattrapé : tous les créneaux vivants ont `seq < flushCursor`).
 *
 * Parcourt les créneaux VIVANTS dans l'ORDRE CHRONOLOGIQUE (`liveCount`
 * créneaux depuis `oldest`) et retourne l'index du PREMIER dont `seq >=
 * flushCursor`. La comparaison est `>=`, JAMAIS `>` (mutant #10, phase B) :
 * une comparaison stricte laisserait le créneau exactement au curseur de côté
 * (ligne sautée) ; à l'inverse, faire avancer le curseur autrement qu'à
 * `seq + 1` après consommation (coquille impure, phase C) rejouerait la même
 * ligne (dupliquée). Ce module ne décide QUE de la cible, pas de l'avancement
 * du curseur — la coquille l'avance après consommation du créneau trouvé.
 *
 * @param slots       tableau de descripteurs (sLogSlots), non modifié.
 * @param n           nombre de créneaux dans le tableau (kMaxLogEntries).
 * @param oldest      index du créneau le plus ancien vivant.
 * @param liveCount   nombre de créneaux vivants (<= n).
 * @param flushCursor prochain seq à flusher.
 * @return            index dans `slots` (0..n-1), ou `n` si rien à flusher.
 */
size_t logNextFlushSlot(const LogSlot* slots, size_t n, size_t oldest,
                         size_t liveCount, uint32_t flushCursor);

/**
 * @brief Le plan d'écriture `[writeOff, writeOff + writeLen)` recoupe-t-il
 *        encore un créneau VIVANT de l'anneau ?
 *
 * Filet de sécurité INDÉPENDANT de `logArenaPlan()`/`logRingEvictCount()` —
 * l'incident du 16/09/2026 a montré qu'une erreur de géométrie dans ces deux
 * fonctions peut laisser passer un recouvrement SANS qu'aucun des cinq
 * compteurs de l'arène ne le révèle (les compteurs restaient parfaitement
 * cohérents PENDANT la corruption). L'appelant DOIT invoquer cette fonction
 * après avoir appliqué l'éviction décidée par `logArenaPlan()` (donc avec le
 * `liveCount`/`oldest` déjà à jour) et AVANT tout `memcpy` dans `sLogArena`.
 *
 * Parcourt les `liveCount` créneaux vivants depuis `oldest` (borné à `n`,
 * best-effort) et teste le chevauchement de deux intervalles. C'est un test
 * NON circulaire (pas d'arithmétique modulo `cap`) : la politique « pas
 * d'enjambement » (C4) garantit qu'aucun créneau — ni celui qu'on s'apprête à
 * écrire — ne franchit jamais la frontière de fin d'arène, donc chaque
 * intervalle `[off, off+len)` est déjà contigu et borné par `cap`.
 *
 * @param slots     tableau de descripteurs (sLogSlots), non modifié.
 * @param n         nombre de créneaux dans le tableau (kMaxLogEntries).
 * @param oldest    index du créneau le plus ancien vivant, APRÈS éviction.
 * @param liveCount nombre de créneaux vivants (<= n), APRÈS éviction : ce
 *                  sont les créneaux qui vont SURVIVRE à l'écriture en cours.
 * @param writeOff  offset où le nouveau message va être écrit (plan.writeOff,
 *                  ou 0 après un repli d'urgence).
 * @param writeLen  longueur du nouveau message (finalLen, après troncature).
 * @return          true si l'intervalle d'écriture recoupe un créneau vivant.
 */
bool logArenaWriteOverlapsLive(const LogSlot* slots, size_t n, size_t oldest,
                                size_t liveCount, uint16_t writeOff, uint16_t writeLen);

#endif  // LOG_ARENA_LOGIC_H
