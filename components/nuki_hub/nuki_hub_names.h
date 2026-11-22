#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace esphome::nuki_hub {

/// A value of the Nuki protocol and the text Nuki Hub uses for it
struct Name {
  uint8_t value;
  const char *text;
};

template<size_t N> const char *name_of(const Name (&table)[N], uint8_t value, const char *fallback) {
  for (const auto &row : table) {
    if (row.value == value) {
      return row.text;
    }
  }
  return fallback;
}

template<size_t N> bool value_of(const Name (&table)[N], const std::string &text, uint8_t *value) {
  for (const auto &row : table) {
    if (text == row.text) {
      *value = row.value;
      return true;
    }
  }
  return false;
}

/// Action names are accepted with a lower or an upper case first letter, like Nuki Hub does
template<size_t N> bool action_of(const Name (&table)[N], const std::string &text, uint8_t *value) {
  for (const auto &row : table) {
    if (!text.empty() && text.size() == strlen(row.text) && tolower(text[0]) == tolower(row.text[0]) &&
        text.compare(1, std::string::npos, row.text + 1) == 0) {
      *value = row.value;
      return true;
    }
  }
  return false;
}

static const Name ADVERTISING_MODES[] = {{0, "Automatic"}, {1, "Normal"}, {2, "Slow"}, {3, "Slowest"}};
static const Name BATTERY_TYPES[] = {{0, "Alkali"}, {1, "Accumulators"}, {2, "Lithium"}, {3, "No Warnings"}};
static const Name KEYPAD_TRIGGERS[] = {{0, "arrowkey"}, {1, "code"}, {2, "fingerprint"}};

}  // namespace esphome::nuki_hub
