#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nat.h"

static const char *TAG = "nat";

static esp_netif_t *s_wan;
static esp_netif_t *s_lan;
static bool s_napt_on;

static void lan_set_dns_from_wan(void)
{
    if (s_lan == NULL || s_wan == NULL) {
        return;
    }
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_wan, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_lan, ESP_NETIF_DNS_MAIN, &dns));
}

static void apply_wan_up(const esp_netif_ip_info_t *ip)
{
    ESP_LOGI(TAG, "WAN IP " IPSTR, IP2STR(&ip->ip));
    esp_netif_set_default_netif(s_wan);
    lan_set_dns_from_wan();
    if (!s_napt_on && s_lan) {
        if (esp_netif_napt_enable(s_lan) == ESP_OK) {
            s_napt_on = true;
            ESP_LOGI(TAG, "NAPT enabled on LAN");
        } else {
            ESP_LOGE(TAG, "NAPT enable failed");
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_ETH_GOT_IP && event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *event = event_data;
    if (s_wan == NULL || event->esp_netif != s_wan) {
        return;
    }
    apply_wan_up(&event->ip_info);
}

esp_err_t net_nat_bind(esp_netif_t *wan, esp_netif_t *lan)
{
    s_wan = wan;
    s_lan = lan;
    esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (s_wan) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_wan, &ip) == ESP_OK && ip.ip.addr != 0) {
            apply_wan_up(&ip);
        }
    }
    return ESP_OK;
}
