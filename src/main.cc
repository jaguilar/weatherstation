#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <tuple>

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/time.h"

std::string_view LevelToDirection(uint8_t adc_reading) {
  constexpr std::array<std::pair<std::string_view, uint16_t>, 16> adc_targets{
      std::make_pair("NE", 1571),
      std::make_pair("E", 3425),
      std::make_pair("SE", 2862),
      std::make_pair("S", 2321),
      std::make_pair("SW", 990),
      std::make_pair("W", 167),
      std::make_pair("NW", 298),
      std::make_pair("N", 548),
      std::make_pair("NNE", 1790),
      std::make_pair("ENE", 3487),
      std::make_pair("ESE", 3609),
      std::make_pair("SSE", 3211),
      std::make_pair("SSW", 2536),
      std::make_pair("WSW", 1087),
      std::make_pair("WNW", 442),
      std::make_pair("NNW", 774),
  };
  int min_diff = std::numeric_limits<int>::max();
  int min_index = -1;
  for (int i = 0; i < adc_targets.size(); ++i) {
    const int diff = std::abs(adc_targets[i].second - adc_reading);
    if (min_diff > diff) {
      min_diff = diff;
      min_index = i;
    }
  }
  return adc_targets[min_index].first;
}

extern "C" void main_task(void* args) {
  gpio_init(26);
  adc_init();
  adc_gpio_init(26);
  adc_select_input(0);

  while (true) {
    const uint8_t level = adc_read();
    printf("%d - %s\n", level, LevelToDirection(level).data());
    sleep_ms(100);
  }
}