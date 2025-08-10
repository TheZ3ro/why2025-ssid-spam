#include "wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"

static const char *TAG = "WIFI";
static wifi_mode_t curr_mode = WIFI_MODE_NULL;
QueueHandle_t wifi_queue;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int FAIL_BIT = BIT1;

static int retry_num = 0;
static int ap_clients_num = 0;
static esp_timer_handle_t inactivity_timer;

// WiFi Marauder variables
static const uint8_t marauder_channels[] = {1, 6, 11}; // Non-overlapping channels
static char marauder_empty_ssid[32];
static uint8_t marauder_channel_index = 0;
static uint8_t marauder_mac_addr[6];
static uint8_t marauder_wifi_channel = 1;
static uint32_t marauder_packet_counter = 0;
static uint32_t marauder_attack_time = 0;
static uint32_t marauder_packet_rate_time = 0;
static bool marauder_running = false;
static TaskHandle_t marauder_task_handle = NULL;

// SSID list for WiFi Marauder (stored in PROGMEM equivalent)
// Multiple entries with slight variations to ensure WiFi scanners see them as separate networks
static const char* marauder_ssids[] = {
    "FREE PALESTINE",
    "FREE PALESTINE ",
    " FREE PALESTINE",
    "FREE PALESTINE  ",
    "  FREE PALESTINE",
    "FREE PALESTINE   ",
    "   FREE PALESTINE",
    "FREE PALESTINE    ",
    "    FREE PALESTINE",
    "FREE PALESTINE     ",
};

// WiFi Marauder helper functions
static void marauder_next_channel(void) {
    if (sizeof(marauder_channels) < 2) {
        return;
    }
    
    uint8_t ch = marauder_channels[marauder_channel_index];
    
    marauder_channel_index++;
    if (marauder_channel_index >= sizeof(marauder_channels)) {
        marauder_channel_index = 0;
    }
    
    if (ch != marauder_wifi_channel && ch >= 1 && ch <= 14) {
        marauder_wifi_channel = ch;
        esp_wifi_set_channel(marauder_wifi_channel, WIFI_SECOND_CHAN_NONE);
    }
}

static void marauder_random_mac(void) {
    for (int i = 0; i < 6; i++) {
        marauder_mac_addr[i] = esp_random() % 256;
    }
}

// Create a beacon frame dynamically for better control
static int create_beacon_frame(uint8_t *frame, const uint8_t *mac, const char *ssid, uint8_t channel) {
    int offset = 0;
    
    // Frame Control (0-1)
    frame[offset++] = 0x80; // Type: Management, Subtype: Beacon
    frame[offset++] = 0x00; // Flags
    
    // Duration (2-3)
    frame[offset++] = 0x00;
    frame[offset++] = 0x00;
    
    // Destination Address (4-9) - Broadcast
    frame[offset++] = 0xFF;
    frame[offset++] = 0xFF;
    frame[offset++] = 0xFF;
    frame[offset++] = 0xFF;
    frame[offset++] = 0xFF;
    frame[offset++] = 0xFF;
    
    // Source Address (10-15)
    memcpy(&frame[offset], mac, 6);
    offset += 6;
    
    // BSSID (16-21) - Same as source
    memcpy(&frame[offset], mac, 6);
    offset += 6;
    
    // Sequence Number (22-23)
    frame[offset++] = 0x00;
    frame[offset++] = 0x00;
    
    // Timestamp (24-31)
    uint64_t timestamp = esp_timer_get_time() / 1000; // Convert to microseconds
    for (int i = 0; i < 8; i++) {
        frame[offset++] = (timestamp >> (i * 8)) & 0xFF;
    }
    
    // Beacon Interval (32-33)
    frame[offset++] = 0x64; // 100 TU = 100ms
    frame[offset++] = 0x00;
    
    // Capability Information (34-35)
    frame[offset++] = 0x21; // ESS, Privacy
    frame[offset++] = 0x00;
    
    // SSID Parameter Set (36-37 + SSID)
    frame[offset++] = 0x00; // SSID parameter tag
    frame[offset++] = strlen(ssid); // SSID length
    memcpy(&frame[offset], ssid, strlen(ssid));
    offset += strlen(ssid);
    
    // Supported Rates (38 + rates)
    frame[offset++] = 0x01; // Supported Rates parameter tag
    frame[offset++] = 8; // 8 rates
    frame[offset++] = 0x82; // 1 Mbps
    frame[offset++] = 0x84; // 2 Mbps
    frame[offset++] = 0x8b; // 5.5 Mbps
    frame[offset++] = 0x96; // 11 Mbps
    frame[offset++] = 0x24; // 18 Mbps
    frame[offset++] = 0x30; // 24 Mbps
    frame[offset++] = 0x48; // 36 Mbps
    frame[offset++] = 0x6c; // 54 Mbps
    
    // Current Channel (39-40)
    frame[offset++] = 0x03; // DS Parameter Set tag
    frame[offset++] = 0x01; // Length
    frame[offset++] = channel; // Channel number
    
    // RSN Information (WPA2)
    frame[offset++] = 0x30; // RSN tag
    frame[offset++] = 0x18; // Length
    frame[offset++] = 0x01; // Version
    frame[offset++] = 0x00;
    frame[offset++] = 0x00; // Group cipher suite
    frame[offset++] = 0x0f;
    frame[offset++] = 0xac;
    frame[offset++] = 0x02;
    frame[offset++] = 0x02; // Pairwise cipher suite count
    frame[offset++] = 0x00;
    frame[offset++] = 0x00; // Pairwise cipher suite 1
    frame[offset++] = 0x0f;
    frame[offset++] = 0xac;
    frame[offset++] = 0x04;
    frame[offset++] = 0x00; // Pairwise cipher suite 2
    frame[offset++] = 0x0f;
    frame[offset++] = 0xac;
    frame[offset++] = 0x04;
    frame[offset++] = 0x01; // Authentication suite count
    frame[offset++] = 0x00;
    frame[offset++] = 0x00; // Authentication suite
    frame[offset++] = 0x0f;
    frame[offset++] = 0xac;
    frame[offset++] = 0x02;
    
    return offset;
}

static void marauder_init_ssids(void) {
    // Initialize empty SSID with spaces
    for (int i = 0; i < 32; i++) {
        marauder_empty_ssid[i] = ' ';
    }
    
    // Generate random MAC address
    marauder_random_mac();
    
    ESP_LOGI(TAG, "WiFi Marauder initialized with %d SSIDs", MARAUDER_SSID_COUNT);
}

static void inactivity_timer_callback(void* arg)
{
	if(!ap_clients_num){
		stop_wifi();
	} else {
		ESP_LOGE(__FILE__, "Timer should not be running...");
	}
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
		ap_clients_num++;
		esp_timer_stop(inactivity_timer);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
		ap_clients_num--;
		if(ap_clients_num < 0) ap_clients_num = 0;
		if(!ap_clients_num) esp_timer_start_once(inactivity_timer, AP_INACTIVITY_TIMEOUT_S * 1000000);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num < STA_MAXIMUM_RETRY) {
			ui_connection_progress(retry_num+1, STA_MAXIMUM_RETRY);
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", retry_num, STA_MAXIMUM_RETRY);
			xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        } else {
            xEventGroupSetBits(wifi_event_group, FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } 
}

void wifi_init(void)
{
	esp_log_level_set("wifi", ESP_LOG_WARN);
	static bool initialized = false;
	if (initialized) {
		return;
	}
	ESP_ERROR_CHECK(esp_netif_init());
	wifi_event_group = xEventGroupCreate();
	
	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
	assert(ap_netif);
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &event_handler, NULL) );
	// ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler, NULL) );


	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
	ESP_ERROR_CHECK( esp_wifi_start() );

    curr_mode = WIFI_MODE_NULL;

	initialized = true;
}

bool start_wifi_ap(void)
{
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
	
	const char* AP_WIFI_SSID = badge_obj.ap_ssid;
	const char* AP_WIFI_PASSWORD = badge_obj.ap_password;
	
	wifi_config_t wifi_config = { 0 };
	snprintf((char*)wifi_config.ap.ssid, SIZEOF(wifi_config.ap.ssid), "%s", AP_WIFI_SSID);
	snprintf((char*)wifi_config.ap.password, SIZEOF(wifi_config.ap.password), "%s", AP_WIFI_PASSWORD);
	wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
	wifi_config.ap.ssid_len = strlen(AP_WIFI_SSID);
	wifi_config.ap.max_connection = AP_MAX_STA_CONN;

	if (strlen(AP_WIFI_PASSWORD) == 0) {
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}


	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );
	ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config) );
	ESP_ERROR_CHECK( esp_wifi_start() );
	ESP_ERROR_CHECK( esp_wifi_set_inactive_time(WIFI_IF_AP, AP_INACTIVITY_TIMEOUT_S) );

	const esp_timer_create_args_t timer_args = {
		.callback = &inactivity_timer_callback,
		.name = "inactivity-timer"
	};

	ESP_ERROR_CHECK(esp_timer_create(&timer_args, &inactivity_timer));

	ESP_LOGI(TAG, "WIFI_MODE_AP started. SSID:%s password:%s",
			 AP_WIFI_SSID, AP_WIFI_PASSWORD);

    curr_mode = WIFI_MODE_AP;
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

	return ESP_OK;
}

void stop_wifi(){
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    ESP_LOGI(TAG, "WIFI disabled");
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
}

void wifi_task(void *arg)
{   
    wifi_queue = xQueueCreate(4, sizeof(uint32_t));
    uint32_t wifi_event;

    while (1) {
        if(!xQueueReceive(wifi_queue, &wifi_event, portMAX_DELAY))
            continue;
        
        switch (wifi_event) {
            case EVENT_HOTSPOT_START:
                stop_wifi();
                start_wifi_ap();
				esp_timer_start_once(inactivity_timer, AP_INACTIVITY_TIMEOUT_S * 1000000);
                break;
			case EVENT_MARAUDER_START:
                stop_wifi();
                start_wifi_marauder();
                break;
            case EVENT_HOTSPOT_STOP:
                stop_wifi();
				esp_timer_stop(inactivity_timer);
                break;    
			case EVENT_MARAUDER_STOP:
				stop_wifi_marauder();
				break;           
            default:
                ESP_LOGI(__FILE__, "not exists event 0x%04" PRIx32, wifi_event);
                break;
        }
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }
}

// WiFi Marauder functions
bool start_wifi_marauder(void)
{
    ESP_LOGI(TAG, "Starting WiFi Marauder...");
    
    if (marauder_running) {
        ESP_LOGW(TAG, "WiFi Marauder already running");
        return true;
    }
    
    // Stop any existing WiFi
    stop_wifi();
    
    // Initialize WiFi Marauder
    marauder_init_ssids();
    
    // Initialize WiFi with proper configuration for raw packet transmission
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0; // Disable NVS for this operation
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set WiFi mode to NULL first, then to STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_channel(marauder_channels[0], WIFI_SECOND_CHAN_NONE));
    
    // Enable promiscuous mode for raw packet transmission
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Wait a bit for WiFi to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Create the marauder task
    BaseType_t ret = xTaskCreate(
        wifi_marauder_task,
        "wifi_marauder",
        8192, // Increased stack size
        NULL,
        5,
        &marauder_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi Marauder task");
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    
    marauder_running = true;
    curr_mode = WIFI_MODE_STA;
    
    ESP_LOGI(TAG, "WiFi Marauder started successfully");
    return true;
}

void stop_wifi_marauder(void)
{
    ESP_LOGI(TAG, "Stopping WiFi Marauder...");
    
    if (!marauder_running) {
        ESP_LOGW(TAG, "WiFi Marauder not running");
        return;
    }
    
    marauder_running = false;
    
    // Delete the marauder task
    if (marauder_task_handle != NULL) {
        vTaskDelete(marauder_task_handle);
        marauder_task_handle = NULL;
    }
    
    // Disable promiscuous mode
    esp_wifi_set_promiscuous(false);
    
    // Stop WiFi
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_deinit();
    
    curr_mode = WIFI_MODE_NULL;
    ESP_LOGI(TAG, "WiFi Marauder stopped");
}

void wifi_marauder_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi Marauder task started");
    
    // Reset counters
    marauder_packet_counter = 0;
    marauder_attack_time = 0;
    marauder_packet_rate_time = 0;
    
    while (marauder_running) {
        uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
        
        // Send out SSIDs every 100ms (faster for better AP visibility)
        if (current_time - marauder_attack_time > 100) {
            marauder_attack_time = current_time;
            
            // Check if WiFi is still running
            if (curr_mode != WIFI_MODE_STA) {
                ESP_LOGW(TAG, "WiFi mode changed, stopping marauder task");
                break;
            }
            
            // Send beacon frames for each SSID on current channel
            for (int ssid_index = 0; ssid_index < MARAUDER_SSID_COUNT; ssid_index++) {
                if (!marauder_running) break;
                
                const char* ssid = marauder_ssids[ssid_index];
                if (ssid == NULL) {
                    ESP_LOGW(TAG, "Invalid SSID at index %d", ssid_index);
                    continue;
                }
                
                uint8_t ssid_len = strlen(ssid);
                if (ssid_len > 32) {
                    ESP_LOGW(TAG, "SSID too long at index %d: %d", ssid_index, ssid_len);
                    continue;
                }
                
                // Generate completely different MAC address for each SSID
                // This ensures WiFi scanners see them as different devices
                uint8_t unique_mac[6];
                for (int i = 0; i < 6; i++) {
                    unique_mac[i] = esp_random() % 256;
                }
                // Ensure the first bit is 0 (unicast) and second bit is 0 (globally unique)
                unique_mac[0] &= 0xFC;
                
                // Create beacon frame dynamically
                uint8_t beacon_frame[256]; // Increased buffer size for dynamic frames
                int frame_len = create_beacon_frame(beacon_frame, unique_mac, ssid, marauder_wifi_channel);
                
                // Debug: Log beacon frame details
                ESP_LOGI(TAG, "Beacon frame: SSID='%s' (len=%d), MAC="MACSTR", Channel=%d, FrameLen=%d", 
                         ssid, ssid_len, MAC2STR(unique_mac), marauder_wifi_channel, frame_len);
                
                // Send packet once per SSID to reduce memory pressure
                esp_err_t ret = esp_wifi_80211_tx(ESP_IF_WIFI_STA, beacon_frame, frame_len, 0);
                if (ret == ESP_OK) {
                    marauder_packet_counter++;
                    ESP_LOGI(TAG, "Sent beacon for SSID %d: %s with MAC "MACSTR" on channel %d", 
                             ssid_index, ssid, MAC2STR(unique_mac), marauder_wifi_channel);
                } else {
                    ESP_LOGW(TAG, "Failed to send beacon packet: %s", esp_err_to_name(ret));
                    // If we get memory errors, wait a bit longer
                    if (ret == ESP_ERR_NO_MEM) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                
                // Very small delay between SSIDs to make them appear simultaneously
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            
            // Change channel every 2 seconds (slower channel switching for better AP detection)
            static uint32_t last_channel_change = 0;
            if (current_time - last_channel_change > 2000) {
                marauder_next_channel();
                last_channel_change = current_time;
                ESP_LOGI(TAG, "WiFi Marauder switched to channel %d", marauder_wifi_channel);
            }
        }
        
        // Show packet rate every 5 seconds
        if (current_time - marauder_packet_rate_time > MARAUDER_PACKET_RATE_INTERVAL_MS) {
            marauder_packet_rate_time = current_time;
            
            ESP_LOGI(TAG, "WiFi Marauder: %lu packets sent", marauder_packet_counter);
            marauder_packet_counter = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(20)); // Reduced delay for more responsive AP announcements
    }
    
    ESP_LOGI(TAG, "WiFi Marauder task ended");
    vTaskDelete(NULL);
}