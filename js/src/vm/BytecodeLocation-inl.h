/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeLocation_inl_h
#define vm_BytecodeLocation_inl_h

#include "vm/BytecodeLocation.h"

#include "vm/JSScript.h"

#include "vm/BytecodeUtil-inl.h"
#include "vm/JSScript-inl.h"

namespace js {

inline JS_PUBLIC_API bool BytecodeLocation::isValid(
    const JSScript* script) const {
  // Note: Don't create a new BytecodeLocation during the implementation of
  // this, as it is used in the constructor, and will recurse forever.
  return script->contains(*this) || toRawBytecode() == script->codeEnd();
}

inline bool BytecodeLocation::isInBounds(const JSScript* script) const {
  return script->contains(*this);
}
inline uint32_t BytecodeLocation::bytecodeToOffset(
    const JSScript* script) const {
  MOZ_ASSERT(this->isInBounds());
  return script->pcToOffset(this->rawBytecode_);
}

inline JSAtom* BytecodeLocation::getAtom(const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  return script->getAtom(this->rawBytecode_);
}

inline PropertyName* BytecodeLocation::getPropertyName(
    const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  return script->getName(this->rawBytecode_);
}

inline JS::BigInt* BytecodeLocation::getBigInt(const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  MOZ_ASSERT(is(JSOp::BigInt));
  return script->getBigInt(this->rawBytecode_);
}

inline JSObject* BytecodeLocation::getObject(const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  MOZ_ASSERT(is(JSOp::CallSiteObj) || is(JSOp::Object) ||
             is(JSOp::NewArrayCopyOnWrite));
  return script->getObject(this->rawBytecode_);
}

inline js::RegExpObject* BytecodeLocation::getRegExp(
    const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  MOZ_ASSERT(is(JSOp::RegExp));
  return script->getRegExp(this->rawBytecode_);
}

inline js::Scope* BytecodeLocation::getScope(const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  return script->getScope(this->rawBytecode_);
}

inline Scope* BytecodeLocation::innermostScope(const JSScript* script) const {
  MOZ_ASSERT(this->isValid());
  return script->innermostScope(this->rawBytecode_);
}

inline uint32_t BytecodeLocation::tableSwitchCaseOffset(
    const JSScript* script, uint32_t caseIndex) const {
  return script->tableSwitchCaseOffset(this->rawBytecode_, caseIndex);
}

inline uint32_t BytecodeLocation::getJumpTargetOffset(
    const JSScript* script) const {
  MOZ_ASSERT(this->isJump() || this->is(JSOp::TableSwitch));
  return this->bytecodeToOffset(script) + GET_JUMP_OFFSET(this->rawBytecode_);
}

inline uint32_t BytecodeLocation::getTableSwitchDefaultOffset(
    const JSScript* script) const {
  MOZ_ASSERT(this->is(JSOp::TableSwitch));
  return this->bytecodeToOffset(script) + GET_JUMP_OFFSET(this->rawBytecode_);
}

inline uint32_t BytecodeLocation::useCount() const {
  return GetUseCount(this->rawBytecode_);
}

inline uint32_t BytecodeLocation::defCount() const {
  return GetDefCount(this->rawBytecode_);
}

}  // namespace js

#endif
