"""The parsing helpers of nuki_hub, compiled for the host.

Run with: python3 -m unittest discover -s tests -v
Needs a C++ compiler and the ArduinoJson headers: set ARDUINOJSON_DIR to a directory holding
ArduinoJson.h, or build any ESPHome configuration with this component once (the copy ESPHome
downloads is found by itself). Skipped if neither is there.
"""

import glob
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def find_arduinojson():
    candidates = [os.environ.get("ARDUINOJSON_DIR", "")]
    for pattern in (
        str(ROOT / "**/.esphome/**/bblanchon__arduinojson*/src"),
        str(Path.home() / ".cache/esphome/**/bblanchon__arduinojson*/src"),
        str(Path.home() / ".platformio/lib/**/ArduinoJson/src"),
    ):
        candidates += sorted(glob.glob(pattern, recursive=True))
    for candidate in candidates:
        if candidate and (Path(candidate) / "ArduinoJson.h").exists():
            return candidate
    return None


PRELUDE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <ArduinoJson.h>

namespace esphome::nuki_hub {
inline ArduinoJson::Allocator *json_memory() { return ArduinoJson::detail::DefaultAllocator::instance(); }
namespace util {
void date_time(char *out, int year, int month, int day, int hour, int minute, int second);
void time_of_day(char *out, int hour, int minute);
void weekdays_to_json(uint8_t weekdays, JsonArray array);
uint8_t weekdays_from_json(JsonVariantConst value);
std::string fixed_string(const uint8_t *data, size_t size);
bool parse_date_time(const std::string &text, uint16_t *out);
bool parse_time_of_day(const std::string &text, uint8_t *out);
std::string as_text(JsonVariantConst value);
bool is_number(const std::string &text);
std::string request_id(const std::string &payload);
}  // namespace util
'''

TESTS = r'''
}  // namespace esphome::nuki_hub

using namespace esphome::nuki_hub::util;

static JsonDocument parse(const char *text) {
  JsonDocument doc;
  assert(!deserializeJson(doc, text));
  return doc;
}

int main() {
  uint16_t dt[6];
  assert(parse_date_time("2026-09-17 08:05:09", dt));
  assert(dt[0] == 2026 && dt[1] == 9 && dt[2] == 17 && dt[3] == 8 && dt[4] == 5 && dt[5] == 9);
  assert(parse_date_time("2026-09-17T08:05:09", dt));
  assert(!parse_date_time("2026-09-17 08:05", dt));
  assert(!parse_date_time("2026/09/17 08:05:09", dt));
  assert(!parse_date_time("2026-13-17 08:05:09", dt));
  assert(!parse_date_time("2026-09-32 08:05:09", dt));
  assert(!parse_date_time("2026-09-17 24:00:00", dt));
  assert(!parse_date_time("1999-09-17 08:05:09", dt));
  assert(!parse_date_time("2026-09-17 0a:05:09", dt));
  assert(!parse_date_time("", dt));

  uint8_t tod[2];
  assert(parse_time_of_day("08:30", tod) && tod[0] == 8 && tod[1] == 30);
  assert(parse_time_of_day("23:59", tod));
  assert(!parse_time_of_day("24:00", tod));
  assert(!parse_time_of_day("08-30", tod));
  assert(!parse_time_of_day("8:30", tod));

  char buf[24];
  date_time(buf, 2026, 9, 7, 8, 5, 9);
  assert(std::string(buf) == "2026-09-07 08:05:09");
  date_time(buf, 0, 0, 0, 0, 0, 0);
  assert(std::string(buf) == "0000-00-00 00:00:00");
  // Whatever the device sends, the text fits
  date_time(buf, 65535, 255, 255, 255, 255, 255);
  assert(strlen(buf) == 19);
  time_of_day(buf, 7, 3);
  assert(std::string(buf) == "07:03");
  time_of_day(buf, 255, 255);
  assert(strlen(buf) == 5);

  // bit 6 = Monday ... bit 0 = Sunday
  assert(weekdays_from_json(parse(R"(["mon","sun"])").as<JsonVariantConst>()) == 0x41);
  assert(weekdays_from_json(parse(R"("tue,wed")").as<JsonVariantConst>()) == 0x30);
  assert(weekdays_from_json(parse(R"([])").as<JsonVariantConst>()) == 0);
  JsonDocument days;
  weekdays_to_json(0x41, days.to<JsonArray>());
  std::string text;
  serializeJson(days, text);
  assert(text == R"(["mon","sun"])");
  for (int value = 0; value < 128; value++) {
    JsonDocument round;
    weekdays_to_json(value, round.to<JsonArray>());
    assert(weekdays_from_json(round.as<JsonVariantConst>()) == value);
  }

  const uint8_t full[4] = {'a', 'b', 'c', 'd'};
  assert(fixed_string(full, sizeof(full)) == "abcd");
  const uint8_t part[4] = {'a', 0, 'c', 'd'};
  assert(fixed_string(part, sizeof(part)) == "a");

  JsonDocument values = parse(R"({"s":"text","n":345678,"b":true,"z":null,"f":1.5})");
  assert(as_text(values["s"]) == "text");
  assert(as_text(values["n"]) == "345678");
  assert(as_text(values["b"]) == "true");
  assert(as_text(values["z"]) == "");
  assert(as_text(values["missing"]) == "");

  assert(is_number("0") && is_number("4294967295"));
  assert(!is_number("") && !is_number("-1") && !is_number("12a") && !is_number("12345678901"));

  assert(request_id(R"({"action":"add","requestId":"a1"})") == "a1");
  assert(request_id(R"({"action":"add","requestId":17})") == "17");
  assert(request_id(R"({"action":"add"})") == "");
  assert(request_id("not json requestId") == "");
  assert(request_id("") == "");
  puts("ok");
  return 0;
}
'''


class UtilTests(unittest.TestCase):
    def test_helpers(self):
        compiler = shutil.which("c++")
        include = find_arduinojson()
        if compiler is None or include is None:
            self.skipTest("needs a C++ compiler and the ArduinoJson headers")
        source = (ROOT / "components/nuki_hub/nuki_hub.cpp").read_text()
        start = source.index("namespace util {")
        end = source.index("}  // namespace util", start) + len("}  // namespace util")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "util_test.cpp"
            path.write_text(PRELUDE + source[start:end] + TESTS)
            binary = Path(directory) / "util_test"
            build = subprocess.run(
                [compiler, "-std=gnu++20", "-Wall", "-Wextra", "-Werror", "-I", include, str(path), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr[-3000:])
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
