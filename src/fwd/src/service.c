/*
 *  ____  ____      _    ____ ___ _   _  ___  
 *  |  _ \|  _ \    / \  / ___|_ _| \ | |/ _ \ 
 *  | | | | |_) |  / _ \| |  _ | ||  \| | | | |
 *  | |_| |  _ <  / ___ \ |_| || || |\  | |_| |
 *  |____/|_| \_\/_/   \_\____|___|_| \_|\___/ 
 *
 * Dragino_gw_fwd -- An opensource lora gateway forward 
 *
 * See http://www.dragino.com for more information about
 * the lora gateway project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Maintainer: skerlan
 *
 */

/*!
 * \file
 * \brief GW service process 
 *      
*/

#include <string.h>
#include <stdio.h>

#include <sys/socket.h>			/* socket specific definitions */
#include <netinet/in.h>			/* INET constants and stuff */
#include <arpa/inet.h>			/* IP address conversion stuff */
#include <netdb.h>				/* gai_strerror */

#include <errno.h>				

#include "fwd.h"
#include "db.h"
#include "service.h"
#include "semtech_service.h"
#include "pkt_service.h"
#include "relay_service.h"
#include "delay_service.h"
#include "mqtt_service.h"

DECLARE_GW;

int init_sock(const char *addr, const char *port, const void *timeout, int size) {
    int i;
    int sockfd;
    /* network socket creation */
    struct addrinfo hints;
    struct addrinfo *result;    /* store result of getaddrinfo */
    struct addrinfo *q;         /* pointer to move into *result data */

    char host_name[64];
    char port_name[64];

    /* prepare hints to open network sockets */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;	/* WA: Forcing IPv4 as AF_UNSPEC makes connection on localhost to fail */
    hints.ai_socktype = SOCK_DGRAM;

    /* look for server address w/ upstream port */
    i = getaddrinfo(addr, port, &hints, &result);
    if (i != 0) {
        //lgw_log(LOG_ERROR, "%s[NETWORK] getaddrinfo on address %s (PORT %s) returned %s\n", ERRMSG, addr, port, gai_strerror(i));
        return -1;
    }

    /* try to open socket for upstream traffic */
    for (q = result; q != NULL; q = q->ai_next) {
        sockfd = socket(q->ai_family, q->ai_socktype, q->ai_protocol);
        if (sockfd == -1)
            continue;       /* try next field */
        else
            break;          /* success, get out of loop */
    }

    if (q == NULL) {
        lgw_log(LOG_ERROR, "%s[NETWORK] failed to open socket to any of server %s addresses (port %s)\n", ERRMSG, addr, port);
        i = 1;
        for (q = result; q != NULL; q = q->ai_next) {
            getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
            ++i;
        }
        Close(sockfd);
        freeaddrinfo(result);

        return -1;
    }

    /* connect so we can send/receive packet with the server only */
    i = connect(sockfd, q->ai_addr, q->ai_addrlen);
    if (i != 0) {
        lgw_log(LOG_ERROR, "%s[NETWORK] connecting... %s\n", WARNMSG, strerror(errno));
        Close(sockfd);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);

    if ((setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, timeout, size)) != 0) {
        Close(sockfd);
        lgw_log(LOG_ERROR, "%s[NETWORK] setsockopt returned %s\n", ERRMSG, strerror(errno));
        return -1;
    }

    return sockfd;
}

bool pkt_basic_filter(serv_s* serv, const uint32_t addr, const uint8_t fport, const char* deveui) {
    char addr_key[64] = {0};
    char fport_key[48] = {0};
    char nwkid_key[64] = {0};
    char deveui_key[64] = {0};
    uint8_t nwkid = 0;

    snprintf(addr_key, sizeof(addr_key), "%s/devaddr/%08X", serv->info.name, addr);
    snprintf(fport_key, sizeof(fport_key), "%s/fport/%u", serv->info.name, fport);
#ifdef BIGENDIAN
        nwkid = addr & 0x7F;
#else
        nwkid = (addr >> 25) & 0x7F;   /* Devaddr Format:  31..25(NwkID)  24..0(NwkAddr) */
#endif
    snprintf(nwkid_key, sizeof(nwkid_key), "%s/nwkid/%02X", serv->info.name, nwkid);
    snprintf(deveui_key, sizeof(deveui_key), "%s/deveui/%s", serv->info.name, deveui);

    lgw_log(LOG_INFO, "%s[%s-filter] fport-lv=%d, addr-lv=%d, nwkid-lv=%d, deveui-lv=%d, addr_key=%s, fport_key=%s, nwkid_key=%s, deveui_key=%s\n", INFOMSG, 
                        serv->info.name, serv->filter.fport, serv->filter.devaddr, serv->filter.nwkid, serv->filter.deveui, 
                        addr_key, fport_key, nwkid_key, deveui_key);
    
    switch (serv->filter.fport) {
    case INCLUDE: // 1
        if (lgw_db_key_exist(fport_key)){
            lgw_log(LOG_INFO, "%s[%s-filter] fport filter include\n", INFOMSG, serv->info.name);
            return true;  // filter
        }
        lgw_log(LOG_INFO, "%s[%s-filter] fport filter not include\n", INFOMSG, serv->info.name);
        break;
    case EXCLUDE: // 2
        if (!lgw_db_key_exist(fport_key) && fport>0){
            lgw_log(LOG_INFO, "%s[%s-filter] fport filter exclude\n", INFOMSG, serv->info.name);
            return true;  //filter
        }
        lgw_log(LOG_INFO, "%s[%s-filter] fport filter not exclude\n", INFOMSG, serv->info.name);
        break;
    default:
        lgw_log(LOG_INFO, "%s[%s-filter] fport no filter\n", INFOMSG, serv->info.name);
        break;
    }

    switch(serv->filter.devaddr) {
    case INCLUDE: //1
        if (lgw_db_key_exist(addr_key)){
            lgw_log(LOG_INFO, "%s[%s-filter] devaddr filter include\n", INFOMSG, serv->info.name);
            return true; // filter
        }
        lgw_log(LOG_INFO, "%s[%s-filter] devaddr filter not include\n", INFOMSG, serv->info.name);
        break;
    case EXCLUDE:
        if (!lgw_db_key_exist(addr_key) && addr>0){
            lgw_log(LOG_INFO, "%s[%s-filter] devaddr filter exclude\n", INFOMSG, serv->info.name);
            return true; 
        }
        lgw_log(LOG_INFO, "%s[%s-filter] devaddr filter not exclude\n", INFOMSG, serv->info.name);
        break;
    default:
        lgw_log(LOG_INFO, "%s[%s-filter] devaddr no filter\n", INFOMSG, serv->info.name);
        break;
    }

    switch(serv->filter.nwkid) {
    case INCLUDE: //1
        if (lgw_db_key_exist(nwkid_key)){
            lgw_log(LOG_INFO, "%s[%s-filter] nwkid(%02X) filter include \n", INFOMSG, serv->info.name, nwkid);
            return true; // filter
        }
        lgw_log(LOG_INFO, "%s[%s-filter] nwkid(%02X) filter not include \n", INFOMSG, serv->info.name, nwkid);
        break;
    case EXCLUDE:
        if (!lgw_db_key_exist(nwkid_key) && addr>0){
            lgw_log(LOG_INFO, "%s[%s-filter] nwkid(%02X) filter exclude \n", INFOMSG, serv->info.name, nwkid);
            return true; 
        }
        lgw_log(LOG_INFO, "%s[%s-filter] nwkid(%02X) filter not exclude \n", INFOMSG, serv->info.name, nwkid);
        break;
    default:
        lgw_log(LOG_INFO, "%s[%s-filter] nwkid(%02X) no filter\n", INFOMSG, serv->info.name, nwkid);
        break;
    }

    switch(serv->filter.deveui) {
    case INCLUDE: //1
        if (lgw_db_key_exist(deveui_key)){
            lgw_log(LOG_INFO, "%s[%s-filter] deveui(%s) filter include \n", INFOMSG, serv->info.name, deveui);
            return true; // filter
        }
        lgw_log(LOG_INFO, "%s[%s-filter] deveui(%s) filter not include \n", INFOMSG, serv->info.name, deveui);
        break;
    case EXCLUDE:
        if (!lgw_db_key_exist(deveui_key)){
            lgw_log(LOG_INFO, "%s[%s-filter] deveui(%s) filter exclude \n", INFOMSG, serv->info.name, deveui);
            return true; 
        }
        lgw_log(LOG_INFO, "%s[%s-filter] deveui(%s) filter not exclude \n", INFOMSG, serv->info.name, deveui);
        break;
    default:
        lgw_log(LOG_INFO, "%s[%s-filter] deveui(%s) no filter\n", INFOMSG, serv->info.name, deveui);
        break;
    }
    //lgw_log(LOG_DEBUG, "%s[%s-filter] default: no filter\n", DEBUGMSG, serv->info.name);
    return false;  // no-filter
}

/*
void service_handle_rxpkt(rxpkts_s* rxpkt) {
    serv_s* serv_entry = NULL;
    rxpkts_s* rxpkt_entry = NULL;
    LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) { 
        rxpkt_entry = (rxpkts_s*)lgw_malloc(sizeof(rxpkts_s));
        if (NULL == rxpkt_entry) continue;
        memcpy(rxpkt_entry, rxpkt, sizeof(rxpkts_s));
        LGW_LIST_LOCK(&serv_entry->rxpkts_list);
        LGW_LIST_INSERT_HEAD(&serv_entry->rxpkts_list, rxpkt_entry, list);
        LGW_LIST_UNLOCK(&GW.rxpkts_list);
        sem_post(&serv_entry->thread.sema);
    }
}
*/

void service_start() {
    serv_s* serv_entry;
    LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) { 
        switch (serv_entry->info.type) {
        case semtech:
            semtech_start(serv_entry);
            break;
        case pkt:
            pkt_start(serv_entry);
            break;
        case relay:
            relay_start(serv_entry);
            break;
        case delay:
            delay_start(serv_entry);
            break;
        case mqtt:
            mqtt_start(serv_entry);
            break;
        default:
            break;
        }
    }
}

void service_stop() {
    serv_s* serv_entry;
    LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) { 
        switch (serv_entry->info.type) {
        case semtech:
            semtech_stop(serv_entry);
            break;
        case pkt:
            pkt_stop(serv_entry);
            break;
        case relay:
            relay_stop(serv_entry);
            break;
        case delay:
            delay_stop(serv_entry);
            break;
        case mqtt:
            mqtt_stop(serv_entry);
            break;
        default:
            semtech_stop(serv_entry);
            break;
        }
    }
}

uint16_t crc16(const uint8_t * data, unsigned size) {
    const uint16_t crc_poly = 0x1021;
    const uint16_t init_val = 0x0000;
    uint16_t x = init_val;
    unsigned i, j;

    if (data == NULL)  {
        return 0;
    }

    for (i=0; i<size; ++i) {
        x ^= (uint16_t)data[i] << 8;
        for (j=0; j<8; ++j) {
            x = (x & 0x8000) ? (x<<1) ^ crc_poly : (x<<1);
        }
    }

    return x;
}

