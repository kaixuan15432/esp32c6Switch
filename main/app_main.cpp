/*
 * ESP32-C6 Matter 延时开关
 * 框架：ESP-Matter (官方 SDK)
 * 协议：Matter over Wi-Fi
 * 支持：Apple Home / Google Home / Amazon Alexa
 *
 * 继电器：低电平触发（IN=LOW 时闭合）
 *
 * GPIO 分配：
 *   GPIO4  → 继电器 IN（低电平触发）
 *   GPIO8  → 状态 LED（高电平亮）
 *   GPIO9  → 物理按钮（低电平触发，内部上拉）
 */

#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

/* ESP-Matter 头文件 */
#include <esp_matter.h>
#include <esp_matter_console.h>

/* Matter 数据模型 */
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "matter-switch";

/* ================================================================
 * GPIO 定义
 * ================================================================ */
#define RELAY_GPIO          GPIO_NUM_4
#define LED_GPIO            GPIO_NUM_8
#define BUTTON_GPIO         GPIO_NUM_9

/* ================================================================
 * 延时配置（单位：秒，0 = 不自动关闭）
 * ================================================================ */
#define DEFAULT_DELAY_SEC   0
#define MAX_DELAY_SEC       3600

/* ================================================================
 * 全局状态
 * ================================================================ */
static bool             s_relay_on       = false;
static uint32_t         s_delay_sec      = DEFAULT_DELAY_SEC;
static TimerHandle_t    s_delay_timer    = NULL;
static SemaphoreHandle_t s_button_sem    = NULL;

/* Matter 端点 / 属性句柄 */
static uint16_t         s_endpoint_id    = 0;
static uint32_t         s_cluster_id     = OnOff::Id;
static uint32_t         s_attr_id        = OnOff::Attributes::OnOff::Id;

/* ================================================================
 * 底层继电器 & LED 控制
 *   继电器低电平触发：ON → GPIO=0，OFF → GPIO=1
 * ================================================================ */
static void relay_set_level(bool on)
{
    s_relay_on = on;
    gpio_set_level(RELAY_GPIO, on ? 0 : 1);   /* 低电平触发：ON=0 */
    gpio_set_level(LED_GPIO,   on ? 1 : 0);   /* LED 高电平亮    */
    ESP_LOGI(TAG, "继电器 → %s", on ? "闭合(ON)" : "断开(OFF)");
}

/* ================================================================
 * 延时定时器
 * ================================================================ */
static void delay_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "延时到期，自动断开继电器");
    relay_set_level(false);

    /* 通知 Matter 层属性已变化，让 Apple Home 同步 */
    esp_matter_attr_val_t val = esp_matter_bool(false);
    attribute::update(s_endpoint_id, s_cluster_id, s_attr_id, &val);
}

static void start_delay_timer(void)
{
    if (s_delay_sec == 0) return;

    TickType_t ticks = pdMS_TO_TICKS((uint64_t)s_delay_sec * 1000);

    if (s_delay_timer == NULL) {
        s_delay_timer = xTimerCreate("delay_off", ticks,
                                      pdFALSE, NULL, delay_timer_cb);
    } else {
        xTimerChangePeriod(s_delay_timer, ticks, portMAX_DELAY);
    }

    if (s_delay_timer) {
        xTimerStart(s_delay_timer, 0);
        ESP_LOGI(TAG, "延时定时器已启动：%lu 秒后自动关闭", s_delay_sec);
    }
}

static void stop_delay_timer(void)
{
    if (s_delay_timer && xTimerIsTimerActive(s_delay_timer)) {
        xTimerStop(s_delay_timer, 0);
        ESP_LOGI(TAG, "延时定时器已取消");
    }
}

/* ================================================================
 * Matter 属性更新回调
 *   当 Apple Home / 语音助手 修改属性时，此函数被调用
 * ================================================================ */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                          uint16_t endpoint_id,
                                          uint32_t cluster_id,
                                          uint32_t attribute_id,
                                          esp_matter_attr_val_t *val,
                                          void *priv_data)
{
    /* 只处理写前回调（PRE_UPDATE），在这里驱动硬件 */
    if (type != PRE_UPDATE) return ESP_OK;

    /* OnOff Cluster → OnOff 属性 */
    if (cluster_id  == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id)
    {
        bool new_state = val->val.b;
        ESP_LOGI(TAG, "Matter 写入 OnOff = %s", new_state ? "true" : "false");

        if (new_state) {
            relay_set_level(true);
            start_delay_timer();
        } else {
            stop_delay_timer();
            relay_set_level(false);
        }
    }

    return ESP_OK;
}

/* ================================================================
 * Matter 标识回调（设备被 "识别" 时闪烁 LED）
 * ================================================================ */
static esp_err_t app_identification_cb(identification::callback_type_t type,
                                        uint16_t endpoint_id,
                                        uint8_t effect_id,
                                        uint8_t effect_variant,
                                        void *priv_data)
{
    ESP_LOGI(TAG, "Identify 回调：endpoint=%d effect=%d", endpoint_id, effect_id);
    return ESP_OK;
}

/* ================================================================
 * 物理按钮：处理逻辑 (消抖 + 短按切换 + 长按复位)
 * ================================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    if (s_button_sem) {
        xSemaphoreGiveFromISR(s_button_sem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void button_worker_task(void *arg)
{
    ESP_LOGI(TAG, "按钮处理任务已启动");
    
    while (1) {
        /* 等待中断信号 */
        if (xSemaphoreTake(s_button_sem, portMAX_DELAY) == pdTRUE) {
            /* 1. 简单的消抖：等待 20ms 确认状态 */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (gpio_get_level(BUTTON_GPIO) != 0) continue;

            /* 2. 记录按下时间，监测是否长按 */
            uint32_t press_start = xTaskGetTickCount();
            bool long_press_detected = false;

            while (gpio_get_level(BUTTON_GPIO) == 0) {
                if ((xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(5000)) {
                    long_press_detected = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (long_press_detected) {
                /* --- 工厂复位流程 --- */
                ESP_LOGW(TAG, "检测到长按 5 秒 $\rightarrow$ 正在执行深度工厂复位...");
                
                /* A. 擦除 NVS */
                nvs_flash_erase();
                ESP_LOGI(TAG, "NVS 已擦除");

                /* B. 擦除 Matter Storage 分区 (关键：清除 Fabric 数据) */
                /* 使用 ESP_PARTITION_SUBTYPE_ANY 配合标签 "storage" 来查找 */
                const esp_partition_t *storage_part = esp_partition_find_first(
                    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
                
                if (storage_part) {
                    ESP_LOGI(TAG, "找到 storage 分区 (addr: 0x%lx, size: %lu)", 
                             (unsigned long)storage_part->address, (unsigned long)storage_part->size);
                    esp_partition_erase_range(storage_part, 0, storage_part->size);
                    ESP_LOGI(TAG, "Storage 分区已擦除");
                } else {
                    ESP_LOGE(TAG, "未找到 storage 分区，无法清除配网凭据");
                }

                ESP_LOGE(TAG, "深度复位完成，设备即将重启...");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                /* --- 短按切换状态 --- */
                bool new_state = !s_relay_on;
                ESP_LOGI(TAG, "物理按钮触发 $\rightarrow$ %s", new_state ? "ON" : "OFF");

                if (new_state) {
                    relay_set_level(true);
                    start_delay_timer();
                } else {
                    stop_delay_timer();
                    relay_set_level(false);
                }

                /* 同步到 Matter / Apple Home (在 Worker Task 栈中运行，避免 Tmr Svc 溢出) */
                esp_matter_attr_val_t val = esp_matter_bool(new_state);
                attribute::update(s_endpoint_id, s_cluster_id, s_attr_id, &val);
            }
        }
    }
}

/* ================================================================
 * GPIO 初始化
 * ================================================================ */
static void gpio_init_all(void)
{
    /* --- 继电器 --- */
    gpio_config_t relay_cfg = {
        .pin_bit_mask = BIT64(RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    gpio_set_level(RELAY_GPIO, 1); /* 初始高电平 = 继电器断开 */

    /* --- LED --- */
    gpio_config_t led_cfg = {
        .pin_bit_mask = BIT64(LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_GPIO, 0);

    /* --- 按钮（下降沿触发，内部上拉） --- */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    ESP_LOGI(TAG, "GPIO 初始化完成");

    ESP_LOGI(TAG, "  RELAY → GPIO%d（低电平触发）", RELAY_GPIO);
    ESP_LOGI(TAG, "  LED   → GPIO%d", LED_GPIO);
    ESP_LOGI(TAG, "  BTN   → GPIO%d", BUTTON_GPIO);
}

/* ================================================================
 * 构建 Matter 数据模型
 *   Root Node + On/Off Light endpoint（苹果 Home 识别为"开关"）
 * ================================================================ */
static void matter_data_model_init(void)
{
    /* ---- 根节点 ---- */
    node::config_t node_cfg;
    snprintf(node_cfg.root_node.basic_information.node_label,
             sizeof(node_cfg.root_node.basic_information.node_label),
             "延时开关");

    node_t *node = node::create(&node_cfg,
                                  app_attribute_update_cb,
                                  app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "创建 Matter 节点失败");
        return;
    }

    /* ---- On/Off Light 端点（Apple Home 识别为灯/开关） ---- */
    on_off_light::config_t light_cfg;
    light_cfg.on_off.on_off         = false;  /* 初始关闭 */

    endpoint_t *ep = on_off_light::create(node, &light_cfg,
                                            ENDPOINT_FLAG_NONE, NULL);
    if (ep == nullptr) {
        ESP_LOGE(TAG, "创建端点失败");
        return;
    }


    s_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Matter 端点 ID = %d", s_endpoint_id);
}

/* ================================================================
 * app_main
 * ================================================================ */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-C6 Matter 延时开关  v1.0");
    ESP_LOGI(TAG, "========================================");

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* GPIO */
    gpio_init_all();

    /* 启动物理按钮处理任务 (消抖 + 切换 + 复位) */
    s_button_sem = xSemaphoreCreateBinary();
    if (s_button_sem == NULL) {
        ESP_LOGE(TAG, "无法创建按钮信号量");
    } else {
        xTaskCreate(button_worker_task, "btn_worker", 4096, NULL, 5, NULL);
    }

    /* Matter 数据模型 */
    matter_data_model_init();

    /* 启动 Matter（内含 Wi-Fi Provisioning） */
    esp_matter::start(NULL);

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::init();
#endif

    ESP_LOGI(TAG, "Matter 已启动，等待配网...");
    ESP_LOGI(TAG, "延时关闭: %lu 秒（0=不自动关闭）", s_delay_sec);
    ESP_LOGI(TAG, "配网方式: Apple Home App → 添加配件 → 扫描二维码");
}
