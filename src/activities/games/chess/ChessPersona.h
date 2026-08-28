#pragma once

#include <I18n.h>

#include "ChessSearch.h"

// The five opponents the player picks between. Personas, not "Level 3": a name
// and a rating is what a player recognises, and the numeric level only ever
// appears as an array index. Order matches chess::LEVELS, weakest first.
namespace chess_persona {

struct Persona {
  StrId name;
  StrId styleHint;  // one-line description shown under the name in the picker
  uint16_t rating;  // approximate, and labelled as such in the UI
};

inline const Persona* table() {
  static const Persona PERSONAS[chess::LEVEL_COUNT] = {
      {StrId::STR_CHESS_PERSONA_1, StrId::STR_CHESS_PERSONA_HINT_1, 600},
      {StrId::STR_CHESS_PERSONA_2, StrId::STR_CHESS_PERSONA_HINT_2, 900},
      {StrId::STR_CHESS_PERSONA_3, StrId::STR_CHESS_PERSONA_HINT_3, 1200},
      {StrId::STR_CHESS_PERSONA_4, StrId::STR_CHESS_PERSONA_HINT_4, 1500},
      {StrId::STR_CHESS_PERSONA_5, StrId::STR_CHESS_PERSONA_HINT_5, 1800},
  };
  return PERSONAS;
}

inline int clampLevel(int level) {
  if (level < 0) return 0;
  return (level >= chess::LEVEL_COUNT) ? chess::LEVEL_COUNT - 1 : level;
}

inline const Persona& at(int level) { return table()[clampLevel(level)]; }

}  // namespace chess_persona
