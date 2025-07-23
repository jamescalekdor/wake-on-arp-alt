#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/ethernet.h>

int load_config(struct config *conf) {
    memset(conf, 0, sizeof(*conf));
    conf->subnet = 24;
    conf->allow_gateway = false;
    conf->num_targets = 0;

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", CONFIG_FILE);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[64];
        if (sscanf(line, "%63s %63s", key, value) != 2) continue;

        if (strcmp(key, "broadcast_ip") == 0) {
            inet_pton(AF_INET, value, &conf->broadcast_ip);
        } else if (strcmp(key, "net_device") == 0) {
            strncpy(conf->net_device, value, IF_NAMESIZE - 1);
        } else if (strcmp(key, "lan_device") == 0) {
            strncpy(conf->lan_device, value, IF_NAMESIZE - 1);
        } else if (strcmp(key, "subnet") == 0) {
            conf->subnet = atoi(value);
        } else if (strcmp(key, "allow_gateway") == 0) {
            conf->allow_gateway = (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0);
        } else if (strncmp(key, "target_ip_", 10) == 0) {
            int idx = atoi(key + 10) - 1;
            if (idx >= 0 && idx < MAX_TARGETS) {
                inet_pton(AF_INET, value, &conf->target_ip[idx]);
                if (idx + 1 > conf->num_targets) conf->num_targets = idx + 1;
            }
        } else if (strncmp(key, "target_mac_", 11) == 0) {
            int idx = atoi(key + 11) - 1;
            if (idx >= 0 && idx < MAX_TARGETS) {
                ether_aton_r(value, &conf->target_mac[idx]);
            }
        }
    }
    fclose(f);
    return 0;
}
