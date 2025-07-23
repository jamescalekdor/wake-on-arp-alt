#ifndef CONFIG_H
#define CONFIG_H

#include <netinet/in.h>
#include <net/ethernet.h>
#include <net/if.h>

#define MAX_TARGETS 10
#ifndef CONFIG_FILE
#define CONFIG_FILE CONFIG_PREFIX "/wake-on-arp-alt.conf"
#endif

struct config {
    struct in_addr broadcast_ip;
    char net_device[IF_NAMESIZE];
    char lan_device[IF_NAMESIZE];
    int subnet;
    bool allow_gateway;
    int num_targets;
    struct in_addr target_ip[MAX_TARGETS];
    struct ether_addr target_mac[MAX_TARGETS];
};

int load_config(struct config *conf);

#endif
