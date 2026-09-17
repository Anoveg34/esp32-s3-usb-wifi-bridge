#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nat.h"

static const char *TAG = "nat";
#define DHCPS_OFFER_DNS 0x02

static esp_netif_t *s_wan;
static esp_netif_t *s_ap;
static bool s_napt_on;

static void ap_set_dns_from_wan(void)
{
    if (s_ap == NULL || s_wan == NULL) {
        return;
    }
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_wan, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    }
    uint8_t offer = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                                                         ESP_NETIF_DOMAIN_NAME_SERVER,
                                                         &offer, sizeof(offer)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap));
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_ETH_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *event = event_data;
    ESP_LOGI(TAG, "USB WAN IP " IPSTR, IP2STR(&event->ip_info.ip));
    esp_netif_set_default_netif(s_wan);
    ap_set_dns_from_wan();
    if (!s_napt_on && s_ap) {
        if (esp_netif_napt_enable(s_ap) == ESP_OK) {
            s_napt_on = true;
            ESP_LOGI(TAG, "NAPT enabled on SoftAP");
        } else {
            ESP_LOGE(TAG, "NAPT enable failed");
        }
    }
}

esp_err_t net_nat_bind(esp_netif_t *wan, esp_netif_t *ap)
{
    s_wan = wan;
    s_ap = ap;
    return esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL);
}
