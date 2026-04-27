#include "ethernet_manager.h"

#include "device_profile.h"
#include "network_ready.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "ETH"

static bool ethernet_initialized = false;
static bool ethernet_ready = false;
static bool ethernet_handlers_registered = false;
static bool ethernet_spi_bus_initialized = false;
static int ethernet_spi_bus_host = -1;

static esp_eth_handle_t ethernet_handle = NULL;
static esp_eth_netif_glue_handle_t ethernet_glue = NULL;
static esp_netif_t *ethernet_netif = NULL;

static const char *ethernet_mode_name(device_profile_ethernet_mode_t mode)
{
    switch (mode)
    {
        case DEVICE_PROFILE_ETH_INTERNAL_LAN8720:
            return "internal-lan8720";
        case DEVICE_PROFILE_ETH_SPI_W5500:
            return "spi-w5500";
        case DEVICE_PROFILE_ETH_NONE:
        default:
            return "none";
    }
}

static void ethernet_manager_seed_local_mac(esp_eth_handle_t handle)
{
    uint8_t mac_addr[6] = {0};

    if (!handle)
        return;

    if (esp_read_mac(mac_addr, ESP_MAC_WIFI_STA) != ESP_OK)
        return;

    mac_addr[0] = (uint8_t)((mac_addr[0] | 0x02U) & 0xFEU);
    mac_addr[5] ^= 0xA5U;

    (void)esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, mac_addr);
}

static void ethernet_manager_reset_w5500(const device_network_w5500_profile_t *cfg)
{
    if (!cfg || cfg->reset_gpio == GPIO_NUM_NC)
        return;

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << (uint64_t)cfg->reset_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&io_cfg) != ESP_OK)
        return;

    gpio_set_level(cfg->reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(cfg->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

#if CONFIG_ETH_SPI_ETHERNET_W5500
static esp_err_t ethernet_manager_start_w5500(const device_network_w5500_profile_t *cfg)
{
    esp_err_t ret;
    spi_bus_config_t bus_cfg = {0};
    spi_device_interface_config_t dev_cfg = {0};
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    bool bus_initialized_here = false;
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();

    if (!cfg)
        return ESP_ERR_INVALID_ARG;

    if (!cfg || !device_profile_w5500_is_configured())
        return ESP_ERR_NOT_FOUND;

    ethernet_manager_reset_w5500(cfg);

    bus_cfg.mosi_io_num = cfg->mosi_gpio;
    bus_cfg.miso_io_num = cfg->miso_gpio;
    bus_cfg.sclk_io_num = cfg->sclk_gpio;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;

    if (!ethernet_spi_bus_initialized)
    {
        ret = spi_bus_initialize((spi_host_device_t)cfg->spi_host_id, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Falha ao iniciar SPI do W5500: %s", esp_err_to_name(ret));
            return ret;
        }

        ethernet_spi_bus_initialized = true;
        ethernet_spi_bus_host = cfg->spi_host_id;
        bus_initialized_here = true;
    }

    dev_cfg.mode = 0;
    dev_cfg.clock_speed_hz = ((int)(cfg->clock_mhz ? cfg->clock_mhz : 20)) * 1000 * 1000;
    dev_cfg.spics_io_num = cfg->cs_gpio;
    dev_cfg.queue_size = 20;

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG((spi_host_device_t)cfg->spi_host_id, &dev_cfg);
    w5500_cfg.int_gpio_num = cfg->int_gpio;

    phy_cfg.phy_addr = cfg->phy_addr;
    phy_cfg.reset_gpio_num = cfg->reset_gpio;

    mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    phy = esp_eth_phy_new_w5500(&phy_cfg);

    if (!mac || !phy)
    {
        ESP_LOGE(TAG, "Falha ao criar MAC/PHY do W5500");
        ret = ESP_FAIL;
        goto fail;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_cfg, &ethernet_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao instalar driver W5500: %s", esp_err_to_name(ret));
        goto fail;
    }

    ethernet_manager_seed_local_mac(ethernet_handle);

    ethernet_netif = esp_netif_new(&netif_cfg);
    if (!ethernet_netif)
    {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Falha ao criar netif Ethernet");
        goto fail;
    }

    ethernet_glue = esp_eth_new_netif_glue(ethernet_handle);
    if (!ethernet_glue)
    {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Falha ao criar glue Ethernet");
        goto fail;
    }

    ret = esp_netif_attach(ethernet_netif, ethernet_glue);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao anexar netif Ethernet: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_eth_start(ethernet_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao iniciar Ethernet: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG, "Driver W5500 iniciado (host=%d, clock=%d MHz)", cfg->spi_host_id, cfg->clock_mhz);
    return ESP_OK;

fail:
    if (ethernet_glue)
    {
        esp_eth_del_netif_glue(ethernet_glue);
        ethernet_glue = NULL;
    }

    if (ethernet_netif)
    {
        esp_netif_destroy(ethernet_netif);
        ethernet_netif = NULL;
    }

    if (ethernet_handle)
    {
        (void)esp_eth_driver_uninstall(ethernet_handle);
        ethernet_handle = NULL;
    }

    if (phy)
        (void)phy->del(phy);

    if (mac)
        (void)mac->del(mac);

    if (bus_initialized_here && ethernet_spi_bus_initialized)
    {
        (void)spi_bus_free((spi_host_device_t)cfg->spi_host_id);
        ethernet_spi_bus_initialized = false;
        ethernet_spi_bus_host = -1;
    }

    return ret;
}
#endif

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;

    if (event_base == ETH_EVENT)
    {
        switch (event_id)
        {
            case ETHERNET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "Link Ethernet conectado");
                break;
            case ETHERNET_EVENT_DISCONNECTED:
            case ETHERNET_EVENT_STOP:
                ethernet_ready = false;
                network_ready_publish_down(NETWORK_READY_LINK_ETHERNET);
                break;
            default:
                break;
        }

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        ethernet_ready = true;
        network_ready_publish_up(
            NETWORK_READY_LINK_ETHERNET,
            event ? event->ip_info.ip.addr : 0U,
            event ? event->ip_info.netmask.addr : 0U);
        ESP_LOGI(TAG, "Ethernet com IP pronto");
    }
}

void ethernet_manager_init(void)
{
    const device_network_profile_t *network = device_profile_network();
    esp_err_t ret;

    if (ethernet_initialized)
    {
        ESP_LOGW(TAG, "Ethernet manager ja inicializado");
        return;
    }

    ethernet_initialized = true;

    if (!network || network->ethernet_mode == DEVICE_PROFILE_ETH_NONE)
    {
        ESP_LOGI(TAG, "Ethernet indisponivel neste perfil");
        return;
    }

    ESP_LOGI(TAG, "Ethernet manager preparado para %s", ethernet_mode_name(network->ethernet_mode));

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ret);

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ret);

    if (!ethernet_handlers_registered)
    {
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethernet_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethernet_event_handler, NULL));
        ethernet_handlers_registered = true;
    }

    if (!network->ethernet_supported)
    {
        ESP_LOGI(TAG, "Perfil sinaliza backend %s, mas a placa atual nao habilitou Ethernet fisica ainda",
                 ethernet_mode_name(network->ethernet_mode));
        return;
    }

    switch (network->ethernet_mode)
    {
#if CONFIG_ETH_SPI_ETHERNET_W5500
        case DEVICE_PROFILE_ETH_SPI_W5500:
            ret = ethernet_manager_start_w5500(&network->w5500);
            if (ret == ESP_ERR_NOT_FOUND)
            {
                ESP_LOGW(TAG, "Perfil W5500 ainda sem pinagem SPI definida");
                return;
            }
            ESP_ERROR_CHECK(ret);
            break;
#else
        case DEVICE_PROFILE_ETH_SPI_W5500:
            ESP_LOGW(TAG, "W5500 nao habilitado no sdkconfig");
            return;
#endif
        case DEVICE_PROFILE_ETH_INTERNAL_LAN8720:
            ESP_LOGW(TAG, "LAN8720 ainda nao implementado");
            return;
        case DEVICE_PROFILE_ETH_NONE:
        default:
            return;
    }
}

bool ethernet_manager_is_ready(void)
{
    return ethernet_ready;
}
