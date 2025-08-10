#include "ui.h"
#include "led.h"

enum screen_order {SCREEN_LOGO, SCREEN_ADMIN, NUM_SCREENS};
static lv_obj_t* screens[NUM_SCREENS];
static int8_t current_screen = SCREEN_LOGO;

static uint32_t last_trigger = -1;

static lv_obj_t *admin_switch, *admin_switch_sta, *admin_sync;
static lv_obj_t *admin_switch_text, *admin_switch_sta_text, *admin_sync_text;
static lv_obj_t *admin_ssid, *admin_pass;
static lv_obj_t *admin_client_ip, *admin_gateway_ip;

static bool sta_connected = false;

static uint8_t admin_state = ADMIN_STATE_OFF;

static uint8_t up_button_press_counter = 0;
static uint8_t down_button_press_counter = 0;
static int8_t counter_screen = -1; // Initialize to invalid screen index

// Forward declarations
void ui_update_ip_info(void);
void ui_list_all_netifs(void);

/*
* Update the screen backlight status
* returns status of backlight (true if backlight off)
*/

static bool ui_update_backlight(bool trigger)
{
    uint32_t span = lv_tick_get() - last_trigger;

    if (trigger)
    {
        set_screen_led_backlight(badge_obj.brightness_max);
        last_trigger = lv_tick_get();
    }
    else
    {
        if (span > BRIGHT_OFF_TIMEOUT_MS){
            set_screen_led_backlight(badge_obj.brightness_off);
        }
        else if (span > BRIGHT_MID_TIMEOUT_MS){
            set_screen_led_backlight(badge_obj.brightness_mid);
        }
    }

    /* Avoid doing action when backlight off */
    if (span > BRIGHT_OFF_TIMEOUT_MS){
        return true;
    }
    return false;
}

void ui_send_wifi_event(int event)
{
    xQueueSend(wifi_queue, &event, portMAX_DELAY);
}

void scroll_up(lv_obj_t *screen){
    lv_obj_t *page = lv_obj_get_child(screen, NULL);
    lv_page_scroll_ver(page, 80);
}

void scroll_down(lv_obj_t *screen){
    lv_obj_t *page = lv_obj_get_child(screen, NULL);
    lv_page_scroll_ver(page, -80);
}

void ui_button_up()
{
    // Check if this is the first button press or if we've changed screens
    if (counter_screen != current_screen) {
        // Reset counters when screen changes
        up_button_press_counter = 0;
        down_button_press_counter = 0;
        // Update counter_screen to current screen
        counter_screen = current_screen;
        //printf("DEBUG: Started counting UP presses on screen index: %d\n", current_screen);
    }
    
    // Increment counter for UP button presses
    up_button_press_counter++;
    ESP_LOGI("UI", "UP button press count: %d on screen index: %d", up_button_press_counter, current_screen);
    
    // Check if we've reached 7 presses
    if (up_button_press_counter == 7) {
        // printf("DEBUG: UP button pressed 7 times on screen %s (index: %d)\n", current_screen);
        
        // Call set_completed() function when on SCREEN_LOGO (index 0)
        if (current_screen == SCREEN_LOGO) {
            ESP_LOGI("UI", "Summoning sequence activated!");
            set_completed();
        }        
        
        // Reset the counter after reaching 7
        up_button_press_counter = 0;
    }

    // Check if the backlight update is needed
    if(ui_update_backlight(true)){
        return;
    }

    switch(current_screen){
        case SCREEN_ADMIN:
            switch(admin_state){
                case ADMIN_STATE_OFF: // AP and STA disabled: enable AP
                    ui_send_wifi_event(EVENT_HOTSPOT_START);
                    lv_obj_set_hidden(admin_switch_sta, true);
                    admin_state = ADMIN_STATE_AP;
                    break;
                case ADMIN_STATE_AP: // AP enabled: disable AP
                    ui_send_wifi_event(EVENT_HOTSPOT_STOP);
                    lv_obj_set_hidden(admin_switch_sta, false);
                    admin_state = ADMIN_STATE_OFF;
                    break;
                case ADMIN_STATE_MARAUDER: // Marauder enabled: disable Marauder
                    ui_send_wifi_event(EVENT_MARAUDER_STOP);
                    lv_obj_set_hidden(admin_switch_sta, false);
                    admin_state = ADMIN_STATE_OFF;
                    break;
            }   
            break;
        default:
            ESP_LOGI(__FILE__, "Button up, no actions");
    }
}

void ui_button_down()
{
    // Check if this is the first button press or if we've changed screens
    if (counter_screen != current_screen) {
        // Reset counters when screen changes
        up_button_press_counter = 0;
        down_button_press_counter = 0;
        // Update counter_screen to current screen
        counter_screen = current_screen;
    }
    
    // Increment counter for DOWN button presses
    down_button_press_counter++;
    ESP_LOGI("UI", "DOWN button press count: %d on screen index: %d", down_button_press_counter, current_screen);
    
    // Check if we've reached 7 presses
    if (down_button_press_counter == 7) {
        // Call rainbow() function when on SCREEN_LOGO (index 0)
        if (current_screen == SCREEN_LOGO) {
            ESP_LOGI("UI", "Rainbow sequence activated!");
            rainbow();
        }        
        
        // Reset the counter after reaching 7
        down_button_press_counter = 0;
    }

    if(ui_update_backlight(true)){
        return;
    }

    switch(current_screen){
        case SCREEN_ADMIN:
            switch(admin_state){
                case ADMIN_STATE_OFF: // AP and STA disabled: enable STA
                    ui_send_wifi_event(EVENT_MARAUDER_START);
                    lv_label_set_text(admin_switch_sta_text, "Started...");
                    admin_state = ADMIN_STATE_MARAUDER;
                    break;
                case ADMIN_STATE_AP: // AP enabled: test showing IP labels
                    ESP_LOGI("UI", "Force show IP labels test (DOWN button in AP mode)");
                    ui_force_show_ip_labels();
                    break;
                case ADMIN_STATE_MARAUDER: // Marauder enabled: test showing IP labels
                    ESP_LOGI("UI", "Force show IP labels test (DOWN button in Marauder mode)");
                    ui_force_show_ip_labels();
                    break;
            }
            break;
        default:
            ESP_LOGI(__FILE__, "Button down, no actions");
    }
}

static void ui_backlight_task(lv_task_t *arg){
    ui_update_backlight(false);
}

void ui_screen_splash_init(){
    LV_IMG_DECLARE(img_logo);

    screen_logo = lv_obj_create(NULL, NULL);
    lv_obj_t *logo = lv_img_create(screen_logo, NULL);
    lv_img_set_src(logo, &img_logo);
    lv_obj_align(logo, NULL, LV_ALIGN_CENTER, 0, 0);
  /*Change the logo's background color*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_opa(&style, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_bg_color(&style, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x34, 0x3a, 0x40));
    lv_obj_add_style(logo, LV_OBJ_PART_MAIN, &style);

    screens[SCREEN_LOGO] = screen_logo;
}

void ui_screen_admin_init(){
    // page for admin
    screen_admin = lv_obj_create(NULL, NULL);
    admin_switch = lv_btn_create(screen_admin, NULL);
    lv_obj_set_size(admin_switch, 200, 50);
    lv_obj_set_pos(admin_switch, 60, 35);
    admin_switch_text = lv_label_create(admin_switch, NULL);
    lv_label_set_text(admin_switch_text, "TURN ON AP");

    admin_sync = lv_btn_create(screen_admin, NULL);
    lv_obj_set_size(admin_sync, 200, 50);
    lv_obj_set_pos(admin_sync, 60, 180);
    admin_sync_text = lv_label_create(admin_sync, NULL);
    lv_label_set_text(admin_sync_text, "WIFI MARAUDER");
    lv_obj_set_hidden(admin_sync, true);

    admin_ssid = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_ssid, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 10);
    lv_obj_set_hidden(admin_ssid, true);
    admin_pass = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_pass, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 30);
    lv_obj_set_hidden(admin_pass, true);

    admin_client_ip = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_client_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 50);
    lv_label_set_text(admin_client_ip, "Client IP: [Not Connected]");
    lv_obj_set_hidden(admin_client_ip, true);
    admin_gateway_ip = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_gateway_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 70);
    lv_label_set_text(admin_gateway_ip, "Gateway: [Not Available]");
    lv_obj_set_hidden(admin_gateway_ip, true);

    admin_switch_sta = lv_btn_create(screen_admin, NULL);
    lv_obj_set_size(admin_switch_sta, 200, 50);
    lv_obj_set_pos(admin_switch_sta, 60, 180);
    admin_switch_sta_text = lv_label_create(admin_switch_sta, NULL);
    lv_label_set_text(admin_switch_sta_text, "WIFI MARAUDER");

    screens[SCREEN_ADMIN] = screen_admin;
}

void ui_sta_connected_handler(){
    sta_connected = true;

    ESP_LOGI("UI", "STA connected handler called");
    ESP_LOGI("UI", "Current admin_state: %d", admin_state);
    ESP_LOGI("UI", "Current screen: %d", current_screen);
    
    lv_btn_set_state(admin_switch_sta, LV_BTN_STATE_CHECKED_PRESSED);
    lv_label_set_text(admin_switch_sta_text, "Downloading...");
    
    // Update IP information when connected as station immediately
    ESP_LOGI("UI", "About to call ui_update_ip_info from STA connected handler");
    ui_update_ip_info();
    ESP_LOGI("UI", "ui_update_ip_info call completed from STA connected handler");
    
    // TODO: Also create a delayed task to retry getting IP info
    // xTaskCreate(ui_delayed_ip_update_task, "delayed_ip_update", 2048, NULL, 5, NULL);
    
    admin_state = ADMIN_STATE_STA;
}

void ui_sta_disconnected_handler(){
    sta_connected = false;
    lv_btn_set_state(admin_switch_sta, LV_BTN_STATE_RELEASED);
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
}

void ui_sta_stop_handler() {
    sta_connected = false;
    lv_label_set_text(admin_switch_sta_text, "WIFI MARAUDER");
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
}

void ui_connection_progress(uint8_t cur, uint8_t max){
    if(cur != max){
        char buf[BADGE_BUF_SIZE + 20] = {0}; // Increase the size of buf to accommodate the entire formatted string
        snprintf(buf, sizeof(buf), "Connecting (%d/%d)", cur, max);
        lv_label_set_text(admin_switch_sta_text, buf);
        lv_obj_set_hidden(admin_switch_sta_text, false);
    } else {
        lv_label_set_text(admin_switch_sta_text, "Connection failed!");
        lv_obj_set_hidden(admin_switch_sta_text, false);
    }
}

void ui_toggle_sync(){
    lv_btn_set_state(admin_sync, LV_BTN_STATE_RELEASED);
    ui_send_wifi_event(EVENT_MARAUDER_STOP);
}

void ui_update_ip_info() {
    char buf[BADGE_BUF_SIZE + 30] = {0};
    
    ESP_LOGI("UI", "=== IP INFO DEBUG ===");
    ESP_LOGI("UI", "sta_connected: %s, admin_state: %d", 
             sta_connected ? "true" : "false", 
             admin_state);
    
    // First, list all network interfaces for debugging
    ui_list_all_netifs();
    
    // Get STA interface and show STA IP when connected as station
    if (sta_connected) {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        ESP_LOGI("UI", "STA netif handle (WIFI_STA_DEF): %p", sta_netif);
        
        if (sta_netif) {
            esp_netif_ip_info_t ip_info;
            esp_err_t ret = esp_netif_get_ip_info(sta_netif, &ip_info);
            ESP_LOGI("UI", "STA esp_netif_get_ip_info returned: %s", esp_err_to_name(ret));
            ESP_LOGI("UI", "STA IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI("UI", "STA Gateway: " IPSTR, IP2STR(&ip_info.gw));
            ESP_LOGI("UI", "STA Netmask: " IPSTR, IP2STR(&ip_info.netmask));
            
            if (ret == ESP_OK && ip_info.ip.addr != 0) {
                ESP_LOGI("UI", "STA IP is valid, updating UI labels...");
                snprintf(buf, sizeof(buf), "Client IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                ESP_LOGI("UI", "Set admin_client_ip text to: %s", buf);
                
                snprintf(buf, sizeof(buf), "Gateway: " IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ESP_LOGI("UI", "Set admin_gateway_ip text to: %s", buf);
                ESP_LOGI("UI", "Successfully displayed STA IP info");
                return;
            } else {
                ESP_LOGW("UI", "STA IP is not valid or error occurred. ret=%s, ip.addr=0x%08x", 
                         esp_err_to_name(ret), ip_info.ip.addr);
            }
        } else {
            ESP_LOGW("UI", "Could not get STA netif handle");
        }
    }
    
    // If we reach here, we couldn't get IP info through normal methods
    // Try iterating through all interfaces as fallback
    ESP_LOGI("UI", "Primary methods failed, trying to iterate through all interfaces...");
    
    esp_netif_t *netif = NULL;
    esp_netif_t *temp_netif = esp_netif_next(netif);
    bool ip_found = false;
    
    while (temp_netif != NULL && !ip_found) {
        esp_netif_ip_info_t ip_info;
        esp_err_t ret = esp_netif_get_ip_info(temp_netif, &ip_info);
        
        if (ret == ESP_OK && ip_info.ip.addr != 0) {
            const char* desc = esp_netif_get_desc(temp_netif);
            ESP_LOGI("UI", "Found valid IP on interface %s: " IPSTR, 
                     desc ? desc : "unknown", IP2STR(&ip_info.ip));
            
            // Use interface description to determine type instead of IP range heuristic
            if (desc && strstr(desc, "sta")) {
                // STA interface
                snprintf(buf, sizeof(buf), "Client IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                
                snprintf(buf, sizeof(buf), "\nConnect to\nhttp://" IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ip_found = true;
            }
            ESP_LOGI("UI", "Successfully displayed IP info from interface iteration");
        }
        
        temp_netif = esp_netif_next(temp_netif);
    }
    
    if (!ip_found) {
        ESP_LOGW("UI", "No valid IP information found to display");
        lv_obj_set_hidden(admin_client_ip, true);
        lv_obj_set_hidden(admin_gateway_ip, true);
    }
    
    ESP_LOGI("UI", "=== END IP INFO DEBUG ===");
}


static void ui_init(void)
{
    ui_screen_splash_init();

    ui_screen_admin_init();
    
    // show first page.
    lv_scr_load(screens[current_screen]);

    // Turn on backlight and run backlight management task
    ui_update_backlight(true);
    backlight_task_handle = lv_task_create(ui_backlight_task, 1000, LV_TASK_PRIO_LOW, NULL);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ui_sta_connected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ui_sta_disconnected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &ui_sta_stop_handler, NULL));
}

static void ui_tick_task(void *arg)
{
    lv_tick_inc(1);
}

void ui_task(void *arg)
{
    SemaphoreHandle_t xGuiSemaphore;
    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();
    lvgl_driver_init();

    lv_color_t *buf1 = (lv_color_t*) heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t*) heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);

    static lv_disp_buf_t disp_buf;
    uint32_t size_in_px = DISP_BUF_SIZE;

    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;
    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &ui_tick_task,
        .name = "ui_tick_task",
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

    ui_init();

    while (1)
    {
        /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
        {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    free(buf1);
    free(buf2);
    vTaskDelete(NULL);
}

void ui_switch_page_down()
{
    ui_update_backlight(true);

    current_screen++;
    current_screen %= NUM_SCREENS;
    ESP_LOGI("DISPLAY", "DISPLAY COUNTER: %d/%d", current_screen+1, NUM_SCREENS);

    lv_scr_load_anim(screens[current_screen], LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, false);
}

void ui_switch_page_up()
{
    ui_update_backlight(true);

    current_screen--;
    current_screen = (NUM_SCREENS + (current_screen % NUM_SCREENS)) % NUM_SCREENS;
    ESP_LOGI("DISPLAY", "DISPLAY COUNTER: %d/%d", current_screen+1, NUM_SCREENS);
    
    lv_scr_load_anim(screens[current_screen], LV_SCR_LOAD_ANIM_OVER_BOTTOM, 300, 0, false);
}

void button_task(void *arg)
{
    static button_event_t curr_ev;
    static button_event_t prev_ev[2];
    static QueueHandle_t button_events;
    button_events = button_init(PIN_BIT(BUTTON_1) | PIN_BIT(BUTTON_2));

    while (true)
    {
        if (xQueueReceive(button_events, &curr_ev, 1000 / portTICK_PERIOD_MS))
        {
            uint8_t btn_id = curr_ev.pin - 0x08;
            if (curr_ev.event == BUTTON_HELD)
            {
                set_screen_led_backlight(badge_obj.brightness_mid);
            }
            if (curr_ev.pin == BUTTON_1) // DOWN button event
            {
                if ((prev_ev[btn_id].event == BUTTON_HELD) && (curr_ev.event == BUTTON_UP))
                {
                    ui_switch_page_down();
                }
                else if ((prev_ev[btn_id].event == BUTTON_DOWN) && (curr_ev.event == BUTTON_UP))
                {
                    ui_button_down();
                }
            }

            if (curr_ev.pin == BUTTON_2) // UP button event
            {
                if ((prev_ev[btn_id].event == BUTTON_HELD) && (curr_ev.event == BUTTON_UP))
                {
                    ui_switch_page_up();
                }
                else if ((prev_ev[btn_id].event == BUTTON_DOWN) && (curr_ev.event == BUTTON_UP))
                {
                    ui_button_up();
                }
            }
            prev_ev[btn_id] = curr_ev;
        }
    }
}

void ui_list_all_netifs() {
    ESP_LOGI("UI", "=== LISTING ALL NETWORK INTERFACES ===");
    
    // Try to iterate through all available network interfaces
    esp_netif_t *netif = NULL;
    esp_netif_t *temp_netif = esp_netif_next(netif);
    int count = 0;
    
    while (temp_netif != NULL) {
        count++;
        ESP_LOGI("UI", "Found netif %d: %p", count, temp_netif);
        
        // Get interface description
        const char* desc = esp_netif_get_desc(temp_netif);
        ESP_LOGI("UI", "Interface %d description: %s", count, desc ? desc : "unknown");
        
        // Get IP info for this interface
        esp_netif_ip_info_t ip_info;
        esp_err_t ret = esp_netif_get_ip_info(temp_netif, &ip_info);
        ESP_LOGI("UI", "Interface %d IP info (ret: %s):", count, esp_err_to_name(ret));
        ESP_LOGI("UI", "  IP: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI("UI", "  Gateway: " IPSTR, IP2STR(&ip_info.gw));
        ESP_LOGI("UI", "  Netmask: " IPSTR, IP2STR(&ip_info.netmask));
        
        temp_netif = esp_netif_next(temp_netif);
    }
    
    ESP_LOGI("UI", "Total network interfaces found: %d", count);
    ESP_LOGI("UI", "=== END NETIF LISTING ===");
}

void ui_manual_ip_update() {
    ESP_LOGI("UI", "Manual IP update triggered from admin screen");
    ui_update_ip_info();
}

void ui_force_show_ip_labels() {
    ESP_LOGI("UI", "Force showing IP labels for testing - calling ui_update_ip_info instead");
    ui_update_ip_info();
}
