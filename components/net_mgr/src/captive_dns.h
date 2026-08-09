#pragma once

/* Captive-portal DNS: answers every query with the analyzer's own address so
 * a client that joins the AP is redirected to the portal automatically.
 * Started and stopped by net_mgr alongside the access point. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* `ap_ip_network_order` is the address to hand out — 192.168.4.1 as stored in
 * an esp_ip4_addr_t, i.e. already network byte order. Non-fatal on failure:
 * the portal still works, the user just has to type the address. */
esp_err_t captive_dns_start(uint32_t ap_ip_network_order);
void      captive_dns_stop(void);
bool      captive_dns_running(void);
