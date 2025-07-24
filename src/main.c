#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <time.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include "config.h"

#define THROTTLE_SEC 30  // Min seconds between WoL sends per target

static char if_name[IF_NAMESIZE];  // For cleanup
static short orig_flags;  // Original interface flags

void cleanup_promisc() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr;
    strncpy(ifr.ifr_name, if_name, IFNAMSIZ - 1);
    ifr.ifr_flags = orig_flags;
    ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
    printf("Restored %s to non-promiscuous mode\n", if_name);
}

int get_interface_info(const char *dev, struct in_addr *ip, struct ether_addr *mac, int *ifindex) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) goto err;
    *ifindex = ifr.ifr_ifindex;
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) goto err;
    *ip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) goto err;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    close(s);
    return 0;
err:
    close(s);
    return -1;
}

int send_wol(const struct config *conf, int target_idx, struct in_addr lan_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    struct sockaddr_in myaddr = {0};
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr = lan_ip;
    myaddr.sin_port = 0;
    if (bind(sock, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) goto err;
    uint8_t payload[102];
    uint8_t *p = payload;
    memset(p, 0xff, 6); p += 6;
    for (int j = 0; j < 16; j++) {
        memcpy(p, &conf->target_mac[target_idx], ETH_ALEN);
        p += ETH_ALEN;
    }
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_addr = conf->broadcast_ip;
    dest.sin_port = htons(9);
    sendto(sock, payload, p - payload, 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);
    return 0;
err:
    close(sock);
    return -1;
}

int main() {
    struct config conf;
    if (load_config(&conf) < 0) return 1;
    struct in_addr my_ip, lan_ip;
    struct ether_addr my_mac, lan_mac;
    int ifindex, lan_ifindex;
    if (get_interface_info(conf.net_device, &my_ip, &my_mac, &ifindex) < 0) {
        perror("WiFi interface info");
        return 1;
    }
    if (get_interface_info(conf.lan_device, &lan_ip, &lan_mac, &lan_ifindex) < 0) {
        perror("LAN interface info");
        return 1;
    }
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sock < 0) {
        perror("IP socket");
        return 1;
    }
    struct sockaddr_ll bind_sa = {0};
    bind_sa.sll_family = AF_PACKET;
    bind_sa.sll_protocol = htons(ETH_P_IP);
    bind_sa.sll_ifindex = ifindex;
    if (bind(sock, (struct sockaddr *)&bind_sa, sizeof(bind_sa)) < 0) {
        perror("bind IP socket");
        close(sock);
        return 1;
    }

    // Set promiscuous mode
    int ioctl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ioctl_sock < 0) {
        perror("ioctl socket");
        close(sock);
        return 1;
    }
    struct ifreq ifr;
    strncpy(ifr.ifr_name, conf.net_device, IFNAMSIZ - 1);
    strncpy(if_name, conf.net_device, IF_NAMESIZE - 1);  // Save for cleanup
    if (ioctl(ioctl_sock, SIOCGIFFLAGS, &ifr) < 0) {
        perror("get if flags");
        close(ioctl_sock);
        close(sock);
        return 1;
    }
    orig_flags = ifr.ifr_flags;
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl(ioctl_sock, SIOCSIFFLAGS, &ifr) < 0) {
        perror("set promisc");
        close(ioctl_sock);
        close(sock);
        return 1;
    }
    close(ioctl_sock);
    atexit(cleanup_promisc);
    printf("Set %s to promiscuous mode\n", conf.net_device);

    time_t last_send[MAX_TARGETS] = {0};
    while (1) {
        uint8_t buf[ETH_FRAME_LEN];
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len < 0) {
            perror("recv");
            continue;
        }
        printf("Received packet, len=%d\n", len);  // Debug: Confirm packets are received

        if (len < ETH_HLEN + sizeof(struct iphdr)) continue;
        struct iphdr *ip = (struct iphdr *)(buf + ETH_HLEN);
        if (ip->version != 4 || ip->ihl < 5) continue;
        printf("IP packet, proto=%d\n", ip->protocol);  // Debug: See IP protocols

        if (ip->protocol == IPPROTO_TCP) {
            unsigned int iphlen = ip->ihl * 4;
            if (len < ETH_HLEN + iphlen + sizeof(struct tcphdr)) continue;
            struct tcphdr *tcp = (struct tcphdr *)(buf + ETH_HLEN + iphlen);
            if (tcp->syn) {
                char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(ip->saddr), src_str, INET_ADDRSTRLEN);
                inet_ntop(AF_INET, &(ip->daddr), dst_str, INET_ADDRSTRLEN);
                printf("Detected SYN from %s to %s\n", src_str, dst_str);  // Debug: See SYNs

                for (int i = 0; i < conf.num_targets; i++) {
                    if (ip->saddr == conf.target_ip[i].s_addr &&
                        ip->daddr == conf.target_server_ip[i].s_addr) {
                        printf("Matched target %d (client %s to server %s)\n", i, src_str, dst_str);  // Debug: Match found
                        time_t now = time(NULL);
                        if (now - last_send[i] > THROTTLE_SEC) {
                            printf("Sending WoL for target %d\n", i);  // Debug: WoL trigger
                            if (send_wol(&conf, i, lan_ip) < 0) {
                                perror("send WoL");
                            }
                            last_send[i] = now;
                        } else {
                            printf("Throttled WoL for target %d (last sent %ld sec ago)\n", i, now - last_send[i]);
                        }
                    }
                }
            }
        }
    }
    close(sock);
    return 0;
}
