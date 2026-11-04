/*
 * proxy_hook.h — lwIP IPv4 input hook for the HTTP proxy redirect.
 *
 * Compiled into the lwip component only (main/CMakeLists.txt sets
 * ESP_IDF_LWIP_HOOK_FILENAME on the lwip target), via ESP-IDF's
 * lwip_default_hooks.h. ip4_input() calls the hook for every IPv4 packet
 * before it validates the header or decides between local delivery and
 * forwarding, which is what lets http_proxy.c turn a phone's connection to
 * some_host:80 into a local one. See http_proxy.c.
 */

#ifndef PROXY_HOOK_H
#define PROXY_HOOK_H

struct pbuf;
struct netif;

/* Never eats the packet: returns 0, at most after rewriting its header. */
int proxy_ip4_input_hook(struct pbuf *p, struct netif *inp);

#define LWIP_HOOK_IP4_INPUT(p, inp) proxy_ip4_input_hook((p), (inp))

#endif
