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
#include <sys/select.h>
#include "config.h"

#define POLL_INTERVAL 5  // seconds between poll cycles
#define REPLY_TIMEOUT_US 500000  // 0.5 sec for ARP reply

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

int send_arp_request(int sock, int ifindex, struct in_addr my_ip, struct ether_addr my_mac, struct in_addr target_ip) {
    struct sockaddr_ll sa = {0};
    sa.sll_family = AF_PACKET;
    sa.sll_ifindex = ifindex;
    sa.sll_halen = ETH_ALEN;
    memset(sa.sll_addr, 0xff, ETH_ALEN);
    uint8_t buf[ETH_FRAME_LEN] = {0};
    struct ether_header *eh = (struct ether_header *)buf;
    memset(eh->ether_dhost, 0xff, ETH_ALEN);
    memcpy(eh->ether_shost, &my_mac, ETH_ALEN);
    eh->ether_type = htons(ETH_P_ARP);
    uint8_t *p = buf + ETH_HLEN;
    *p++ = 0x00; *p++ = 0x01;  // ar_hrd
    *p++ = 0x08; *p++ = 0x00;  // ar_pro
    *p++ = ETH_ALEN;  // ar_hln
    *p++ = 4;  // ar_pln
    *p++ = 0x00; *p++ = 0x01;  // ar_op (request)
    memcpy(p, &my_mac, ETH_ALEN); p += ETH_ALEN;
    memcpy(p, &my_ip.s_addr, 4); p += 4;
    memset(p, 0, ETH_ALEN); p += ETH_ALEN;
    memcpy(p, &target_ip.s_addr, 4); p += 4;
    int len = p - buf;
    return sendto(sock, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

int is_arp_reply(const uint8_t *buf, int len, struct in_addr target_ip) {
    if (len < ETH_HLEN + 28) return 0;
    const struct ether_header *eh = (const struct ether_header *)buf;
    if (ntohs(eh->ether_type) != ETH_P_ARP) return 0;
    const uint8_t *p = buf + ETH_HLEN;
    if (p[6] != 0x00 || p[7] != 0x02) return 0;  // op == reply
    struct in_addr sender_ip;
    memcpy(&sender_ip, p + 14, 4);  // sender IP offset
    return (sender_ip.s_addr == target_ip.s_addr);
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
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (sock < 0) {
        perror("ARP socket");
        return 1;
    }
    struct sockaddr_ll bind_sa = {0};
    bind_sa.sll_family = AF_PACKET;
    bind_sa.sll_protocol = htons(ETH_P_ARP);
    bind_sa.sll_ifindex = ifindex;
    if (bind(sock, (struct sockaddr *)&bind_sa, sizeof(bind_sa)) < 0) {
        perror("bind ARP socket");
        close(sock);
        return 1;
    }
    struct timeval tv = {0, REPLY_TIMEOUT_US};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int states[MAX_TARGETS] = {0};
    while (1) {
        for (int i = 0; i < conf.num_targets; i++) {
            if (send_arp_request(sock, ifindex, my_ip, my_mac, conf.target_ip[i]) < 0) {
                perror("send ARP request");
                continue;
            }
            int replied = 0;
            while (1) {
                uint8_t buf[1024];
                int n = recv(sock, buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("recv ARP");
                    break;
                }
                if (is_arp_reply(buf, n, conf.target_ip[i])) {
                    replied = 1;
                }
            }
            if (replied) {
                if (states[i] == 0) {
                    if (send_wol(&conf, i, lan_ip) < 0) {
                        perror("send WoL");
                    }
                    states[i] = 1;
                }
            } else {
                states[i] = 0;
            }
        }
        sleep(POLL_INTERVAL);
    }
    close(sock);
    return 0;
}
