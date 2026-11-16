#ifndef NATIVE_FAKE_PREFERENCES_H
#define NATIVE_FAKE_PREFERENCES_H

// =============================================================================
// FAUX Preferences (NVS) pour compiler le VRAI src/ota_signature_switch.cpp en natif
// (feature-151 D3-b2, condition C5). Header-only, C pur (aucune libc++ dans CE shim ; le code testé, lui, inclut <atomic>), état dans des
// variables `inline` partagées : une suite pilote le faux via FakeNvs::*.
//
// Le faux est VOLONTAIREMENT PIÉGEUX : getUInt() rend la valeur stockée MÊME si le type
// n'est pas U32 (un appelant qui sauterait isKey/getType serait trompé), et le type
// stocké est pilotable. Il ne décide de rien : il rend ce que la NVS rendrait.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum PreferenceType { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID };

namespace FakeNvs {
struct Entry { bool used; char key[16]; PreferenceType type; uint32_t value; };
inline Entry entries[8];
inline bool failBegin = false;
inline bool failPut = false;     // putUInt écrit 0 octet
inline bool failRemove = false;  // remove() rend false
inline bool failPutAfterRemoveFail = false;
inline int beginCount = 0;
inline void reset() {
  for (auto& e : entries) e.used = false;
  failBegin = failPut = failRemove = failPutAfterRemoveFail = false;
  beginCount = 0;
}
inline Entry* find(const char* k) {
  for (auto& e : entries) if (e.used && strcmp(e.key, k) == 0) return &e;
  return nullptr;
}
inline void seed(const char* k, PreferenceType t, uint32_t v) {
  Entry* e = find(k);
  if (!e) { for (auto& x : entries) if (!x.used) { e = &x; break; } }
  e->used = true; snprintf(e->key, sizeof(e->key), "%s", k); e->type = t; e->value = v;
}
}  // namespace FakeNvs

class Preferences {
 public:
  bool begin(const char*, bool = false) {
    FakeNvs::beginCount++;
    open_ = !FakeNvs::failBegin;
    return open_;
  }
  void end() { open_ = false; }
  bool isKey(const char* k) { return open_ && FakeNvs::find(k) != nullptr; }
  PreferenceType getType(const char* k) {
    auto* e = open_ ? FakeNvs::find(k) : nullptr;
    return e ? e->type : PT_INVALID;
  }
  uint32_t getUInt(const char* k, uint32_t def = 0) {
    auto* e = open_ ? FakeNvs::find(k) : nullptr;
    return e ? e->value : def;  // piège : ignore le type
  }
  size_t putUInt(const char* k, uint32_t v) {
    if (!open_ || FakeNvs::failPut) return 0;
    FakeNvs::seed(k, PT_U32, v);
    return sizeof(uint32_t);
  }
  bool remove(const char* k) {
    if (!open_ || FakeNvs::failRemove) return false;
    auto* e = FakeNvs::find(k);
    if (!e) return false;
    e->used = false;
    return true;
  }
 private:
  bool open_ = false;
};

#endif
