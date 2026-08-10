/* Captive-portal DNS.
 *
 * Answers every A query with the AP's own address, so a phone or laptop that
 * joins the analyzer's access point resolves whatever hostname it probes to
 * us. Combined with the catch-all redirect in web_server.c, that is what makes
 * the operating system pop the portal open by itself instead of leaving the
 * user to discover that they must type 192.168.4.1.
 *
 * Deliberately minimal: it does not parse the question section beyond finding
 * its end, and it answers everything identically. That is the correct
 * behaviour for a captive portal — the point is precisely to lie about every
 * name — and it means no DNS parsing bugs to get wrong. Requests that are not
 * standard queries are dropped rather than answered.
 *
 * Runs only while the AP is up; net_mgr starts and stops it with the AP.
 */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "captive_dns.h"

static const char *TAG = "captive_dns";

#define DNS_PORT       53
#define DNS_MAX_LEN   512   /* classic UDP DNS limit; longer requests are ignored */
#define DNS_TTL        60   /* short, so a client re-resolves soon after leaving  */

/* DNS header, network byte order on the wire. */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static TaskHandle_t s_task;
static int          s_sock = -1;
static volatile bool s_run;
static uint32_t     s_answer_ip;   /* network byte order */

/* Walk a QNAME: a sequence of length-prefixed labels ending in a zero byte.
 * Returns the offset just past the terminator, or 0 if malformed/truncated. */
static size_t skip_qname(const uint8_t *buf, size_t len, size_t off)
{
    while (off < len) {
        uint8_t l = buf[off];
        if (l == 0) return off + 1;
        /* A compression pointer has no business in a question; refuse it
         * rather than follow it. */
        if ((l & 0xC0) != 0) return 0;
        off += 1 + l;
    }
    return 0;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX_LEN];

    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < (int)sizeof(dns_header_t)) continue;

        dns_header_t *hdr = (dns_header_t *)buf;

        /* Standard query, one question, not already a response. */
        uint16_t flags = ntohs(hdr->flags);
        if (flags & 0x8000)              continue;   /* it is a response      */
        if ((flags >> 11) & 0x0F)        continue;   /* non-standard opcode   */
        if (ntohs(hdr->qd_count) != 1)   continue;

        size_t qend = skip_qname(buf, (size_t)n, sizeof(dns_header_t));
        if (qend == 0 || qend + 4 > (size_t)n) continue;   /* no QTYPE/QCLASS */
        qend += 4;

        /* Answer: NAME as a compression pointer to the question at offset 12,
         * TYPE A, CLASS IN, TTL, RDLENGTH 4, then the address. */
        static const uint8_t answer_tpl[] = {
            0xC0, 0x0C,             /* pointer to offset 12 (the question) */
            0x00, 0x01,             /* TYPE  A                             */
            0x00, 0x01,             /* CLASS IN                            */
            0x00, 0x00, 0x00, DNS_TTL,
            0x00, 0x04,             /* RDLENGTH                            */
        };
        if (qend + sizeof(answer_tpl) + 4 > sizeof(buf)) continue;

        hdr->flags    = htons(0x8180);   /* response, recursion available */
        hdr->an_count = htons(1);
        hdr->ns_count = 0;
        hdr->ar_count = 0;

        memcpy(buf + qend, answer_tpl, sizeof(answer_tpl));
        memcpy(buf + qend + sizeof(answer_tpl), &s_answer_ip, 4);
        size_t reply_len = qend + sizeof(answer_tpl) + 4;

        sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
    }

    /* Closed by captive_dns_stop(); just retire. */
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ap_ip_network_order)
{
    if (s_task) return ESP_OK;   /* already running */

    s_answer_ip = ap_ip_network_order;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGW(TAG, "socket() failed — the portal will still work, but only "
                      "if the user types the address");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "bind(:53) failed — captive portal disabled");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_run = true;
    /* Priority 3: below the LVGL port task (4), like every other background
     * worker here, so it can never starve the UI. */
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, &s_task) != pdPASS) {
        s_run = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "captive DNS up — every name resolves to the analyzer");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_task) return;
    s_run = false;
    /* Closing the socket wakes the blocked recvfrom() so the task can exit;
     * there is no other way to interrupt it. */
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

bool captive_dns_running(void)
{
    return s_task != NULL;
}
