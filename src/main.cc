#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>

#include "freertosxx/mutex.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "homeassistant/homeassistant.h"
#include "lwip/err.h"
#include "lwipxx/mqtt.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "pico/types.h"
#include "portmacro.h"
#include "task.h"

using lwipxx::MqttClient;

// We average wind speed and report it every 30s. Rain is reported every 10
// mins (although the scaled rate per hour is the value we report).
constexpr int kRainReportPeriodSecs = 10 * 60;
constexpr int kWindReportPeriodSecs = 5;
constexpr uint64_t kRainGaugeFlushUs = kRainReportPeriodSecs * 1e6;
constexpr uint64_t kAnemometerFlushUs = kWindReportPeriodSecs * 1e6;

using SensorPubFn = std::function<void(std::string_view, std::string_view)>;

void SensorPublish(
    MqttClient& client, std::string_view topic, std::string_view payload) {
  err_t err = client.Publish(
      topic, payload, MqttClient::kAtLeastOnce, true, [](err_t err) {
        if (err != ERR_OK) {
          printf(
              "%s\n",
              std::format("error publishing {}", lwip_strerr(err)).c_str());
        } else {
          // On successful Publish, update the watchdog.
          watchdog_update();
        }
      });
  if (err != ERR_OK) {
    printf(
        "%s\n",
        std::format("error dispatching publish request {}", lwip_strerr(err))
            .c_str());
  }
}

std::string_view LevelToDirection(int32_t adc_reading) {
  // ADC target levels assume a divider impedance of 3377 ohms, which can be
  // achieved by putting a 5100 ohm and 10000 ohm resistor in parallel.
  constexpr std::array<std::pair<std::string_view, int32_t>, 16> adc_targets{
      // New: I have the divider, then the ADC, then the windvane, then ground.
      {std::make_pair("NE", 2901),
       std::make_pair("E", 936),
       std::make_pair("SE", 1616),
       std::make_pair("S", 2195),
       std::make_pair("SW", 3382),
       std::make_pair("W", 3984),
       std::make_pair("NW", 3893),
       std::make_pair("N", 3716),
       std::make_pair("NNE", 2705),
       std::make_pair("ENE", 855),
       std::make_pair("ESE", 693),
       std::make_pair("SSE", 1204),
       std::make_pair("SSW", 1972),
       std::make_pair("WSW", 3305),
       std::make_pair("WNW", 3792),
       std::make_pair("NNW", 3548)}};
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

void wind_direction_task(void* args) {
  MqttClient& mqtt = *static_cast<MqttClient*>(args);
  gpio_init(26);
  adc_init();
  adc_gpio_init(26);
  adc_select_input(0);

  using namespace homeassistant;
  CommonDeviceInfo windvane("weatherstation_wind_dir");
  windvane.name = "windvane";
  windvane.component = "sensor";
  windvane.device_class = "enum";

  {
    JsonBuilder json;
    AddCommonInfo(windvane, json);
    AddSensorInfo(windvane, std::nullopt, json);
    PublishDiscovery(mqtt, windvane, std::move(json).Finish());
  }

  std::string state_topic = AbsoluteChannel(windvane, topic_suffix::kState);

  while (true) {
    const uint16_t level = adc_read();
    std::string_view direction = LevelToDirection(level);
    SensorPublish(mqtt, state_topic, direction);
    sleep_ms(kWindReportPeriodSecs * 1000);
  }
}

struct RateLimitedCounter {
  const uint64_t update_period;
  uint64_t next_update = 0;
  int count = 0;

  void Inc(uint64_t timestamp) {
    if (timestamp > next_update) {
      ++count;
      next_update = timestamp + update_period;
    }
  }

  int Flush() { return std::exchange(count, 0); }
};

// We'll measure up to 50mph wind. We're assuming a simple linear
// relationship, when in reality doubling the tick speed is probably more than
// doubling the wind speed.
constexpr float kAnemometerSpeedPerTick = 1.73;  // mph
constexpr float kAnemometerMaxSpeed = 100;
constexpr uint64_t kAnemometerUpdateUs =
    1e6 / (kAnemometerMaxSpeed / kAnemometerSpeedPerTick);
RateLimitedCounter& AnemometerCounter() {
  static RateLimitedCounter anemometer_counter{
      .update_period = kAnemometerUpdateUs};
  return anemometer_counter;
}

// We'll measure up to six inches of rain per hour.
constexpr float kRainGaugeInchesPerTick = 0.011;
constexpr float kRainGaugeMaxInchesPerSecond = 6. / (60 * 60);
constexpr uint64_t kRainGaugeUpdateUs =
    1e6 / (kRainGaugeMaxInchesPerSecond / kRainGaugeInchesPerTick);
RateLimitedCounter& RainGaugeCounter() {
  static RateLimitedCounter rain_gauge_counter{
      .update_period = kRainGaugeUpdateUs};
  return rain_gauge_counter;
}

constexpr int kAnemometerPin = 14;
constexpr int kRainGaugePin = 15;

void setup_wind_and_rain(
    MqttClient& client, std::string& wind_topic, std::string& rain_topic) {
  using namespace homeassistant;
  CommonDeviceInfo rain_device("weatherstation_rain_gauge");
  rain_device.name = "rainfall sensor";
  rain_device.component = "sensor";
  rain_device.device_class = "precipitation_intensity";

  {
    JsonBuilder json;
    AddCommonInfo(rain_device, json);
    AddSensorInfo(rain_device, "in/h", json);
    PublishDiscovery(client, rain_device, std::move(json).Finish());
    rain_topic = AbsoluteChannel(rain_device, topic_suffix::kState);
  }

  homeassistant::CommonDeviceInfo wind_device("weatherstation_anemometer");
  wind_device.name = "windspeed sensor";
  wind_device.component = "sensor";
  wind_device.device_class = "wind_speed";

  {
    JsonBuilder json;
    AddCommonInfo(wind_device, json);
    AddSensorInfo(wind_device, "mph", json);
    PublishDiscovery(client, wind_device, std::move(json).Finish());
    wind_topic = AbsoluteChannel(wind_device, topic_suffix::kState);
  }
}

void track_wind_and_rain(
    MqttClient& mqtt, std::string_view wind_topic,
    std::string_view rain_topic) {
  // Keep this task on the current core so that when we pause interrupts
  // we are pausing them on the same core as the one we live on.
  vTaskCoreAffinitySet(nullptr, 1 << get_core_num());

  gpio_init(kAnemometerPin);
  gpio_init(kRainGaugePin);
  gpio_set_dir(kAnemometerPin, GPIO_IN);
  gpio_set_dir(kRainGaugePin, GPIO_IN);
  gpio_pull_up(kAnemometerPin);
  gpio_pull_up(kRainGaugePin);

  gpio_irq_callback_t callback = [](uint gpio, uint32_t events) {
    const uint64_t timestamp = time_us_64();
    switch (gpio) {
      case kAnemometerPin:
        AnemometerCounter().Inc(timestamp);
        break;
      case kRainGaugePin:
        RainGaugeCounter().Inc(timestamp);
        break;
      default:
        printf("Unexpected gpio %d", gpio);
        break;
    }
  };
  gpio_set_irq_enabled(kAnemometerPin, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_enabled(kRainGaugePin, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_callback(callback);
  irq_set_enabled(IO_IRQ_BANK0, true);

  absolute_time_t next_rain_gauge_flush =
      delayed_by_us(get_absolute_time(), kRainGaugeFlushUs);
  absolute_time_t next_anemometer_flush =
      delayed_by_us(get_absolute_time(), kAnemometerFlushUs);

  while (true) {
    // Wait until the sooner of the two flush times.
    absolute_time_t min_flush_time =
        absolute_time_diff_us(next_rain_gauge_flush, next_anemometer_flush) > 0
            ? next_rain_gauge_flush
            : next_anemometer_flush;

    printf(
        "sleeping for %lld usec\n",
        absolute_time_diff_us(get_absolute_time(), min_flush_time));

    sleep_until(min_flush_time);
    absolute_time_t now = get_absolute_time();

    // Disable interrupts and flush whichever of the two counters is ready.
    std::optional<int> rain_gauge_flush;
    std::optional<absolute_time_t> rain_gauge_end_time;
    std::optional<int> anemometer_flush;
    std::optional<absolute_time_t> anemometer_end_time;
    portDISABLE_INTERRUPTS();
    if (absolute_time_diff_us(now, next_rain_gauge_flush) < 0) {
      rain_gauge_flush = RainGaugeCounter().Flush();
      rain_gauge_end_time = std::exchange(
          next_rain_gauge_flush, delayed_by_us(now, kRainGaugeFlushUs));
    }
    if (absolute_time_diff_us(now, next_anemometer_flush) < 0) {
      anemometer_flush = AnemometerCounter().Flush();
      anemometer_end_time = std::exchange(
          next_anemometer_flush, delayed_by_us(now, kAnemometerFlushUs));
    }
    portENABLE_INTERRUPTS();

    // Publish whichever of the two counters was flushed.
    if (rain_gauge_flush.has_value()) {
      const double elapsed_time_sec =
          (kRainGaugeFlushUs +
           absolute_time_diff_us(now, *rain_gauge_end_time)) /
          1e6;
      const double rain_inches = *rain_gauge_flush * kRainGaugeInchesPerTick;
      const double rain_inches_per_hour = rain_inches / elapsed_time_sec * 3600;
      printf(
          "collected %d ticks, %.1f in/h\n",
          *rain_gauge_flush,
          rain_inches_per_hour);
      SensorPublish(mqtt, rain_topic, std::to_string(rain_inches_per_hour));
    }

    if (anemometer_flush) {
      const double elapsed_time_sec =
          (kAnemometerFlushUs +
           absolute_time_diff_us(now, *anemometer_end_time)) /
          1e6;
      const double counted_wind_mph =
          *anemometer_flush * kAnemometerSpeedPerTick;
      const double wind_mph = counted_wind_mph / elapsed_time_sec;
      printf("collected %d ticks, %.1f mph\n", *anemometer_flush, wind_mph);
      SensorPublish(mqtt, wind_topic, std::to_string(wind_mph));
    }
  }
}

void wind_and_rain_task(void* args) {
  MqttClient& mqtt = *static_cast<MqttClient*>(args);
  std::string wind_topic, rain_topic;
  setup_wind_and_rain(mqtt, wind_topic, rain_topic);
  track_wind_and_rain(mqtt, wind_topic, rain_topic);
}

extern "C" void main_task(void* args) {
  MqttClient::ConnectInfo connect_info{
      .broker_address = MQTT_HOST,
      .client_id = MQTT_CLIENT_ID,
      .user = MQTT_USER,
      .password = MQTT_PASSWORD,
  };
  homeassistant::SetAvailablityLwt(connect_info);
  std::unique_ptr<MqttClient> mqtt;
  while (!mqtt) {
    auto maybe_mqtt = MqttClient::Create(connect_info);
    if (!maybe_mqtt) {
      printf("Failed to create MQTT client: %d\n", maybe_mqtt.error());
      sleep_ms(5000);
      continue;
    }
    mqtt = *std::move(maybe_mqtt);
  }
  homeassistant::PublishAvailable(*mqtt);

  xTaskCreate(
      wind_and_rain_task,
      "wind_and_rain",
      512,
      static_cast<void*>(mqtt.get()),
      1,
      nullptr);
  wind_direction_task(static_cast<void*>(mqtt.get()));

  // We will update the watchdog each time we successfully publish a message.
  // If we fail to publish for
  constexpr int kMaxMissedPublishPeriods = 3;
  constexpr int kWatchdogMs =
      (1 + 3 * std::min({kWindReportPeriodSecs, kRainReportPeriodSecs})) * 1000;
  watchdog_enable(kWatchdogMs, true);
}