// =============================================================================
// Tests unitaires natifs — registre pur des clients WS authentifiés (bug-081)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// Vérifient le COMPORTEMENT observable du header pur
// src/ws_client_registry_logic.h (ADR-0017) : insert / erase / snapshot /
// empty / count / clear sur un tableau fixe de slots atomiques, id==0 = libre.
//
// Dimensionnement réel du projet : kWsRegistrySlots = kWsMaxClients + 1 = 5
// (constants.h). On teste sur N=5 pour coller à l'instanciation réelle, plus
// quelques cas sur un N plus petit pour les scénarios de saturation rapides.
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include "ws_client_registry_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. insert simple / idempotence / réutilisation de slot après erase
// ---------------------------------------------------------------------------

void test_insert_simple(void) {
  WsClientRegistry<5> reg;
  TEST_ASSERT_TRUE(reg.empty());
  WsRegistryInsert r = reg.insert(42);
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, r);
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
  TEST_ASSERT_FALSE(reg.empty());
}

void test_insert_idempotent_same_id_twice(void) {
  WsClientRegistry<5> reg;
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(7));
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
  // Même id une seconde fois : kAlreadyPresent, cardinalité INCHANGÉE.
  WsRegistryInsert r2 = reg.insert(7);
  TEST_ASSERT_EQUAL(WsRegistryInsert::kAlreadyPresent, r2);
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
  // Et une troisième fois, pour être sûr qu'aucun slot n'est consommé.
  reg.insert(7);
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
}

void test_insert_idempotent_does_not_block_other_ids(void) {
  WsClientRegistry<5> reg;
  reg.insert(1);
  reg.insert(1);  // idempotent, ne doit pas manger de slot
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(2));
  TEST_ASSERT_EQUAL_UINT(2, reg.count());
}

void test_insert_after_erase_reuses_slot(void) {
  WsClientRegistry<2> reg;  // N petit : sature vite
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(1));
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(2));
  // Plein : un 3e id distinct est refusé.
  TEST_ASSERT_EQUAL(WsRegistryInsert::kFull, reg.insert(3));
  // On libère un slot, puis le 3e id doit maintenant passer.
  TEST_ASSERT_TRUE(reg.erase(1));
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(3));
  TEST_ASSERT_EQUAL_UINT(2, reg.count());
}

// ---------------------------------------------------------------------------
// 2. erase présent / absent
// ---------------------------------------------------------------------------

void test_erase_present_id(void) {
  WsClientRegistry<5> reg;
  reg.insert(10);
  TEST_ASSERT_TRUE(reg.erase(10));
  TEST_ASSERT_TRUE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(0, reg.count());
}

void test_erase_absent_id_returns_false_and_breaks_nothing(void) {
  WsClientRegistry<5> reg;
  reg.insert(10);
  reg.insert(20);
  TEST_ASSERT_FALSE(reg.erase(999));  // jamais inséré
  TEST_ASSERT_EQUAL_UINT(2, reg.count());  // rien cassé
  TEST_ASSERT_TRUE(reg.erase(10));
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
  TEST_ASSERT_FALSE(reg.erase(10));  // déjà effacé : absent maintenant
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
}

void test_erase_zero_is_rejected(void) {
  WsClientRegistry<5> reg;
  // id == 0 est la sentinelle « slot libre » : erase(0) ne doit jamais
  // prétendre avoir effacé quoi que ce soit, même sur un registre vide.
  TEST_ASSERT_FALSE(reg.erase(0));
}

void test_insert_zero_is_rejected(void) {
  WsClientRegistry<5> reg;
  TEST_ASSERT_EQUAL(WsRegistryInsert::kFull, reg.insert(0));
  TEST_ASSERT_TRUE(reg.empty());
}

// ---------------------------------------------------------------------------
// 3. snapshot : contenu exact, cardinalité, troncature, aucun id 0
// ---------------------------------------------------------------------------

void test_snapshot_exact_content_and_cardinality(void) {
  WsClientRegistry<5> reg;
  reg.insert(11);
  reg.insert(22);
  reg.insert(33);
  uint32_t out[5] = {0, 0, 0, 0, 0};
  size_t n = reg.snapshot(out, 5);
  TEST_ASSERT_EQUAL_UINT(3, n);
  // Ordre non garanti par le contrat (c'est un ensemble de slots), on vérifie
  // la PRÉSENCE de chaque id attendu parmi les n premiers éléments écrits.
  bool found11 = false, found22 = false, found33 = false;
  for (size_t i = 0; i < n; ++i) {
    if (out[i] == 11) found11 = true;
    if (out[i] == 22) found22 = true;
    if (out[i] == 33) found33 = true;
    TEST_ASSERT_NOT_EQUAL(0, out[i]);  // jamais 0 dans un snapshot
  }
  TEST_ASSERT_TRUE(found11 && found22 && found33);
}

void test_snapshot_cap_smaller_than_n_truncates_and_announces(void) {
  WsClientRegistry<5> reg;
  reg.insert(1);
  reg.insert(2);
  reg.insert(3);
  reg.insert(4);
  uint32_t out[2] = {0, 0};
  size_t n = reg.snapshot(out, 2);  // cap < N et < count() réel (4)
  TEST_ASSERT_EQUAL_UINT(2, n);  // troncature annoncée par la valeur de retour
  TEST_ASSERT_NOT_EQUAL(0, out[0]);
  TEST_ASSERT_NOT_EQUAL(0, out[1]);
}

void test_snapshot_never_contains_id_zero_on_empty_registry(void) {
  WsClientRegistry<5> reg;
  uint32_t out[5] = {99, 99, 99, 99, 99};  // valeurs sentinelles non nulles
  size_t n = reg.snapshot(out, 5);
  TEST_ASSERT_EQUAL_UINT(0, n);  // rien écrit : les 99 restent inchangés,
  // mais surtout aucun 0 n'a été produit par la fonction elle-même.
}

void test_snapshot_never_contains_id_zero_on_partial_registry(void) {
  WsClientRegistry<5> reg;
  reg.insert(5);
  reg.insert(6);
  // 3 slots restent à 0 en interne : vérifie que snapshot() ne les recopie pas.
  uint32_t out[5] = {0, 0, 0, 0, 0};
  size_t n = reg.snapshot(out, 5);
  TEST_ASSERT_EQUAL_UINT(2, n);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_NOT_EQUAL(0, out[i]);
  }
}

// ---------------------------------------------------------------------------
// 4. saturation
// ---------------------------------------------------------------------------

void test_saturation_at_ws_registry_slots_yields_full(void) {
  // Dimensionnement réel du projet : kWsRegistrySlots = kWsMaxClients + 1 = 5.
  WsClientRegistry<5> reg;
  for (uint32_t id = 1; id <= 5; ++id) {
    TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(id));
  }
  TEST_ASSERT_EQUAL_UINT(5, reg.count());
  // Le 6e id, distinct de tous les précédents, doit être refusé.
  TEST_ASSERT_EQUAL(WsRegistryInsert::kFull, reg.insert(6));
  TEST_ASSERT_EQUAL_UINT(5, reg.count());  // inchangé par le refus
}

void test_saturation_then_erase_then_insert_succeeds(void) {
  WsClientRegistry<5> reg;
  for (uint32_t id = 1; id <= 5; ++id) reg.insert(id);
  TEST_ASSERT_EQUAL(WsRegistryInsert::kFull, reg.insert(999));
  TEST_ASSERT_TRUE(reg.erase(3));
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(999));
  TEST_ASSERT_EQUAL_UINT(5, reg.count());
}

// ---------------------------------------------------------------------------
// 5. empty() / count() sur registre vide, partiel, plein
// ---------------------------------------------------------------------------

void test_empty_and_count_on_empty_registry(void) {
  WsClientRegistry<5> reg;
  TEST_ASSERT_TRUE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(0, reg.count());
}

void test_empty_and_count_on_partial_registry(void) {
  WsClientRegistry<5> reg;
  reg.insert(1);
  reg.insert(2);
  TEST_ASSERT_FALSE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(2, reg.count());
}

void test_empty_and_count_on_full_registry(void) {
  WsClientRegistry<5> reg;
  for (uint32_t id = 1; id <= 5; ++id) reg.insert(id);
  TEST_ASSERT_FALSE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(5, reg.count());
}

// ---------------------------------------------------------------------------
// 6. clear() — utilisé par la révocation (bug-081, condition bloquante #3)
// ---------------------------------------------------------------------------

void test_clear_empties_full_registry(void) {
  WsClientRegistry<5> reg;
  for (uint32_t id = 1; id <= 5; ++id) reg.insert(id);
  TEST_ASSERT_FALSE(reg.empty());
  reg.clear();
  TEST_ASSERT_TRUE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(0, reg.count());
  uint32_t out[5];
  TEST_ASSERT_EQUAL_UINT(0, reg.snapshot(out, 5));
}

void test_clear_then_insert_again_works(void) {
  WsClientRegistry<5> reg;
  reg.insert(1);
  reg.clear();
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(1));
  TEST_ASSERT_EQUAL_UINT(1, reg.count());
}

void test_clear_on_already_empty_registry_is_safe(void) {
  WsClientRegistry<5> reg;
  reg.clear();  // ne doit rien casser sur un registre déjà vide
  TEST_ASSERT_TRUE(reg.empty());
  TEST_ASSERT_EQUAL_UINT(0, reg.count());
}

// ---------------------------------------------------------------------------
// 1bis. Idempotence AVEC UN SLOT LIBRE AVANT LE DOUBLON (revue bug-081)
// ---------------------------------------------------------------------------
// Trouvé en revue : la suite initiale ne couvrait l'idempotence que lorsque le
// doublon occupait le PREMIER slot. Dans ce cas, le filet de rattrapage de la
// passe 2 (`if (expected == id)`) l'absorbait, et retirer la passe 1 ne tuait
// AUCUN test — le mutant survivait.
//
// Ici on creuse un trou AVANT le doublon : insert(1), insert(2), erase(1).
// Le slot 0 est libre, le slot 1 porte l'id 2. Sans la passe 1, la passe 2
// prend le slot 0 libre AVANT de rencontrer l'id 2, et l'id 2 se retrouve
// DEUX fois dans le registre.
//
// Conséquence en production, et c'est pourquoi ce test existe : la trame
// partirait deux fois au même client (2 des 5 slots de WS_MAX_QUEUED_MESSAGES)
// et erase() n'en libérerait qu'un seul — le second slot serait fuité
// DÉFINITIVEMENT, jusqu'au redémarrage.
void test_insert_idempotent_with_free_slot_before_duplicate(void) {
  WsClientRegistry<5> reg;
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(1));
  TEST_ASSERT_EQUAL(WsRegistryInsert::kInserted, reg.insert(2));
  TEST_ASSERT_TRUE(reg.erase(1));           // trou au slot 0, id 2 au slot 1
  TEST_ASSERT_EQUAL_UINT(1, reg.count());

  TEST_ASSERT_EQUAL_MESSAGE(WsRegistryInsert::kAlreadyPresent, reg.insert(2),
      "id deja present apres un trou : doit etre kAlreadyPresent, pas kInserted");
  TEST_ASSERT_EQUAL_MESSAGE(1, reg.count(),
      "le doublon ne doit PAS consommer un second slot (slot fuite sinon)");

  // Et le doublon ne doit apparaitre qu'une fois dans le snapshot.
  uint32_t ids[5] = {0};
  size_t n = reg.snapshot(ids, 5);
  TEST_ASSERT_EQUAL_UINT(1, n);
  TEST_ASSERT_EQUAL_UINT32(2, ids[0]);

  // Un seul erase doit suffire a vider le registre.
  TEST_ASSERT_TRUE(reg.erase(2));
  TEST_ASSERT_TRUE_MESSAGE(reg.empty(),
      "un seul erase doit suffire : sinon un slot est reste occupe");
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_insert_simple);
  RUN_TEST(test_insert_idempotent_same_id_twice);
  RUN_TEST(test_insert_idempotent_does_not_block_other_ids);
  RUN_TEST(test_insert_idempotent_with_free_slot_before_duplicate);
  RUN_TEST(test_insert_after_erase_reuses_slot);
  RUN_TEST(test_erase_present_id);
  RUN_TEST(test_erase_absent_id_returns_false_and_breaks_nothing);
  RUN_TEST(test_erase_zero_is_rejected);
  RUN_TEST(test_insert_zero_is_rejected);
  RUN_TEST(test_snapshot_exact_content_and_cardinality);
  RUN_TEST(test_snapshot_cap_smaller_than_n_truncates_and_announces);
  RUN_TEST(test_snapshot_never_contains_id_zero_on_empty_registry);
  RUN_TEST(test_snapshot_never_contains_id_zero_on_partial_registry);
  RUN_TEST(test_saturation_at_ws_registry_slots_yields_full);
  RUN_TEST(test_saturation_then_erase_then_insert_succeeds);
  RUN_TEST(test_empty_and_count_on_empty_registry);
  RUN_TEST(test_empty_and_count_on_partial_registry);
  RUN_TEST(test_empty_and_count_on_full_registry);
  RUN_TEST(test_clear_empties_full_registry);
  RUN_TEST(test_clear_then_insert_again_works);
  RUN_TEST(test_clear_on_already_empty_registry_is_safe);
  return UNITY_END();
}
