#include "mgos.h"
#include "mgos_mqtt.h"
#include "rom/rtc.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "mgos_mdns.h"

bool mqtt_conn_flag = false;
// Boot counter
static size_t RTC_DATA_ATTR s_boot_count=0;

static void led_timer_cb(void *arg) {
  bool val = mgos_gpio_toggle(mgos_sys_config_get_pins_led());
  LOG(LL_INFO, ("%s uptime: %.2lf, RAM: %lu, %lu free", val ? "Tick" : "Tock",
                mgos_uptime(), (unsigned long) mgos_get_heap_size(),
                (unsigned long) mgos_get_free_heap_size()));
  (void) arg;
}

static void button_cb(int pin, void *arg) {
  //char topic[100];
  // snprintf(topic, sizeof(topic), "/devices/%s/events",
  //            mgos_sys_config_get_device_id());
  //char topic[100] = "/devices/esp8266_D6F626/events";
  char topic[100];
  snprintf(topic, sizeof(topic), "%s", mgos_sys_config_get_topic_event());
  char message[100];
  struct json_out out = JSON_OUT_BUF(message, sizeof(message));
  json_printf(&out, "{total_ram: %lu, free_ram: %lu}",
              (unsigned long) mgos_get_heap_size(),
              (unsigned long) mgos_get_free_heap_size());
  bool res = mgos_mqtt_pub(topic, message, strlen(message), 1, false);
  LOG(LL_INFO, ("Pin: %d, published: %s", pin, res ? "yes" : "no"));
  (void) arg;
}

static void deep_sleep_handler(void *arg) {
  esp_sleep_enable_ext0_wakeup(mgos_sys_config_get_pins_button(), 0);		// 1 (Button Open), 0 (Button Close)
  //esp_deep_sleep(-1);  
  
  LOG(LL_INFO, ("All done.  Going to sleep in "));
  for(int i=5; i>0; --i){
    LOG(LL_INFO, ("%d...", i));
    sleep(2);
  }
  LOG(LL_INFO, ("Zzzzz..."));
  esp_deep_sleep_start();
  (void) arg;
}

//  HELPER FUNCTION: Decode the reason for resetting. 
//  Refer to:
//    https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/ResetReason/ResetReason.ino
//    https://github.com/espressif/esp-idf/blob/master/components/esp32/include/rom/rtc.h
//
int why_reset(){
  int reset_reason = rtc_get_reset_reason(0); 
  LOG(LL_INFO, ("Reset Reason (%d): ", reset_reason));

  switch (reset_reason) {
    case 1  : LOG(LL_INFO, ("Vbat power on reset"));break;
    case 3  : LOG(LL_INFO, ("Software reset digital core"));break;
    case 4  : LOG(LL_INFO, ("Legacy watch dog reset digital core"));break;
    case 5  : LOG(LL_INFO, ("Deep Sleep reset digital core"));break;
    case 6  : LOG(LL_INFO, ("Reset by SLC module, reset digital core"));break;
    case 7  : LOG(LL_INFO, ("Timer Group0 Watch dog reset digital core"));break;
    case 8  : LOG(LL_INFO, ("Timer Group1 Watch dog reset digital core"));break;
    case 9  : LOG(LL_INFO, ("RTC Watch dog Reset digital core"));break;
    case 10 : LOG(LL_INFO, ("Instrusion tested to reset CPU"));break;
    case 11 : LOG(LL_INFO, ("Time Group reset CPU"));break;
    case 12 : LOG(LL_INFO, ("Software reset CPU"));break;
    case 13 : LOG(LL_INFO, ("RTC Watch dog Reset CPU"));break;
    case 14 : LOG(LL_INFO, ("for APP CPU, reseted by PRO CPU"));break;
    case 15 : LOG(LL_INFO, ("Reset when the vdd voltage is not stable"));break;
    case 16 : LOG(LL_INFO, ("RTC Watch dog reset digital core and rtc module"));break;
    default : LOG(LL_INFO, ("NO_MEAN"));
  }
  return reset_reason;
}

//  HELPER FUNCTION: Decode our reason for waking.
//
int why_wake(){
  int wake_cause = esp_sleep_get_wakeup_cause();
  LOG(LL_INFO, ("Wake Cause (%d): ", wake_cause));
  switch (wake_cause) {
    case 1  : LOG(LL_INFO, ("Wakeup caused by external signal using RTC_IO"));break;
    case 2  : LOG(LL_INFO, ("Wakeup caused by external signal using RTC_CNTL"));break;
    case 3  : LOG(LL_INFO, ("Wakeup caused by timer"));break;
    case 4  : LOG(LL_INFO, ("Wakeup caused by touchpad"));break;
    case 5  : LOG(LL_INFO, ("Wakeup caused by ULP program"));break;
    default : LOG(LL_INFO, ("Undefined.  In case of deep sleep, reset was not caused by exit from deep sleep."));
  }
  return wake_cause;
}

static void net_cb(int ev, void *evd, void *arg) {
  switch (ev) {
    case MGOS_NET_EV_DISCONNECTED:
      LOG(LL_INFO, ("%s", "Net disconnected"));break;
    case MGOS_NET_EV_CONNECTING:
      LOG(LL_INFO, ("%s", "Net connecting..."));break;
    case MGOS_NET_EV_CONNECTED:
      LOG(LL_INFO, ("%s", "Net connected"));break;
    case MGOS_NET_EV_IP_ACQUIRED:
      LOG(LL_INFO, ("%s", "Net got IP address"));break;
  }

  (void) evd;
  (void) arg;
}

static void mqtt_ev_handler(struct mg_connection *c, int ev, void *p, void *user_data) {
  struct mg_mqtt_message *msg = (struct mg_mqtt_message *) p;
  if (ev == MG_EV_MQTT_CONNACK) {
    LOG(LL_INFO, ("CONNACK: %d", msg->connack_ret_code));
    mqtt_conn_flag = true;
    if(rtc_get_reset_reason(0) == TG0WDT_SYS_RESET) {
      button_cb(mgos_sys_config_get_pins_button(), NULL);
    }
  } else if (ev == MG_EV_CLOSE) {
      mqtt_conn_flag = false;
  }
  (void) user_data;
  (void) c;
}

// mDNS
static void dns_ev_handler(struct mg_connection *c, int ev, void *ev_data,
                           void *user_data) {
  LOG(LL_INFO, ("Entering dns_ev_handler ev = %d", ev));
  if (ev == MG_DNS_MESSAGE) {
    struct mg_dns_message *msg = (struct mg_dns_message *) ev_data;

    const char *peer;
    peer = inet_ntoa(c->sa.sin.sin_addr);
    LOG(LL_DEBUG, ("---- DNS packet from %s (%d questions, %d answers)", peer, msg->num_questions, msg->num_answers));
  }
  
  (void) user_data;
}

enum mgos_app_init_result mgos_app_init(void) {
  LOG(LL_INFO, ("-------------- STARTING APPLICATION -------------"));
  s_boot_count++;
  LOG(LL_INFO, ("Boot count: %d", s_boot_count));
  
  int reset_reason = why_reset();
  int wake_cause = why_wake();

  /* Reconfigure after deep sleep*/
  rtc_gpio_deinit(mgos_sys_config_get_pins_led());
  rtc_gpio_deinit(mgos_sys_config_get_pins_button());

  /* Blink built-in LED every second */
  mgos_gpio_set_mode(mgos_sys_config_get_pins_led(), MGOS_GPIO_MODE_INPUT);
  mgos_set_timer(1000, MGOS_TIMER_REPEAT, led_timer_cb, NULL);

  /* Publish to MQTT on button press */
  mgos_gpio_set_button_handler(mgos_sys_config_get_pins_button(),
                               MGOS_GPIO_PULL_UP, MGOS_GPIO_INT_EDGE_NEG, 200,
                               button_cb, NULL);

  /* Going in Deep sleep after 1min*/
  mgos_set_timer(60000, MGOS_TIMER_REPEAT, deep_sleep_handler, NULL);

  /* Network connectivity events */
  mgos_event_add_group_handler(MGOS_EVENT_GRP_NET, net_cb, NULL);

  /* TODO: Change to MQTT Events */
  mgos_mqtt_add_global_handler(mqtt_ev_handler, NULL);

  // mDNS
  mgos_mdns_add_handler(dns_ev_handler, NULL);

  return MGOS_APP_INIT_SUCCESS;
}